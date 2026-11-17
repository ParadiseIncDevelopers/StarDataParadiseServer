"""Check durable compute queue states and restart recovery on a disposable fork."""

import concurrent.futures
from contextlib import contextmanager
import json
import secrets
import subprocess
import time

from .fixture import COMPATIBILITY, HIDDEN, query, require, run


JOBS = COMPATIBILITY / "jobs" / "compute_jobs.py"


def command(instance, *args):
    _, _, cli = instance
    return [*cli, "--file", str(JOBS), *map(str, args)]


def job(instance, *args, failure=None):
    env, _, _ = instance
    result = run(command(instance, *args), env, 30)
    if failure:
        require(result.returncode != 0 and failure in result.stderr, result.stdout + result.stderr)
        return None
    require(result.returncode == 0, result.stdout + result.stderr)
    line = next((line for line in result.stdout.splitlines() if line.startswith("SDP_JOB ")), None)
    require(line is not None, result.stdout + result.stderr)
    return json.loads(line.removeprefix("SDP_JOB "))


@contextmanager
def claimed_worker(server, job_id):
    env, _, _ = server
    with subprocess.Popen(command(server, "work-once"), env=env, stdin=subprocess.DEVNULL,
                          stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL,
                          creationflags=HIDDEN) as worker:
        try:
            deadline = time.monotonic() + 8
            while time.monotonic() < deadline and worker.poll() is None:
                state = query(server, f"SELECT state FROM sdp_jobs.compute_jobs WHERE job_id='{job_id}'")
                if state == "running":
                    break
            else:
                raise RuntimeError("worker did not claim the job fixture")
            yield worker
        finally:
            if worker.poll() is None:
                worker.kill()
            worker.wait(timeout=10)


def check_execution_history(server, job_id, attempts, states):
    attempt, event_data = query(server, f"""SELECT attempt, CAST(events AS CHAR CHARACTER SET utf8mb4)
        FROM sdp_jobs.compute_jobs WHERE job_id='{job_id}'""").split("\t", 1)
    events = json.loads(event_data)
    require(int(attempt) == attempts and [event["state"] for event in events] == states
            and all(event.keys() == {"state", "at"} and type(event["at"]) is int and event["at"] > 0
                    for event in events), f"job execution history differed: {attempt} {events}")


def upgrade_queue(server):
    job(server, "install")
    # Restore the pre-execution-history queue shape only on the disposable fixture.
    query(server, "ALTER TABLE sdp_jobs.compute_jobs DROP COLUMN attempt, DROP COLUMN events")
    finished, running = secrets.token_hex(16), secrets.token_hex(16)
    query(server, f"""INSERT INTO sdp_jobs.compute_jobs
        (job_id, owner, request_token, iterations, timeout_ms, state, checksum)
        VALUES ('{finished}', CURRENT_USER(), '{secrets.token_hex(16)}', 1000, 20000, 'succeeded', '15407749770570313270'),
               ('{running}', CURRENT_USER(), '{secrets.token_hex(16)}', 1000, 20000, 'running', NULL)""")
    job(server, "install")
    job(server, "install")
    require(job(server, "status", finished)["result"]["checksum"] == "15407749770570313270",
            "existing queue result changed during the upgrade")
    check_execution_history(server, finished, 0, [])
    check_execution_history(server, running, 0, [])
    require(job(server, "work-once")["job_id"] is None, "old running job must not be resumed")
    check_execution_history(server, running, 0, ["interrupted"])
    print("PASS existing queue upgrade preserves job records and does not infer earlier attempts or events")


def interval_schedule(server):
    token = secrets.token_hex(16)
    arguments = ("schedule", token, 1000, "--every-seconds", 2)
    schedule_id = job(server, *arguments)["schedule_id"]
    require(job(server, *arguments)["schedule_id"] == schedule_id, "schedule retry changed identity")
    job(server, "schedule", token, 1001, "--every-seconds", 2, failure="different schedule parameters")
    snapshot = job(server, "schedule-status", schedule_id)
    require(snapshot["active"] and snapshot["every_seconds"] == 2 and snapshot["next_utc"].endswith("Z"),
            "schedule configuration or UTC due time was lost")

    env, _, _ = server
    with subprocess.Popen(command(server, "serve"), env=env, stdin=subprocess.DEVNULL,
                          stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL,
                          creationflags=HIDDEN) as worker:
        try:
            deadline = time.monotonic() + 20
            while time.monotonic() < deadline and worker.poll() is None:
                result = query(server, f"""SELECT job.job_id, job.checksum FROM sdp_jobs.compute_jobs AS job
                    JOIN sdp_jobs.compute_occurrences AS event ON event.job_id=job.job_id
                    WHERE event.schedule_id='{schedule_id}' AND job.state='succeeded' LIMIT 1""")
                if result.endswith("\t15407749770570313270"):
                    job_id, _ = result.split("\t", 1)
                    check_execution_history(server, job_id, 1, ["queued", "running", "succeeded"])
                    break
                time.sleep(0.2)
            else:
                raise RuntimeError("the live worker did not publish and run a scheduled job")
            require(not job(server, "cancel-schedule", schedule_id)["active"], "schedule cancellation lost")
            occurrences = f"SELECT COUNT(*) FROM sdp_jobs.compute_occurrences WHERE schedule_id='{schedule_id}'"
            before = query(server, occurrences)
            time.sleep(2.5)
            require(worker.poll() is None and query(server, occurrences) == before,
                    "a cancelled schedule still published jobs or the worker exited")
        finally:
            if worker.poll() is None:
                worker.kill()
            worker.wait(timeout=10)
    print("PASS compute schedules: token reuse, live UTC interval publication and cancellation")


def verify(fixture):
    with fixture.scratch("jobs") as root:
        with fixture.disposable(root / "server") as server:
            upgrade_queue(server)
            token = secrets.token_hex(16)
            with concurrent.futures.ThreadPoolExecutor(max_workers=2) as pool:
                calls = [pool.submit(job, server, "submit", token, 1000) for _ in range(2)]
                first, second = (call.result()["job_id"] for call in calls)
            require(first == second, "concurrent retries generated distinct jobs")
            check_execution_history(server, first, 0, ["queued"])
            job(server, "submit", token, 1001, failure="different job parameters")
            queued = job(server, "submit", secrets.token_hex(16), 1000)["job_id"]
            require(job(server, "cancel", queued)["state"] == "cancelled", "queued cancellation failed")
            require(job(server, "cancel", queued)["state"] == "cancelled", "cancel retry did not find the job")
            check_execution_history(server, queued, 0, ["queued", "cancelled"])
            require(job(server, "work-once")["job_id"] == first, "dispatcher skipped first job")
            assert_result = {"count": 1000, "checksum": "15407749770570313270"}
            completed = job(server, "status", first)
            require(completed["state"] == "succeeded" and completed["result"] == assert_result,
                    "the durable checksum did not match the legacy algorithm")
            check_execution_history(server, first, 1, ["queued", "running", "succeeded"])
            timed = job(server, "submit", secrets.token_hex(16), 1_000_000_000, "--timeout-ms", 1)["job_id"]
            require(job(server, "work-once")["job_id"] == timed, "timeout job not selected")
            require(job(server, "status", timed).get("error") == "timeout", "bounded time was not enforced")
            check_execution_history(server, timed, 1, ["queued", "running", "failed"])

            running = job(server, "submit", secrets.token_hex(16), 1_000_000_000,
                          "--timeout-ms", 60_000)["job_id"]
            with claimed_worker(server, running) as worker:
                deadline = time.monotonic() + 10
                job(server, "cancel", running)
                require(worker.wait(timeout=max(0, deadline - time.monotonic())) == 0,
                        "cancelled worker did not exit normally")
            cancelled = job(server, "status", running)
            require(cancelled["state"] == "cancelled" and "result" not in cancelled,
                    "running cancellation did not persist the terminal state")
            check_execution_history(server, running, 1, ["queued", "running", "cancelled"])

            interrupted = job(server, "submit", secrets.token_hex(16), 1_000_000_000)["job_id"]
            with claimed_worker(server, interrupted) as worker:
                worker.kill()
            require(job(server, "work-once")["job_id"] is None, "interrupted job was run again")
            require(job(server, "status", interrupted)["state"] == "interrupted", "lost worker was not recovered")
            check_execution_history(server, interrupted, 1, ["queued", "running", "interrupted"])
            print("PASS compute attempts and events: retries, result, cancellations, timeout and real worker loss")
            interval_schedule(server)
