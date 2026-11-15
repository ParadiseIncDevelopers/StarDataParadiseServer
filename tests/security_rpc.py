"""Verify authenticated session monitoring, cancellation and idle rollback over loopback TLS."""

import argparse
import json
from pathlib import Path
import secrets
import ssl
import struct
import tempfile
import time

from service_smoke import certificates, connection, exchange, execute, listening, receive, require, save_config
from sql_rpc import INVALID_STATE, client, i64, text, token, write


CANCELLED = 1009
FIXTURE_TIMEOUT_MS = 2500


def session_summaries(controller):
    return controller.call("session_list")["sessions"]


def await_waiter(controller, request_id):
    deadline = time.monotonic() + 1.5
    while time.monotonic() < deadline:
        waiting = [
            session for session in session_summaries(controller)
            if session["request_id"] == request_id and session["operation"] == "query.prepare"
            and session["lock_wait_ms"] > 0 and session["running_ms"] > 0
        ]
        if waiting:
            require(len(waiting) == 1, "more than one test session reported the same active request")
            return waiting[0]
        time.sleep(0.005)
    raise RuntimeError("waiting query was not visible with positive lock_wait_ms")


def receive_reply(client_):
    length, = struct.unpack("!I", receive(client_.sock, 4))
    require(0 < length <= 1024**2, "invalid response length")
    response = json.loads(receive(client_.sock, length))
    require(response["id"] == client_.id and response["v"] == 1, "uncorrelated queued query response")
    return response


def cancel_wait(port, context, password):
    with client(port, context, password) as holder, client(port, context, password) as waiting, \
            client(port, context, password) as controller:
        holder.call("tx_begin")
        waiting.send_without_receiving("query.prepare", sql="SELECT id FROM events")
        target = await_waiter(controller, waiting.id)
        require(not target["transaction_active"],
                "waiting query reported owning the held transaction")
        require(controller.call("query_cancel", session_id=target["session_id"], request_id=waiting.id + 1)
                == {"accepted": False}, "cancel accepted an incorrect request identifier")
        require(controller.call("query_cancel", session_id=target["session_id"], request_id=waiting.id)
                == {"accepted": True}, "cancel did not accept an active request")

        response = receive_reply(waiting)
        require(not response["ok"] and response["error"]["code"] == CANCELLED,
                "accepted cancellation did not finish with CANCELLED")
        require(waiting.call("health")["status"] == "ready", "query cancellation closed the connection")
        require(controller.call("query_cancel", session_id=target["session_id"], request_id=waiting.id - 1)
                == {"accepted": False}, "completed request remained cancellable")
        require(any(session["transaction_active"] for session in session_summaries(controller)),
                "cancel affected the unrelated transaction holder")

        holder.call("tx_rollback")
        selected, _ = waiting.query("SELECT id FROM events")
        require(selected == [[i64(1)]], "connection could not query after cancelling its previous request")


def await_transaction_expiry(holder, controller, session_id):
    timeout_seconds = FIXTURE_TIMEOUT_MS / 1000
    deadline = time.monotonic() + 2 * timeout_seconds
    while time.monotonic() < deadline:
        summaries = session_summaries(controller)
        owner = next((session for session in summaries if session["session_id"] == session_id), None)
        require(owner is not None, "TCP session expired before its idle transaction could be checked")
        if not owner["transaction_active"]:
            return
        # A harmless transport request keeps the socket open without extending the transaction lease.
        require(holder.call("health")["status"] == "ready", "transaction owner lost its connection")
        time.sleep(0.1)
    raise RuntimeError("idle transaction did not expire")


def idle_rollback(port, context, password):
    with client(port, context, password) as holder, client(port, context, password) as controller:
        holder.call("tx_begin")
        holder.mutate("UPDATE events SET label = 'uncommitted' WHERE id = 1")
        holder.mutate("INSERT INTO events VALUES (2,'uncommitted row')")
        active = [session for session in session_summaries(controller) if session["transaction_active"]]
        require(len(active) == 1, "idle timeout fixture did not observe the active transaction")
        await_transaction_expiry(holder, controller, active[0]["session_id"])

        # This request must acquire the storage lease before it can return the earlier committed data.
        rows, _ = controller.query("SELECT id, label FROM events")
        require(rows == [[i64(1), text("original")]], "expired transaction published private edits")
        reply = holder.raw("tx_commit", request_token=token())
        require(not reply["ok"] and reply["error"]["code"] in (CANCELLED, INVALID_STATE),
                "expired transaction could still commit")
        require(holder.call("health")["status"] == "ready", "idle transaction cleanup closed the connection")

        write(controller, "INSERT INTO events VALUES (2,'committed after expiry')", 1)
        rows, _ = controller.query("SELECT label FROM events WHERE id = 2")
        require(rows == [[text("committed after expiry")]], "idle transaction kept its lock or private row")


def check_reserved_permissions(port, context, administrator):
    limited = secrets.token_urlsafe(32)
    with client(port, context, administrator) as owner:
        write(owner, "CREATE USER visitor", 0)
        owner.call("password_set", username="visitor", new_password=limited, request_token=token())
    with connection(port, context) as sock:
        require(exchange(sock, 1, "hello", {}).get("ok"), "limited session hello failed")
        require(exchange(sock, 2, "auth", {"username": "visitor", "password": limited}).get("ok"),
                "limited account authentication failed")
        for request_id, operation in enumerate(("history.log", "backup.start", "debug.attach"), start=3):
            denied = exchange(sock, request_id, operation, {})
            require(not denied["ok"] and denied["error"]["code"] == 1006,
                    f"{operation} exposed a capability to an unprivileged session")


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--server", type=Path, required=True)
    parser.add_argument("--openssl", type=Path, required=True)
    parser.add_argument("--scratch", type=Path, required=True)
    args = parser.parse_args()
    server = str(args.server.resolve(strict=True))
    openssl = str(args.openssl.resolve(strict=True))
    args.scratch.mkdir(parents=True, exist_ok=True)
    with tempfile.TemporaryDirectory(prefix="s15-", dir=args.scratch) as directory:
        fixture = Path(directory)
        for name in ("data", "log", "temp"):
            (fixture / name).mkdir()
        certificates(openssl, fixture)
        config = save_config(fixture, timeout_ms=FIXTURE_TIMEOUT_MS, max_connections=6)
        require(execute(server, config, "--initialize-storage").returncode == 0, "security storage setup failed")
        password = secrets.token_urlsafe(32)
        repeated = ((password + "\n") * 2).encode("ascii")
        require(execute(server, config, "--bootstrap-admin", "local-admin", stdin=repeated).returncode == 0,
                "security administrator setup failed")
        context = ssl.create_default_context(cafile=str(fixture / "ca.pem"))

        with listening(server, config) as (_, port):
            with client(port, context, password) as setup:
                features = setup.call("capabilities")["features"]
                require(features.get("sql_security_v1") is True, "initialized Server did not advertise security")
                write(setup, "CREATE TABLE events (id BIGINT PRIMARY KEY, label TEXT)", 0)
                write(setup, "INSERT INTO events VALUES (1,'original')", 1)
            cancel_wait(port, context, password)
            idle_rollback(port, context, password)
            check_reserved_permissions(port, context, password)

        require(execute(server, config, "--check-storage").returncode == 0, "security fixture failed storage verification")

    print("Authenticated lock wait monitoring, correlated query cancellation and idle transaction rollback passed")


if __name__ == "__main__":
    main()
