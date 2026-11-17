"""Verify legacy History and compute RPCs against an isolated Windows fork."""

import argparse
import json
from pathlib import Path
import re
import secrets
import ssl
import sys
import time

from durable_fork import BASEDIR, BIN, CODE, MIGRATION, SHELL, Fixture, certificates, local_port, mysql_client, run, start, stop, wait_ready
from durable_acceptance import client, token
from acceptance import require
from legacy_rpc import Backend, compute
from provision_ledger import provision as provision_receipts
from mysql import connector

sys.path.insert(1, str(CODE.parents[1]))
from compatibility.history.provision import Column, Table, provision
from compatibility.history.restore import provision_restore


def verify(port, tls, owner, reader, backend):
    target = "main.public.tracked"
    with client(port, tls, *owner) as writer, client(port, tls, *reader) as observer:
        require(not writer.call("capabilities")["features"]["history_v1"], "incomplete History family enabled")
        first = observer.call("history_log", target=target, limit=1)
        require(first["next_offset"] == 1 and first["current_revision"] == "2", "History log pagination")
        row_id = first["entries"][0]["row_id"]
        log = observer.call("history_log", target=target, row_id=row_id, offset=1, limit=1)
        require(log["next_offset"] is None and log["entries"][0]["revision"] == "2", "History log next page")
        snapshot = observer.call("history_read", target=target, revision="1", row_id=row_id)
        require(snapshot["rows"][0]["values"]["score"]["v"] == "10", "History row at prior revision")
        delta = observer.call("history_diff", target=target, **{"from": "1", "to": "2"})
        require(delta["entries"][0]["after"]["values"]["score"]["v"] == "20", "History updated image")
        observer.error("history_restore", 1006, target=target, revision="1", row_id=row_id)
        observer.error("history_log", 1006, target="main.public.unregistered")
        observer.error("history_log", 1004, target=target, since_utc=0)

        plan = writer.call("history_restore", target=target, revision="1", row_id=row_id)
        require(plan["object_id"] == "41" and plan["effects"]["updated"] == 1, "legacy plan bindings")
        restore = plan["plan_id"]
        writer.error("history_revert", 1013, plan_id=restore, request_token=token())
        restore_token = token()
        restored = writer.call("history_restore", plan_id=restore, request_token=restore_token)
        require(restored["revision"] == "3" and restored["status"] == "committed" and not restored["replayed"],
                "restore revision and legacy commit response")
        with client(port, tls, *owner) as reconnect:
            replayed = reconnect.call("history_restore", plan_id=restore, request_token=restore_token)
            require(replayed == {**restored, "replayed": True}, "same-plan recovery on a new TLS connection")
            require(reconnect.call("tx_outcome", request_token=restore_token)
                    == {"status": "committed", "lsn": restored["lsn"]}, "global History receipt")
        observer.error("history_restore", 1006, plan_id=restore, request_token=restore_token)
        require(backend.run_sql("SELECT score FROM wire_legacy.tracked").fetch_one() == (10,), "restore wrote row")
        backend.run_sql("UPDATE wire_legacy.tracked SET score=30 WHERE id=9")
        revert = writer.call("history_revert", target=target, revision="4", row_id=row_id, field="score")["plan_id"]
        require(writer.call("history_revert", plan_id=revert, request_token=token())["revision"] == "5",
                "revert revision")
        require(backend.run_sql("SELECT score FROM wire_legacy.tracked").fetch_one() == (10,), "revert wrote field")
        writer.call("session_reset")
        writer.error("history_revert", 1013, plan_id=revert, request_token=token())
        writer.call("tx_begin")
        writer.error("history_log", 1013, target=target)
        writer.call("tx_rollback")
        print("PASS History log paging, typed read/diff, policy, real row restore and field revert", flush=True)

        require(writer.call("job_capabilities")["kinds"] == ["compute"], "compute capability missing")
        observer.error("job_capabilities", 1006)
        request = token()
        writer.error("job_submit", 1013, kind="compute", count=2048, request_token=restore_token)
        first_job = writer.call("job_submit", kind="compute", count=2048, request_token=request)
        job = first_job["job_id"]
        require(writer.call("job_submit", kind="compute", count=2048, request_token=request) == first_job,
                "duplicate submission produced a different job/receipt")
        require(writer.call("tx_outcome", request_token=request)
                == {"status": "committed", "lsn": first_job["lsn"]}, "global compute receipt")
        writer.error("job_submit", 1013, kind="compute", count=2049, request_token=request)
        prior = token()
        backend.run_sql(f"""INSERT INTO {compute.QUEUE}
            (job_id, owner, request_token, iterations, timeout_ms, events)
            VALUES (?, 'rpc_writer@localhost', ?, 1, 20000, JSON_ARRAY())""", [token(), prior])
        writer.error("job_submit", 1013, kind="compute", count=1, request_token=prior)
        require(writer.call("tx_outcome", request_token=prior) == {"status": "unknown"},
                "an earlier native job acquired a wire receipt")
        backend.run_sql(f"DELETE FROM {compute.QUEUE} WHERE request_token=?", [prior])
        require(writer.call("job_watch", job_id=job)["state"] == "queued", "queued state")
        compute.work_once(backend)
        status = writer.call("job_status", job_id=job)
        require(status["state"] == "succeeded" and status["result"]["checksum"] == compute.compute(2048, lambda: None),
                "compute checksum")
        require(status["attempt"] == 1 and status["progress"] is None
                and [item["state"] for item in status["events"]] == ["queued", "running", "succeeded"], "legacy events")
        waiting = writer.call("job_submit", kind="compute", count=10, request_token=token())["job_id"]
        require(writer.call("job_cancel", job_id=waiting)["state"] == "cancelled", "queued cancellation")
        writer.call("tx_begin")
        writer.error("job_submit", 1013, kind="compute", count=2048, request_token=restore_token)
        rolled_back = writer.call("job_submit", kind="compute", count=10, request_token=token())["job_id"]
        writer.call("tx_rollback")
        writer.error("job_status", 1006, job_id=rolled_back)
        schedule_token = token()
        result = writer.call("job_schedule_create", kind="compute", count=19, timeout_ms=4000,
                             request_token=schedule_token, rule={"every_seconds": 1, "misfire": "run_once"})
        scheduled = result["schedule_id"]
        require(writer.call("tx_outcome", request_token=schedule_token)["lsn"] == result["lsn"], "schedule receipt")
        time.sleep(1.1)
        compute.publish_due_schedules(backend)
        occurrence = writer.call("job_schedule_status", schedule_id=scheduled)
        require(occurrence["next_utc"] > 0 and occurrence["last_job_id"], "UTC schedule did not publish")
        compute.work_once(backend)
        require(writer.call("job_status", job_id=occurrence["last_job_id"])["state"] == "succeeded",
                "scheduled job failed")
        require(not writer.call("job_schedule_cancel", schedule_id=scheduled)["enabled"], "schedule cancellation")
        print("PASS compute idempotency, checksum, cancellation, rollback and UTC interval occurrence", flush=True)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--openssl", type=Path, default=MIGRATION / "toolchain/openssl-firedaemon/x64/bin/openssl.exe")
    args = parser.parse_args()
    fork = Fixture(BIN / "mysqld.exe", BIN / "mysql.exe", BASEDIR, SHELL, None)
    with fork.scratch("wire-legacy-rpc") as folder:
        root = folder / "fork"
        root.mkdir()
        env = fork.environment(root)
        for name in ("front", "back"):
            (root / name).mkdir()
            certificates(str(args.openssl), root / name)
        front, back = root / "front", root / "back"
        key = secrets.token_bytes(32)
        key_file = root / "ledger.key"
        key_file.write_text(key.hex(), encoding="ascii")
        sql_port = local_port()
        admin = mysql_client(fork.mysql, ["--host=127.0.0.1", f"--port={sql_port}", "--user=root",
                                         "--ssl-mode=VERIFY_IDENTITY", f"--ssl-ca={back / 'ca.pem'}"])
        common = [str(fork.mysqld), "--no-defaults", f"--basedir={fork.basedir}", f"--datadir={root / 'data'}",
                  "--console", "--no-monitor"]
        require(run(common + ["--initialize-insecure"], env, 90).returncode == 0, "fixture init")
        command = [*common, "--bind-address=127.0.0.1", f"--port={sql_port}", "--loose-mysqlx=OFF",
                   f"--log-bin={root / 'data' / 'binlog'}", "--gtid-mode=ON", "--enforce-gtid-consistency=ON",
                   "--log-bin-trust-function-creators=ON", "--require-secure-transport=ON",
                   f"--pid-file={root / 'mysqld.pid'}", f"--tmpdir={root}", f"--ssl-ca={back / 'ca.pem'}",
                   f"--ssl-cert={back / 'server.pem'}", f"--ssl-key={back / 'server.key'}"]

        def native(sql):
            result = run(admin, env, 20, script=sql)
            require(result.returncode == 0, "fixture SQL failed: " + result.stderr)
            return result.stdout.strip()

        owner, reader = ((name, secrets.token_hex(24)) for name in ("rpc_writer", "rpc_reader"))
        destroy = root / "mysqld.log"
        with start(command, env, destroy, root) as server:
            wait_ready(server, admin, root, env, 90)
            try:
                uuid = native("SELECT @@server_uuid")
                for name, password in (owner, reader):
                    created = run(admin, env, 15,
                                  script=f"CREATE USER '{name}'@'localhost' IDENTIFIED BY '{password}' REQUIRE SSL;")
                    require(created.returncode == 0, "fixture account creation")
                table = Table("wire_legacy", "tracked", "wire_history", "id", (Column("id", "bigint"), Column("score", "bigint")))
                native(provision(table) + "\n" + provision_restore(table))
                native("""INSERT INTO wire_legacy.tracked VALUES (9,10);
UPDATE wire_legacy.tracked SET score=20 WHERE id=9;
GRANT SELECT, TRIGGER, UPDATE ON wire_legacy.tracked TO 'rpc_writer'@'localhost';
GRANT SELECT ON wire_legacy.tracked TO 'rpc_reader'@'localhost';
GRANT SELECT, INSERT, UPDATE, EXECUTE ON wire_history.* TO 'rpc_writer'@'localhost';
GRANT SELECT, EXECUTE ON wire_history.* TO 'rpc_reader'@'localhost';""")
                with connector.connect(host="127.0.0.1", user="root", port=sql_port, use_pure=True, autocommit=True,
                                       ssl_ca=str(back / "ca.pem"), ssl_verify_cert=True, ssl_verify_identity=True) as db:
                    backend = Backend(db)
                    provision_receipts(db, key, [name + "@localhost" for name, _ in (owner, reader)])
                    compute.install(backend)
                    native("GRANT SELECT, INSERT, UPDATE ON sdp_jobs.* TO 'rpc_writer'@'localhost';")
                    config = root / "rpc.json"
                    config.write_text(json.dumps({"jobs": ["rpc_writer@localhost"], "history": {
                        "tracked": {"table": "tracked", "journal": "wire_history", "object_id": "41", "readers": [
                            "rpc_writer@localhost", "rpc_reader@localhost"], "writers": ["rpc_writer@localhost"]}}}),
                        encoding="utf-8")
                    args = [sys.executable, "-B", str(CODE / "service.py"), "--port", "0",
                            "--cert", str(front / "server.pem"), "--key", str(front / "server.key"),
                            "--mysql-port", str(sql_port), "--mysql-ca", str(back / "ca.pem"), "--mysql-uuid", uuid,
                            "--schema", "wire_legacy", "--ledger-key-file", str(key_file), "--legacy-rpc-config", str(config)]
                    log = root / "bridge.log"
                    with start(args, env, log, root) as bridge:
                        for _ in range(100):
                            details = log.read_text(encoding="utf-8", errors="replace")
                            ready = re.search(r"^SDP_WIRE_V1_READY (\d+)$", details, re.M)
                            if ready:
                                break
                            require(bridge.poll() is None, details)
                            time.sleep(0.1)
                        require(ready is not None, "bridge did not start")
                        verify(int(ready[1]), ssl.create_default_context(cafile=str(front / "ca.pem")),
                               owner, reader, backend)
            finally:
                stop(server, admin, env, True)


if __name__ == "__main__":
    main()
