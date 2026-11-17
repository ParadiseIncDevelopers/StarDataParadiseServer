"""Exercise atomic wire receipts and lost replies on a disposable MySQL fork."""

from concurrent.futures import ThreadPoolExecutor
from contextlib import contextmanager
import json
import secrets
import struct
import time

from acceptance import Client, connection, i64, require


@contextmanager
def client(port, tls, username, password):
    with connection(port, tls) as sock:
        yield Client(sock, username, password)


def token():
    return secrets.token_hex(16)


def send_without_reply(peer, operation, **body):
    peer.request_id += 1
    payload = json.dumps({"v": 1, "id": peer.request_id, "op": operation, "body": body}).encode()
    peer.socket.sendall(struct.pack("!I", len(payload)) + payload)


def committed(peer, key, wait=False):
    deadline = time.monotonic() + (10 if wait else 0)
    while True:
        outcome = peer.call("tx_outcome", request_token=key)
        if outcome.get("status") == "committed":
            lsn = outcome["lsn"]
            require(isinstance(lsn, str) and lsn.isdecimal() and int(lsn) > 0, "invalid receipt sequence")
            return lsn
        require(time.monotonic() < deadline, "missing durable outcome")
        time.sleep(0.05)


def unknown(peer, key):
    require(peer.call("tx_outcome", request_token=key) == {"status": "unknown"},
            "a missing, rolled-back, expired or foreign receipt was exposed")


def write(peer, statement, **fields):
    with peer.statement(statement) as handle:
        return peer.call("execute", statement_id=handle, **fields)


def verify(port, tls, owner, reader, limited):
    with client(port, tls, *owner) as peer, client(port, tls, *reader) as observer:
        features = peer.call("capabilities")["features"]
        require(features["wire_sql_transactions_v1"] and not features["sql_crud_v1"], "wrong capability flags")
        first = token()
        accepted = write(peer, "INSERT INTO durable_sample VALUES (10)", request_token=first)
        require(accepted == {"affected_rows": 1, "lsn": committed(peer, first)}, "autocommit receipt differs")
        require(observer.call("capabilities")["features"]["wire_sql_transactions_v1"], "reader receipt API unavailable")
        unknown(observer, first)
        with observer.statement("SELECT id FROM durable_sample") as handle:
            require(observer.call("execute", statement_id=handle)["affected_rows"] == 0, "observer read failed")
            observer.call("tx_rollback")
        observer.error("query.prepare", 1006, sql="DELETE FROM durable_sample")
        with peer.statement("INSERT INTO durable_sample VALUES (11)") as handle:
            peer.error("execute", 1012, statement_id=handle)
            peer.error("execute", 1013, statement_id=handle, request_token=first)
            peer.error("execute", 1012, statement_id=handle, request_token="A" * 32)
        peer.query("SELECT id FROM durable_sample", [[i64(10)]])
        failed = token()
        with peer.statement("INSERT INTO durable_sample VALUES (10)") as handle:
            peer.error("execute", 1012, statement_id=handle, request_token=failed)
        unknown(peer, failed)

        peer.call("tx_begin")
        write(peer, "INSERT INTO durable_sample VALUES (12)")
        peer.call("tx_savepoint", name="later")
        write(peer, "INSERT INTO durable_sample VALUES (13)")
        peer.call("tx_rollback_to", name="later")
        peer.call("tx_release", name="later")
        peer.error("tx_commit", 1012)
        peer.error("tx_commit", 1013, request_token=first)
        second = token()
        reply = peer.call("tx_commit", request_token=second)
        require(reply == {"status": "committed", "lsn": committed(peer, second)}
                and int(reply["lsn"]) > int(accepted["lsn"]), "explicit commit receipt differs")

        denied = token()
        with peer.statement("INSERT INTO nontransactional_direct VALUES (1)") as handle:
            peer.error("execute", 1004, statement_id=handle, request_token=denied)
            unknown(peer, denied)
            peer.call("tx_begin")
            peer.error("execute", 1004, statement_id=handle)
            peer.call("tx_rollback")
        with peer.statement("INSERT INTO durable_trigger VALUES (1)") as handle:
            peer.error("execute", 1004, statement_id=handle, request_token=denied)
            peer.call("tx_begin")
            peer.error("execute", 1004, statement_id=handle)
            peer.call("tx_rollback")
        unknown(peer, denied)

        peer.call("tx_begin")
        write(peer, "INSERT INTO durable_sample VALUES (14)")
        peer.call("session_reset")
        require(peer.call("capabilities")["features"]["wire_sql_transactions_v1"], "session reset lost receipt support")
        require(committed(peer, second) == reply["lsn"], "session reset changed durable receipt")
        peer.query("SELECT id FROM durable_sample ORDER BY id", [[i64(10)], [i64(12)]])

    with client(port, tls, *limited) as peer:
        require(not peer.call("capabilities")["features"]["wire_sql_transactions_v1"], "unprovisioned account advertised receipts")
        with peer.statement("INSERT INTO durable_sample VALUES (15)") as handle:
            peer.call("tx_begin")
            peer.error("execute", 1004, statement_id=handle)
            peer.call("tx_rollback")
        peer.error("tx_outcome", 1004, request_token=first)

    dropped_write = token()
    with client(port, tls, *owner) as dropped:
        handle = dropped.call("query.prepare", sql="INSERT INTO durable_sample VALUES (20)")["statement_id"]
        send_without_reply(dropped, "execute", statement_id=handle, request_token=dropped_write)
    dropped_commit = token()
    with client(port, tls, *owner) as dropped:
        dropped.call("tx_begin")
        write(dropped, "INSERT INTO durable_sample VALUES (30)")
        send_without_reply(dropped, "tx_commit", request_token=dropped_commit)
    with client(port, tls, *owner) as peer:
        recorded = {key: committed(peer, key, wait=True) for key in (dropped_write, dropped_commit)}
        peer.query("SELECT id FROM durable_sample ORDER BY id", [[i64(id_)] for id_ in (10, 12, 20, 30)])

    shared = token()

    def contender(id_):
        with client(port, tls, *owner) as peer, peer.statement(f"INSERT INTO durable_sample VALUES ({id_})") as handle:
            return peer.request("execute", {"statement_id": handle, "request_token": shared})

    with ThreadPoolExecutor(max_workers=2) as workers:
        outcomes = list(workers.map(contender, (40, 41)))
    require(sum(reply["ok"] for reply in outcomes) == 1 and
            any(reply.get("error", {}).get("code") == 1013 for reply in outcomes),
            "concurrent requests reused a token or lost a definite duplicate result")
    with client(port, tls, *owner) as peer:
        recorded[shared] = committed(peer, shared)
        peer.query("SELECT COUNT(*) FROM durable_sample WHERE id IN (40, 41)", [[i64(1)]])
    print("PASS TLS wire commit and autocommit receipts, actor isolation, rollback and lost replies", flush=True)
    print("PASS duplicate concurrency, settings and nontransactional write guards", flush=True)
    return recorded, first


def verify_restart(port, tls, owner, recorded, expired):
    with client(port, tls, *owner) as peer:
        for key, lsn in recorded.items():
            require(committed(peer, key) == lsn, "MySQL restart changed a durable receipt")
        unknown(peer, expired)
        with peer.statement("INSERT INTO durable_sample VALUES (99)") as handle:
            peer.error("execute", 1013, statement_id=handle, request_token=expired)
        peer.query("SELECT COUNT(*) FROM durable_sample WHERE id IN (20, 30)", [[i64(2)]])
    print("PASS receipts and writes survive MySQL restart. Expired lookup does not allow token reuse", flush=True)
