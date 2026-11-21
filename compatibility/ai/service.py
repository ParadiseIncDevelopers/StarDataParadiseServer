"""Supervise a dedicated worker session; reload trusted bindings between jobs."""
from __future__ import annotations

from datetime import datetime, timezone
import json
import os
from pathlib import Path
import time
import threading

from .worker import RunWorker


def atomic_json(path, value):
    path = Path(path)
    temporary = path.with_name(path.name + '.tmp-' + str(os.getpid()))
    try:
        temporary.write_text(json.dumps(value, ensure_ascii=False), encoding='utf-8')
        temporary.replace(path)
    finally:
        temporary.unlink(missing_ok=True)


def configuration(path):
    path = Path(path)
    if path.stat().st_size > 1024 * 1024:
        raise ValueError('Worker configuration exceeds limit')
    value = json.loads(path.read_text(encoding='utf-8-sig'))
    if not isinstance(value, dict) or not isinstance(value.get('credentials', {}), dict) or not isinstance(value.get('local_runtime', {}), dict):
        raise ValueError('Invalid trusted configuration')
    return value


def serve(connect, config_path, status_path, stop_path, *, sleep=time.sleep, worker_type=RunWorker, max_cycles=None):
    """Never retry an interrupted job: the catalog lease resolves its outcome.

    Connection failures recreate only the dedicated SQL session. Binding reload
    occurs before the next claim, so no in-flight call changes credentials.
    Heartbeats deliberately omit exception messages, SQL, keys and responses.
    """
    session, worker, current = None, None, None
    sequence, last_status, completed = 0, 0, None
    heartbeat_stop = threading.Event()
    def heartbeat():
        while not heartbeat_stop.wait(3):
            try: os.utime(status_path, None)
            except OSError: pass
    heartbeat_thread = threading.Thread(target=heartbeat, daemon=True)
    heartbeat_thread.start()
    def status(state, error=None):
        atomic_json(status_path, dict(state=state, error=error, pid=os.getpid(),
            updated_at=datetime.now(timezone.utc).isoformat(), completed=completed))
    try:
        while not Path(stop_path).exists() and (max_cycles is None or sequence < max_cycles):
            sequence += 1
            try:
                updated = configuration(config_path)
                if session is None:
                    session = connect()
                    tls = session.run_sql("SHOW SESSION STATUS LIKE 'Ssl_cipher'").fetch_one()
                    if tls is None or not tls[1]:
                        raise ValueError('Worker requires verified TLS')
                if worker is None or current != updated:
                    worker = worker_type(session, updated)
                    current = updated
                if time.monotonic() - last_status >= 5:
                    status('ready')
                    last_status = time.monotonic()
                result = worker.work_once()
                if result is not None:
                    completed = {key: result[key] for key in ('run_id', 'job_id', 'model_id', 'state') if key in result}
                    status('ready')
                else:
                    sleep(.5)
            except Exception as error:
                # A partially completed RPC is never repeated here. Reconnect
                # and let the durable queue/lease decide the next eligible job.
                status('error', type(error).__name__)
                if session is not None:
                    try: session.close()
                    except Exception: pass
                session, worker, current = None, None, None
                sleep(3)
    finally:
        heartbeat_stop.set()
        heartbeat_thread.join(timeout=4)
        if session is not None:
            try: session.close()
            except Exception: pass
        status('stopped')
