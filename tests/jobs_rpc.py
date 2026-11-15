"""Isolated TLS job lifecycle, cancellation, transaction and scheduler acceptance."""
import argparse
import secrets
import signal
import ssl
import tempfile
import time
from pathlib import Path

from history_restore_rpc import select
from service_smoke import certificates, connection, exchange, execute, listening, require, save_config
from sql_rpc import client, i64, token, write


def wait_for(session, job_id, check, seconds=20):
    end = time.monotonic() + seconds
    while time.monotonic() < end:
        state = session.call("job_watch", job_id=job_id)
        if check(state):
            return state
        time.sleep(.035)
    raise RuntimeError("job did not reach the required state")


def terminal(state):
    return state["state"] in {"succeeded", "failed", "cancelled"}


def submit(session, count, **kwargs):
    return session.call("job_submit", request_token=token(), kind="compute", count=count, **kwargs)["job_id"]


def caller_disconnect(port, context, password):
    with client(port, context, password) as origin:
        request_id = token()
        job_id = origin.call("job_submit", request_token=request_id, kind="compute", count=2500000)["job_id"]
    with client(port, context, password) as monitor:
        done = wait_for(monitor, job_id, terminal)
        require(done["state"] == "succeeded" and done["result"]["count"] == 2500000,
                "job did not finish the actual computation after the client closed")
        require(done["attempt"] == 1 and done["result"]["checksum"].isdigit()
                and [event["state"] for event in done["events"]] == ["queued", "starting", "running", "succeeded"],
                "persistent job attempt or event stream was wrong")
        duplicate = monitor.call("job_submit", request_token=request_id, kind="compute", count=2500000)
        require(duplicate["job_id"] == job_id and duplicate["state"] == "succeeded",
                "duplicate request launched another worker")
        return job_id


def isolation_and_quotas(owner, fixture):
    running = submit(owner, 1_000_000_000, timeout_ms=60000)
    live = wait_for(owner, running, lambda state: state["worker_pid"] is not None or terminal(state))
    require(live["state"] == "running" and isinstance(live["worker_pid"], int), "isolated worker was not observable")
    require(select(owner, "SELECT 7") == [[i64(7)]], "running worker held the OLTP lease")
    import os
    os.kill(live["worker_pid"], signal.SIGTERM)
    failed = wait_for(owner, running, terminal)
    require(failed["state"] == "failed" and failed["result"] is None, "killed worker published success")
    require(select(owner, "SELECT 8") == [[i64(8)]], "worker kill stopped SQL")

    small = submit(owner, 50000, output_bytes=1)
    over = wait_for(owner, small, terminal)
    require(over["state"] == "failed" and over["reason"] == "output_limit" and over["result"] is None,
            "worker output exceeded its budget without failing closed")
    cancelling = submit(owner, 1_000_000_000, timeout_ms=60000)
    wait_for(owner, cancelling, lambda state: state["worker_pid"] is not None)
    acknowledgement = owner.call("job_cancel", job_id=cancelling)
    require(acknowledgement["cancel_requested"] is True, "cancel request was not durably acknowledged")
    require(wait_for(owner, cancelling, terminal)["state"] == "cancelled", "worker did not exit after cancel")
    require(not any((fixture / "temp").iterdir()), "worker scratch was left populated")


def rollback_and_schedule(owner):
    owner.call("tx_begin")
    aborted = submit(owner, 10000)
    require(owner.call("job_status", job_id=aborted)["state"] == "queued", "staged job should remain private")
    schedule = owner.call("job_schedule_create", request_token=token(), kind="compute", count=1000,
                          rule={"every_seconds": 1})["schedule_id"]
    owner.call("tx_rollback")
    owner.error("job_status", 1006, job_id=aborted)
    owner.error("job_schedule_status", 1006, schedule_id=schedule)

    created = owner.call("job_schedule_create", request_token=token(), kind="compute", count=42000,
                         rule={"every_seconds": 1}, misfire="run_once")["schedule_id"]
    end = time.monotonic() + 12
    while time.monotonic() < end:
        status = owner.call("job_schedule_status", schedule_id=created)
        active = status["last_job_id"]
        if active:
            require(owner.call("job_schedule_cancel", schedule_id=created)["enabled"] is False,
                    "schedule cancel did not preserve inactive state")
            done = wait_for(owner, active, terminal)
            require(done["state"] == "succeeded" and done["result"]["count"] == 42000 and done["attempt"] == 1,
                    "scheduled worker did not run the exact configured task once")
            return created, active
        time.sleep(.04)
    raise RuntimeError("persistent scheduler missed a due occurrence")


def scheduled_retry(owner):
    schedule = owner.call("job_schedule_create", request_token=token(), kind="compute", count=30000, output_bytes=1,
                          rule={"every_seconds": 1}, retries=1, backoff_seconds=1)["schedule_id"]
    end = time.monotonic() + 12
    while time.monotonic() < end:
        launched = owner.call("job_schedule_status", schedule_id=schedule)["last_job_id"]
        if launched:
            owner.call("job_schedule_cancel", schedule_id=schedule)
            failed = wait_for(owner, launched, terminal)
            require(failed["state"] == "failed" and failed["attempt"] == 2 and failed["reason"] == "output_limit",
                    "bounded backoff did not retry the same job identity exactly once")
            return
        time.sleep(.04)
    raise RuntimeError("retry fixture schedule never launched")


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    for argument in ("server", "openssl", "scratch"):
        parser.add_argument("--" + argument, type=Path, required=True)
    args = parser.parse_args()
    server = str(args.server.resolve(strict=True))
    openssl = str(args.openssl.resolve(strict=True))
    args.scratch.mkdir(parents=True, exist_ok=True)
    with tempfile.TemporaryDirectory(prefix="job-", dir=args.scratch.resolve()) as directory:
        fixture = Path(directory)
        for name in ("data", "log", "temp"):
            (fixture / name).mkdir()
        certificates(openssl, fixture)
        config = save_config(fixture, "config.json", timeout_ms=20000, max_connections=8)
        require(execute(server, config, "--initialize-storage").returncode == 0, "cannot initialize job instance")
        password = secrets.token_urlsafe(32)
        require(execute(server, config, "--bootstrap-admin", "local-admin",
                        stdin=((password + "\n") * 2).encode("ascii")).returncode == 0,
                "cannot bootstrap job instance")
        context = ssl.create_default_context(cafile=str(fixture / "ca.pem"))
        guest_password = secrets.token_urlsafe(32)
        with listening(server, config) as (_, port):
            previous = caller_disconnect(port, context, password)
            with client(port, context, password) as owner:
                require(owner.call("capabilities")["features"].get("jobs_v1") is True, "jobs capability absent")
                require(owner.call("job_capabilities")["maximum_workers"] == 1, "OLTP reservation policy absent")
                isolation_and_quotas(owner, fixture)
                schedule, scheduled_job = rollback_and_schedule(owner)
                write(owner, "CREATE USER visitor", 0)
                owner.call("password_set", username="visitor", new_password=guest_password, request_token=token())
                with connection(port, context) as visitor:
                    require(exchange(visitor, 1, "hello")["ok"], "guest hello failed")
                    require(exchange(visitor, 2, "auth", {"username": "visitor", "password": guest_password})["ok"],
                            "guest authentication failed")
                    for number, operation in enumerate(("job_status", "job_watch", "job_cancel"), start=3):
                        denied = exchange(visitor, number, operation, {"job_id": previous})
                        require(not denied["ok"] and denied["error"]["code"] == 1006, "guest accessed another actor's job")
                    denied = exchange(visitor, 6, "job_schedule_status", {"schedule_id": schedule})
                    require(not denied["ok"] and denied["error"]["code"] == 1006, "guest accessed another actor's schedule")
                scheduled_retry(owner)
                interrupted = submit(owner, 1_000_000_000, timeout_ms=60000)
                live = wait_for(owner, interrupted, lambda state: state["worker_pid"] is not None)
                require(live["state"] == "running", "restart fixture missed worker startup")

        with listening(server, config) as (_, port), client(port, context, password) as owner:
            schedule_state = owner.call("job_schedule_status", schedule_id=schedule)
            require(not schedule_state["enabled"] and schedule_state["last_job_id"] == scheduled_job,
                    "restart activated or duplicated a stopped schedule")
            for job_id in (previous, scheduled_job):
                done = owner.call("job_status", job_id=job_id)
                require(done["state"] == "succeeded" and done["attempt"] == 1,
                        "restart lost or repeated a completed job")
            crashed = wait_for(owner, interrupted, terminal)
            require(crashed["state"] == "failed" and crashed["reason"] == "interrupted" and crashed["result"] is None,
                    "server restart published or repeated an interrupted job")
            require(not any((fixture / "temp").iterdir()), "server restart left orphaned worker scratch")
            require(owner.call("history_verify")["verified"], "job state did not participate in durable WAL")
        require(execute(server, config, "--check-storage").returncode == 0, "job database integrity failed")
    print("TLS persistent jobs: disconnect, OS isolation, quota/cancel, rolled-back submission, schedule/restart and ACL passed")


if __name__ == "__main__":
    main()
