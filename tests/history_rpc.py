"""Verify durable History queries, permissions, pins and quotas on an isolated TLS Server."""

import argparse
import hashlib
from pathlib import Path
import secrets
import ssl
import tempfile

from service_smoke import certificates, connection, exchange, execute, listening, require, save_config
from sql_rpc import BLOB, TEXT, Client, INVALID_STATE, client, i64, text, token, write


PERMISSION_DENIED = 1006
RESOURCE_LIMIT = 1014
TABLE = "history_fixture"
BLOB_BYTES = bytes.fromhex("00ff617e35")
BLOB_VALUE = {"t": "binary", "hex": BLOB_BYTES.hex()}
BLOB_SUMMARY = {"type": "BLOB", "scope": "value", "size": len(BLOB_BYTES),
                "sha256": hashlib.sha256(BLOB_BYTES).hexdigest()}


def install_fixture(session):
    write(session, f"CREATE TABLE {TABLE} (id BIGINT PRIMARY KEY, label TEXT, payload BLOB)", 0)
    created = write(
        session, f"INSERT INTO {TABLE} VALUES (1,:label,:payload)", 1,
        types={"label": TEXT, "payload": BLOB}, values={"label": text("first"), "payload": BLOB_VALUE},
    )["lsn"]
    changed = write(session, f"UPDATE {TABLE} SET id=2, label='updated' WHERE id=1", 1)["lsn"]
    deleted = write(session, f"DELETE FROM {TABLE} WHERE id=2", 1)["lsn"]
    write(session, "CREATE PROCEDURE history_probe(p_out OUT BIGINT) AS BEGIN p_out := 7; END;", 0)
    return created, changed, deleted


def revisions(session, created, changed, deleted):
    body = {"target": TABLE, "from": created, "to": deleted}
    full = session.call("history_log", **body, limit=256)
    entries = full["entries"]
    row_changes = [entry for entry in entries if entry["kind"] == "row"]
    require([entry["revision"] for entry in row_changes] == [created, changed, deleted],
            "History lost a row event or changed commit order")
    row_id = row_changes[0]["row_id"]
    require(int(row_id) > 0 and all(entry["row_id"] == row_id for entry in row_changes),
            "primary-key change created a new stable row identity")
    require(all(entry["actor"] == "local-admin" and int(entry["commit_utc"]) > 0
                and int(entry["parent"]) < int(entry["revision"])
                and entry["name"] == ["main", "public", TABLE] for entry in row_changes),
            "History lost actor, timestamp, parent ordering or canonical object name")

    first, update, last = row_changes
    require(first["before"] is None and last["after"] is None, "History lost insert/delete semantics")
    require(first["after"]["values"]["id"] == i64(1) and update["before"]["values"]["id"] == i64(1)
            and update["after"]["values"]["id"] == i64(2) and last["before"]["values"]["id"] == i64(2),
            "History did not preserve primary-key before/after values")
    require(all(image["values"]["payload"] == BLOB_SUMMARY
                for entry in row_changes for image in (entry["before"], entry["after"]) if image is not None),
            "historical BLOB diff did not expose only size, hash and scope")

    accumulated = []
    offset = 0
    for _ in range(len(entries) + 1):
        page = session.call("history_log", **body, offset=offset, limit=1)
        accumulated.extend(page["entries"])
        next_offset = page["next_offset"]
        if next_offset is None:
            break
        require(len(page["entries"]) == 1 and next_offset > offset, "History page did not advance")
        offset = next_offset
    require(accumulated == entries, "History pagination changed the underlying entries")

    dates = [int(entry["commit_utc"]) for entry in row_changes]
    dated = session.call("history_log", **body, since_utc=min(dates), until_utc=max(dates), limit=256)
    empty = session.call("history_log", **body, since_utc=max(dates) + 1, limit=256)
    require(dated["entries"] == entries and not empty["entries"], "History UTC bounds changed matching revisions")

    diff = session.call("history_diff", target=TABLE, **{"from": created, "to": changed}, row_id=row_id)["entries"]
    require(len(diff) == 1 and diff[0]["row_id"] == row_id and diff[0]["before"]["values"]["id"] == i64(1)
            and diff[0]["after"]["values"]["id"] == i64(2), "net diff did not preserve same-row primary-key update")
    for revision, key, label in ((created, 1, "first"), (changed, 2, "updated")):
        historical = session.call("history_read", target=TABLE, revision=revision, row_id=row_id)
        records = historical["rows"]
        require(historical["revision"] == revision and len(records) == 1 and records[0]["row_id"] == row_id
                and records[0]["values"] == {"id": i64(key), "label": text(label), "payload": BLOB_SUMMARY},
                "historical read did not reconstruct the selected revision")
    require(session.call("history_read", target=TABLE, revision=deleted, row_id=row_id)["rows"] == [],
            "deleted row remained visible at its deletion revision")

    through_sql = session.mutate(f"HISTORY READ {TABLE} AS OF REVISION {changed} ROW {row_id}")["history"]
    require(through_sql == session.call("history_read", target=TABLE, revision=changed, row_id=row_id),
            "History SQL and RPC returned different state")
    require(any(entry["kind"] == "routine" and entry["name"][-1] == "history_probe"
                for entry in session.call("history_log", limit=256)["entries"]),
            "routine source DDL was not represented in History")
    return full


class HistoryReader(Client):
    def __init__(self, sock, password):
        self.sock = sock
        self.id = 2
        require(exchange(sock, 1, "hello")["ok"], "History reader hello failed")
        require(exchange(sock, 2, "auth", {"username": "history_reader", "password": password})["ok"],
                "limited History reader authentication failed")


def test_current_permissions(port, context, administrator, owner, revision):
    password = secrets.token_urlsafe(32)
    write(owner, "CREATE USER history_reader", 0)
    owner.call("password_set", username="history_reader", new_password=password, request_token=token())
    with connection(port, context) as sock:
        reader = HistoryReader(sock, password)
        reader.error("history_read", PERMISSION_DENIED, target=TABLE, revision=revision)
        write(owner, f"GRANT HISTORY ON TABLE {TABLE} TO USER history_reader", 0)
        reader.error("history_read", PERMISSION_DENIED, target=TABLE, revision=revision)
        write(owner, f"GRANT SELECT ON TABLE {TABLE} TO USER history_reader", 0)
        visible = reader.call("history_read", target=TABLE, revision=revision)["rows"]
        require(len(visible) == 1 and visible[0]["values"]["payload"] == BLOB_SUMMARY,
                "scoped historical row was unavailable or contained raw BLOB bytes")
        reader.error("history_status", PERMISSION_DENIED)
        write(owner, f"REVOKE SELECT ON TABLE {TABLE} FROM USER history_reader", 0)
        reader.error("history_read", PERMISSION_DENIED, target=TABLE, revision=revision)


def test_pins(session, created, changed):
    pin = {"pin_kind": "snapshot", "pin_name": "acceptance-snapshot"}
    session.call("history_pin", **pin, revision=created)
    pinned = session.call("history_status")
    require(any(item["kind"] == "snapshot" and item["name"] == pin["pin_name"] and item["revision"] == created
                for item in pinned["pins"]), "History status lost a durable pin")
    session.error("history_purge", INVALID_STATE, revision=changed)
    require(session.call("history_status")["earliest_revision"] == pinned["earliest_revision"]
            and len(session.call("history_read", target=TABLE, revision=created)["rows"]) == 1,
            "purge crossed an active History dependency")

    session.call("history_unpin", **pin)
    session.call("history_purge", revision=changed)
    retained = session.call("history_status")
    require(retained["earliest_revision"] == changed and not retained["pins"], "purge did not move the retained floor")
    rejected = session.raw("history_read", target=TABLE, revision=created)
    require(not rejected["ok"] and rejected["error"]["code"] == INVALID_STATE
            and rejected["error"]["history_reason"] == "revision_not_recoverable", "retired revision did not fail closed")
    require(len(session.call("history_read", target=TABLE, revision=changed)["rows"]) == 1,
            "earliest retained revision lost its anchor state")
    return retained


def test_quota(session, retained):
    quota = max(64 * 1024, int(retained["retained_bytes"]) + 4096)
    session.call("history_retain", revision=retained["earliest_revision"], quota_bytes=str(quota))
    configured = session.call("history_status")
    require(int(configured["quota_bytes"]) == quota, "History quota configuration was not persisted")

    refused = token()
    with session.prepared(f"INSERT INTO {TABLE} (id,label) VALUES (3,:large)", {"large": TEXT}) as statement:
        session.error("execute", RESOURCE_LIMIT, statement_id=statement, request_token=refused,
                      parameters={"large": text("x" * (160 * 1024))})
    require(session.call("tx_outcome", request_token=refused) == {"status": "unknown"},
            "History quota failure left a committed outcome")
    require(session.query(f"SELECT id FROM {TABLE} WHERE id = 3")[0] == [],
            "History quota overflow published data without its history")
    require(session.call("history_status")["current_revision"] == configured["current_revision"],
            "quota failure created a new committed revision")
    require(session.call("history_verify")["verified"] is True, "retained History integrity verification failed")


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--server", type=Path, required=True)
    parser.add_argument("--openssl", type=Path, required=True)
    parser.add_argument("--scratch", type=Path, required=True)
    args = parser.parse_args()
    server = str(args.server.resolve(strict=True))
    openssl = str(args.openssl.resolve(strict=True))
    args.scratch.mkdir(parents=True, exist_ok=True)
    with tempfile.TemporaryDirectory(prefix="s19-", dir=args.scratch) as directory:
        fixture = Path(directory)
        for name in ("data", "log", "temp"):
            (fixture / name).mkdir()
        certificates(openssl, fixture)
        config = save_config(fixture, timeout_ms=15_000, max_connections=6)
        require(execute(server, config, "--initialize-storage").returncode == 0, "History fixture setup failed")
        password = secrets.token_urlsafe(32)
        password_input = ((password + "\n") * 2).encode("ascii")
        require(execute(server, config, "--bootstrap-admin", "local-admin", stdin=password_input).returncode == 0,
                "History fixture administrator setup failed")
        context = ssl.create_default_context(cafile=str(fixture / "ca.pem"))

        with listening(server, config) as (process, port):
            with client(port, context, password) as owner:
                created, changed, deleted = install_fixture(owner)
                committed = revisions(owner, created, changed, deleted)
                owner.call("tx_begin")
                owner.mutate(f"INSERT INTO {TABLE} (id,label) VALUES (4,'private')")
                require(owner.query(f"SELECT id FROM {TABLE}")[0] == [[i64(4)]], "the crash fixture was not staged")
                process.kill()
                process.wait(timeout=5)

        with listening(server, config) as (_, port):
            with client(port, context, password) as owner:
                require(owner.query(f"SELECT id FROM {TABLE}")[0] == [], "recovery published an uncommitted SQL row")
                require(owner.call("history_log", target=TABLE, **{"from": created, "to": deleted}, limit=256)
                        == committed, "recovery changed committed History or recorded the private edit")
                require(owner.call("history_verify")["verified"] is True, "History verification failed after restart")
                test_current_permissions(port, context, password, owner, changed)
                retained = test_pins(owner, created, changed)
                test_quota(owner, retained)

        require(execute(server, config, "--check-storage").returncode == 0, "History fixture failed WAL/storage verification")

    print("History row identity, typed reads/diff, UTC/pagination, crash atomicity, current grants, pins and quotas passed")


if __name__ == "__main__":
    main()
