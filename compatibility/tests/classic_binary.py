"""Minimal TLS Classic binary-protocol client for disposable native acceptance.

Only the fixture's passwordless loopback account is supported. This deliberately
uses COM_STMT_PREPARE/EXECUTE rather than SQL PREPARE or client interpolation.
"""
from contextlib import contextmanager
import socket
import ssl
import struct

from .fixture import require


def length_integer(packet, offset=0):
    first = packet[offset]
    offset += 1
    if first < 0xfb:
        return first, offset
    if first == 0xfb:
        return None, offset
    widths = {0xfc: 2, 0xfd: 3, 0xfe: 8}
    require(first in widths, 'invalid length-encoded integer')
    width = widths[first]
    require(offset + width <= len(packet), 'truncated length-encoded integer')
    return int.from_bytes(packet[offset:offset+width], 'little'), offset+width


def length_string(packet, offset=0):
    length, offset = length_integer(packet, offset)
    require(length is not None and offset+length <= len(packet), 'truncated length-encoded string')
    return packet[offset:offset+length], offset+length


def encode_string(value):
    length = len(value)
    if length < 0xfb:
        return bytes((length,)) + value
    if length <= 0xffff:
        return b'\xfc' + struct.pack('<H', length) + value
    if length <= 0xffffff:
        return b'\xfd' + length.to_bytes(3, 'little') + value
    return b'\xfe' + struct.pack('<Q', length) + value


def column_definition(packet):
    fields, offset = [], 0
    for _ in range(6):
        value, offset = length_string(packet, offset)
        fields.append(value.decode('utf-8'))
    fixed, offset = length_integer(packet, offset)
    require(fixed == 12 and offset+12 <= len(packet), 'invalid ColumnDefinition41')
    charset, length, kind, flags, decimals = struct.unpack_from('<HIBHB', packet, offset)
    return {'schema': fields[1], 'table': fields[2], 'org_table': fields[3],
            'name': fields[4], 'org_name': fields[5], 'charset': charset, 'length': length,
            'type': kind, 'flags': flags, 'decimals': decimals}


class ClassicBinary:
    def __init__(self, host, port, ca, user='root'):
        require(host == '127.0.0.1', 'binary fixture must use its disposable loopback server')
        self.socket = socket.create_connection((host, port), timeout=15)
        self.sequence = 0
        greeting = self.read_packet()
        require(greeting[0] == 10, 'unexpected Classic greeting')
        offset = greeting.index(b'\0', 1) + 1 + 4 + 8 + 1
        lower = struct.unpack_from('<H', greeting, offset)[0]
        upper = struct.unpack_from('<H', greeting, offset+2+1+2)[0]
        capabilities = lower | (upper << 16)
        flags = (1 | 4 | 0x200 | 0x800 | 0x2000 | 0x8000 | 0x20000 | 0x40000 | 0x80000)
        require(capabilities & 0x800 and capabilities & 0x80000, 'fixture must support TLS and plugin authentication')
        flags &= capabilities
        header = struct.pack('<IIB23s', flags, 16*1024*1024, 45, bytes(23))
        self.write_packet(header)
        context = ssl.create_default_context(cafile=ca)
        context.check_hostname = False  # VERIFY_CA, matching the native fixture.
        self.socket = context.wrap_socket(self.socket, server_hostname='localhost')
        require('\0' not in user, 'invalid fixture account')
        self.write_packet(header + user.encode('utf-8') + b'\0\0caching_sha2_password\0')
        packet = self.read_packet()
        if packet[:1] == b'\xfe':
            self.write_packet(b'')
            packet = self.read_packet()
        if packet[:2] == b'\x01\x04':
            self.write_packet(b'\0')  # Empty password, already encrypted by TLS.
            packet = self.read_packet()
        elif packet[:2] == b'\x01\x03':
            packet = self.read_packet()
        self.check_error(packet)
        require(packet[:1] == b'\0', 'disposable account authentication failed')

    def receive(self, length):
        chunks = []
        while length:
            chunk = self.socket.recv(length)
            require(bool(chunk), 'Classic server closed the acceptance connection')
            chunks.append(chunk)
            length -= len(chunk)
        return b''.join(chunks)

    def read_packet(self):
        chunks = []
        while True:
            header = self.receive(4)
            length = int.from_bytes(header[:3], 'little')
            require(header[3] == self.sequence % 256, 'Classic packet sequence mismatch')
            self.sequence += 1
            chunks.append(self.receive(length))
            require(sum(map(len, chunks)) <= 128*1024*1024, 'unexpectedly large inspection result')
            if length != 0xffffff:
                return b''.join(chunks)

    def write_packet(self, payload):
        require(len(payload) < 0xffffff, 'acceptance request exceeds one Classic packet')
        self.socket.sendall(len(payload).to_bytes(3, 'little') + bytes((self.sequence % 256,)) + payload)
        self.sequence += 1

    @staticmethod
    def check_error(packet):
        if packet[:1] == b'\xff':
            code = struct.unpack_from('<H', packet, 1)[0]
            raise RuntimeError(f'Classic native error {code}: '+packet[3:].decode('utf-8', errors='replace'))

    def metadata(self, count):
        result = []
        for _ in range(count):
            packet = self.read_packet()
            self.check_error(packet)
            result.append(column_definition(packet))
        if count:
            eof = self.read_packet()
            require(eof[:1] == b'\xfe' and len(eof) < 9, 'missing Classic metadata EOF')
        return result

    def prepare(self, sql):
        self.sequence = 0
        self.write_packet(b'\x16' + sql.encode('utf-8'))
        packet = self.read_packet()
        self.check_error(packet)
        require(packet[:1] == b'\0' and len(packet) >= 12, 'invalid COM_STMT_PREPARE response')
        statement, columns, parameters = struct.unpack_from('<IHH', packet, 1)
        self.metadata(parameters)
        metadata = self.metadata(columns)
        return statement, parameters, metadata

    def query(self, sql):
        self.sequence = 0
        self.write_packet(b'\x03' + sql.encode('utf-8'))
        packet = self.read_packet()
        self.check_error(packet)
        if packet[:1] == b'\0':
            return [], []
        count, _ = length_integer(packet)
        metadata = self.metadata(count)
        rows = []
        while True:
            packet = self.read_packet()
            self.check_error(packet)
            if packet[:1] == b'\xfe' and len(packet) < 9:
                break
            row, offset = [], 0
            for _ in range(count):
                length, offset = length_integer(packet, offset)
                if length is None:
                    row.append(None)
                else:
                    require(offset+length <= len(packet), 'truncated Classic text row')
                    row.append(packet[offset:offset+length].decode('utf-8'))
                    offset += length
            require(offset == len(packet), 'text row did not match result metadata')
            rows.append(row)
        return metadata, rows

    def execute(self, statement, value=None):
        payload = b'\x17' + struct.pack('<IBI', statement, 0, 1)
        if value is not None:
            payload += b'\0\x01\xfd\0' + encode_string(value.encode('utf-8'))
        self.sequence = 0
        self.write_packet(payload)
        packet = self.read_packet()
        self.check_error(packet)
        count, _ = length_integer(packet)
        metadata = self.metadata(count)
        require(all(field['type'] in (0x0f, 0xf9, 0xfa, 0xfb, 0xfc, 0xfd, 0xfe) for field in metadata),
                'inspection binary result must contain fixed string/blob columns')
        rows = []
        while True:
            packet = self.read_packet()
            self.check_error(packet)
            if packet[:1] == b'\xfe' and len(packet) < 9:
                break
            require(packet[:1] == b'\0', 'expected a binary result row')
            bitmap_length = (count+2+7)//8
            bitmap = int.from_bytes(packet[1:1+bitmap_length], 'little')
            offset, row = 1+bitmap_length, []
            for index in range(count):
                if bitmap & (1 << (index+2)):
                    row.append(None)
                else:
                    value, offset = length_string(packet, offset)
                    row.append(value.decode('utf-8'))
            require(offset == len(packet), 'binary row did not match prepared result metadata')
            rows.append(row)
        return metadata, rows

    def close_statement(self, statement):
        self.sequence = 0
        self.write_packet(b'\x19' + struct.pack('<I', statement))

    def close(self):
        self.socket.close()


@contextmanager
def binary_connection(instance):
    _, command, _ = instance
    options = dict(argument[2:].split('=', 1) for argument in command
                   if argument.startswith('--') and '=' in argument)
    connection = ClassicBinary(options['host'], int(options['port']), options['ssl-ca'], options.get('user', 'root'))
    try:
        yield connection
    finally:
        connection.close()
