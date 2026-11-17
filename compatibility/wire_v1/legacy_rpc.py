"""Opt-in legacy History and compute RPCs backed by the MySQL compatibility modules."""

from dataclasses import dataclass
import importlib.util
import json
from pathlib import Path
import re
import sys
from types import SimpleNamespace

from mysql.connector.errors import Error

from ledger import request_token
from legacy_mutations import mutate
from sql import Fault, integer_field, require


SQL_NAME = re.compile(r"[A-Za-z_][A-Za-z0-9_]{0,63}")
TOKEN = re.compile(r"[0-9a-f]{32}")
U64 = (1 << 64) - 1
U32 = (1 << 32) - 1
HISTORY = {"history_log", "history_read", "history_diff", "history_restore", "history_revert"}
JOBS = {"job_capabilities", "job_submit", "job_status", "job_watch", "job_cancel", "job_schedule_create",
        "job_schedule_status", "job_schedule_cancel"}
OPERATIONS = HISTORY | JOBS


def component(name, relative):
    path = Path(__file__).resolve().parents[1] / relative
    spec = importlib.util.spec_from_file_location(name, path)
    module = importlib.util.module_from_spec(spec)
    sys.modules[name] = module
    spec.loader.exec_module(module)
    return module


compute = component("sdp_wire_compute_jobs", "jobs/compute_jobs.py")


def accounts(values):
    if not isinstance(values, list) or any(not isinstance(value, str) or "@" not in value
                                           or not value or len(value) > 288 or any(ord(c) < 32 for c in value)
                                           for value in values):
        raise ValueError("expected effective MySQL account names (USER@HOST)")
    return frozenset(values)


@dataclass(frozen=True)
class HistoryTarget:
    alias: str
    table: str
    journal: str
    object_id: str | None
    readers: frozenset
    writers: frozenset


@dataclass(frozen=True)
class Config:
    jobs: frozenset
    history: dict


def load_config(path):
    data = json.loads(path.read_text(encoding="utf-8"))
    if not isinstance(data, dict) or set(data) - {"jobs", "history"}:
        raise ValueError("unknown legacy RPC configuration field")
    history = {}
    targets = data.get("history", {})
    if not isinstance(targets, dict):
        raise ValueError("history must be an object keyed by legacy table name")
    for alias, entry in targets.items():
        if (not SQL_NAME.fullmatch(alias) or not isinstance(entry, dict)
                or set(entry) - {"table", "journal", "object_id", "readers", "writers"}
                or any(not isinstance(entry.get(key), str) or not SQL_NAME.fullmatch(entry[key])
                       for key in ("table", "journal"))):
            raise ValueError("invalid legacy History target")
        readers, writers = (accounts(entry.get(key, [])) for key in ("readers", "writers"))
        object_id = entry.get("object_id")
        if object_id is not None and (not isinstance(object_id, str)
                                      or not re.fullmatch(r"[1-9][0-9]{0,19}", object_id)
                                      or int(object_id) > U64):
            raise ValueError("invalid legacy History object ID")
        if writers and object_id is None:
            raise ValueError("History restore requires a stable compatibility object ID")
        if alias.casefold() in history or not writers <= readers:
            raise ValueError("duplicate History alias or a writer without read access")
        if object_id is not None and any(item.object_id == object_id for item in history.values()):
            raise ValueError("duplicate History object ID")
        if any(item.journal.casefold() == entry["journal"].casefold() for item in history.values()):
            raise ValueError("each History target must have a separate journal")
        history[alias.casefold()] = HistoryTarget(alias, entry["table"], entry["journal"], object_id, readers, writers)
    return Config(accounts(data.get("jobs", [])), history)


def fields(body, allowed):
    require(not body.keys() - set(allowed))


def identifier(body, key):
    value = body.get(key)
    require(isinstance(value, str) and TOKEN.fullmatch(value))
    return value


def number(body, key, required=False):
    value = body.get(key)
    if key not in body and not required:
        return None
    if isinstance(value, str):
        require(re.fullmatch(r"(?:0|[1-9][0-9]{0,19})", value) is not None)
        value = int(value)
    require(type(value) is int and 0 <= value <= U64)
    return value


@dataclass
class Rows:
    values: list
    count: int

    def fetch_one(self):
        return self.values[0] if self.values else None

    def fetch_all(self):
        return self.values

    def get_affected_items_count(self):
        return self.count


class Backend:
    def __init__(self, db):
        self.db = db

    def run_sql(self, statement, params=()):
        # All SQL here comes from audited constants or identifiers loaded by load_config.
        with self.db.cursor(buffered=True) as cursor:
            cursor.execute(statement.replace("?", "%s"), tuple(params))
            result = Rows(cursor.fetchall() if cursor.with_rows else [], cursor.rowcount)
            while cursor.nextset():
                require(not cursor.with_rows or not cursor.fetchall(), 1015)
        return result


def jobs_status(backend, job_id):
    try:
        state, count, checksum, reason, cancel, attempt, events = compute.status_record(backend, job_id)
    except ValueError:
        raise Fault(1006) from None
    if state == "interrupted":
        state, reason = "failed", "interrupted"
    return {"job_id": job_id, "state": state, "cancel_requested": cancel, "events": events,
            "attempt": attempt, "progress": None, "worker_pid": None,
            "result": None if checksum is None else {"count": count, "checksum": checksum}, "reason": reason}


def schedule_state(backend, schedule_id):
    row = backend.run_sql(f"""SELECT active,interval_seconds,
        TIMESTAMPDIFF(SECOND,'1970-01-01 00:00:00',next_due),
        (SELECT j.job_id FROM {compute.OCCURRENCES} AS o JOIN {compute.QUEUE} AS j USING(job_id)
         WHERE o.schedule_id=s.schedule_id ORDER BY j.sequence DESC LIMIT 1)
        FROM {compute.SCHEDULES} AS s WHERE owner=CURRENT_USER() AND schedule_id=?""", [schedule_id]).fetch_one()
    require(row is not None, 1006)
    enabled, every, due, last = row
    return {"schedule_id": schedule_id, "enabled": bool(enabled), "next_utc": int(due), "last_job_id": last,
            "rule": {"every_seconds": every, "misfire": "run_once"}, "retries": 0, "backoff_seconds": 1}


def submit_job(session, backend, operation, body):
    scheduled = operation == "job_schedule_create"
    allowed = {"kind", "request_token", "count", "timeout_ms"}
    fields(body, allowed | ({"rule", "misfire", "retries", "backoff_seconds"} if scheduled else set()))
    require(body.get("kind") == "compute", 1004)
    token = request_token(body)
    count = integer_field(body, "count", None, 1_000_000_000)
    timeout = integer_field(body, "timeout_ms", 20_000, 60_000)
    require(timeout >= 100)
    every = None
    if scheduled:
        rule = body.get("rule")
        require(isinstance(rule, dict))
        fields(rule, {"every_seconds", "misfire"})
        require(not ("misfire" in body and "misfire" in rule))
        require(body.get("misfire", rule.get("misfire")) == "run_once", 1004)
        require(type(body.get("retries", 0)) is int and body.get("retries", 0) == 0, 1004)
        require(type(body.get("backoff_seconds", 1)) is int and body.get("backoff_seconds", 1) == 1, 1004)
        every = integer_field(rule, "every_seconds", None, 86_400)

    queue, key = (compute.SCHEDULES, "schedule_id") if scheduled else (compute.QUEUE, "job_id")
    status = schedule_state if scheduled else jobs_status

    def existing(value):
        return backend.run_sql(f"SELECT {key} FROM {queue} WHERE owner=CURRENT_USER() AND request_token=?",
                               [value]).fetch_one()

    def replay(value):
        row = existing(value)
        return status(backend, row[0]) if row is not None else None

    def submit(value, *, allow_replay):
        try:
            if scheduled:
                args = SimpleNamespace(request_token=value, count=count, timeout_ms=timeout, every_seconds=every)
                return schedule_state(backend, compute.create_schedule(backend, args, allow_replay=allow_replay))
            return jobs_status(backend, compute.submit(backend, value, count, timeout, allow_replay=allow_replay))
        except ValueError:
            raise Fault(1013) from None
        except Error as error:
            if error.errno == 1062:
                raise Fault(1013) from None
            raise

    def apply(value):
        # Earlier native submissions must not acquire a new ledger receipt retroactively.
        return submit(value, allow_replay=False)

    if session.db.in_transaction:
        if session.receipts is not None:
            require(session.receipts.scoped_replay, 1004)
            require(session.receipts.command("occupied", token) == 0, 1013)
        return submit(token, allow_replay=True)
    return mutate(session, body, apply, scope=(operation, count, timeout, every), replay=replay)


def jobs(session, backend, operation, body):
    if operation == "job_capabilities":
        fields(body, ())
        return {"kinds": ["compute"], "schedule_kinds": ["compute"], "schedule_types": ["interval"],
                "maximum_workers": 1, "hard_memory_limit": False, "wall_time_limit": True,
                "checkpoint_resume": False}
    if operation in {"job_submit", "job_schedule_create"}:
        return submit_job(session, backend, operation, body)
    if operation.startswith("job_schedule_"):
        fields(body, {"schedule_id"})
        schedule_id = identifier(body, "schedule_id")
        if operation == "job_schedule_cancel":
            backend.run_sql(f"UPDATE {compute.SCHEDULES} SET active=FALSE WHERE owner=CURRENT_USER() AND schedule_id=?",
                            [schedule_id])
        return schedule_state(backend, schedule_id)
    fields(body, {"job_id"})
    job_id = identifier(body, "job_id")
    if operation == "job_cancel":
        try:
            compute.cancel(backend, job_id)
        except ValueError:
            raise Fault(1006) from None
    return jobs_status(backend, job_id)


def select_target(session, body, config):
    name = body.get("target")
    require(isinstance(name, str))
    parts = name.split(".")
    require(len(parts) <= 3 and all(SQL_NAME.fullmatch(part) for part in parts))
    target = config.history.get(parts[-1].casefold())
    require(target is not None and parts[:-1] in ([], ["public"], ["main", "public"], [session.config.schema])
            and session.identity[1] in target.readers, 1006)
    return target


def history_page(session, backend, operation, body, target):
    journal = f"`{target.journal}`"
    name = ["main", "public", target.alias]
    backend.run_sql(f"SELECT * FROM `{session.config.schema}`.`{target.table}` LIMIT 0")
    offset = body.get("offset", 0)
    require(type(offset) is int and 0 <= offset <= U32)
    limit = integer_field(body, "limit", 32, 256)
    row = number(body, "row_id")
    head = backend.run_sql(f"SELECT head FROM {journal}.revision_counter WHERE singleton=1").fetch_one()[0]
    common = {"target", "row_id", "offset", "limit"}
    if operation == "history_read":
        fields(body, common | {"revision"})
        revision = number(body, "revision", True)
        args = [revision, row]
    else:
        fields(body, common | {"from", "to", "since_utc", "until_utc"})
        require(not ({"since_utc", "until_utc"} & body.keys()), 1004)
        diff = operation == "history_diff"
        first, last = (number(body, key, diff) for key in ("from", "to"))
        if last is None:
            last = head
        args = [first, last, row] if diff else [first, last, row, None, None]

    def page(start, size):
        marks = ",".join("?" for _ in range(len(args) + 2))
        try:
            return backend.run_sql(f"CALL {journal}.{operation}({marks})", [*args, start, size]).fetch_all()
        except Error as error:
            if error.sqlstate == "22023" or error.errno == 1644:
                raise Fault(1012) from None
            raise

    records = page(offset, limit)
    next_offset = offset + limit if len(records) == limit and offset <= U32 - limit and page(offset + limit, 1) else None

    def image(value):
        return None if value is None else json.loads(value)

    if operation == "history_read":
        return {"revision": str(revision), "name": name, "object_id": target.object_id, "object": {"kind": "table"},
                "rows": [{**image(value), "row_id": str(row_id)} for row_id, value in records],
                "next_offset": next_offset}
    if operation == "history_diff":
        return {"from_revision": str(first), "to_revision": str(last), "earliest_revision": "0",
                "entries": [{"kind": "row", "name": name, "row_id": str(row_id), "before": image(before),
                             "after": image(after)} for row_id, before, after in records], "next_offset": next_offset}
    return {"current_revision": str(head), "earliest_revision": "0", "next_offset": next_offset,
            "entries": [{"revision": str(rev), "parent": str(parent), "row_id": str(row_id), "actor": actor,
                         "kind": "row", "name": name, "transaction": None, "object_id": target.object_id, "commit_utc": None,
                         "observed_utc": observed, "before": image(before), "after": image(after)}
                        for rev, parent, row_id, actor, observed, before, after in records]}


def history_change(session, backend, operation, body, config):
    if "plan_id" not in body:
        fields(body, {"target", "revision", "row_id", "field"})
        target = select_target(session, body, config)
        require(session.identity[1] in target.writers, 1006)
        args = [number(body, "revision", True), number(body, "row_id", True)]
        if operation == "history_revert":
            field = body.get("field")
            require(isinstance(field, str) and SQL_NAME.fullmatch(field))
            args.append(field)
        else:
            require("field" not in body, 1004)
        require(session.receipts is not None, 1004)
        reply = history_function(backend, target, operation, "preview", args)
        reply.update(object_id=target.object_id, name=["main", "public", target.alias],
                     mode="revert" if operation == "history_revert" else "restore",
                     dependencies=[], effects={"inserted": 0, "updated": 1, "deleted": 0, "catalog": 0})
        return reply

    fields(body, {"plan_id", "request_token"})
    plan_id = identifier(body, "plan_id")
    actor = session.identity[1]
    targets = [target for target in config.history.values() if actor in target.writers]
    require(targets, 1006)
    require(session.receipts is not None, 1004)
    change_field = operation == "history_revert"
    for target in targets:
        plans = f"`{target.journal}`.restore_plans"
        found = backend.run_sql(f"""SELECT JSON_CONTAINS_PATH(image,'one','$.revert_field')
            FROM {plans} WHERE plan_id=UNHEX(?) AND owner=CURRENT_USER()""", [plan_id]).fetch_one()
        if found is not None and bool(found[0]) == change_field:
            break
    else:
        raise Fault(1013)

    backend.run_sql(f"SELECT * FROM `{session.config.schema}`.`{target.table}` LIMIT 0")

    def apply(token):
        reply = history_function(backend, target, operation, "apply", [plan_id, token])
        require(not reply["replayed"], 1013)
        return {**reply, "status": "committed"}

    def replay(token):
        row = backend.run_sql(f"""SELECT applied_revision FROM {plans}
            WHERE owner=CURRENT_USER() AND plan_id=UNHEX(?) AND request_token=UNHEX(?)""", [plan_id, token]).fetch_one()
        if row is None or row[0] is None:
            return None
        return {"status": "committed", "revision": str(row[0]), "replayed": True}

    return mutate(session, body, apply, scope=(operation, target.journal, plan_id), replay=replay)


def history_function(backend, target, operation, step, args):
    marks = ",".join("?" for _ in args)
    try:
        value = backend.run_sql(f"SELECT `{target.journal}`.{operation}_{step}({marks})", args).fetch_one()[0]
    except Error as error:
        if error.errno in {1644, 1305}:
            raise Fault(1013 if error.errno == 1644 else 1004) from None
        raise
    return json.loads(value)


def dispatch(session, operation, body, config):
    require(config is not None, 1004)
    require(session.result is None, 1013)
    backend = Backend(session.db)
    if operation in JOBS:
        require(session.identity[1] in config.jobs, 1006)
        return jobs(session, backend, operation, body)
    require(not session.db.in_transaction, 1013)
    if operation in {"history_restore", "history_revert"}:
        return history_change(session, backend, operation, body, config)
    return history_page(session, backend, operation, body, select_target(session, body, config))
