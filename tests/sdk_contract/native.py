"""Run the shared Stage 47 contract through an installed Native DLL's C ABI."""

from contextlib import contextmanager
import ctypes as c
import json
import math
import os
from pathlib import Path
import secrets
import sys


class Text(c.Structure):
    _fields_ = [("data", c.c_char_p), ("size", c.c_uint32)]


class Buffer(c.Structure):
    _fields_ = [("data", c.c_void_p), ("size", c.c_uint32)]


class Options(c.Structure):
    _fields_ = [("size", c.c_uint32), ("abi", c.c_uint32), ("address", Text), ("server", Text),
                ("ca", Text), ("user", Text), ("password", Text), ("port", c.c_uint16),
                ("reserved", c.c_uint16), ("timeout", c.c_uint32)]


class Error(c.Structure):
    _fields_ = [("code", c.c_int32), ("retryable", c.c_uint32), ("unknown", c.c_uint32),
                ("line", c.c_uint32), ("column", c.c_uint32), ("sqlstate", c.c_char * 6),
                ("message", c.c_char * 96), ("correlation_id", c.c_char * 80)]


class Failure(Exception):
    def __init__(self, operation, status, details):
        super().__init__(f"{operation} C ABI failed with status {status}")
        self.status = status
        self.details = details


def expect_failure(action, status):
    try:
        action()
    except Failure as failure:
        assert failure.status == failure.details.code == status, failure
        return failure.details
    raise AssertionError(f"Expected C ABI error {status}")


def setting(name):
    return os.environ["SDP_SDK_" + name]


def text(value):
    raw = value if isinstance(value, bytes) else value.encode("utf-8")
    return Text(raw, len(raw))


def body(value):
    return text(json.dumps(value))


def i64(value):
    return {"t": "i64", "v": str(value)}


class Native:
    def __init__(self, path, *, foundation_only=False):
        self.dll = c.CDLL(str(Path(path).resolve(strict=True)))
        handle, output, error = c.c_uint64, c.POINTER(Buffer), c.POINTER(Error)
        number = c.c_uint32
        operations = {
            "open": (c.POINTER(Options), c.POINTER(handle), error),
            "close": (handle, error),
            "health": (handle, error),
            "get_capabilities": (handle, output, error),
        }
        current = {
            "get_session_info": (handle, output, error),
            "prepare": (handle, Text, output, error),
            "prepare_typed": (handle, Text, Text, output, error),
            "execute": (handle, Text, Text, Text, number, output, error),
            "fetch": (handle, Text, number, output, error),
            "statement_close": (handle, Text, error),
            "tx_begin": (handle, error),
            "tx_savepoint": (handle, Text, error),
            "tx_rollback_to": (handle, Text, error),
            "tx_release": (handle, Text, error),
            "tx_rollback": (handle, error),
            "tx_commit": (handle, Text, output, error),
            "tx_outcome": (handle, Text, output, error),
            "session_reset": (handle, error),
            "history_command": (handle, number, Text, output, error),
            "model_command": (handle, number, Text, output, error),
        }
        if not foundation_only:
            operations.update(current)
        for name, arguments in operations.items():
            function = getattr(self.dll, "sdp_" + name)
            function.argtypes = arguments
            function.restype = c.c_int32
        self.dll.sdp_buffer_free.argtypes = (output,)
        self.dll.sdp_buffer_free.restype = None
        for name in ("sdp_library_version", "sdp_abi_version"):
            function = getattr(self.dll, name)
            function.argtypes = ()
            function.restype = number
        assert self.dll.sdp_abi_version() == 1

    def call(self, operation, *arguments):
        error = Error()
        if status := getattr(self.dll, "sdp_" + operation)(*arguments, c.byref(error)):
            raise Failure(operation, status, error)

    def receive(self, operation, *arguments):
        output = Buffer()
        try:
            self.call(operation, *arguments, c.byref(output))
            return c.string_at(output.data, output.size)
        finally:
            self.dll.sdp_buffer_free(c.byref(output))

    def json(self, operation, *arguments):
        return json.loads(self.receive(operation, *arguments))

    @contextmanager
    def connect(self, login, *, target="PORT", tls_name=None, abi=1):
        user, password = login
        options = Options(c.sizeof(Options), abi, text(setting("HOST")), text(tls_name or setting("TLS_NAME")),
                          text(setting("CA")), text(user), text(password), int(setting(target)), 0, 15000)
        handle = c.c_uint64()
        self.call("open", c.byref(options), c.byref(handle))
        try:
            yield handle
        finally:
            self.call("close", handle)

    def execute(self, handle, query, *, types=None, values=None, write=False):
        if types is None:
            statement = self.receive("prepare", handle, text(query))
        else:
            statement = self.receive("prepare_typed", handle, text(query), body(types))
        statement = text(statement)
        try:
            request = secrets.token_hex(16) if write else ""
            reply = self.json("execute", handle, statement, body(values or {}), text(request), 100)
            result_id = reply.get("result_id")
            if not result_id:
                return reply["affected_rows"]
            names, rows = [], []
            for _ in range(100):
                batch = self.json("fetch", handle, text(result_id), 1)["batch"]
                if batch["batch_index"] == 0:
                    assert batch["result_set"] == len(rows)
                    names.append(batch.get("set_name"))
                    rows.append([])
                assert len(batch["rows"]) <= 1
                rows[-1].extend(batch["rows"])
                if batch["end_of_results"]:
                    return names, rows
            raise AssertionError("Native fetch exceeded the test data size")
        finally:
            self.call("statement_close", handle, statement)


def main():
    dll, version = sys.argv[1:]
    expected = sum(int(part) << shift for part, shift in zip(version.split("."), (16, 8, 0), strict=True))
    foundation_only = version == "0.1.0"
    native = Native(dll, foundation_only=foundation_only)
    assert native.dll.sdp_library_version() == expected
    owner = (setting("USER"), sys.stdin.readline().rstrip("\r\n"))
    guest = (setting("GUEST"), sys.stdin.readline().rstrip("\r\n"))
    assert owner[1] and guest[1]

    def probe(login, **kwargs):
        with native.connect(login, **kwargs):
            pass

    expect_failure(lambda: probe((owner[0], guest[1])), 1005)
    expect_failure(lambda: probe(owner, tls_name="absent.invalid"), 1011)
    expect_failure(lambda: probe(owner, abi=99), 1012)
    if foundation_only:
        with native.connect(owner) as handle:
            native.call("health", handle)
            features = native.json("get_capabilities", handle)
            assert all(features[name] for name in ("tls", "auth", "health", "sql_crud_v1"))
            assert not hasattr(native.dll, "sdp_prepare_typed")
        print("Native 0.1.0 foundation: TLS, authentication, capabilities and health passed")
        print("SDP_MANAGED_OK")
        return
    with native.connect(guest) as handle:
        assert native.execute(handle, "SELECT id FROM sdk_seed WHERE id=1")[1] == [[[i64(1)]]]
        error = expect_failure(lambda: native.json("history_command", handle, 1, body({"target": "sdk_seed"})), 1006)
        assert error.sqlstate == b"42501"

    with native.connect(owner) as handle:
        info = native.json("get_session_info", handle)
        assert info["protocol_version"] == 1
        _, [[first, second]] = native.execute(handle, "SELECT note,amount,stamp,payload FROM sdk_seed ORDER BY id")
        assert first == [{"t": "text", "v": "İstanbul'un notu; a\0b"},
                         {"t": "decimal", "unscaled": "-123456789012345678901234567890123400", "scale": 4},
                         {"t": "timestamp", "seconds": "-1", "nanos": 999999999, "offset_minutes": 180},
                         {"t": "binary", "hex": "00017f80feff"}]
        assert second == [{"t": "null"}] * 4

        native.call("tx_begin", handle)
        names, rows = native.execute(handle, "CALL sdk_list(:key,NULL)", types={"key": {"type": "BIGINT"}},
                                     values={"key": i64(1)})
        assert names == ["out", "found", "all_rows"] and rows == [[[i64(1)]], [[i64(1)]], [[i64(1)], [i64(2)]]]
        native.call("tx_rollback", handle)

        native.execute(handle, "CREATE TABLE sdk_native_tx (id BIGINT PRIMARY KEY,note TEXT)", write=True)
        assert native.execute(handle, "INSERT INTO sdk_native_tx VALUES (:id,:note)", write=True,
                              types={"id": {"type": "BIGINT"}, "note": {"type": "TEXT"}},
                              values={"id": i64(1), "note": {"t": "null"}}) == 1
        assert native.execute(handle, "UPDATE sdk_native_tx SET note='updated' WHERE id=1", write=True) == 1
        assert native.execute(handle, "SELECT note FROM sdk_native_tx WHERE id=1")[1] == [
            [[{"t": "text", "v": "updated"}]]]
        assert native.execute(handle, "DELETE FROM sdk_native_tx WHERE id=1", write=True) == 1
        assert native.execute(handle, "SELECT id FROM sdk_native_tx")[1] == [[]]

        denied = expect_failure(lambda: native.execute(handle, "INSERT INTO sdk_guard VALUES (1,-1)", write=True), 1006)
        assert denied.sqlstate == b"42501" and not denied.unknown
        history = native.json("history_command", handle, 1, body({"target": "sdk_seed"}))["entries"]
        rows = sum(entry["kind"] == "row" for entry in history)
        assert rows == 2
        prediction = native.json("model_command", handle, 5,
                                 body({"name": setting("MODEL"), "features": {"reading": 17}}))["prediction"]
        assert math.isclose(prediction, 58, abs_tol=1e-4)
        observed = {"decision_denied": denied.code, "history_rows": rows, "prediction": round(prediction)}
        if expired := os.environ.get("SDP_SDK_EXPIRED_TOKEN"):
            observed["expired_outcome"] = native.json("tx_outcome", handle, text(expired))["status"]
            assert observed["expired_outcome"] == "unknown"

        native.call("tx_begin", handle)
        native.execute(handle, "INSERT INTO sdk_native_tx VALUES (11,'pending')")
        savepoint = text("native_partial")
        native.call("tx_savepoint", handle, savepoint)
        native.execute(handle, "INSERT INTO sdk_native_tx VALUES (12,'batch')")
        native.call("tx_rollback_to", handle, savepoint)
        native.call("tx_release", handle, savepoint)
        assert native.execute(handle, "SELECT id FROM sdk_native_tx ORDER BY id")[1] == [[[i64(11)]]]
        retained = text(native.receive("prepare", handle, text("SELECT id FROM sdk_native_tx")))
        native.call("session_reset", handle)
        expect_failure(lambda: native.call("statement_close", handle, retained), 1013)
        assert native.json("get_session_info", handle)["session_id"] == info["session_id"]
        assert native.execute(handle, "SELECT id FROM sdk_native_tx")[1] == [[]]
        print("SDP_STAGE47_RESET verified_explicit_session")

    if "SDP_SDK_FAULT_PORT" in os.environ:
        request = text(secrets.token_hex(16))
        with native.connect(owner, target="FAULT_PORT") as handle:
            native.call("tx_begin", handle)
            native.execute(handle, "INSERT INTO sdk_native_tx VALUES (21,'committed')")
            assert expect_failure(lambda: native.json("tx_commit", handle, request), 1010).unknown
        with native.connect(owner) as fresh:
            assert native.json("tx_outcome", fresh, request)["status"] == "committed"
            assert native.execute(fresh, "SELECT id FROM sdk_native_tx")[1] == [[[i64(21)]]]

    if "SDP_SDK_READ_FAULT_PORT" in os.environ:
        with native.connect(owner, target="READ_FAULT_PORT") as handle:
            dropped = expect_failure(lambda: native.execute(handle, "SELECT id FROM sdk_seed WHERE id=1"), 1010)
            assert not dropped.unknown
        with native.connect(owner, target="READ_FAULT_PORT") as fresh:
            assert native.execute(fresh, "SELECT id FROM sdk_seed WHERE id=1")[1] == [[[i64(1)]]]
        print("SDP_STAGE47_READ_RECOVERY caller_reconnect")
    print("SDP_STAGE47_RESULT " + json.dumps(observed, sort_keys=True))
    print("SDP_MANAGED_OK")


if __name__ == "__main__":
    main()
