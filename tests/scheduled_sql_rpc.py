"""Verify durable scheduled SQL, actor policy, rollback, revocation and restart over real TLS."""

import argparse
from contextlib import contextmanager
import json
from pathlib import Path
import secrets
import ssl
import tempfile
import time

from history_restore_rpc import select
from history_rpc import PERMISSION_DENIED, RESOURCE_LIMIT
from jobs_rpc import terminal, wait_for
from service_smoke import certificates, connection, exchange, execute, listening, require, save_config
from sql_rpc import Client, INVALID_ARGUMENT, client, i64, token, write


UNSUPPORTED = 1004
TABLE = "schedule_entries"
GUEST = "schedule_writer"


class Writer(Client):
    def __init__(self, sock, password):
        self.sock = sock
        self.id = 2
        require(exchange(sock, 1, "hello")["ok"], "schedule writer hello failed")
        require(exchange(sock, 2, "auth", {"username": GUEST, "password": password})["ok"],
                "schedule writer could not authenticate")


@contextmanager
def writer(port, context, password):
    with connection(port, context) as sock:
        sock.settimeout(18)
        yield Writer(sock, password)


def request(statements, *, period=1, **options):
    return dict(kind="sql_script", sql=statements, request_token=token(),
                rule={"every_seconds": period, "misfire": "run_once"}, **options)


def wait_occurrence(admin, schedule, *, seconds=18):
    deadline = time.monotonic() + seconds
    while time.monotonic() < deadline:
        view = admin.call("job_schedule_status", schedule_id=schedule)
        if job_id := view["last_job_id"]:
            stopped = admin.call("job_schedule_cancel", schedule_id=schedule)
            require(stopped["enabled"] is False and stopped["last_job_id"] == job_id,
                    "schedule did not retain the selected durable occurrence")
            return wait_for(admin, job_id, terminal)
        time.sleep(.04)
    raise RuntimeError("scheduled SQL occurrence did not start")


def create_fixture(admin, guest_secret):
    write(admin, f"CREATE TABLE {TABLE} (id BIGINT PRIMARY KEY, amount BIGINT NOT NULL)", 0)
    write(admin, f"CREATE DECISION RULE schedule_writer_guard ON {TABLE} FOR INSERT PRIORITY 100 "
                 f"WHEN (ACTOR = '{GUEST}' OR ACTOR = 'local-admin') THEN ALLOW", 0)
    write(admin, f"CREATE USER {GUEST}", 0)
    admin.call("password_set", username=GUEST, new_password=guest_secret, request_token=token())
    write(admin, f"GRANT INSERT, UPDATE, SELECT ON TABLE {TABLE} TO USER {GUEST}", 0)


def check_submission(admin):
    features = admin.call("capabilities")["features"]
    require(features.get("scheduled_sql_v1") is True, "TLS capability did not advertise scheduled SQL")
    capabilities = admin.call("job_capabilities")
    require("sql_script" in capabilities["schedule_kinds"] and "sql_script" not in capabilities["unavailable_kinds"],
            "scheduler did not expose an available scheduled SQL type")

    for sql in ([], "SELECT 1", ["SELECT 1"] * 17, ["SELECT 1\0"]):
        admin.error("job_schedule_create", INVALID_ARGUMENT, **request(sql))
    admin.error("job_schedule_create", UNSUPPORTED, **request(["CREATE USER unwanted_user"]))
    admin.error("job_schedule_create", RESOURCE_LIMIT, **request(["SELECT 1 " + " " * 4096]))
    admin.error("job_schedule_create", RESOURCE_LIMIT, **request(["SELECT 1 " + " " * 4087] * 5))

    admin.call("tx_begin")
    rolled_back = admin.call("job_schedule_create", **request([f"INSERT INTO {TABLE} VALUES (9,90)"]))
    admin.call("tx_rollback")
    admin.error("job_schedule_status", PERMISSION_DENIED, schedule_id=rolled_back["schedule_id"])


def check_durable_success(admin, port, context, guest_secret):
    script = request([
        f"INSERT INTO {TABLE} VALUES (10,11)",
        f"UPDATE {TABLE} SET amount=15 WHERE id=10",
        f"SELECT amount FROM {TABLE} WHERE id=10 ORDER BY amount",
    ])
    with writer(port, context, guest_secret) as session:
        first = session.call("job_schedule_create", **script)["schedule_id"]
        require(session.call("job_schedule_create", **script)["schedule_id"] == first,
                "a replay allocated a second schedule")
    # No job owner session remains open while the write is dispatched.
    job = wait_occurrence(admin, first)
    require(job["state"] == "succeeded" and job["attempt"] == 1 and job["result"] ==
            {"statements": 3, "affected_rows": 2, "selected_rows": 1},
            f"scheduled script returned the wrong durable outcome ({job.get('reason')})")
    require(select(admin, f"SELECT id,amount FROM {TABLE} ORDER BY id") == [[i64(10), i64(15)]],
            "scheduled multi-statement data commit was not atomic")
    row_edits = [entry for entry in admin.call("history_log", target=TABLE, limit=128)["entries"]
                 if entry["kind"] == "row"]
    require(row_edits and len({edit["revision"] for edit in row_edits}) == 1 and
            len({edit["row_id"] for edit in row_edits}) == 1 and all(edit["actor"] == GUEST for edit in row_edits) and
            any(edit["after"]["values"]["amount"] == i64(15) for edit in row_edits),
            "scheduled data change did not produce one History revision for the original actor")
    return first, job["job_id"]


def check_execution_failure(admin, port, context, guest_secret):
    with writer(port, context, guest_secret) as session:
        failed = session.call("job_schedule_create", **request([
            f"INSERT INTO {TABLE} VALUES (20,1)",
            f"INSERT INTO {TABLE} VALUES (10,99)",
        ], retries=1, backoff_seconds=1))["schedule_id"]
    result = wait_occurrence(admin, failed)
    require(result["state"] == "failed" and result["attempt"] == 1 and result["result"] is None,
            "a permanent constraint failure was retried or published a result")
    require(select(admin, f"SELECT id FROM {TABLE} WHERE id=20") == [],
            "the first statement survived a later constraint failure")
    require(not any(entry["kind"] == "row" and entry["after"] is not None and
                    entry["after"]["values"]["id"] == i64(20)
                    for entry in admin.call("history_log", target=TABLE, limit=128)["entries"]),
            "a rolled-back statement created a History revision")

    with writer(port, context, guest_secret) as session:
        revoked = session.call("job_schedule_create", **request([
            f"UPDATE {TABLE} SET amount=33 WHERE id=10",
            f"INSERT INTO {TABLE} VALUES (30,30)",
        ], period=6))["schedule_id"]
    write(admin, f"REVOKE INSERT ON TABLE {TABLE} FROM USER {GUEST}", 0)
    result = wait_occurrence(admin, revoked)
    require(result["state"] == "failed" and result["result"] is None and "permission" in result["reason"],
            "the scheduler used the user's historical grants")
    require(select(admin, f"SELECT id,amount FROM {TABLE} ORDER BY id") == [[i64(10), i64(15)]],
            "a runtime denial did not roll back an earlier permitted statement")


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    for argument in ("server", "openssl", "scratch"):
        parser.add_argument("--" + argument, type=Path, required=True)
    args = parser.parse_args()
    server = str(args.server.resolve(strict=True))
    openssl = str(args.openssl.resolve(strict=True))
    args.scratch.mkdir(parents=True, exist_ok=True)
    with tempfile.TemporaryDirectory(prefix="scheduled-sql-", dir=args.scratch.resolve()) as directory:
        fixture = Path(directory)
        for name in ("data", "log", "temp"):
            (fixture / name).mkdir()
        certificates(openssl, fixture)
        config = save_config(fixture, timeout_ms=20000, max_connections=8)
        require(execute(server, config, "--initialize-storage").returncode == 0, "cannot initialize schedule fixture")
        password, guest_secret = secrets.token_urlsafe(32), secrets.token_urlsafe(32)
        require(execute(server, config, "--bootstrap-admin", "local-admin",
                        stdin=((password + "\n") * 2).encode("ascii")).returncode == 0,
                "cannot bootstrap schedule fixture")
        context = ssl.create_default_context(cafile=str(fixture / "ca.pem"))

        with listening(server, config) as (_, port), client(port, context, password) as admin:
            create_fixture(admin, guest_secret)
            check_submission(admin)
            schedule, job = check_durable_success(admin, port, context, guest_secret)
            check_execution_failure(admin, port, context, guest_secret)

        settings = json.loads(config.read_text(encoding="utf-8"))
        settings["decision_enabled"] = False
        config.write_text(json.dumps(settings), encoding="utf-8")
        with listening(server, config) as (_, port), client(port, context, password) as admin:
            saved = admin.call("job_schedule_status", schedule_id=schedule)
            require(saved["last_job_id"] == job and not saved["enabled"] and
                    admin.call("job_status", job_id=job)["state"] == "succeeded",
                    "restart revived a stopped schedule or lost its committed outcome")
            blocked = admin.call("job_schedule_create", **request([f"INSERT INTO {TABLE} VALUES (40,40)"]))
            result = wait_occurrence(admin, blocked["schedule_id"])
            require(result["state"] == "failed" and result["result"] is None and
                    select(admin, f"SELECT id,amount FROM {TABLE} ORDER BY id") == [[i64(10), i64(15)]],
                    "scheduled writes bypassed the disabled Decision module or replayed a committed job")
            require(admin.call("history_verify")["verified"], "scheduled SQL damaged History")
        require(execute(server, config, "--check-storage").returncode == 0, "schedule fixture storage check failed")
    print("TLS scheduled SQL: live ACL and actor Decision, atomic rollback, retry gate, restart and History passed")


if __name__ == "__main__":
    main()
