"""Exercise the bridge against a provisioned disposable InnoDB fixture."""

import argparse
from contextlib import contextmanager
from pathlib import Path
import ssl
import sys

sys.path.insert(0, str(Path(__file__).resolve().parents[2] / "tests"))
from service_smoke import connection, exchange, require


MINIMUM = -(1 << 63)
MAXIMUM = (1 << 63) - 1
DECIMAL = {"t": "decimal", "unscaled": "-123456789012345678901234567890123400", "scale": 4}
NOTE = {"t": "text", "v": "İstanbul'un notu; a\0b :replace ?"}
BINARY = {"t": "binary", "hex": "00017f80feff"}
NULL = {"t": "null"}
DECLARATIONS = {"id": {"type": "BIGINT"}, "note": {"type": "TEXT"},
                "amount": {"type": "DECIMAL", "precision": 38, "scale": 4}, "payload": {"type": "BLOB"}}


def i64(value):
    return {"t": "i64", "v": str(value)}


class Client:
    def __init__(self, sock, username, password):
        self.socket = sock
        self.request_id = 0
        hello = self.call("hello")
        require(hello["protocol_version"] == 1 and len(hello["session_id"]) == 32,
                "hello protocol or session ID differs")
        require(self.call("auth", username=username, password=password) == {"authenticated": True},
                "authentication failed")

    def request(self, operation, body):
        self.request_id += 1
        return exchange(self.socket, self.request_id, operation, body)

    def call(self, operation, **body):
        response = self.request(operation, body)
        require(response.get("ok"), f"{operation} failed ({response.get('error', {}).get('code')})")
        return response["body"]

    def error(self, operation, code, **body):
        response = self.request(operation, body)
        require(response.get("error", {}).get("code") == code,
                f"{operation} expected {code}, got {response.get('error', {}).get('code')}")

    @contextmanager
    def statement(self, sql, declarations=None):
        statement_id = self.call("query.prepare", sql=sql, parameter_types=declarations or {})["statement_id"]
        try:
            yield statement_id
        finally:
            self.call("statement_close", statement_id=statement_id)

    def query(self, sql, expected, **options):
        with self.statement(sql, options.pop("types", None)) as statement:
            result = self.call("execute", statement_id=statement, **options)
            rows = []
            for index in range(len(expected) + 2):
                batch = self.call("fetch", result_id=result["result_id"], batch_rows=1)["batch"]
                require(batch["result_set"] == 0 and batch["batch_index"] == index,
                        "result set or batch order differs")
                require((index == 0 and batch.get("columns") == result["columns"])
                        or (index > 0 and "columns" not in batch), "missing or repeated metadata")
                require(len(batch["rows"]) <= 1, "server exceeded the requested batch size")
                rows.extend(batch["rows"])
                if batch["end_of_results"]:
                    require(batch["end_of_set"] and rows == expected, "typed query result differs")
                    return
            raise RuntimeError("fetch did not terminate")


def verify_owner(owner, check_read_only):
    features = owner.call("capabilities")["features"]
    require(features["wire_sql_basic_v1"] and not features["sql_crud_v1"] and not features["history_v1"],
            "incorrect capability advertisement")
    require(owner.call("health")["status"] == "ready", "backend health failed")
    owner.query("SELECT @@SESSION.sdp_compatibility_mode AS mode", [[{"t": "text", "v": "legacy"}]])
    owner.query("SELECT ':ordinary; ?' AS value", [[{"t": "text", "v": ":ordinary; ?"}]])
    owner.error("query.prepare", 1004, sql="SELECT 1; SELECT 2")
    owner.error("query.prepare", 1004, sql="SELECT id INTO @copy FROM wire_sample")
    owner.error("query.prepare", 1004, sql="SELECT id FROM wire_sample FOR UPDATE")
    owner.error("query.prepare", 1004, sql="SELECT id FROM wire_sample LOCK IN SHARE MODE")
    if check_read_only:
        with owner.statement("SELECT wire_read_guard()") as guard:
            owner.error("execute", 1006, statement_id=guard)
        owner.query("SELECT id FROM wire_sample", [])
        print("PASS independent SELECT cannot modify fixture InnoDB via a stored function", flush=True)
    with owner.statement("INSERT INTO wire_sample VALUES (:id,:note,:amount,:payload)", DECLARATIONS) as stmt:
        valid = {"id": i64(MAXIMUM), "note": NOTE, "amount": DECIMAL, "payload": BINARY}
        owner.error("execute", 1013, statement_id=stmt, parameters=valid)
        owner.call("tx_begin")
        owner.error("execute", 1012, statement_id=stmt, parameters={**valid, "id": NOTE})
        require(owner.call("execute", statement_id=stmt, parameters=valid)["affected_rows"] == 1,
                "MySQL affected row count differs")
        owner.call("tx_savepoint", name="before_second")
        other = {"id": i64(MINIMUM), "note": NULL, "amount": NULL, "payload": NULL}
        owner.call("execute", statement_id=stmt, parameters=other)
        owner.query("SELECT id,note,amount,payload FROM wire_sample ORDER BY id", [
            [i64(MINIMUM), NULL, NULL, NULL], [i64(MAXIMUM), NOTE, DECIMAL, BINARY],
        ])
        owner.call("tx_rollback_to", name="before_second")
        owner.call("tx_release", name="before_second")
        owner.error("tx_commit", 1004, request_token="0" * 32)
        require(owner.call("tx_commit") == {"status": "committed"}, "plain commit failed")
    owner.query("SELECT id,note,amount,payload FROM wire_sample", [[i64(MAXIMUM), NOTE, DECIMAL, BINARY]])
    owner.query("SELECT id FROM wire_sample WHERE id = :id OR id = :id", [[i64(MAXIMUM)]],
                types={"id": {"type": "BIGINT"}}, parameters={"id": i64(MAXIMUM)})
    owner.query("SELECT id FROM wire_sample UNION ALL SELECT 99 ORDER BY id", [[i64(99)]], row_limit=1)
    closed = owner.call("query.prepare", sql="SELECT id FROM wire_sample")["statement_id"]
    detached = owner.call("execute", statement_id=closed, row_limit=1)["result_id"]
    owner.call("statement_close", statement_id=closed)
    owner.error("statement_close", 1013, statement_id=closed)
    fetched = owner.call("fetch", result_id=detached)["batch"]
    require(fetched["rows"] == [[i64(MAXIMUM)]] and fetched["end_of_results"],
            "closing the wire statement discarded the live result")
    statement = owner.call("query.prepare", sql="SELECT id FROM wire_sample")["statement_id"]
    owner.call("tx_begin")
    with owner.statement("UPDATE wire_sample SET id = 50") as update:
        owner.call("execute", statement_id=update)
    rows = owner.call("execute", statement_id=statement)
    owner.call("result_cancel", result_id=rows["result_id"])
    owner.error("fetch", 1013, result_id=rows["result_id"])
    owner.call("session_reset")
    owner.error("statement_close", 1013, statement_id=statement)
    owner.query("SELECT id FROM wire_sample", [[i64(MAXIMUM)]])
    print("PASS native prepared statement binding, exact types, batching and transaction controls", flush=True)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--port", type=int, required=True)
    parser.add_argument("--ca", type=Path, required=True)
    parser.add_argument("--owner", required=True)
    parser.add_argument("--reader", required=True)
    parser.add_argument("--check-read-only", action="store_true",
                        help="also probe a disposable wire_read_guard() DML function")
    args = parser.parse_args()
    owner_password, reader_password = [sys.stdin.readline().rstrip("\r\n") for _ in range(2)]
    context = ssl.create_default_context(cafile=str(args.ca))
    try:
        with connection(args.port, context, "wrong.invalid"):
            raise RuntimeError("wrong TLS name accepted")
    except ssl.SSLCertVerificationError:
        pass
    with connection(args.port, context) as sock:
        require(exchange(sock, 1, "hello")["ok"], "hello failed")
        failure = exchange(sock, 2, "auth", {"username": args.owner, "password": owner_password + "invalid"})
        require(failure.get("error", {}).get("code") == 1005, "wrong password accepted")

    with connection(args.port, context) as sock:
        owner = Client(sock, args.owner, owner_password)
        verify_owner(owner, args.check_read_only)
        owner.call("tx_begin")
        with owner.statement("DELETE FROM wire_sample") as statement:
            owner.call("execute", statement_id=statement)
        # Closing without a rollback must let MySQL release this uncommitted delete.
    with connection(args.port, context) as sock:
        reader = Client(sock, args.reader, reader_password)
        reader.query("SELECT id FROM wire_sample", [[i64(MAXIMUM)]])
        reader.error("query.prepare", 1006, sql="DELETE FROM wire_sample")
    print("PASS TLS identity, failed password, caller grants and disconnect rollback", flush=True)


if __name__ == "__main__":
    main()
