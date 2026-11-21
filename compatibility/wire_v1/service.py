"""Serve StarDataParadise wire v1 operations on loopback, using TLS by default."""

import argparse
from contextlib import suppress
from dataclasses import dataclass
import json
from pathlib import Path
import re
import secrets
from select import select
import socket
import socketserver
import ssl
import struct
import threading
import time
from uuid import UUID

from mysql import connector
from mysql.connector.constants import ClientFlag, ServerFlag
from mysql.connector.errors import Error, ReadTimeoutError, WriteTimeoutError

from ledger import Receipts, read_key, request_token
from legacy_backup import OPERATIONS as BACKUP_OPERATIONS, dispatch as dispatch_backup, load_config as load_backup
from legacy_decision import validate as validate_decision
from legacy_model import OPERATIONS as MODEL_OPERATIONS, dispatch as dispatch_model
from native_sql import NativeFault, NativeSession
from legacy_rpc import OPERATIONS as LEGACY_OPERATIONS, dispatch as dispatch_legacy, load_config as load_legacy
from sql import (Fault, binding_values, column_type, integer_field, name_field, object_field,
                 prepare_sql, require, wire_value)


LIMIT = 1 << 20
SQL_MODE = "STRICT_ALL_TABLES,NO_ENGINE_SUBSTITUTION,NO_BACKSLASH_ESCAPES"
SAVEPOINT_OPERATIONS = {"tx_savepoint": "SAVEPOINT", "tx_rollback_to": "ROLLBACK TO SAVEPOINT",
                        "tx_release": "RELEASE SAVEPOINT"}
TRANSACTION_OPERATIONS = {*SAVEPOINT_OPERATIONS, "tx_begin", "tx_rollback", "tx_commit"}
FEATURES = {key: key in {"auth", "health", "wire_sql_basic_v1"} for key in (
    "tls", "auth", "health", "metadata", "sql", "transactions", "server_cancel", "jobs", "admin", "history",
    "decision", "ml", "wire_sql_basic_v1", "sql_crud_v1", "sql_security_v1", "plsql_results_v1", "history_v1",
    "decision_v1", "decision_explain_v1", "jobs_v1", "dataset_v1", "supervised_ml_v1", "backup_v1", "admin_v1",
)}
ERRORS = {
    1001: ("08P01", "Malformed message"), 1002: ("54000", "Message too large"),
    1003: ("0A000", "Incompatible protocol"), 1004: ("0A000", "Feature unsupported"),
    1005: ("28000", "Authentication failed"), 1006: ("42501", "Permission denied"),
    1008: ("HYT00", "Timeout"), 1010: ("08006", "Connection lost"),
    1012: ("22023", "Invalid argument"), 1013: ("55000", "Invalid state"),
    1014: ("53000", "Resource limit"), 1015: ("XX000", "Internal error"),
}


def encode(value):
    return json.dumps(value, ensure_ascii=False, separators=(",", ":"), allow_nan=False).encode("utf-8")


def receive(sock, count, deadline):
    data = bytearray()
    while len(data) < count:
        timeout = deadline - time.monotonic()
        if timeout <= 0:
            raise TimeoutError
        sock.settimeout(timeout)
        chunk = sock.recv(count - len(data))
        if not chunk:
            raise EOFError
        data.extend(chunk)
    return data


def unique_object(pairs):
    result = {}
    for key, value in pairs:
        if key in result:
            raise ValueError("duplicate field")
        result[key] = value
    return result


def invalid_constant(_value):
    raise ValueError("non-finite number")


def validate_json(value, depth=0):
    if isinstance(value, str):
        value.encode("utf-8")
    elif isinstance(value, (dict, list)):
        if depth == 32:
            raise ValueError("too many nested containers")
        if isinstance(value, dict):
            for key, item in value.items():
                key.encode("utf-8")
                validate_json(item, depth + 1)
        else:
            for item in value:
                validate_json(item, depth + 1)


def read_request(sock, previous, header=None):
    if header is None:
        header = receive(sock, 4, time.monotonic() + 120)
    length, = struct.unpack("!I", header)
    if not 1 <= length <= LIMIT:
        raise Fault(1002 if length else 1001, close=True)
    data = receive(sock, length, time.monotonic() + 10)
    try:
        request = json.loads(data.decode("utf-8"), object_pairs_hook=unique_object,
                             parse_constant=invalid_constant)
        validate_json(request)
    except (ValueError, RecursionError):
        raise Fault(1001, close=True) from None
    if (not isinstance(request, dict) or not isinstance(request.get("body"), dict)
            or not isinstance(request.get("op"), str) or type(request.get("id")) is not int
            or not previous < request["id"] <= 0xffffffff or (not previous and request["id"] != 1)):
        raise Fault(1001, close=True)
    return request


def send(sock, request_id, *, body=None, fault=None):
    response = {"v": 1, "id": request_id, "ok": fault is None}
    if fault is None:
        response["body"] = body
    else:
        if isinstance(fault, NativeFault):
            response["error"] = fault.document
        else:
            sqlstate, message = ERRORS[fault.code]
            response["error"] = {
                "code": fault.code, "sqlstate": sqlstate, "message": message, "line": None, "column": None,
                "retryable": False, "unknown_outcome": fault.uncertain, "correlation_id": None,
            }
    payload = encode(response)
    if len(payload) > LIMIT:
        raise Fault(1002, close=True)
    sock.settimeout(10)
    sock.sendall(struct.pack("!I", len(payload)) + payload)


def backend_fault(error, *, committing=False):
    if isinstance(error, (ReadTimeoutError, WriteTimeoutError)) or error.errno in {1205, 3024}:
        return Fault(1008, close=True, uncertain=committing)
    if error.errno in {2006, 2013, 2055}:
        return Fault(1010, close=True, uncertain=committing)
    if error.errno == 1785:
        return Fault(1004)
    if error.errno in {1044, 1045, 1142, 1143, 1227, 1370, 1792}:
        return Fault(1006)
    if error.errno in {1064, 1146, 1054, 1062, 1210, 1264, 1292, 1364, 1406}:
        return Fault(1012)
    if error.errno in {1305, 1213}:
        return Fault(1013)
    return Fault(1015, close=True, uncertain=committing)


@dataclass
class Statement:
    verb: str
    native: dict
    declarations: dict
    ordered_names: list


@dataclass
class Result:
    result_id: str
    statement_id: str
    native_id: int
    native_columns: list
    columns: list
    remaining: int
    owned_read: bool
    index: int = 0
    pending: tuple | None = None


class Session:
    def __init__(self, config):
        self.config = config
        self.wire_id = secrets.token_hex(16)
        self.db = None
        self.identity = None
        self.nonce = secrets.token_hex(32)
        self.receipts = None
        self.anchor = None
        self.statements = {}
        self.result = None
        self.native_sql = None
        self.default_schema_ready = not bool(getattr(config,'native_sql',False))

    def identity_setup(self, username):
        with self.db.cursor(buffered=True) as cursor:
            cursor.execute("SELECT @@server_uuid, CURRENT_USER(), CONNECTION_ID(), VERSION()")
            identity = cursor.fetchone()
            require(identity[0].lower() == self.config.mysql_uuid
                    and identity[1].rpartition("@")[0] == username
                    and identity[3] == self.config.product_version + "-StarDataParadise", 1005)
            cursor.execute("SHOW SESSION STATUS LIKE 'Ssl_cipher'")
            require(bool(cursor.fetchone()[1]), 1005)
            if self.config.ledger_key is not None:
                cursor.execute("SET SESSION TRANSACTION ISOLATION LEVEL READ COMMITTED")
            cursor.execute("SET SESSION sdp_compatibility_mode = 'legacy'")
        return identity

    def authenticate(self, body):
        username = body.get("username")
        password = body.pop("password", None)
        if (not isinstance(username, str) or not re.fullmatch(r"[A-Za-z0-9_.-]{1,32}", username)
                or not isinstance(password, str) or not 1 <= len(password) <= 1024 or "\0" in password):
            raise Fault(1005, close=True)
        try:
            multiple_results = (self.config.legacy_rpc and self.config.legacy_rpc.history) or getattr(self.config,'native_sql',False)
            self.db = connector.connect(
                host=self.config.mysql_host, port=self.config.mysql_port, user=username, password=password,
                database=None if getattr(self.config,'native_sql',False) else self.config.schema,
                use_pure=True, autocommit=True, charset="utf8mb4",
                sql_mode=SQL_MODE, ssl_verify_cert=True, ssl_verify_identity=True, ssl_ca=str(self.config.mysql_ca),
                tls_versions=["TLSv1.2", "TLSv1.3"], allow_local_infile=False,
                client_flags=[-ClientFlag.MULTI_STATEMENTS,
                              ClientFlag.MULTI_RESULTS if multiple_results else -ClientFlag.MULTI_RESULTS,
                              ClientFlag.PS_MULTI_RESULTS if getattr(self.config,'native_sql',False) else -ClientFlag.PS_MULTI_RESULTS],
                connection_timeout=5, read_timeout=20, write_timeout=20,
                conn_attrs={"sdp_wire_nonce": self.nonce} if self.config.ledger_key else None,
            )
            self.identity = self.identity_setup(username)
            self.setup_receipts()
        except (Error, ValueError, Fault):
            raise Fault(1005, close=True) from None
        finally:
            del password
        return {"authenticated": True}

    def setup_receipts(self):
        self.receipts = None
        if self.config.ledger_key is not None:
            ledger = Receipts(self.db, self.config.ledger_key, self.nonce, self.identity)
            if ledger.available():
                self.receipts = ledger

    def features(self):
        return {**FEATURES, "tls": not self.config.plaintext_loopback,
                "native_sql_v1": bool(getattr(self.config,'native_sql',False)),
                "wire_sql_transactions_v1": self.receipts is not None}

    def close(self):
        if self.db is not None:
            with suppress(Error, OSError):
                self.db.close()

    def health(self):
        with self.db.cursor(buffered=True) as cursor:
            cursor.execute("SELECT @@server_uuid, CURRENT_USER(), CONNECTION_ID(), VERSION()")
            if cursor.fetchone() != self.identity:
                raise Fault(1010, close=True)
        return {"status": "ready", "product_version": self.config.product_version}

    def prepare(self, body):
        require(len(self.statements) < 64, 1014)
        declarations = object_field(body, "parameter_types")
        verb, sql, names = prepare_sql(body.get("sql"), declarations)
        native = self.db.cmd_stmt_prepare(sql)
        if native["num_params"] != len(names):
            self.db.cmd_stmt_close(native["statement_id"])
            raise Fault(1012)
        statement_id = secrets.token_hex(16)
        self.statements[statement_id] = Statement(verb, native, declarations, names)
        return {"statement_id": statement_id}

    def statement(self, body):
        statement_id = body.get("statement_id")
        require(isinstance(statement_id, str) and statement_id in self.statements, 1013)
        return statement_id, self.statements[statement_id]

    def execute(self, body):
        statement_id, statement = self.statement(body)
        row_limit = integer_field(body, "row_limit", 10_000, 1_000_000)
        selecting = statement.verb == "SELECT"
        values = binding_values(body, statement.declarations, statement.ordered_names)
        if not selecting and self.receipts is not None and not self.db.in_transaction:
            return self.autocommit_write(statement, values, request_token(body))
        require("request_token" not in body, 1012 if self.receipts is not None else 1004)
        require(selecting or self.receipts is not None or self.config.ledger_key is None, 1004)
        require(selecting or self.db.in_transaction, 1013)
        if self.receipts is not None and self.db.in_transaction:
            self.receipts.command("probe")
        native = statement.native
        owned_read = selecting and not self.db.in_transaction
        if owned_read:
            self.db.start_transaction(readonly=True)
        try:
            response = self.db.cmd_stmt_execute(native["statement_id"], data=values,
                                                parameters=native["parameters"], flags=int(selecting))
        except Error:
            if owned_read:
                with suppress(Error):
                    self.db.rollback()
            raise
        if isinstance(response, dict):
            if owned_read:
                self.db.rollback()
            return {"affected_rows": response["affected_rows"]}
        count, columns, eof = response
        flags = eof.get("status_flag", eof.get("server_status", 0))
        if not flags & ServerFlag.STATUS_CURSOR_EXISTS:
            raise Fault(1004, close=True)
        try:
            require(count <= 64, 1014)
            metadata = [{"name": column[0], "type": column_type(column)} for column in columns]
        except Fault:
            self.db.cmd_stmt_reset(native["statement_id"])
            if owned_read:
                self.db.rollback()
            raise
        result_id = secrets.token_hex(16)
        self.result = Result(result_id, statement_id, native["statement_id"], columns, metadata, row_limit, owned_read)
        return {"affected_rows": 0, "result_id": result_id, "columns": metadata}

    def autocommit_write(self, statement, values, token):
        self.db.start_transaction()
        try:
            self.receipts.command("reserve", token)
            native = statement.native
            response = self.db.cmd_stmt_execute(native["statement_id"], data=values,
                                                parameters=native["parameters"])
            lsn = self.receipts.command("finish", token)
        except (Error, Fault):
            with suppress(Error):
                self.db.rollback()
            raise
        self.commit_backend()
        return {"affected_rows": response["affected_rows"], "lsn": str(lsn)}

    def commit_backend(self):
        try:
            self.db.commit()
        except Error as error:
            raise backend_fault(error, committing=True) from None

    def active_result(self, body):
        require(self.result is not None and body.get("result_id") == self.result.result_id, 1013)
        return self.result

    def discard_result(self):
        result = self.result
        if result.statement_id in self.statements:
            self.db.cmd_stmt_reset(result.native_id)
        else:
            self.db.cmd_stmt_close(result.native_id)
        self.result = None
        if result.owned_read:
            self.db.rollback()

    def next_row(self, result):
        self.db.cmd_stmt_fetch(result.native_id, rows=1)
        rows, eof = self.db.get_rows(binary=True, columns=result.native_columns)
        require(eof is not None and len(rows) <= 1, 1015)
        flags = eof.get("status_flag", eof.get("server_status", 0))
        end = not rows or bool(flags & ServerFlag.STATUS_LAST_ROW_SENT)
        if not rows:
            return None, end
        return [wire_value(value, metadata["type"])
                for value, metadata in zip(rows[0], result.columns, strict=True)], end

    def fetch(self, body):
        result = self.active_result(body)
        count = integer_field(body, "batch_rows", 128, 1024)
        batch = {"result_set": 0, "batch_index": result.index, "rows": [],
                 "end_of_set": False, "end_of_results": False}
        if result.index == 0:
            batch["columns"] = result.columns
        budget = LIMIT - len(encode(batch)) - 128
        try:
            for _ in range(min(count, result.remaining)):
                row, end = result.pending if result.pending is not None else self.next_row(result)
                result.pending = None
                if row is not None:
                    size = len(encode(row)) + 1
                    if size > budget:
                        require(bool(batch["rows"]), 1002)
                        result.pending = (row, end)
                        break
                    budget -= size
                    result.remaining -= 1
                    batch["rows"].append(row)
                if end or not result.remaining:
                    self.discard_result()
                    batch.update(end_of_set=True, end_of_results=True)
                    break
        except Fault:
            if self.result is not None:
                self.discard_result()
            raise
        result.index += 1
        return {"batch": batch}

    def transaction(self, operation, body):
        if operation == "tx_outcome":
            require(self.result is None, 1013)
            require(self.receipts is not None, 1004)
            lsn = self.receipts.command("lookup", request_token(body))
            return {"status": "unknown"} if lsn is None else {"status": "committed", "lsn": str(lsn)}
        if operation == "tx_rollback":
            if self.result is not None:
                self.discard_result()
            self.db.rollback()
            self.anchor = None
            return {}
        require(self.result is None, 1013)
        if operation == "tx_begin":
            require(not self.db.in_transaction, 1013)
            self.anchor = None
            if self.receipts is None:
                self.db.start_transaction(readonly=True if self.config.ledger_key is not None else None)
            else:
                self.db.start_transaction(isolation_level="SERIALIZABLE")
                # An InnoDB write must precede user SQL for GTID consistency to reject
                # writes to nontransactional tables, including through triggers or views.
                anchor = secrets.token_hex(16)
                try:
                    self.receipts.command("anchor", anchor=anchor)
                except (Error, Fault):
                    with suppress(Error):
                        self.db.rollback()
                    raise
                self.anchor = anchor
            return {}
        require(self.db.in_transaction, 1013)
        if operation == "tx_commit":
            if self.receipts is None:
                require("request_token" not in body, 1004)
                self.commit_backend()
                return {"status": "committed"}
            require(self.anchor is not None, 1013)
            token = request_token(body)
            self.receipts.command("reserve", token)
            try:
                lsn = self.receipts.command("finish", token, self.anchor)
            except (Error, Fault):
                with suppress(Error):
                    self.db.rollback()
                self.anchor = None
                raise
            self.commit_backend()
            self.anchor = None
            return {"status": "committed", "lsn": str(lsn)}
        keyword = SAVEPOINT_OPERATIONS[operation]
        name = name_field(body, "name")
        self.db.cmd_query(f"{keyword} `{name}`")
        return {}

    def dispatch(self, operation, body):
        if operation == "capabilities":
            return {"features": self.features()}
        if operation == "native.open":
            require(getattr(self.config,'native_sql',False),1004)
            require(self.native_sql is None and self.result is None and not self.statements and
                    self.anchor is None and not self.db.in_transaction,1013)
            auto=body.get('autocommit',True)
            require(type(auto) is bool)
            if not self.default_schema_ready:
                try:
                    self.db.database=self.config.schema
                    self.default_schema_ready=True
                except Error as error:
                    # An RPC worker can authenticate and execute fully-qualified
                    # native catalog commands without access to the bridge's
                    # legacy application schema. No schema grant is invented.
                    if error.errno!=1044:raise
            self.db.autocommit=auto
            self.native_sql=NativeSession(self.db,auto)
            return {'native_sql_v1':True,'autocommit':auto}
        if operation == "native.execute":
            require(self.native_sql is not None,1013)
            return self.native_sql.execute(body)
        if operation == "native.fetch":
            require(self.native_sql is not None,1013)
            return self.native_sql.fetch(body)
        # Native SQL controls its own transaction/session lifecycle. The legacy
        # CRUD receipt API cannot implicitly commit an active AI scope.
        require(self.native_sql is None,1013)
        if not self.default_schema_ready:
            self.db.database=self.config.schema
            self.default_schema_ready=True
        if operation == "fetch":
            return self.fetch(body)
        if operation == "result_cancel":
            self.active_result(body)
            self.discard_result()
            return {}
        if operation == "session_reset":
            if self.result is not None:
                self.discard_result()
            require(self.db.cmd_reset_connection(), 1015)
            self.db.autocommit = True
            self.db.sql_mode = SQL_MODE
            self.db.database = self.config.schema
            self.default_schema_ready=True
            identity = self.identity_setup(self.identity[1].rpartition("@")[0])
            if identity != self.identity:
                raise Fault(1010, close=True)
            self.statements.clear()
            self.anchor = None
            self.setup_receipts()
            return {}
        if operation in TRANSACTION_OPERATIONS or operation == "tx_outcome":
            return self.transaction(operation, body)
        if operation == "statement_close":
            statement_id, statement = self.statement(body)
            # JDBC closes its wire statement once the execution exists. The
            # native handle still owns the cursor until the last fetch or cancel.
            if self.result is None or self.result.statement_id != statement_id:
                self.db.cmd_stmt_close(statement.native["statement_id"])
            del self.statements[statement_id]
            return {}
        if operation in {"health", "query.prepare", "execute"}:
            require(self.result is None, 1013)
            if operation == "health":
                return self.health()
            if operation == "query.prepare":
                return self.prepare(body)
            return self.execute(body)
        if operation in LEGACY_OPERATIONS:
            return dispatch_legacy(self, operation, body, self.config.legacy_rpc)
        if operation in BACKUP_OPERATIONS:
            return dispatch_backup(self, operation, body, self.config.legacy_backup)
        if operation in MODEL_OPERATIONS and self.config.legacy_model_worker is not None:
            return dispatch_model(self, operation, body, self.config.legacy_model_worker)
        if operation == "decision_validate" and self.config.legacy_decision_operator:
            return validate_decision(self, body, self.config.legacy_decision_operator)
        raise Fault(1004)


class Handler(socketserver.BaseRequestHandler):
    def handle(self):
        self.request.settimeout(10)
        try:
            if self.server.config.share_classic_loopback:
                self.serve_shared()
            elif self.server.tls is None:
                self.serve(self.request)
            else:
                with self.server.tls.wrap_socket(self.request, server_side=True) as sock:
                    self.serve(sock)
        except (EOFError, OSError):
            pass

    def serve_shared(self):
        config = self.server.config
        deadline = time.monotonic() + 5
        with socket.create_connection((config.mysql_host, config.mysql_port), timeout=5) as classic:
            header = receive(classic, 4, deadline)
            size = int.from_bytes(header[:3], "little")
            if header[3] != 0 or not 2 <= size <= 255:
                return
            greeting = receive(classic, size, deadline)
            version, separator, _ = greeting[1:].partition(b"\0")
            if greeting[0] != 10 or not separator or version != (config.product_version + "-StarDataParadise").encode():
                return
            self.request.sendall(header + greeting)

            first_header = receive(self.request, 4, time.monotonic() + 10)
            # A wire hello fits in one byte of big-endian length. A Classic login has a nonzero
            # little-endian length and sequence number 1.
            if first_header[:3] != b"\0\0\0":
                if first_header[3] != 1 or int.from_bytes(first_header[:3], "little") < 32:
                    return
                classic.sendall(first_header)
                self.relay_classic(classic)
                return

        self.serve(self.request, first_header)

    def relay_classic(self, classic):
        self.request.settimeout(15)
        classic.settimeout(15)
        peers = {classic: self.request, self.request: classic}
        readers = list(peers)
        while readers:
            readable, _, _ = select(readers, [], [])
            for source in readable:
                destination = peers[source]
                data = source.recv(65536)
                if data:
                    destination.sendall(data)
                else:
                    with suppress(OSError):
                        destination.shutdown(socket.SHUT_WR)
                    readers.remove(source)

    def serve(self, sock, first_header=None):
        session = Session(self.server.config)
        try:
            self.serve_session(sock, session, first_header)
        finally:
            session.close()

    def serve_session(self, sock, session, header):
        state, previous = "hello", 0
        while True:
            try:
                request = read_request(sock, previous, header)
            except Fault as fault:
                send(sock, 0, fault=fault)
                return
            header = None
            previous = request["id"]
            operation, body = request["op"], request["body"]
            try:
                if type(request.get("v")) is not int or request["v"] != 1:
                    raise Fault(1003, close=True)
                if state == "hello":
                    if operation != "hello":
                        raise Fault(1013, close=True)
                    reply = {"session_id": session.wire_id, "product_version": self.server.config.product_version,
                             "protocol_version": 1}
                    state = "auth"
                elif state == "auth":
                    if operation != "auth":
                        raise Fault(1013, close=True)
                    reply = session.authenticate(body)
                    state = "connected"
                else:
                    reply = session.dispatch(operation, body)
                send(sock, previous, body=reply)
            except Error as error:
                fault = backend_fault(error)
                send(sock, previous, fault=fault)
                if fault.close:
                    return
            except Fault as fault:
                send(sock, previous, fault=fault)
                if fault.close:
                    return
            except (EOFError, OSError):
                raise
            except Exception:
                send(sock, previous, fault=Fault(1015, close=True))
                return


class Server(socketserver.ThreadingTCPServer):
    daemon_threads = True

    def __init__(self, config, tls):
        self.config = config
        self.tls = tls
        self.slots = threading.BoundedSemaphore(config.max_sessions)
        super().__init__(("127.0.0.1", config.port), Handler)

    def verify_request(self, request, _address):
        return self.slots.acquire(blocking=False)

    def process_request(self, request, address):
        try:
            super().process_request(request, address)
        except BaseException:
            self.slots.release()
            raise

    def process_request_thread(self, request, address):
        try:
            super().process_request_thread(request, address)
        finally:
            self.slots.release()

    def handle_error(self, _request, _address):
        # Connection errors can carry SQL or login information. Leave details to MySQL's own logs.
        return


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--port", type=int, required=True)
    parser.add_argument("--cert", type=Path)
    parser.add_argument("--key", type=Path)
    parser.add_argument("--plaintext-loopback", action="store_true", help="accept unencrypted wire v1 on 127.0.0.1")
    parser.add_argument("--share-classic-loopback", action="store_true",
                        help="accept Classic TCP and plaintext wire v1 on the same local port")
    parser.add_argument("--mysql-host", choices=("localhost", "127.0.0.1", "::1"), default="127.0.0.1")
    parser.add_argument("--mysql-port", type=int, required=True)
    parser.add_argument("--mysql-ca", type=Path, required=True)
    parser.add_argument("--mysql-uuid", type=lambda value: str(UUID(value)), required=True)
    parser.add_argument("--schema", required=True)
    parser.add_argument("--product-version", default="26.10.0")
    parser.add_argument("--ledger-key-file", type=Path)
    parser.add_argument("--legacy-rpc-config", type=Path)
    parser.add_argument("--legacy-backup-config", type=Path)
    parser.add_argument("--legacy-model-worker", type=Path)
    parser.add_argument("--legacy-decision-operator", action="append", default=[])
    parser.add_argument("--native-sql",action="store_true",help="enable versioned native SQL sessions and typed JSON results")
    parser.add_argument("--max-sessions", type=int, default=8)
    args = parser.parse_args()
    if args.share_classic_loopback and (not args.plaintext_loopback or args.port == args.mysql_port):
        parser.error("--share-classic-loopback requires --plaintext-loopback and a separate internal Classic port")
    if args.plaintext_loopback:
        if args.cert is not None or args.key is not None:
            parser.error("--plaintext-loopback cannot be combined with --cert or --key")
    elif args.cert is None or args.key is None:
        parser.error("--cert and --key are required unless --plaintext-loopback is set")
    if not (0 <= args.port <= 65535 and 1 <= args.mysql_port <= 65535 and 1 <= args.max_sessions <= 32
            and re.fullmatch(r"[0-9]+\.[0-9]+\.[0-9]+", args.product_version)
            and re.fullmatch(r"[A-Za-z0-9_]{1,64}", args.schema)):
        parser.error("invalid port, package version, schema or session limit")

    try:
        args.ledger_key = read_key(args.ledger_key_file) if args.ledger_key_file else None
    except (OSError, ValueError) as error:
        parser.error(f"could not load ledger key ({type(error).__name__})")
    try:
        args.legacy_rpc = load_legacy(args.legacy_rpc_config) if args.legacy_rpc_config else None
        args.legacy_backup = load_backup(args.legacy_backup_config) if args.legacy_backup_config else None
    except (OSError, ValueError) as error:
        parser.error(f"could not load legacy RPC configuration ({type(error).__name__})")
    if args.legacy_model_worker:
        args.legacy_model_worker = args.legacy_model_worker.resolve()
        if not args.legacy_model_worker.is_file():
            parser.error("invalid legacy model worker")
    if any(not re.fullmatch(r"[A-Za-z0-9_.-]{1,32}@[A-Za-z0-9_.:%-]{1,255}", actor)
           for actor in args.legacy_decision_operator):
        parser.error("invalid legacy Decision operator")

    tls = None
    if not args.plaintext_loopback:
        tls = ssl.SSLContext(ssl.PROTOCOL_TLS_SERVER)
        tls.minimum_version = ssl.TLSVersion.TLSv1_2
        tls.load_cert_chain(args.cert, args.key)
    with Server(args, tls) as server:
        print(f"SDP_WIRE_V1_READY {server.server_address[1]}", flush=True)
        try:
            server.serve_forever()
        except KeyboardInterrupt:
            return


if __name__ == "__main__":
    main()
