"""Measure one durable Server workload in a disposable Windows loopback fixture."""

import argparse
import ctypes
from ctypes import wintypes
import hashlib
import json
import math
from pathlib import Path
import platform
import secrets
import ssl
import statistics
import tempfile
import time

from service_smoke import certificates, execute, listening, require, save_config
from sql_rpc import BIGINT, TEXT, client, i64, text, token, write


ROWS = 1200
PAYLOAD = "x" * 480
READS = 160
WRITES = 72
RESTARTS = 12
SELECT = "SELECT id,payload FROM resource_rows WHERE id=:key"


class MemoryCounters(ctypes.Structure):
    _fields_ = [("size", wintypes.DWORD), ("page_faults", wintypes.DWORD)] + [
        (name, ctypes.c_size_t) for name in (
            "peak_working_set", "working_set", "peak_paged_pool", "paged_pool",
            "peak_nonpaged_pool", "nonpaged_pool", "pagefile", "peak_pagefile", "private_bytes",
        )
    ]


def memory_for(pid):
    kernel = ctypes.WinDLL("kernel32", use_last_error=True)
    psapi = ctypes.WinDLL("psapi", use_last_error=True)
    kernel.OpenProcess.argtypes = [wintypes.DWORD, wintypes.BOOL, wintypes.DWORD]
    kernel.OpenProcess.restype = wintypes.HANDLE
    kernel.CloseHandle.argtypes = [wintypes.HANDLE]
    kernel.CloseHandle.restype = wintypes.BOOL
    psapi.GetProcessMemoryInfo.argtypes = [wintypes.HANDLE, ctypes.POINTER(MemoryCounters), wintypes.DWORD]
    psapi.GetProcessMemoryInfo.restype = wintypes.BOOL
    process = kernel.OpenProcess(0x1000, False, pid)
    if not process:
        raise ctypes.WinError(ctypes.get_last_error())
    try:
        counters = MemoryCounters()
        counters.size = ctypes.sizeof(counters)
        if not psapi.GetProcessMemoryInfo(process, ctypes.byref(counters), counters.size):
            raise ctypes.WinError(ctypes.get_last_error())
        return {name: getattr(counters, name) for name in ("working_set", "peak_working_set", "private_bytes")}
    finally:
        kernel.CloseHandle(process)


def timed_call(samples, action):
    start = time.perf_counter_ns()
    result = action()
    samples.append(time.perf_counter_ns() - start)
    return result


def summarize(samples):
    ordered = sorted(samples)
    return {
        "samples": len(samples),
        "p50_ms": statistics.median(samples) / 1_000_000,
        "p95_ms": ordered[math.ceil(len(samples) * 0.95) - 1] / 1_000_000,
        "p99_ms": ordered[math.ceil(len(samples) * 0.99) - 1] / 1_000_000,
        "completed_operations_per_second": len(samples) * 1_000_000_000 / sum(samples),
    }


def footprint(directory):
    return {item.relative_to(directory).as_posix(): item.stat().st_size
            for item in sorted(directory.rglob("*")) if item.is_file()}


def query_one(session, key):
    rows, _ = session.query(SELECT, types={"key": BIGINT}, values={"key": i64(key)})
    require(rows == [[i64(key), text(PAYLOAD)]], "benchmark read changed exact payload")


def seed(session):
    first = write(session, "CREATE TABLE resource_rows (id BIGINT PRIMARY KEY,payload TEXT NOT NULL)", 0)
    for base in range(0, ROWS, 100):
        expressions = ",".join(f"({key},:payload)" for key in range(base, base + 100))
        last = write(session, "INSERT INTO resource_rows VALUES " + expressions, 100,
                     types={"payload": TEXT}, values={"payload": text(PAYLOAD)})
    return first["lsn"], last["lsn"]


def history_rows(session, operation, start, end, page_size):
    offset = 0
    rows = 0
    for page_count in range(1, ROWS + WRITES + 3):
        page = session.call(operation, target="resource_rows", **{"from": start, "to": end},
                            offset=offset, limit=page_size)
        entries = page["entries"]
        require(len(entries) <= page_size, "History ignored the page limit")
        rows += sum(entry["kind"] == "row" for entry in entries)
        following = page["next_offset"]
        if following is None:
            return rows, page_count
        require(following > offset and entries, "History pagination did not advance")
        offset = following
    raise RuntimeError("History pagination exceeded the fixture size")


def live_run(session, pid, root, samples):
    first_lsn, baseline_lsn = seed(session)
    query_one(session, 7)
    history_before, _ = history_rows(session, "history_log", first_lsn, baseline_lsn, 64)
    baseline = footprint(root / "data")
    memory_before = memory_for(pid)
    for read in range(READS):
        timed_call(samples["warm_roundtrip_read"], lambda: query_one(session, read * 7 % ROWS))
    with session.prepared("INSERT INTO resource_rows VALUES (:key,:payload)",
                          {"key": BIGINT, "payload": TEXT}) as statement:
        for key in range(ROWS, ROWS + WRITES):
            request_token = token()
            reply = timed_call(samples["durable_autocommit"], lambda: session.call(
                "execute", statement_id=statement, request_token=request_token,
                parameters={"key": i64(key), "payload": text(PAYLOAD)},
            ))
            require(reply["affected_rows"] == 1, "durable insert affected an unexpected row count")
            durable = session.call("tx_outcome", request_token=request_token)
            require(durable == {"status": "committed", "lsn": reply["lsn"]}, "durable ledger mismatch")
    rows, batches = session.query("SELECT id,payload FROM resource_rows", row_limit=193, batch_rows=17)
    require(len(rows) == len({row[0]["v"] for row in rows}) == 193 and batches > 1,
            "large result did not honor row and batch limits")
    require(all(row[1] == text(PAYLOAD) for row in rows), "bounded result lost value bytes")
    result = session.call("history_verify")
    require(result["verified"], "History verification rejected the workload")
    history_after, history_pages = history_rows(session, "history_log", first_lsn, reply["lsn"], 64)
    diff_rows, diff_pages = history_rows(session, "history_diff", baseline_lsn, reply["lsn"], 17)
    require(history_before == ROWS and history_after == ROWS + WRITES and diff_rows == WRITES,
            "History growth or diff did not match the committed workload")
    memory_after = memory_for(pid)
    after = footprint(root / "data")
    return {"rows": len(rows), "batches": batches, "baseline_files": baseline, "after_files": after,
            "history_rows_before": history_before, "history_rows_after": history_after,
            "history_pages": history_pages, "history_diff_rows": diff_rows, "history_diff_pages": diff_pages,
            "history_last_lsn": reply["lsn"],
            "memory_before": memory_before, "memory_after": memory_after}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    for name in ("server", "openssl", "scratch"):
        parser.add_argument("--" + name, type=Path, required=True)
    args = parser.parse_args()
    args.scratch.mkdir(parents=True, exist_ok=True)
    samples = {"warm_roundtrip_read": [], "durable_autocommit": [], "restarted_first_read": []}
    with tempfile.TemporaryDirectory(prefix="bench-", dir=args.scratch.resolve()) as temporary:
        root = Path(temporary)
        for name in ("data", "log", "temp"):
            (root / name).mkdir()
        certificates(args.openssl, root)
        config = save_config(root, timeout_ms=12000, max_connections=4)
        initialized = execute(args.server, config, "--initialize-storage")
        require(initialized.returncode == 0,
                f"bench storage failed ({initialized.returncode}): {initialized.stderr.decode(errors='replace').strip()}")
        password = secrets.token_urlsafe(26)
        require(execute(args.server, config, "--bootstrap-admin", "local-admin",
                        stdin=((password + "\n") * 2).encode("ascii")).returncode == 0, "bench bootstrap failed")
        trust = ssl.create_default_context(cafile=str(root / "ca.pem"))
        with listening(args.server, config) as (process, port), client(port, trust, password) as session:
            observations = live_run(session, process.pid, root, samples)
        for restart in range(RESTARTS):
            start = time.perf_counter_ns()
            with listening(args.server, config) as (_, port), client(port, trust, password) as session:
                query_one(session, ROWS + WRITES - 1)
                samples["restarted_first_read"].append(time.perf_counter_ns() - start)
                if restart == RESTARTS - 1:
                    require(session.call("history_verify")["verified"], "History failed after restart")
    with args.server.open("rb") as image:
        digest = hashlib.file_digest(image, "sha256").hexdigest()
    data = {
        "platform": platform.platform(), "cpu_architecture": platform.machine(), "python": platform.python_version(),
        "server_sha256": digest, "seed_rows": ROWS, "payload_bytes": len(PAYLOAD), "clients": 1,
        "durability": "product-default durable WAL autocommit; tx_outcome checks excluded from latency",
        "restart": "fresh process after terminating the owned fixture; OS file cache uncontrolled",
        "warm_read": "whole prepared RPC sequence; repeated after one validated warmup",
        "sample_units": "ns", "raw_samples": samples, "metrics": {key: summarize(value) for key, value in samples.items()},
        "observations": observations,
    }
    output = args.scratch / "resource-benchmark.json"
    output.write_text(json.dumps(data, indent=2) + "\n", encoding="utf-8")
    print(json.dumps(data["metrics"], indent=2))
    print(f"History {observations['history_rows_after']} row events and bounded result {observations['rows']} rows")


if __name__ == "__main__":
    main()
