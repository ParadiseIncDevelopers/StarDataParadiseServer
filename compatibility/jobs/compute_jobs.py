"""Run the legacy compute checksum with a persistent MySQL job queue."""

import argparse
import json
import re
import sys
import time
import uuid


QUEUE = "sdp_jobs.compute_jobs"
SCHEDULES = "sdp_jobs.compute_schedules"
OCCURRENCES = "sdp_jobs.compute_occurrences"
WORKER_LOCK = "stardataparadise:compute-worker"
SCHEMA_LOCK = "stardataparadise:compute-schema"
MASK64 = (1 << 64) - 1
EXECUTION_COLUMNS = {
    "attempt": "INT UNSIGNED NOT NULL DEFAULT 0",
    "events": "JSON NOT NULL DEFAULT (JSON_ARRAY())",
}
QUEUED_EVENT = "JSON_ARRAY(JSON_OBJECT('state', 'queued', 'at', UNIX_TIMESTAMP()))"


def append_event(state):
    return f"JSON_ARRAY_APPEND(events, '$', JSON_OBJECT('state', {state}, 'at', UNIX_TIMESTAMP()))"


def upgrade_execution_columns(session):
    if session.run_sql("SELECT GET_LOCK(?, 10)", [SCHEMA_LOCK]).fetch_one()[0] != 1:
        raise ValueError("could not lock the compute queue schema")
    try:
        existing = {row[0] for row in session.run_sql("""SELECT COLUMN_NAME
            FROM information_schema.COLUMNS WHERE TABLE_SCHEMA = 'sdp_jobs' AND TABLE_NAME = 'compute_jobs'
              AND COLUMN_NAME IN ('attempt', 'events')""").fetch_all()}
        additions = [f"ADD COLUMN {name} {definition}"
                     for name, definition in EXECUTION_COLUMNS.items() if name not in existing]
        if additions:
            session.run_sql(f"ALTER TABLE {QUEUE} " + ", ".join(additions))
    finally:
        session.run_sql("SELECT RELEASE_LOCK(?)", [SCHEMA_LOCK])


def install(session):
    session.run_sql("CREATE SCHEMA IF NOT EXISTS sdp_jobs")
    session.run_sql(f"""CREATE TABLE IF NOT EXISTS {QUEUE} (
        sequence BIGINT UNSIGNED NOT NULL AUTO_INCREMENT PRIMARY KEY,
        job_id CHAR(32) CHARACTER SET ascii COLLATE ascii_bin NOT NULL UNIQUE,
        owner VARCHAR(288) CHARACTER SET utf8mb4 COLLATE utf8mb4_bin NOT NULL,
        request_token CHAR(32) CHARACTER SET ascii COLLATE ascii_bin NOT NULL,
        iterations INT UNSIGNED NOT NULL,
        timeout_ms INT UNSIGNED NOT NULL,
        state ENUM('queued','running','succeeded','failed','cancelled','interrupted') NOT NULL DEFAULT 'queued',
        cancel_requested BOOLEAN NOT NULL DEFAULT FALSE,
        checksum VARCHAR(20) CHARACTER SET ascii,
        error_code VARCHAR(32) CHARACTER SET ascii,
        {", ".join(f"{name} {definition}" for name, definition in EXECUTION_COLUMNS.items())},
        UNIQUE KEY (owner, request_token), KEY (state, sequence)
    ) ENGINE=InnoDB""")
    upgrade_execution_columns(session)
    session.run_sql(f"""CREATE TABLE IF NOT EXISTS {SCHEDULES} (
        schedule_id CHAR(32) CHARACTER SET ascii COLLATE ascii_bin NOT NULL PRIMARY KEY,
        owner VARCHAR(288) CHARACTER SET utf8mb4 COLLATE utf8mb4_bin NOT NULL,
        request_token CHAR(32) CHARACTER SET ascii COLLATE ascii_bin NOT NULL,
        iterations INT UNSIGNED NOT NULL,
        timeout_ms INT UNSIGNED NOT NULL,
        interval_seconds INT UNSIGNED NOT NULL CHECK (interval_seconds BETWEEN 1 AND 86400),
        next_due DATETIME(6) NOT NULL,
        active BOOLEAN NOT NULL DEFAULT TRUE,
        UNIQUE KEY (owner, request_token), KEY (active, next_due)
    ) ENGINE=InnoDB""")
    session.run_sql(f"""CREATE TABLE IF NOT EXISTS {OCCURRENCES} (
        schedule_id CHAR(32) CHARACTER SET ascii COLLATE ascii_bin NOT NULL,
        job_id CHAR(32) CHARACTER SET ascii COLLATE ascii_bin NOT NULL PRIMARY KEY,
        KEY (schedule_id), FOREIGN KEY (schedule_id) REFERENCES {SCHEDULES}(schedule_id),
        FOREIGN KEY (job_id) REFERENCES {QUEUE}(job_id)
    ) ENGINE=InnoDB""")


def submit(session, token, count, timeout_ms, *, allow_replay=True):
    job_id = uuid.uuid4().hex
    duplicate = "ON DUPLICATE KEY UPDATE sequence = sequence" if allow_replay else ""
    session.run_sql(f"""INSERT INTO {QUEUE}(job_id, owner, request_token, iterations, timeout_ms, events)
        VALUES (?, CURRENT_USER(), ?, ?, ?, {QUEUED_EVENT}) {duplicate}""",
                    [job_id, token, count, timeout_ms])
    if not allow_replay:
        return job_id
    row = session.run_sql(f"""SELECT job_id, iterations, timeout_ms FROM {QUEUE}
        WHERE owner = CURRENT_USER() AND request_token = ?""", [token]).fetch_one()
    if row is None or (row[1], row[2]) != (count, timeout_ms):
        raise ValueError("request token is already bound to different job parameters")
    return row[0]


def status_record(session, job_id):
    row = session.run_sql(f"""SELECT state, iterations, checksum, error_code, cancel_requested,
        attempt, CAST(events AS CHAR CHARACTER SET utf8mb4) FROM {QUEUE}
        WHERE owner = CURRENT_USER() AND job_id = ?""", [job_id]).fetch_one()
    if row is None:
        raise ValueError("job not found")
    state, count, checksum, error, cancel, attempt, events = (row[index] for index in range(7))
    return state, count, checksum, error, bool(cancel), attempt, json.loads(events)


def status(session, job_id):
    state, count, checksum, error, *_ = status_record(session, job_id)
    result = {"job_id": job_id, "state": state}
    if checksum is not None:
        result["result"] = {"count": count, "checksum": checksum}
    if error is not None:
        result["error"] = error
    return result


def cancel(session, job_id):
    session.run_sql(f"""UPDATE {QUEUE}
        SET events = IF(state = 'queued', {append_event("'cancelled'")}, events),
            cancel_requested = TRUE, state = IF(state = 'queued', 'cancelled', state)
        WHERE owner = CURRENT_USER() AND job_id = ? AND state IN ('queued','running')""", [job_id])
    return status(session, job_id)


def create_schedule(session, args, *, allow_replay=True):
    parameters = (args.count, args.timeout_ms, args.every_seconds)
    schedule_id = uuid.uuid4().hex
    duplicate = "ON DUPLICATE KEY UPDATE schedule_id = schedule_id" if allow_replay else ""
    session.run_sql(f"""INSERT INTO {SCHEDULES} (schedule_id, owner, request_token, iterations, timeout_ms,
        interval_seconds, next_due) VALUES (?, CURRENT_USER(), ?, ?, ?, ?, TIMESTAMPADD(SECOND, ?, UTC_TIMESTAMP(6)))
        {duplicate}""", [schedule_id, args.request_token, *parameters, args.every_seconds])
    if not allow_replay:
        return schedule_id
    row = session.run_sql(f"""SELECT schedule_id, iterations, timeout_ms, interval_seconds FROM {SCHEDULES}
        WHERE owner = CURRENT_USER() AND request_token = ?""", [args.request_token]).fetch_one()
    if row is None or tuple(row[index] for index in range(1, 4)) != parameters:
        raise ValueError("request token is already bound to different schedule parameters")
    return row[0]


def schedule_status(session, schedule_id):
    row = session.run_sql(f"""SELECT active, interval_seconds, DATE_FORMAT(next_due, '%Y-%m-%dT%H:%i:%s.%fZ')
        FROM {SCHEDULES} WHERE owner = CURRENT_USER() AND schedule_id = ?""", [schedule_id]).fetch_one()
    if row is None:
        raise ValueError("schedule not found")
    return {"schedule_id": schedule_id, "active": bool(row[0]), "every_seconds": row[1], "next_utc": row[2]}


def publish_due_schedules(session):
    session.run_sql("START TRANSACTION")
    try:
        result = session.run_sql(f"""SELECT schedule_id, owner, iterations, timeout_ms FROM {SCHEDULES}
            WHERE active AND next_due <= UTC_TIMESTAMP(6) ORDER BY next_due, schedule_id LIMIT 128 FOR UPDATE""")
        for schedule in result.fetch_all():
            schedule_id, owner, iterations, timeout_ms = [schedule[index] for index in range(4)]
            pending = session.run_sql(f"""SELECT 1 FROM {OCCURRENCES} AS occurrence
                JOIN {QUEUE} AS job ON job.job_id = occurrence.job_id
                WHERE occurrence.schedule_id = ? AND job.state IN ('queued','running') LIMIT 1""",
                                      [schedule_id]).fetch_one()
            if pending is None:
                job_id = uuid.uuid4().hex
                session.run_sql(f"""INSERT INTO {QUEUE}(job_id, owner, request_token, iterations, timeout_ms, events)
                    VALUES (?, ?, ?, ?, ?, {QUEUED_EVENT})""",
                                [job_id, owner, uuid.uuid4().hex, iterations, timeout_ms])
                session.run_sql(f"INSERT INTO {OCCURRENCES}(schedule_id, job_id) VALUES (?, ?)", [schedule_id, job_id])
            session.run_sql(f"""UPDATE {SCHEDULES} SET next_due = TIMESTAMPADD(SECOND,
                (TIMESTAMPDIFF(SECOND, next_due, UTC_TIMESTAMP(6)) DIV interval_seconds + 1) * interval_seconds,
                next_due) WHERE schedule_id = ?""", [schedule_id])
        session.run_sql("COMMIT")
    except Exception:
        session.run_sql("ROLLBACK")
        raise


def compute(count, check):
    # Match the original worker's unsigned 64-bit arithmetic and decimal output.
    checksum = 0x9E3779B97F4A7C15
    for first in range(1, count + 1, 4096):
        check()
        for iteration in range(first, min(first + 4096, count + 1)):
            checksum ^= (iteration + 0x9E3779B97F4A7C15) & MASK64
            checksum = ((checksum ^ (checksum >> 30)) * 0xBF58476D1CE4E5B9) & MASK64
            checksum = ((checksum ^ (checksum >> 27)) * 0x94D049BB133111EB) & MASK64
            checksum ^= checksum >> 31
    check()
    return str(checksum)


class Cancelled(Exception):
    pass


def work_once(session):
    if session.run_sql("SELECT GET_LOCK(?, 0)", [WORKER_LOCK]).fetch_one()[0] != 1:
        raise ValueError("a compute worker already holds the queue lock")
    try:
        session.run_sql(f"""UPDATE {QUEUE} SET state = 'interrupted', error_code = 'worker_interrupted',
            events = {append_event("'interrupted'")}
            WHERE state = 'running'""")
        publish_due_schedules(session)
        while True:
            job = session.run_sql(f"""SELECT job_id, iterations, timeout_ms FROM {QUEUE}
                WHERE state = 'queued' ORDER BY sequence LIMIT 1""").fetch_one()
            if job is None:
                return None
            job_id, count, timeout_ms = [job[index] for index in range(3)]
            claim = session.run_sql(f"""UPDATE {QUEUE} SET state = 'running', attempt = attempt + 1,
                events = {append_event("'running'")}
                WHERE job_id = ? AND state = 'queued'""", [job_id])
            if claim.get_affected_items_count() == 1:
                break

        deadline = time.monotonic() + timeout_ms / 1000

        def check():
            row = session.run_sql(f"SELECT cancel_requested FROM {QUEUE} WHERE job_id = ?", [job_id]).fetch_one()
            if row is None or row[0]:
                raise Cancelled()
            if time.monotonic() >= deadline:
                raise TimeoutError()

        state, checksum, error = "succeeded", None, None
        try:
            checksum = compute(count, check)
        except Cancelled:
            state = "cancelled"
        except TimeoutError:
            state, error = "failed", "timeout"
        session.run_sql(f"""UPDATE {QUEUE}
            SET checksum = IF(cancel_requested, NULL, ?),
                error_code = IF(cancel_requested, NULL, ?),
                events = {append_event("IF(cancel_requested, 'cancelled', ?)")},
                state = IF(cancel_requested, 'cancelled', ?)
            WHERE job_id = ? AND state = 'running'""", [checksum, error, state, state, job_id])
        return job_id
    finally:
        session.run_sql("SELECT RELEASE_LOCK(?)", [WORKER_LOCK])


def hexadecimal(value):
    if re.fullmatch(r"[0-9a-f]{32}", value) is None:
        raise argparse.ArgumentTypeError("expected 32 lowercase hexadecimal characters")
    return value


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__)
    subparsers = parser.add_subparsers(dest="command", required=True)
    subparsers.add_parser("install")
    subparsers.add_parser("work-once")
    subparsers.add_parser("serve")
    for command in ("submit", "schedule"):
        create = subparsers.add_parser(command)
        create.add_argument("request_token", type=hexadecimal)
        create.add_argument("count", type=int)
        create.add_argument("--timeout-ms", type=int, default=20_000)
        if command == "schedule":
            create.add_argument("--every-seconds", type=int, required=True)
    for command in ("status", "cancel"):
        subparsers.add_parser(command).add_argument("job_id", type=hexadecimal)
    for command in ("schedule-status", "cancel-schedule"):
        subparsers.add_parser(command).add_argument("schedule_id", type=hexadecimal)
    args = parser.parse_args(argv)
    from mysqlsh import globals as shell_globals

    try:
        session = shell_globals.shell.get_session()
        if session is None or not session.is_open():
            raise ValueError("connect MySQL Shell to the server before running this command")
        tls = session.run_sql("SHOW SESSION STATUS LIKE 'Ssl_cipher'").fetch_one()
        if tls is None or not tls[1] or session.run_sql("SELECT @@autocommit").fetch_one()[0] != 1:
            raise ValueError("use TLS and an autocommit connection")
        result = {"action": args.command}
        if args.command == "install":
            install(session)
        elif args.command in ("submit", "schedule"):
            if not 1 <= args.count <= 1_000_000_000 or not 1 <= args.timeout_ms <= 60_000:
                raise ValueError("iteration count or timeout is out of range")
            if args.command == "submit":
                result["job_id"] = submit(session, args.request_token, args.count, args.timeout_ms)
            else:
                if not 1 <= args.every_seconds <= 86_400:
                    raise ValueError("schedule interval is out of range")
                result["schedule_id"] = create_schedule(session, args)
        elif args.command == "work-once":
            result["job_id"] = work_once(session)
        elif args.command == "serve":
            while True:
                job_id = work_once(session)
                if job_id is None:
                    time.sleep(1)
                else:
                    print("SDP_JOB " + json.dumps({"action": "worked", "job_id": job_id}), flush=True)
        elif args.command == "status":
            result.update(status(session, args.job_id))
        elif args.command == "cancel":
            result.update(cancel(session, args.job_id))
        else:
            if args.command == "cancel-schedule":
                session.run_sql(f"UPDATE {SCHEDULES} SET active = FALSE WHERE owner = CURRENT_USER() AND schedule_id = ?",
                                [args.schedule_id])
            result.update(schedule_status(session, args.schedule_id))
        print("SDP_JOB " + json.dumps(result, sort_keys=True))
    except KeyboardInterrupt:
        return 0
    except ValueError as error:
        print(f"jobs: {error}", file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
