"""Exercise durable History restore, inverse changes and replay over an isolated TLS Server."""

import argparse
from pathlib import Path
import secrets
import ssl
import tempfile
import time

from service_smoke import certificates, execute, files_in, listening, require, save_config
from sql_rpc import BLOB, INVALID_ARGUMENT, INVALID_STATE, TIMEOUT, client, i64, text, token, write


def read_at(session, table, revision, **selection):
    return session.call("history_read", target=table, revision=revision, **selection)


def head(session):
    return session.call("history_status")["current_revision"]


def preview(session, mode, table, revision, **selection):
    plan = session.call("history_" + mode, target=table, revision=revision, **selection)
    require(plan["mode"] == mode and plan["revision"] == revision and bool(plan["plan_id"])
            and plan["current_revision"] == head(session) and isinstance(plan["dependencies"], list),
            "History preview failed to identify its snapshot, target or dependencies")
    return plan


def apply(session, mode, plan, request_token=None):
    outcome = session.call("history_" + mode, plan_id=plan["plan_id"], request_token=request_token or token())
    require(outcome["status"] == "committed" and outcome["replayed"] is False, "preview did not publish a new revision")
    require(int(outcome["lsn"]) > int(plan["current_revision"]), "restore did not advance History")
    return outcome["lsn"]


def conflict(reply, reason):
    error = reply.get("error", {})
    require(not reply["ok"] and error.get("code") == INVALID_STATE and error.get("history_reason") == reason,
            f"expected {reason}, got code {error.get('code')} and reason {error.get('history_reason')}")


def select(session, sql):
    return session.query(sql)[0]


def test_restore_row(session):
    blob = {"t": "binary", "hex": "00c2ff7324"}
    write(session, "CREATE TABLE restore_row (id BIGINT PRIMARY KEY, label TEXT, payload BLOB)", 0)
    source = write(session, "INSERT INTO restore_row VALUES (10,'retained',:payload_value)", 1,
                   types={"payload_value": BLOB}, values={"payload_value": blob})["lsn"]
    historical = read_at(session, "restore_row", source)
    old = historical["rows"]
    require(len(old) == 1, "history fixture did not track the original row")
    row_id = old[0]["row_id"]
    deleted = write(session, "DELETE FROM restore_row WHERE id=10", 1)["lsn"]
    plan = preview(session, "restore", "restore_row", source, row_id=row_id)
    require(plan["effects"]["inserted"] == 1 and plan["object_id"] == historical["object_id"],
            "row restoration plan selected the wrong effect or object")
    restored = apply(session, "restore", plan)
    require(select(session, "SELECT id,label,payload FROM restore_row") == [[i64(10), text("retained"), blob]],
            "row restoration changed its typed scalar or BLOB values")
    require(read_at(session, "restore_row", restored)["rows"] == old,
            "restoration changed the stable physical row identity")
    require(read_at(session, "restore_row", deleted)["rows"] == []
            and read_at(session, "restore_row", source)["rows"] == old, "restoration rewrote prior revisions")


def test_restore_table(session, operation, table):
    write(session, f"CREATE TABLE {table} (id BIGINT PRIMARY KEY, label TEXT)", 0)
    source = write(session, f"INSERT INTO {table} VALUES (1,'left'),(2,'right')", 2)["lsn"]
    previous = read_at(session, table, source)
    write(session, f"{operation} TABLE {table}", 0)
    if operation == "DROP":
        session.error("query.prepare", INVALID_ARGUMENT, sql=f"SELECT id FROM {table}")
    else:
        require(select(session, f"SELECT id FROM {table}") == [], "TRUNCATE left rows visible")

    plan = preview(session, "restore", table, source)
    require(plan["effects"]["inserted"] == 2 and plan["object_id"] == previous["object_id"],
            "table restore did not plan both original row identities")
    if operation == "DROP":
        require(plan["effects"]["catalog"] > 0, "DROP restoration omitted its catalog effects")
    revision = apply(session, "restore", plan)
    require(select(session, f"SELECT id,label FROM {table} ORDER BY id")
            == [[i64(1), text("left")], [i64(2), text("right")]], "restored table contents differ")
    current = read_at(session, table, revision)
    require(current["object_id"] == previous["object_id"] and current["rows"] == previous["rows"],
            "table restore changed its original object or row IDs")


def reject_conflicting_restore(session, table, revision, **selection):
    before = head(session)
    reply = session.raw("history_restore", target=table, revision=revision, **selection)
    if reply["ok"]:
        reply = session.raw("history_restore", plan_id=reply["body"]["plan_id"], request_token=token())
    conflict(reply, "restore_conflict")
    require(head(session) == before, "conflicting restore left a successful History revision")


def test_conflicts(session):
    write(session, "CREATE TABLE conflict_pk (id BIGINT PRIMARY KEY, label TEXT)", 0)
    original = write(session, "INSERT INTO conflict_pk VALUES (5,'old identity')", 1)["lsn"]
    old_id = read_at(session, "conflict_pk", original)["rows"][0]["row_id"]
    write(session, "DELETE FROM conflict_pk WHERE id=5", 1)
    write(session, "INSERT INTO conflict_pk VALUES (5,'reused identity')", 1)
    reject_conflicting_restore(session, "conflict_pk", original, row_id=old_id)
    reject_conflicting_restore(session, "conflict_pk", original)
    require(select(session, "SELECT label FROM conflict_pk") == [[text("reused identity")]],
            "restore overwrote a different row that reused the old primary key")

    write(session, "CREATE TABLE conflict_parent (id BIGINT PRIMARY KEY)", 0)
    write(session, "CREATE TABLE conflict_child (id BIGINT PRIMARY KEY, parent_id BIGINT REFERENCES conflict_parent(id))", 0)
    write(session, "INSERT INTO conflict_parent VALUES (7)", 1)
    source = write(session, "INSERT INTO conflict_child VALUES (1,7)", 1)["lsn"]
    write(session, "DELETE FROM conflict_child WHERE id=1", 1)
    write(session, "DELETE FROM conflict_parent WHERE id=7", 1)
    reject_conflicting_restore(session, "conflict_child", source)
    require(select(session, "SELECT id FROM conflict_child") == []
            and select(session, "SELECT id FROM conflict_parent") == [], "restore bypassed the current foreign key")

    write(session, "CREATE TABLE conflict_schema (id BIGINT PRIMARY KEY, amount BIGINT)", 0)
    old_revision = write(session, "INSERT INTO conflict_schema VALUES (1,17)", 1)["lsn"]
    write(session, "ALTER TABLE conflict_schema ADD COLUMN added TEXT", 0)
    current = head(session)
    conflict(session.raw("history_restore", target="conflict_schema", revision=old_revision), "schema_conflict")
    require(head(session) == current
            and select(session, "SELECT id,amount FROM conflict_schema") == [[i64(1), i64(17)]],
            "schema conflict altered the current table")


def test_field_revert(session):
    write(session, "CREATE TABLE revert_field (id BIGINT PRIMARY KEY, amount BIGINT, label TEXT)", 0)
    initial = write(session, "INSERT INTO revert_field VALUES (1,5,'initial')", 1)["lsn"]
    row_id = read_at(session, "revert_field", initial)["rows"][0]["row_id"]
    changed = write(session, "UPDATE revert_field SET amount=9 WHERE id=1", 1)["lsn"]
    independent = write(session, "UPDATE revert_field SET label='later independent edit' WHERE id=1", 1)["lsn"]
    statement = f"HISTORY REVERT revert_field REVISION {changed} ROW {row_id} FIELD amount"
    plan = session.mutate(statement)["history"]
    require(plan["mode"] == "revert" and plan["effects"]["updated"] == 1, "field revert was not planned")
    plan_id = plan["plan_id"].replace("'", "''")
    restored = session.mutate(f"HISTORY REVERT APPLY '{plan_id}' REQUEST '{token()}'")["history"]
    require(restored["status"] == "committed" and restored["replayed"] is False
            and int(restored["lsn"]) > int(independent), "SQL revert did not create a new revision")
    require(select(session, "SELECT amount,label FROM revert_field") == [[i64(5), text("later independent edit")]],
            "field revert overwrote a subsequent independent edit")
    prior = read_at(session, "revert_field", independent)["rows"][0]
    require(prior["row_id"] == row_id and prior["values"]["amount"] == i64(9),
            "field revert modified an older revision or row identity")


def test_stale_preview(session):
    write(session, "CREATE TABLE stale_restore (id BIGINT PRIMARY KEY)", 0)
    source = write(session, "INSERT INTO stale_restore VALUES (1)", 1)["lsn"]
    write(session, "DELETE FROM stale_restore WHERE id=1", 1)
    plan = preview(session, "restore", "stale_restore", source)
    checkpoint = write(session, "INSERT INTO stale_restore VALUES (2)", 1)["lsn"]
    conflict(session.raw("history_restore", plan_id=plan["plan_id"], request_token=token()), "stale_preview")
    require(head(session) == checkpoint and select(session, "SELECT id FROM stale_restore") == [[i64(2)]],
            "stale preview overwrote the newer independent row")


def test_routine_source(session):
    initial = """CREATE FUNCTION restored_routine() RETURN BIGINT AS
BEGIN
    RETURN 7;
END;"""
    original = write(session, initial, 0)["lsn"]
    old = read_at(session, "restored_routine", original)
    write(session, "CREATE OR REPLACE FUNCTION restored_routine() RETURN BIGINT AS BEGIN RETURN 9; END;", 0)
    require(select(session, "SELECT restored_routine()") == [[i64(9)]], "routine replacement did not take effect")
    plan = preview(session, "restore", "restored_routine", original)
    require(plan["object_id"] == old["object_id"] and plan["effects"]["catalog"] > 0,
            "routine restoration did not describe its source effect")
    revision = apply(session, "restore", plan)
    require(select(session, "SELECT restored_routine()") == [[i64(7)]], "restored routine did not execute its prior source")
    retained = read_at(session, "restored_routine", original)
    reopened = read_at(session, "restored_routine", revision)
    require(retained == old and reopened["object_id"] == old["object_id"] and "RETURN 7;" in reopened["object"]["source"],
            "routine restore rewrote historical source or lost its identity")


def test_restore_trigger(session):
    write(session, "CREATE TABLE restore_triggered (id BIGINT PRIMARY KEY)", 0)
    write(session, "CREATE TABLE restore_audit (id BIGINT)", 0)
    write(session, """CREATE TRIGGER trace_restored_insert AFTER INSERT ON restore_triggered FOR EACH ROW
BEGIN
    INSERT INTO restore_audit VALUES (:NEW.id);
END;""", 0)
    source = write(session, "INSERT INTO restore_triggered VALUES (8)", 1)["lsn"]
    write(session, "DELETE FROM restore_triggered WHERE id=8", 1)
    restored = apply(session, "restore", preview(session, "restore", "restore_triggered", source))
    require(select(session, "SELECT id FROM restore_triggered") == [[i64(8)]]
            and select(session, "SELECT id FROM restore_audit") == [[i64(8)], [i64(8)]],
            "restore bypassed the current trigger")
    audit = session.call("history_log", target="restore_audit", **{"from": restored, "to": restored})["entries"]
    require(len(audit) == 1 and audit[0]["kind"] == "row" and audit[0]["revision"] == restored,
            "the restored insert and trigger audit were not committed in the same revision")


def start_lost_reply(session):
    write(session, "CREATE TABLE lost_restore (id BIGINT PRIMARY KEY)", 0)
    source = write(session, "INSERT INTO lost_restore VALUES (7)", 1)["lsn"]
    write(session, "DELETE FROM lost_restore WHERE id=7", 1)
    plan = preview(session, "restore", "lost_restore", source)
    request = {"plan_id": plan["plan_id"], "request_token": token()}
    session.send_without_receiving("history_restore", **request)
    return request


def wait_for_commit(session, request):
    deadline = time.monotonic() + 8
    while time.monotonic() < deadline:
        response = session.raw("tx_outcome", request_token=request["request_token"])
        if response["ok"]:
            outcome = response["body"]
            if outcome["status"] == "committed":
                return outcome["lsn"]
            require(outcome["status"] == "unknown", "lost response gave an incorrect transaction status")
        else:
            require(response["error"]["code"] == TIMEOUT, "could not query the lost restore outcome")
        time.sleep(0.05)
    raise RuntimeError("lost-reply restore did not commit")


def verify_replay(session, request, revision):
    require(session.call("history_restore", **request) == {"status": "committed", "lsn": revision, "replayed": True},
            "reconnected retry did not recover the durable restore receipt")
    require(select(session, "SELECT id FROM lost_restore") == [[i64(7)]], "restore retry created duplicate rows")
    period = {"from": revision, "to": revision}
    entries = session.call("history_log", target="lost_restore", **period)["entries"]
    row_changes = [entry for entry in entries if entry["kind"] == "row"]
    require(len(row_changes) == 1 and row_changes[0]["before"] is None and head(session) == revision,
            "restore retry created an extra History revision")
    require(not session.raw("history_revert", **request)["ok"], "restore receipt was accepted for a different command")
    require(session.call("history_verify")["verified"] is True, "recovered restore history did not verify")


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--server", type=Path, required=True)
    parser.add_argument("--openssl", type=Path, required=True)
    parser.add_argument("--scratch", type=Path, required=True)
    args = parser.parse_args()
    server = str(args.server.resolve(strict=True))
    openssl = str(args.openssl.resolve(strict=True))
    args.scratch.mkdir(parents=True, exist_ok=True)
    with tempfile.TemporaryDirectory(prefix="s20-", dir=args.scratch) as directory:
        fixture = Path(directory)
        for name in ("data", "log", "temp"):
            (fixture / name).mkdir()
        certificates(openssl, fixture)
        config = save_config(fixture, timeout_ms=20_000, max_connections=6)
        require(execute(server, config, "--initialize-storage").returncode == 0, "restore fixture setup failed")
        password = secrets.token_urlsafe(32)
        password_input = ((password + "\n") * 2).encode("ascii")
        require(execute(server, config, "--bootstrap-admin", "local-admin", stdin=password_input).returncode == 0,
                "restore fixture administrator setup failed")
        context = ssl.create_default_context(cafile=str(fixture / "ca.pem"))

        with listening(server, config) as (process, port):
            with client(port, context, password) as owner:
                test_restore_row(owner)
                test_restore_table(owner, "DROP", "dropped_restore")
                test_restore_table(owner, "TRUNCATE", "truncated_restore")
                test_conflicts(owner)
                test_field_revert(owner)
                test_stale_preview(owner)
                test_routine_source(owner)
                test_restore_trigger(owner)
                request = start_lost_reply(owner)
            with client(port, context, password) as verifier:
                revision = wait_for_commit(verifier, request)
                require(select(verifier, "SELECT id FROM lost_restore") == [[i64(7)]],
                        "committed restore was missing before restart")
            process.kill()
            process.wait(timeout=5)

        committed = files_in(fixture / "data")
        with listening(server, config) as (_, port):
            with client(port, context, password) as verifier:
                verify_replay(verifier, request, revision)
        require(execute(server, config, "--check-storage").returncode == 0, "restored storage failed WAL/relational verification")
        require(files_in(fixture / "data") == committed, "replayed receipt changed durable data or WAL")

    print("History stable row/table/routine restore, conflicts, field revert, stale preview and durable retry passed")


if __name__ == "__main__":
    main()
