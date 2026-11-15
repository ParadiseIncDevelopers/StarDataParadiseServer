"""Verify isolated backup, destructive failure cases and real restoration over TLS."""

import argparse
import hashlib
import json
import math
from pathlib import Path
import secrets
import ssl
import tempfile
import time

from history_restore_rpc import read_at, select
from jobs_rpc import submit, terminal, wait_for
from model_rpc import SHA256, activate, create_dataset, train
from service_smoke import certificates, connection, exchange, execute, listening, require, save_config
from sql_rpc import Client, client, i64, token, write


INVALID_STATE = 1013
PERMISSION_DENIED = 1006
TABLE = "backup_measurement"
MODEL = "backup_forecast"
ARCHIVES = "independent-backups"
TYPED_ROWS = [[i64(key), i64(key - 10), i64(3 * (key - 10) + 2)] for key in range(1, 21)]
DATASET_OPTIONS = {
    "features": [{"column": "reading", "transform": "numeric", "impute": "reject"}],
    "target": "outcome_value",
    "target_kind": "numeric",
    "split": {"mode": "random", "seed": 63, "train": 60, "validation": 20},
}


class BackupGuest(Client):
    def __init__(self, sock, password):
        self.sock = sock
        self.id = 2
        require(exchange(sock, 1, "hello")["ok"], "backup guest could not start a TLS session")
        require(exchange(sock, 2, "auth", {"username": "backup_guest", "password": password})["ok"],
                "backup guest could not authenticate")


def bootstrap(server, config, password):
    result = execute(server, config, "--bootstrap-admin", "local-admin", stdin=((password + "\n") * 2).encode("ascii"))
    require(result.returncode == 0, "backup test administrator setup failed")


def source_content(session):
    write(session, f"CREATE TABLE {TABLE} (sample_key BIGINT PRIMARY KEY, reading BIGINT, outcome_value BIGINT)", 0)
    values = ",".join(f"({key},{key - 10},{3 * (key - 10) + 2})" for key in range(1, 21))
    revision = write(session, f"INSERT INTO {TABLE} VALUES " + values, 20)["lsn"]
    write(session, """CREATE FUNCTION restored_adjustment(input_value IN BIGINT) RETURN BIGINT AS
BEGIN
    RETURN input_value * 3 + 2;
END;""", 0)
    snapshot = create_dataset(session, f"SELECT reading,outcome_value FROM {TABLE} ORDER BY sample_key", DATASET_OPTIONS)
    session.call("model_create", request_token=token(), name=MODEL, algorithm="linear")
    initial = train(session, MODEL, snapshot)
    activate(session, MODEL, initial["version"])
    model = session.call("model_evaluate", name=MODEL)
    return snapshot, model, revision, read_at(session, TABLE, revision)["rows"]


def numeric_forecast(session):
    prediction = session.call("model_predict", name=MODEL, features={"reading": 17})
    require(prediction["version"] == 1 and math.isclose(prediction["prediction"], 53.0, abs_tol=1e-4),
            "known model or feature preprocessing failed after backup")


def verified_job(session, job_id):
    outcome = wait_for(session, job_id, terminal, seconds=45)
    result = outcome["result"]
    require(outcome["state"] == "succeeded" and result and result["verified"] is True
            and SHA256.fullmatch(result["manifest_sha256"]),
            f"backup did not finish verification ({outcome.get('reason')})")
    backed_up = session.call("backup_status", job_id=job_id)
    require(backed_up["state"] == "succeeded" and backed_up["result"] == result,
            "backup job status disagreed with the common scheduler")
    return result


def disconnected_submit(session, port, context, password):
    with client(port, context, password) as short_lived:
        job = short_lived.call("backup_submit", request_token=token())
        require(job.get("state", "queued") == "queued", "backup submit did not report a background job")
    return verified_job(session, job["job_id"])


def denied_actions(session, port, context, backup_id):
    password = secrets.token_urlsafe(32)
    write(session, "CREATE USER backup_guest", 0)
    session.call("password_set", username="backup_guest", new_password=password, request_token=token())
    with connection(port, context) as sock:
        guest = BackupGuest(sock, password)
        guest.error("backup_submit", PERMISSION_DENIED, request_token=token())
        guest.error("backup_catalog", PERMISSION_DENIED)
        guest.error("backup_verify", PERMISSION_DENIED, backup_id=backup_id)
        guest.error("backup_restore", PERMISSION_DENIED, backup_id=backup_id, target_name="guest-denied")
        guest.error("backup_delete", PERMISSION_DENIED, backup_id=backup_id)


def checked_archive(session, root, result):
    backup_id = result["backup_id"]
    archive = root / ARCHIVES / backup_id
    manifest = archive / "manifest.json"
    logical = archive / "logical.json"
    store = archive / "store.sdp"
    require(archive.resolve(strict=True).parent == (root / ARCHIVES).resolve(strict=True)
            and logical.is_file() and store.is_file(), "backup omitted its independent logical/physical archive")
    manifest_bytes = manifest.read_bytes()
    require(hashlib.sha256(manifest_bytes).hexdigest() == result["manifest_sha256"],
            "backup job did not identify the published manifest bytes")
    proof = json.loads(manifest_bytes)
    require(proof["format"] == 1 and proof["backup_id"] == backup_id and proof["lsn"] > 0
            and {"published", "retain_from"} <= proof["checkpoint"].keys()
            and {"logical.json", "store.sdp"} <= proof["files"].keys(),
            "manifest omitted the backup point, checkpoint or file inventory")
    for name, file in proof["files"].items():
        require(Path(name).name == name, "archive inventory identified a file outside the verified directory")
        content = (archive / name).read_bytes()
        require(len(content) == file["bytes"] and hashlib.sha256(content).hexdigest() == file["sha256"],
                f"archive member did not match its manifest: {name}")
    require(not (archive / "admin-v1.json").exists() and not (archive / "server.key").exists(),
            "backup contained the out-of-store bootstrap verifier or TLS key")
    details = session.call("backup_verify", backup_id=backup_id)
    require(details["verified"] is True, "published backup did not survive a new verification")
    return archive


def damage_cases(session, root, result, archive):
    backup_id = result["backup_id"]
    rejects = (("logical.json", "missing-logical", None),
               ("manifest.json", "bad-format", json.dumps({"format": 99}).encode("ascii")),
               ("store.sdp", "damaged-store", b"not a physical StarDataParadise backup"))
    restores = root / "temp" / "restores"
    existing = restores / "keep-this-instance"
    existing.mkdir(parents=True)
    marker = existing / "keep.txt"
    marker.write_bytes(b"preserve existing target")
    for filename, target, replacement in rejects:
        file = archive / filename
        original = file.read_bytes()
        try:
            if replacement is None:
                file.unlink()
            else:
                file.write_bytes(replacement)
            session.error("backup_verify", INVALID_STATE, backup_id=backup_id)
            session.error("backup_restore", INVALID_STATE, backup_id=backup_id, target_name=target)
            require(not (restores / target).exists() and marker.read_bytes() == b"preserve existing target",
                    "rejected archive changed the existing target or created a partial restore")
        finally:
            file.write_bytes(original)
    session.error("backup_restore", INVALID_STATE, backup_id=backup_id, target_name=existing.name)
    require(marker.read_bytes() == b"preserve existing target", "existing restore target was overwritten")


def bounded_and_cancelled(session, root):
    before = set((root / ARCHIVES).iterdir())
    limited = session.call("backup_submit", request_token=token(), max_bytes=64)["job_id"]
    insufficient = wait_for(session, limited, terminal, seconds=45)
    require(insufficient["state"] == "failed" and insufficient["reason"] == "backup_space_insufficient"
            and insufficient["result"] is None,
            "insufficient backup byte budget yielded a verified archive")
    require(set((root / ARCHIVES).iterdir()) == before, "over-budget backup published partial files")
    block = submit(session, 1_000_000_000, timeout_ms=60000)
    live = wait_for(session, block, lambda state: state["worker_pid"] is not None or terminal(state))
    require(live["state"] == "running", "backup cancellation fixture did not reserve the shared worker")
    pending = session.call("backup_submit", request_token=token())["job_id"]
    session.call("backup_cancel", job_id=pending)
    cancelled = wait_for(session, pending, terminal)
    require(cancelled["state"] == "cancelled" and cancelled["result"] is None
            and set((root / ARCHIVES).iterdir()) == before, "cancelled backup published an archive")
    session.call("job_cancel", job_id=block)
    require(wait_for(session, block, terminal)["state"] == "cancelled", "shared worker was not released")


def scheduled_backup(session):
    created = session.call("job_schedule_create", request_token=token(), kind="backup", rule={"every_seconds": 1})
    schedule_id = created["schedule_id"]
    limit = time.monotonic() + 15.0
    while time.monotonic() < limit:
        status = session.call("job_schedule_status", schedule_id=schedule_id)
        if status["last_job_id"]:
            session.call("job_schedule_cancel", schedule_id=schedule_id)
            return verified_job(session, status["last_job_id"])
        time.sleep(0.05)
    raise RuntimeError("persistent backup schedule never dispatched an occurrence")


def validate_restored(server, root, context, password, source, target_name):
    target_dir = root / "temp" / "restores" / target_name
    require(target_dir.is_dir() and (target_dir / "store.sdp").is_file()
            and not (target_dir / "admin-v1.json").exists()
            and not (target_dir / "server.key").exists(), "restored storage or out-of-store secret boundary is wrong")
    for directory in ("restore-log", "restore-scratch"):
        (root / directory).mkdir()
    config = save_config(root, "restored.json", timeout_ms=20000, max_connections=6,
                         data_dir=str(target_dir), log_dir="restore-log", temp_dir="restore-scratch")
    require(execute(server, config, "--check-storage").returncode == 0, "restored target failed offline integrity")
    bootstrap(server, config, password)
    snapshot, model, revision, older = source
    with listening(server, config) as (_, port), client(port, context, password) as restored:
        require(select(restored, f"SELECT sample_key,reading,outcome_value FROM {TABLE} ORDER BY sample_key") == TYPED_ROWS
                and select(restored, "SELECT restored_adjustment(17)") == [[i64(53)]],
                "restored table, typed rows or PL/SQL did not match the backup point")
        require(read_at(restored, TABLE, revision)["rows"] == older and restored.call("history_verify")["verified"],
                "restored old revision or History integrity did not match the source")
        require(restored.call("dataset_get", dataset_id=snapshot["dataset_id"]) == snapshot
                and restored.call("model_evaluate", name=MODEL) == model, "restored snapshot or model artifact changed")
        numeric_forecast(restored)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    for argument in ("server", "openssl", "scratch"):
        parser.add_argument("--" + argument, type=Path, required=True)
    args = parser.parse_args()
    server = str(args.server.resolve(strict=True))
    openssl = str(args.openssl.resolve(strict=True))
    args.scratch.mkdir(parents=True, exist_ok=True)
    with tempfile.TemporaryDirectory(prefix="s29-", dir=args.scratch.resolve()) as directory:
        root = Path(directory)
        for name in ("data", "log", "temp", ARCHIVES):
            (root / name).mkdir()
        certificates(openssl, root)
        config = save_config(root, "config.json", timeout_ms=20000, max_connections=12, backup=ARCHIVES)
        require(execute(server, config, "--initialize-storage").returncode == 0, "backup storage setup failed")
        password = secrets.token_urlsafe(32)
        bootstrap(server, config, password)
        context = ssl.create_default_context(cafile=str(root / "ca.pem"))
        target_name = "verified-empty-instance"
        with listening(server, config) as (_, port), client(port, context, password) as owner:
            features = owner.call("capabilities")["features"]
            require(features.get("backup_v1") is True and features.get("pitr_v1") is False,
                    "backup or PITR capability is wrong")
            seeded = source_content(owner)
            numeric_forecast(owner)
            first = disconnected_submit(owner, port, context, password)
            original = checked_archive(owner, root, first)
            catalog = owner.call("backup_catalog")
            require(catalog["pitr"] is False
                    and any(item["backup_id"] == first["backup_id"] and item["verified"] for item in catalog["backups"]),
                    "catalog cannot locate a verified backup")
            denied_actions(owner, port, context, first["backup_id"])
            bounded_and_cancelled(owner, root)
            damage_cases(owner, root, first, original)
            owner.call("backup_verify", backup_id=first["backup_id"])
            write(owner, f"UPDATE {TABLE} SET outcome_value=777 WHERE sample_key=1", 1)
            require(select(owner, f"SELECT outcome_value FROM {TABLE} WHERE sample_key=1") == [[i64(777)]],
                    "source did not advance after completed backup")
            restored = owner.call("backup_restore", backup_id=first["backup_id"], target_name=target_name)
            require(restored["restored"] is True and restored["target_name"] == target_name
                    and restored["backup_id"] == first["backup_id"], "restore did not identify its source and target")
            require(select(owner, f"SELECT outcome_value FROM {TABLE} WHERE sample_key=1") == [[i64(777)]],
                    "restoring a new target changed the source instance")
            extra = scheduled_backup(owner)
            require(extra["backup_id"] != first["backup_id"], "scheduled backup replaced an immutable archive")
            owner.call("backup_delete", backup_id=extra["backup_id"])
            require(not (root / ARCHIVES / extra["backup_id"]).exists(), "deleted archive remained on disk")
        validate_restored(server, root, context, password, seeded, target_name)
        require(execute(server, config, "--check-storage").returncode == 0, "original instance storage was damaged")
    print("Backup TLS disconnected job, manifest and real isolated restore, corruption/quota, auth, schedule and cancel passed")


if __name__ == "__main__":
    main()
