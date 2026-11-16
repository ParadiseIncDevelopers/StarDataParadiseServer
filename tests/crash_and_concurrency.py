"""Check exact publication boundaries and concurrent writes on disposable fixtures."""

import argparse
from contextlib import contextmanager
import csv
from pathlib import Path
import re
import secrets
import ssl
import tempfile
import time

from crash_matrix import crash, execute as run_driver, state
from history_restore_rpc import conflict, read_at, select
from security_rpc import receive_reply, session_summaries
from service_smoke import certificates, execute, listening, require, save_config
from sql_rpc import INVALID_ARGUMENT, client, i64, token, write


@contextmanager
def deny_directory_creation(folder):
    identity = run_driver(["whoami.exe", "/user", "/fo", "csv", "/nh"])
    _, sid = next(csv.reader(identity.strip().splitlines()))
    require(re.fullmatch(r"S-1-(?:\d+-)+\d+", sid) is not None, "could not identify the test account SID")
    # AD denies adding a subdirectory to this fixture directory. Reading remains allowed.
    run_driver(["icacls.exe", folder, "/deny", f"*{sid}:(AD)"])
    witness = folder / "permission-witness"
    try:
        try:
            witness.mkdir()
        except PermissionError:
            yield
        else:
            raise RuntimeError("the test account could still create a subdirectory")
    finally:
        run_driver(["icacls.exe", folder, "/remove:d", f"*{sid}"])
    witness.mkdir()
    witness.rmdir()


def publication_crashes(driver, root):
    for name, expectation, revision in (("before", "v1", 3), ("after", "v2", 4)):
        case = root / f"model-{name}"
        case.mkdir()
        run_driver([driver, "crash-seed", case])
        crash([driver, f"crash-model-{name}", case])
        result = run_driver([driver, f"model-expect-{expectation}", case]).strip()
        print(f"Model publication killed {name} WAL durability at expected History revision {revision}: {result}")

    archive_case = root / "archive"
    archive_case.mkdir()
    run_driver([driver, "crash-seed", archive_case])
    source = state(archive_case)
    archives = archive_case / "archives"
    with deny_directory_creation(archives):
        print(run_driver([driver, "backup-denied", archive_case]).strip())
    crash([driver, "crash-backup", archive_case])
    require(len(list(archives.glob(".partial-*/manifest.json"))) == 1,
            "backup child did not reach a verified staged manifest")
    print(run_driver([driver, "backup-unpublished", archive_case]).strip())
    crash([driver, "crash-backup-after", archive_case])
    print(run_driver([driver, "backup-verify", archive_case]).strip())

    crash([driver, "crash-restore", archive_case])
    restores = archive_case / "temp" / "restores"
    require(len(list(restores.glob(".restore-*/store.sdp"))) == 1,
            "restore child did not reach a staged store copy")
    target = restores / "crash-fixture-restore"
    require(not target.exists(), "interrupted restore left a visible destination")
    print(run_driver([driver, "backup-verify", archive_case]).strip())
    with deny_directory_creation(restores):
        print(run_driver([driver, "restore-denied", archive_case]).strip())
    crash([driver, "crash-restore-after", archive_case])
    print(run_driver([driver, "model-expect-v1", target]).strip())
    require(state(archive_case) == source, "backup or restore changed source store, WAL or checkpoint bytes")
    print("Killed backup and restore before and after publication; source unchanged and restored sealed History verified")


def wait_for_queue(controller, waiters):
    expected = {(session.id, operation) for session, operation in waiters}
    deadline = time.monotonic() + 1.3
    while time.monotonic() < deadline:
        queued = {(entry["request_id"], entry["operation"]) for entry in session_summaries(controller)
                  if entry["lock_wait_ms"] > 0}
        if expected <= queued:
            return
        time.sleep(0.005)
    raise RuntimeError("competing operations were not observed in the real storage lease queue")


def balance_race(holder, contender, controller):
    write(controller, "CREATE TABLE race_balance (id BIGINT PRIMARY KEY,balance BIGINT CHECK (balance>=0))", 0)
    base = write(controller, "INSERT INTO race_balance VALUES (1,100)", 1)["lsn"]
    debit = "UPDATE race_balance SET balance=20 WHERE balance=100"
    with contender.prepared(debit) as statement:
        holder.call("tx_begin")
        require(holder.mutate(debit)["affected_rows"] == 1, "first debit did not stage")
        contender.send_without_receiving("execute", statement_id=statement, request_token=token())
        wait_for_queue(controller, [(contender, "execute")])
        holder.call("tx_commit", request_token=token())
        second = receive_reply(contender)
    require(second["ok"] and second["body"]["affected_rows"] == 0,
            "concurrent debit did not recheck the now-insufficient balance")
    require(select(controller, "SELECT balance FROM race_balance") == [[i64(20)]], "debit totals did not serialize")
    entries = controller.call("history_log", target="race_balance", **{"from": base})["entries"]
    require(sum(entry["kind"] == "row" for entry in entries) == 2, "History did not preserve initial row and one debit")
    print("Concurrent conditional debits: first affected 1, second affected 0, final balance 20, two History row revisions")


def foreign_key_race(holder, contender, controller):
    write(controller, "CREATE TABLE race_parent (id BIGINT PRIMARY KEY)", 0)
    write(controller, "CREATE TABLE race_child (id BIGINT PRIMARY KEY,parent_id BIGINT REFERENCES race_parent (id))", 0)
    write(controller, "INSERT INTO race_parent VALUES (1)", 1)
    missed = token()
    with contender.prepared("INSERT INTO race_child VALUES (1,1)") as statement:
        holder.call("tx_begin")
        require(holder.mutate("DELETE FROM race_parent WHERE id=1")["affected_rows"] == 1,
                "parent delete did not stage")
        contender.send_without_receiving("execute", statement_id=statement, request_token=missed)
        wait_for_queue(controller, [(contender, "execute")])
        holder.call("tx_commit", request_token=token())
        child = receive_reply(contender)
    require(not child["ok"] and child["error"]["code"] == INVALID_ARGUMENT, "queued child insert missed its FK recheck")
    require(controller.call("tx_outcome", request_token=missed) == {"status": "unknown"} and
            not select(controller, "SELECT id FROM race_child") and not select(controller, "SELECT id FROM race_parent"),
            "FK failure left an orphan row or a committed child outcome")
    print("FK race: queued insert rejected after parent delete, no orphan or committed child outcome")


def history_retention_race(holder, pinning, purging, controller):
    write(controller, "CREATE TABLE race_history (id BIGINT PRIMARY KEY,n BIGINT)", 0)
    earlier = write(controller, "INSERT INTO race_history VALUES (1,1)", 1)["lsn"]
    later = write(controller, "UPDATE race_history SET n=2 WHERE id=1", 1)["lsn"]
    floor = controller.call("history_status")["earliest_revision"]
    pin = {"pin_kind": "snapshot", "pin_name": "concurrent-fixture"}
    holder.call("tx_begin")
    pinning.send_without_receiving("history_pin", revision=earlier, **pin)
    purging.send_without_receiving("history_purge", revision=later)
    wait_for_queue(controller, [(pinning, "history_pin"), (purging, "history_purge")])
    holder.call("tx_rollback")
    pin_reply = receive_reply(pinning)
    purge_reply = receive_reply(purging)
    actual = controller.call("history_verify")
    require(actual["verified"], "concurrent retention operations damaged History validation")
    if pin_reply["ok"]:
        conflict(purge_reply, "retention_pinned")
        require(actual["earliest_revision"] == floor and len(read_at(controller, "race_history", earlier)["rows"]) == 1,
                "purge crossed an active pin")
        winner = "pin"
    else:
        conflict(pin_reply, "revision_not_recoverable")
        require(purge_reply["ok"] and actual["earliest_revision"] == later and
                len(read_at(controller, "race_history", later)["rows"]) == 1,
                "purge did not keep its oldest recoverable anchor")
        winner = "purge"
    print(f"Concurrent History pin/purge: winner={winner}, verified retained floor={actual['earliest_revision']}")


def concurrent_writes(server, openssl, root):
    root.mkdir()
    for directory in ("data", "log", "temp"):
        (root / directory).mkdir()
    certificates(openssl, root)
    config = save_config(root, max_connections=8, timeout_ms=20_000)
    password = secrets.token_urlsafe(32)
    require(execute(server, config, "--initialize-storage").returncode == 0, "race storage initialization failed")
    require(execute(server, config, "--bootstrap-admin", "local-admin", stdin=((password + "\n") * 2).encode()).returncode == 0,
            "race administrator bootstrap failed")
    context = ssl.create_default_context(cafile=str(root / "ca.pem"))
    with listening(server, config) as (_, port):
        with (client(port, context, password) as holder, client(port, context, password) as contender,
              client(port, context, password) as controller, client(port, context, password) as purging):
            balance_race(holder, contender, controller)
            foreign_key_race(holder, contender, controller)
            history_retention_race(holder, contender, purging, controller)
            require(controller.call("history_verify")["verified"], "post-race History verification failed")
    require(execute(server, config, "--check-storage").returncode == 0, "post-race on-disk verification failed")


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    for name in ("driver", "server", "openssl", "scratch"):
        parser.add_argument("--" + name, type=Path, required=True)
    args = parser.parse_args()
    args.scratch.mkdir(parents=True, exist_ok=True)
    driver, server, openssl = (path.resolve(strict=True) for path in (args.driver, args.server, args.openssl))
    with tempfile.TemporaryDirectory(prefix="publication-races-", dir=args.scratch.resolve()) as directory:
        root = Path(directory)
        publication_crashes(driver, root)
        concurrent_writes(server, openssl, root / "tls-races")


if __name__ == "__main__":
    main()
