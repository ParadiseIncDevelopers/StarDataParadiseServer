"""Real loopback TLS acceptance for the prepared SQL and transaction RPCs."""

import argparse
from contextlib import contextmanager
import json
from pathlib import Path
import secrets
import ssl
import struct
import tempfile
import time

from service_smoke import (
    authenticate,
    certificates,
    connection,
    exchange,
    execute,
    files_in,
    listening,
    require,
    save_config,
)


INVALID_ARGUMENT = 1012
INVALID_STATE = 1013
TIMEOUT = 1008
BIGINT = {"type": "BIGINT"}
TEXT = {"type": "TEXT"}
BLOB = {"type": "BLOB"}
DECIMAL = {"type": "DECIMAL", "precision": 20, "scale": 4}
NULL = {"t": "null"}
LARGE_ID = 9_223_372_036_854_775_807
NOTE = "İzmir'in notu; sıra 3 — örnek"
AMOUNT = {"t": "decimal", "unscaled": "12345678901234567890", "scale": 4}
BINARY = {"t": "binary", "hex": "00c3b1ff01"}


def i64(value):
    return {"t": "i64", "v": str(value)}


def text(value):
    return {"t": "text", "v": value}


def token():
    return secrets.token_hex(16)


class Client:
    def __init__(self, sock, password):
        self.sock = sock
        self.id = 2
        require(authenticate(sock, password)["ok"], "SQL test authentication failed")

    def raw(self, op, **body):
        self.id += 1
        return exchange(self.sock, self.id, op, body)

    def call(self, op, **body):
        response = self.raw(op, **body)
        require(response.get("ok"), f"{op} failed with {response.get('error', {}).get('code')}")
        return response["body"]

    def error(self, op, code, **body):
        response = self.raw(op, **body)
        actual = response.get("error", {}).get("code")
        require(not response.get("ok") and actual == code, f"{op} expected error {code}, got {actual}")

    @contextmanager
    def prepared(self, statement, types=None):
        statement_id = self.call("query.prepare", sql=statement, parameter_types=types or {})["statement_id"]
        try:
            yield statement_id
        finally:
            self.call("statement_close", statement_id=statement_id)

    def mutate(self, statement, *, types=None, values=None, request_token=None):
        with self.prepared(statement, types) as statement_id:
            fields = {"parameters": values or {}}
            if request_token is not None:
                fields["request_token"] = request_token
            return self.call("execute", statement_id=statement_id, **fields)

    def query(self, statement, *, types=None, values=None, row_limit=10_000, batch_rows=128):
        with self.prepared(statement, types) as statement_id:
            result = self.call("execute", statement_id=statement_id, row_limit=row_limit, parameters=values or {})
            return self.collect(result, row_limit, batch_rows)

    def collect(self, result, row_limit, batch_rows):
        rows = []
        for index in range(row_limit + 1):
            batch = self.call("fetch", result_id=result["result_id"], batch_rows=batch_rows)["batch"]
            require(batch["batch_index"] == index and batch["result_set"] == 0, "result batch order changed")
            require(len(batch["rows"]) <= batch_rows, "fetch exceeded requested batch size")
            if index == 0:
                require(batch["columns"] == result["columns"], "result metadata differs between execute and fetch")
            require(batch["end_of_set"] == batch["end_of_results"], "invalid end-of-results marker")
            rows.extend(batch["rows"])
            require(len(rows) <= row_limit, "execute returned beyond its row limit")
            if batch["end_of_results"]:
                return rows, index + 1
            require(batch["rows"], "fetch did not advance the active result")
        raise RuntimeError("fetch did not terminate")

    def send_without_receiving(self, op, **body):
        self.id += 1
        request = {"v": 1, "id": self.id, "op": op, "body": body}
        payload = json.dumps(request, separators=(",", ":")).encode("utf-8")
        self.sock.sendall(struct.pack("!I", len(payload)) + payload)
        # Leave the connection scope immediately. The request outcome is learned only from a new connection.


@contextmanager
def client(port, context, password):
    with connection(port, context) as sock:
        sock.settimeout(18)
        yield Client(sock, password)


def write(client_, sql, count, **fields):
    request_token = token()
    result = client_.mutate(sql, request_token=request_token, **fields)
    require(result["affected_rows"] == count, "mutation affected an unexpected number of rows")
    expected = {"status": "committed", "lsn": result["lsn"]}
    require(client_.call("tx_outcome", request_token=request_token) == expected, "wrong autocommit outcome")
    return expected


def rows_for(client_, row_id):
    rows, _ = client_.query(
        "SELECT t.id, t.note, t.amount, t.payload FROM events AS t WHERE t.id = :key",
        types={"key": BIGINT},
        values={"key": i64(row_id)},
    )
    return rows


def prepared_crud(client_):
    write(client_, "CREATE TABLE events (id BIGINT NOT NULL, note TEXT, amount DECIMAL(20,4), payload BLOB)", 0)
    result = write(
        client_,
        "INSERT INTO events (id,note,amount,payload) VALUES "
        "(:key,:message,:amount,:payload),(:other,NULL,NULL,NULL),(3,'temporary',NULL,NULL)",
        3,
        types={"key": BIGINT, "message": TEXT, "amount": DECIMAL, "payload": BLOB, "other": BIGINT},
        values={"key": i64(LARGE_ID), "message": text(NOTE), "amount": AMOUNT, "payload": BINARY, "other": i64(2)},
    )
    require(rows_for(client_, LARGE_ID) == [[i64(LARGE_ID), text(NOTE), AMOUNT, BINARY]],
            "non-ASCII text, punctuation, integer, exact decimal or binary changed")
    require(rows_for(client_, 2) == [[i64(2), NULL, NULL, NULL]], "NULL round trip changed")

    bad_token = token()
    with client_.prepared("INSERT INTO events (id) VALUES (:key)", {"key": BIGINT}) as statement:
        client_.error(
            "execute", INVALID_ARGUMENT, statement_id=statement, parameters={"key": text("5")},
            request_token=bad_token,
        )
    require(client_.call("tx_outcome", request_token=bad_token) == {"status": "unknown"},
            "failed parameter binding left a committed ledger entry")
    require(rows_for(client_, 5) == [], "failed parameter binding inserted a row")

    write(
        client_, "UPDATE events AS t SET note = :message WHERE t.id = :key", 1,
        types={"message": TEXT, "key": BIGINT}, values={"message": text("updated"), "key": i64(3)},
    )
    require(rows_for(client_, 3) == [[i64(3), text("updated"), NULL, NULL]], "UPDATE did not persist")
    write(client_, "DELETE FROM events WHERE id = :key", 1, types={"key": BIGINT}, values={"key": i64(3)})
    require(rows_for(client_, 3) == [], "DELETE left a visible row")
    metadata = client_.call("metadata")["objects"]
    return next(obj["id"] for obj in metadata if obj["qualified_name"] == ["main", "public", "events"]), result


def savepoints(client_):
    client_.call("tx_begin")
    client_.call("tx_savepoint", name="before_edits")
    client_.mutate(
        "UPDATE events SET note = :message WHERE id = :key",
        types={"message": TEXT, "key": BIGINT}, values={"message": text("discard me"), "key": i64(LARGE_ID)},
    )
    require(rows_for(client_, LARGE_ID)[0][1] == text("discard me"), "explicit transaction did not read its own edits")
    client_.call("tx_savepoint", name="nested")
    client_.mutate("INSERT INTO events (id) VALUES (17)")
    client_.call("tx_rollback_to", name="before_edits")
    require(rows_for(client_, 17) == [] and rows_for(client_, LARGE_ID)[0][1] == text(NOTE),
            "rollback to savepoint kept later rows or assignment")
    client_.error("tx_release", INVALID_STATE, name="nested")
    client_.call("tx_release", name="before_edits")

    client_.mutate("INSERT INTO events (id) VALUES (18)")
    client_.call("tx_rollback")
    require(rows_for(client_, 18) == [], "full rollback kept a private row")


def stream_and_cancel(port, context, password, reader):
    write(reader, "CREATE TABLE stream_rows (id BIGINT NOT NULL, payload TEXT NOT NULL)", 0)
    fixture = "x" * 3072
    # Independent transactions stay within the storage page and WAL change-set budgets.
    for base in range(0, 300, 100):
        values = ",".join(f"({key},:data)" for key in range(base, base + 100))
        write(
            reader, "INSERT INTO stream_rows VALUES " + values, 100,
            types={"data": TEXT}, values={"data": text(fixture)},
        )
    statement = "SELECT id, payload FROM stream_rows"
    rows, batches = reader.query(statement, row_limit=97, batch_rows=1024)
    keys = {row[0]["v"] for row in rows}
    require(len(rows) == len(keys) == 97 and batches > 1, "large results were not bounded, or repeated rows")
    require(all(row[1] == text(fixture) for row in rows), "large batched values changed")

    with reader.prepared(statement) as prepared:
        result = reader.call("execute", statement_id=prepared)
        result_id = result["result_id"]
        first = reader.call("fetch", result_id=result_id, batch_rows=4)["batch"]
        require(len(first["rows"]) == 4 and not first["end_of_results"], "test cursor did not hold a live result")
        with client(port, context, password) as contender:
            contender.error("metadata", TIMEOUT)
            reader.call("result_cancel", result_id=result_id)
            reader.error("fetch", INVALID_STATE, result_id=result_id)
            write(contender, "INSERT INTO stream_rows VALUES (300,'after cancel')", 1)


def wait_for_outcome(client_, request_token):
    deadline = time.monotonic() + 6
    while time.monotonic() < deadline:
        response = client_.raw("tx_outcome", request_token=request_token)
        if response.get("ok"):
            body = response["body"]
            if body["status"] == "committed":
                return body
            require(body["status"] == "unknown", "unexpected ledger result")
        else:
            require(response["error"]["code"] == TIMEOUT, "unexpected error polling outcome after disconnect")
        time.sleep(0.05)
    raise RuntimeError("could not recover commit outcome after dropping its response")


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--server", type=Path, required=True)
    parser.add_argument("--openssl", type=Path, required=True)
    parser.add_argument("--scratch", type=Path, required=True)
    args = parser.parse_args()
    server = str(args.server.resolve(strict=True))
    ssl_tool = str(args.openssl.resolve(strict=True))
    args.scratch.mkdir(parents=True, exist_ok=True)
    with tempfile.TemporaryDirectory(prefix="s12-", dir=args.scratch) as directory:
        fixture = Path(directory)
        for name in ("data", "log", "temp"):
            (fixture / name).mkdir()
        certificates(ssl_tool, fixture)
        config = save_config(fixture, timeout_ms=15_000, max_connections=4)
        initialized = execute(server, config, "--initialize-storage")
        require(initialized.returncode == 0, "isolated SQL storage initialization failed")
        password = secrets.token_urlsafe(32)
        bootstrap = execute(
            server, config, "--bootstrap-admin", "local-admin", stdin=((password + "\n") * 2).encode("ascii"),
        )
        require(bootstrap.returncode == 0, "isolated SQL administrator bootstrap failed")
        context = ssl.create_default_context(cafile=str(fixture / "ca.pem"))

        with listening(server, config) as (_, port):
            with client(port, context, password) as reader:
                features = reader.call("capabilities")["features"]
                require(features.get("sql_crud_v1") is True,
                        "initialized SQL instance did not advertise its capabilities")
                event_type, initial_commit = prepared_crud(reader)
                savepoints(reader)
                stream_and_cancel(port, context, password, reader)
                require(reader.call("tx_outcome", request_token=token()) == {"status": "unknown"},
                        "absent outcome token reported as committed")

            missing_response = token()
            with client(port, context, password) as dropped:
                dropped.call("tx_begin")
                pending = dropped.mutate("INSERT INTO events (id,note) VALUES (4,'committed once')")
                require(pending["affected_rows"] == 1 and "lsn" not in pending, "explicit write committed too early")
                dropped.send_without_receiving("tx_commit", request_token=missing_response)

            with client(port, context, password) as lookup:
                outcome = wait_for_outcome(lookup, missing_response)
                require(int(outcome["lsn"]) > int(initial_commit["lsn"]), "ledger commit order was not durable")
                with lookup.prepared("INSERT INTO events (id) VALUES (4)") as prepared:
                    lookup.error("execute", INVALID_STATE, statement_id=prepared, request_token=missing_response)
                require(rows_for(lookup, 4) == [[i64(4), text("committed once"), NULL, NULL]],
                        "duplicate request token caused an extra row")

        before_restart = files_in(fixture / "data")
        require(any(".wal." in path.name for path in before_restart), "no persisted WAL produced")
        require(execute(server, config, "--check-storage").returncode == 0, "published data or WAL failed integrity check")
        with listening(server, config) as (_, port):
            with client(port, context, password) as reopened:
                require(reopened.call("tx_outcome", request_token=missing_response) == outcome,
                        "commit outcome did not survive a process restart")
                require(rows_for(reopened, LARGE_ID) == [[i64(LARGE_ID), text(NOTE), AMOUNT, BINARY]],
                        "typed row did not survive a process restart")
                require(rows_for(reopened, 2) == [[i64(2), NULL, NULL, NULL]], "NULLs changed after restart")
                require(rows_for(reopened, 4) == [[i64(4), text("committed once"), NULL, NULL]],
                        "committed row duplicated or missing after restart")
                require(all(rows_for(reopened, key) == [] for key in (3, 5, 17, 18)),
                        "removed or rolled-back rows reappeared")
                metadata = reopened.call("metadata")["objects"]
                require(any(obj["id"] == event_type and obj["qualified_name"] == ["main", "public", "events"]
                            for obj in metadata), "persistent metadata ID changed")
        require(files_in(fixture / "data") == before_restart, "read-only validation changed data or WAL")

    print("SQL RPC prepared CRUD, typed values, savepoints, durable outcome, restart, fetch budgets and cancellation passed")


if __name__ == "__main__":
    main()
