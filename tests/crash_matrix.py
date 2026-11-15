"""Kill only authored test processes and verify durable recovery on isolated data copies."""

import argparse
import hashlib
import json
from pathlib import Path
import queue
import re
import secrets
import shutil
import subprocess
import tempfile
import threading
import zlib


def require(condition, message):
    if not condition:
        raise RuntimeError(message)


def execute(command, *, expect=0, **options):
    result = subprocess.run(command, capture_output=True, text=True, encoding="utf-8",
                            errors="replace", timeout=20, **options)
    require(result.returncode == expect, f"Unexpected exit {result.returncode}: {result.stderr}")
    return result.stdout


def ready(process):
    output = queue.Queue(maxsize=1)
    threading.Thread(target=lambda: output.put(process.stdout.readline().strip()), daemon=True).start()
    try:
        return output.get(timeout=10)
    except queue.Empty as error:
        raise RuntimeError("test child did not reach the intended boundary") from error


def crash(command):
    with subprocess.Popen(command, stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True,
                          creationflags=subprocess.CREATE_NO_WINDOW) as child:
        try:
            require(ready(child) == "CRASH_READY", "test child exited ahead of its crash boundary")
        finally:
            if child.poll() is None:
                child.kill()
            child.communicate(timeout=6)
        require(child.returncode != 0, "crash fixture completed successfully")


def state(path):
    return {entry.name: hashlib.sha256(entry.read_bytes()).hexdigest()
            for entry in path.iterdir() if entry.is_file() and entry.name.startswith("store.sdp")}


def fixture(root, name, driver):
    case = root / name
    case.mkdir()
    path = case / "store.sdp"
    execute([driver, "init", path])
    return path


def replay(driver, path, expected):
    first = execute([driver, "verify", path, str(expected)]).strip()
    values = re.fullmatch(r"LSN (\d+) pages (\d+) tail ([01]) recovery_ms ([0-9.eE+-]+)", first)
    require(values is not None, "missing measured recovery outcome")
    after = state(path.parent)
    second = execute([driver, "verify", path, str(expected)])
    require(f"LSN {expected} pages 0 tail 0" in second and state(path.parent) == after,
            "repeated replay changed durable bytes or journal")
    return dict(lsn=expected, pages_rewritten=int(values[2]), truncated_tail=bool(int(values[3])),
                recovery_ms=float(values[4]), repeat_identical=True)


def change_bytes(path, location, value, checksum=False):
    with path.open("r+b") as file:
        file.seek(location)
        file.write(value)
        if checksum:
            start = (location // 4096) * 4096
            file.seek(start)
            page = file.read(4092)
            file.write(zlib.crc32(page).to_bytes(4, "little"))


def contradict_journal(wal):
    body = bytearray(wal.read_bytes())
    start = 64
    first = int.from_bytes(body[start + 4:start + 8], "little")
    start += first + 24
    size = int.from_bytes(body[start + 4:start + 8], "little")
    pages = int.from_bytes(body[start + 16:start + 20], "little")
    before = start + 32 + pages * (4 + 4096) + 20
    # Change an integer byte in the before-image while leaving the row and frame structurally valid.
    body[before + 10] ^= 1
    commit = start + size
    body[commit + 16:commit + 20] = zlib.crc32(body[start:commit]).to_bytes(4, "little")
    body[commit + 20:commit + 24] = zlib.crc32(body[commit:commit + 20]).to_bytes(4, "little")
    wal.write_bytes(body)


def start_recovery(args, path, label):
    root = path.parent
    for name in ("logs", "tmp"):
        (root / name).mkdir()
    config_template = root / "openssl.cnf"
    config_template.write_text("[req]\ndistinguished_name=dn\n[dn]\n", encoding="ascii")
    execute([args.openssl, "req", "-config", config_template, "-newkey", "rsa:2048", "-nodes", "-x509",
             "-keyout", root / "key.pem", "-out", root / "server.pem", "-subj", "/CN=localhost", "-days", "2"])
    config = root / "server.json"
    config.write_text(json.dumps(dict(data_dir=".", log_dir="logs", temp_dir="tmp",
                                      tls_certificate="server.pem", tls_private_key="key.pem", port=0)), encoding="utf-8")
    server = [args.server, "--config", config]
    password = secrets.token_urlsafe(24)
    execute([*server, "--bootstrap-admin", "local-admin"], input=(password + "\n") * 2)
    with subprocess.Popen(server, stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True,
                          creationflags=subprocess.CREATE_NO_WINDOW) as instance:
        try:
            require(ready(instance).startswith("LISTENING 127.0.0.1 "), "Server did not recover before listening")
        finally:
            if instance.poll() is None:
                instance.terminate()
            instance.communicate(timeout=6)
    require("Storage OK: 2 rows" in execute([*server, "--check-storage"]), "startup did not validate repaired storage")
    require("Recovery OK at LSN 2, 0 pages rewritten" in execute([*server, "--recover-storage"]),
            "explicit Server recovery was not idempotent")
    return label


def matrix(args, root):
    results = {}
    driver = args.driver
    for phase in ("body", "durable", "page", "flush", "publish", "checkpoint-write", "checkpoint-publish"):
        path = fixture(root, phase, driver)
        checkpoint = phase.startswith("checkpoint-")
        if checkpoint:
            execute([driver, "commit", path])
        crash([driver, "checkpoint" if checkpoint else "commit", path, phase])
        results[phase] = replay(driver, path, 1 if phase == "body" else 2)
        if phase == "body":
            require(results[phase]["truncated_tail"], "uncommitted tail was not recovered")
        if checkpoint:
            require(len(list(path.parent.glob("*.wal.*"))) == 2, "checkpoint removed retained history")
            execute([driver, "resume", path])
            results[phase + "-next-commit"] = replay(driver, path, 3)

    path = fixture(root, "torn-data", driver)
    execute([driver, "commit", path])
    original = state(path.parent)
    change_bytes(path, 4096 * 2 + 125, b"\x01\x02\x03")
    change_bytes(path, 7, b"\x01")
    corrupted = state(path.parent)
    execute([driver, "read-only", path], expect=1)
    require(state(path.parent) == corrupted, "read-only scan wrote data")
    results["torn-header-and-page"] = replay(driver, path, 2)
    require(results["torn-header-and-page"]["pages_rewritten"] >= 2 and state(path.parent) == original,
            "torn data did not recover its committed image")

    path = fixture(root, "lost-commit-marker", driver)
    execute([driver, "commit", path])
    wal = next(path.parent.glob("*.wal.*"))
    with wal.open("r+b") as file:
        file.truncate(wal.stat().st_size - 16)
    original = state(path.parent)
    execute([driver, "verify", path, "2"], expect=1)
    require(state(path.parent) == original, "lost commit marker triggered a reset")
    results["lost-marker"] = "rejected, bytes preserved"

    path = fixture(root, "missing-wal-set", driver)
    execute([driver, "commit", path])
    segment = next(path.parent.glob("*.wal.*"))
    segment.rename(path.parent / "withheld-fixture.wal")
    original = state(path.parent)
    execute([driver, "verify", path, "2"], expect=1)
    require(state(path.parent) == original, "missing WAL set triggered reset or inference")
    results["missing-wal-set"] = "checkpoint detected missing history, bytes preserved"

    path = fixture(root, "wal-corruption", driver)
    execute([driver, "commit", path])
    wal = next(path.parent.glob("*.wal.*"))
    change_bytes(wal, wal.stat().st_size - 3, b"\x01\x02\x03")
    original = state(path.parent)
    execute([driver, "verify", path, "2"], expect=1)
    require(state(path.parent) == original, "corrupt WAL was modified")
    results["corrupt-wal"] = "rejected, bytes preserved"

    path = fixture(root, "contradictory-journal", driver)
    execute([driver, "commit", path])
    contradict_journal(next(path.parent.glob("*.wal.*")))
    original = state(path.parent)
    rejected = subprocess.run([driver, "verify", path, "2"], capture_output=True, text=True, timeout=15)
    require(rejected.returncode == 1 and "journal chain is inconsistent" in rejected.stderr
            and state(path.parent) == original, "contradictory journal was accepted or rewritten")
    results["contradictory-journal"] = "rejected despite valid frame checksums"

    path = fixture(root, "future-storage-format", driver)
    change_bytes(path, 8, (99).to_bytes(4, "little"), checksum=True)
    original = state(path.parent)
    execute([driver, "verify", path, "1"], expect=1)
    require(state(path.parent) == original, "unknown storage version was overwritten")
    results["future-format"] = "rejected, bytes preserved"

    path = fixture(root, "index-and-checkpoint", driver)
    execute([driver, "commit", path])
    execute([driver, "index", path])
    execute([driver, "checkpoint", path])
    results["retention-and-index"] = replay(driver, path, 2)

    path = fixture(root, "normal-server-start", driver)
    crash([driver, "commit", path, "durable"])
    results["server-start"] = start_recovery(args, path, "startup and maintenance commands passed")
    results["server-start-durable"] = replay(driver, path, 2)
    return results


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    for key in ("driver", "server", "openssl", "scratch"):
        parser.add_argument("--" + key, required=True, type=Path)
    parser.add_argument("--legacy-storage", type=Path)
    parser.add_argument("--legacy-wal", type=Path)
    args = parser.parse_args()
    args.scratch.mkdir(parents=True, exist_ok=True)
    with tempfile.TemporaryDirectory(prefix="crash-", dir=args.scratch.resolve()) as temp:
        root = Path(temp)
        results = matrix(args, root)
        if args.legacy_storage:
            old = root / "disk08" / "store.sdp"
            old.parent.mkdir()
            shutil.copy2(args.legacy_storage, old)
            original = state(old.parent)
            execute([args.driver, "legacy", old])
            require(original == state(old.parent), "read-only legacy storage was changed")
            results["stage08-storage"] = "legacy fixture read without writes"
        if args.legacy_wal:
            target = root / "wal09"
            target.mkdir()
            for file in args.legacy_wal.glob("atomic.sdp*"):
                shutil.copy2(file, target / file.name)
            execute([args.driver, "legacy", target / "atomic.sdp"])
            results["stage09-wal"] = "legacy data and WAL loaded"
    (args.scratch / "crash-matrix.json").write_text(json.dumps(results, indent=2) + "\n", encoding="utf-8")
    for case, result in results.items():
        print(case, result)


if __name__ == "__main__":
    main()
