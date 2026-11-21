"""Real Classic PS binary/text parameter metadata, with no SQL rewriting."""
import argparse
import json
from pathlib import Path
import struct

from .classic_binary import binary_connection, encode_string, length_integer, length_string
from .fixture import BASEDIR, BIN, SHELL, Fixture, require
from .sql_short_native_acceptance import execute


def bound_select(connection, statement, values, long_data=()):
    count=len(values)
    bitmap=sum(1 << index for index,value in enumerate(values) if value is None)
    payload=b'\x17'+struct.pack('<IBI',statement,0,1)+bitmap.to_bytes((count+7)//8,'little')+b'\x01'
    payload+=b''.join(bytes((6 if value is None else 252 if isinstance(value,bytes) else 8 if isinstance(value,int) else 253,
                            128 if isinstance(value,int) and value>=1<<63 else 0)) for value in values)
    for index,value in enumerate(values):
        if value is not None and index not in long_data:
            payload+=(value.to_bytes(8,'little',signed=value<1<<63) if isinstance(value,int) else
                      encode_string(value if isinstance(value,bytes) else value.encode('utf-8')))
    connection.sequence=0;connection.write_packet(payload)
    packet=connection.read_packet();connection.check_error(packet)
    columns,_=length_integer(packet);metadata=connection.metadata(columns)
    rows=[]
    while True:
        packet=connection.read_packet();connection.check_error(packet)
        if packet[:1]==b'\xfe' and len(packet)<9:break
        require(packet[:1]==b'\0','binary parameter row marker missing')
        width=(columns+2+7)//8;nulls=int.from_bytes(packet[1:1+width],'little')
        offset=1+width;row=[]
        for column in range(columns):
            if nulls & (1 << (column+2)):row.append(None)
            else:
                width={1:1,2:2,3:4,8:8,9:4,13:2}.get(metadata[column]['type'])
                if width:
                    require(offset+width<=len(packet),'truncated integer result')
                    row.append(int.from_bytes(packet[offset:offset+width],'little',signed=not bool(metadata[column]['flags']&32)))
                    offset+=width
                else:
                    value,offset=length_string(packet,offset);row.append(value)
        require(offset==len(packet),'binary parameter packet width differs')
        rows.append(row)
    return metadata,rows


def assert_value(metadata,rows,values):
    expected=[value if value is None or isinstance(value,bytes) else value.encode('utf-8') for value in values]
    require(rows==[expected],'bound parameter bytes changed: '+repr(rows))
    for column,value in zip(metadata,values):
        if isinstance(value,bytes):
            require(column['charset']==63 and column['flags']&128,
                    'binary parameter result metadata claimed text: '+repr(column))
        elif isinstance(value,str):
            require(column['charset']!=63,'text parameter result metadata claimed binary: '+repr(column))


def exercise(instance):
    with binary_connection(instance) as connection:
        connection.query('SET NAMES utf8mb4 COLLATE utf8mb4_0900_ai_ci')
        statement,parameters,prepared=connection.prepare('SELECT ? AS value')
        require(parameters==1 and prepared[0]['charset']!=63,'unbound prepare metadata changed to binary')
        try:
            for value in [b'\x00\x01\xfe\xff',b'',b'valid UTF8 bytes','Türkçe - 🌌',None,b'\x00\xff','son']:
                metadata,rows=bound_select(connection,statement,[value]);assert_value(metadata,rows,[value])
            # COM_STMT_SEND_LONG_DATA has no response, and the streamed value
            # is omitted from COM_STMT_EXECUTE while its BLOB type remains.
            for chunk in [b'\x00\xff',b'',b'\xfe\x01']:
                connection.sequence=0
                connection.write_packet(b'\x18'+struct.pack('<IH',statement,0)+chunk)
            metadata,rows=bound_select(connection,statement,[b''],long_data=(0,))
            assert_value(metadata,rows,[b'\x00\xff\xfe\x01'])
            metadata,rows=bound_select(connection,statement,['stream reset'])
            assert_value(metadata,rows,['stream reset'])
        finally:connection.close_statement(statement)
        # An expression's declared text/JSON/numeric semantics must remain its
        # own; a binary parameter inside CAST cannot reclassify its output.
        for sql,value,expected in [
            ('SELECT CAST(? AS CHAR CHARACTER SET utf8mb4)', 'Türkçe'.encode(), 'Türkçe'.encode()),
            ('SELECT CONVERT(? USING utf8mb4) COLLATE utf8mb4_bin','İstanbul'.encode(),'İstanbul'.encode()),
            ('SELECT CAST(? AS UNSIGNED)',18446744073709551615,18446744073709551615),
            ('SELECT CAST(? AS JSON)','{"n":1}',{'n':1}),
            # Native JSON represents a binary string as an opaque base64
            # value; the resolved VARCHAR parameter has type code15.
            ('SELECT CAST(? AS JSON)',b'{"n":1}','base64:type15:eyJuIjoxfQ=='),
        ]:
            statement,parameters,prepared=connection.prepare(sql)
            try:
                metadata,rows=bound_select(connection,statement,[value])
                require([(field['type'],field['charset'],field['flags']) for field in metadata]==
                        [(field['type'],field['charset'],field['flags']) for field in prepared],
                        'parameter binding changed expression metadata: '+sql)
                if 'AS JSON' in sql:
                    require(json.loads(rows[0][0].decode('utf-8'))==expected,'JSON CAST result changed')
                else:require(rows==[[expected]],'typed expression result changed: '+sql+repr(rows))
            finally:connection.close_statement(statement)
        statement,parameters,prepared=connection.prepare('SELECT ? AS raw_value,? AS text_value,? AS empty_value')
        require(parameters==3 and all(column['charset']!=63 for column in prepared),'mixed prepare metadata leaked a prior value')
        try:
            for values in [[b'\xff\x00','İstanbul',b''],['metin',b'\xfe',None],[b'','',b'\x00']]:
                metadata,rows=bound_select(connection,statement,values);assert_value(metadata,rows,values)
        finally:connection.close_statement(statement)

    # OUT-parameter metadata must still come from its declared SP variable,
    # even if the client bound the placeholder using a binary input type.
    execute(instance,"""CREATE SCHEMA parameter_fixture;
      CREATE PROCEDURE parameter_fixture.output_text(OUT result VARCHAR(32) CHARACTER SET utf8mb4)
        SET result='Türkçe';""")
    import mysql.connector
    from mysql.connector.constants import ClientFlag
    options={part[2:].split('=',1)[0]:part.split('=',1)[1] for part in instance[1] if part.startswith('--') and '=' in part}
    connection=mysql.connector.connect(host='127.0.0.1',port=int(options['port']),user='root',password='',
        ssl_ca=options['ssl-ca'],ssl_verify_cert=True,use_pure=True,autocommit=True,
        client_flags=[ClientFlag.PS_MULTI_RESULTS])
    try:
        cursor=connection.cursor(prepared=True)
        try:
            cursor.execute('CALL parameter_fixture.output_text(?)',(b'\xff',))
            rows=cursor.fetchall()
            require(cursor.description[0][8]!=63,'declared text OUT charset was replaced by binary input metadata')
            normalized=[tuple(value.decode('utf-8') if isinstance(value,(bytes,bytearray)) else value for value in row) for row in rows]
            require(normalized==[('Türkçe',)],'declared text OUT result changed: '+repr(rows))
            while cursor.nextset():
                if cursor.with_rows:cursor.fetchall()
            cursor.execute('SELECT CAST(? AS UNSIGNED)',(42,))
            require(cursor.fetchall()==[(42,)],'OUT result left a trailing packet before the next prepared statement')
        finally:cursor.close()
    finally:connection.close()
    print('PASS native bound binary/text/empty/NULL/mixed/repeated PS and long-data metadata, unbound prepare, declared OUT override and post-OUT continuation',flush=True)


def main():
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--mysqld',type=Path,default=BIN/'mysqld.exe')
    parser.add_argument('--mysql',type=Path,default=BIN/'mysql.exe')
    parser.add_argument('--basedir',type=Path,default=BASEDIR)
    args=parser.parse_args()
    fixture=Fixture(args.mysqld.resolve(strict=True),args.mysql.resolve(strict=True),args.basedir.resolve(strict=True),SHELL,None)
    with fixture.scratch('binary-parameter-native') as root:
        with fixture.disposable(root/'server') as instance:exercise(instance)


if __name__=='__main__':main()
