"""Exercise offline update copying from a real previous Windows Server release."""

import argparse
import hashlib
import json
from pathlib import Path
import secrets
import shutil
import ssl
import subprocess
import tempfile
import zlib

from model_rpc import REGRESSION_OPTIONS, REGRESSION_SQL, create_dataset, prepare_sources, train, verified_artifact
from service_smoke import certificates, listening, require, save_config, send_ctrl_c
from sql_rpc import BLOB, client, i64, write


def invoke(server, config, *arguments, input_data=None, expected=0):
    result = subprocess.run([str(server), "--config", str(config), *map(str, arguments)],
                            input=input_data, capture_output=True, timeout=65)
    details = result.stderr.decode("utf-8", errors="replace")
    require(result.returncode == expected,
            f"{arguments[0]} returned {result.returncode}, expected {expected}: {details}")
    return result


def state(root):
    return {file.name: hashlib.sha256(file.read_bytes()).hexdigest()
            for file in root.iterdir() if file.is_file() and file.name != ".sdp.lock"}


def stop(process):
    send_ctrl_c(process.pid)
    require(process.wait(timeout=15) == 0, "test Server did not stop gracefully")


def stage_config(root, data, label):
    for category in ("logs", "temp"):
        (root / f"{label}-{category}").mkdir()
    return save_config(root, f"{label}.json", data_dir=str(data), log_dir=f"{label}-logs",
                       temp_dir=f"{label}-temp", instance_name=label)


def initialize(server, config, password):
    invoke(server, config, "--initialize-storage")
    invoke(server, config, "--bootstrap-admin", "local-admin",
           input_data=((password + "\n") * 2).encode())


def inspect(server, config):
    return json.loads(invoke(server, config, "--update-inspect").stdout)


def product_version(server):
    output = subprocess.check_output([str(server), "--version"], timeout=10).decode("ascii").split()
    require(len(output) == 2 and output[0] == "StarDataParadiseServer", "unexpected Server version output")
    return tuple(map(int, output[1].split(".")))


def verify(session, dataset):
    rows, _ = session.query("SELECT id, amount, note, payload FROM update_sample ORDER BY id")
    require(rows == [[i64(1), {"t": "decimal", "unscaled": "12345", "scale": 2}, {"t": "null"},
                      {"t": "binary", "hex": "0011ff"}]], "typed legacy row did not survive")
    require(session.query("SELECT update_plus(4)")[0] == [[i64(7)]], "compiled PL/SQL result changed")
    require(session.call("history_verify")["verified"], "legacy History integrity did not survive")
    model = verified_artifact(session, "update_model", "linear", 1, dataset)
    require(abs(model["artifact"]["coefficients"][0] - 3) < 1e-4, "legacy model artifact changed")


def seed(owner):
    write(owner, "CREATE TABLE update_sample (id BIGINT PRIMARY KEY, amount DECIMAL(8,2), note TEXT, payload BLOB)", 0)
    write(owner, "INSERT INTO update_sample VALUES (1,:exact_amount,NULL,:bytes_input)", 1,
          types={"exact_amount": {"type": "DECIMAL", "precision": 8, "scale": 2}, "bytes_input": BLOB},
          values={"exact_amount": {"t": "decimal", "unscaled": "12345", "scale": 2},
                  "bytes_input": {"t": "binary", "hex": "0011ff"}})
    write(owner, "CREATE FUNCTION update_plus(p IN BIGINT) RETURN BIGINT AS BEGIN RETURN p + 3; END;", 0)
    prepare_sources(owner)
    dataset = create_dataset(owner, REGRESSION_SQL, REGRESSION_OPTIONS)
    owner.call("model_create", request_token=secrets.token_hex(16), name="update_model", algorithm="linear")
    train(owner, "update_model", dataset)
    verify(owner, dataset)
    return dataset


def run(root, old, candidate, openssl):
    require(product_version(old) < product_version(candidate), "legacy fixture must use an older product release")
    certificates(str(openssl), root)
    tls = ssl.create_default_context(cafile=str(root / "ca.pem"))
    password = secrets.token_urlsafe(32)
    old_data = root / "legacy"
    old_data.mkdir()
    old_config = stage_config(root, old_data, "legacy")
    initialize(old, old_config, password)
    target = root / "new-data"
    with listening(str(old), old_config) as (process, port):
        with client(port, tls, password) as owner:
            dataset = seed(owner)
            denied = invoke(candidate, old_config, "--update-snapshot", target, expected=1)
            require(b"already locked" in denied.stderr and not target.exists(),
                    "updater did not refuse the old live instance through the native lock")
        stop(process)
    (old_data / "unrelated.txt").write_text("Remain at the source.", encoding="utf-8")
    before = state(old_data)
    old_proof = inspect(candidate, old_config)
    require(int(old_proof["lsn"]) > 0 and old_proof["history_head"] == old_proof["lsn"],
            "inspection did not report the committed History boundary")
    require(old_proof["routines"] == 1 and old_proof["model_versions"] == 1,
            "inspection did not verify stored routine and model records")
    copied = json.loads(invoke(candidate, old_config, "--update-snapshot", target).stdout)
    require(copied["source_sha256"] == old_proof["sha256"] and copied["lsn"] == old_proof["lsn"] ==
            copied["checkpoint_lsn"], "snapshot shifted source or checkpoint boundary")
    require(state(old_data) == before and not (target / "unrelated.txt").exists(),
            "source changed or a non-storage file escaped the allowlist")

    new_config = stage_config(root, target, "candidate")
    target_proof = inspect(candidate, new_config)
    require(copied["sha256"] == target_proof["sha256"] and copied["bytes"] == target_proof["bytes"],
            "published target differs from its verified snapshot")
    existing = state(target)
    ready = json.loads(invoke(candidate, new_config, "--update-readiness").stdout)
    require(ready["readiness"] and ready["bind"] == "127.0.0.1" and ready["port"] > 0
            and ready["sha256"] == copied["sha256"] and state(target) == existing,
            "native readiness detection did not bind with the expected state")
    invoke(candidate, old_config, "--update-snapshot", target, expected=1)
    require(state(target) == existing, "updater overwrote an existing target")
    with listening(str(candidate), new_config) as (process, port):
        with client(port, tls, password) as owner:
            verify(owner, dataset)
            write(owner, "INSERT INTO update_sample VALUES (2,NULL,NULL,NULL)", 1)
        stop(process)
    advanced = inspect(candidate, new_config)
    require(int(advanced["lsn"]) > int(copied["lsn"]) and state(old_data) == before,
            "target writes did not advance only the side-by-side target")

    future = root / "future"
    shutil.copytree(target, future)
    future_config = stage_config(root, future, "future-v99")
    image_path = future / "store.sdp"
    with image_path.open("r+b") as image:
        header = bytearray(image.read(4096))
        header[8:12] = (99).to_bytes(4, "little")
        header[-4:] = zlib.crc32(header[:-4]).to_bytes(4, "little")
        image.seek(0)
        image.write(header)
    future_before = state(future)
    invoke(old, future_config, "--check-storage", expected=1)
    invoke(candidate, future_config, "--update-snapshot", root / "future-target", expected=1)
    require(state(future) == future_before and not (root / "future-target").exists(),
            "unknown format was modified or published")

    no_wal = root / "old-empty"
    no_wal.mkdir()
    no_wal_config = stage_config(root, no_wal, "old-empty")
    initialize(old, no_wal_config, password)
    no_wal_before = state(no_wal)
    normalized = root / "normal-data"
    result = json.loads(invoke(candidate, no_wal_config, "--update-snapshot", normalized).stdout)
    require(result["lsn"] == "0" and list(normalized.glob("store.sdp.wal.*")) and state(no_wal) == no_wal_before,
            "zero-revision legacy data was not normalized exclusively on the target")

    print(f"S46-T01: {old.name}, typed SQL, PL/SQL, History and model preserved at LSN {copied['lsn']}.")
    print("S46-T01: live source refused, checkpoint verified, source hashes retained, old binary rejected sealed v99.")


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    for argument in ("legacy", "server", "openssl", "scratch"):
        parser.add_argument(f"--{argument}", type=Path, required=True)
    args = parser.parse_args()
    args.scratch.mkdir(parents=True, exist_ok=True)
    with tempfile.TemporaryDirectory(prefix="update-native-", dir=args.scratch.resolve()) as name:
        run(Path(name), args.legacy.resolve(strict=True), args.server.resolve(strict=True),
            args.openssl.resolve(strict=True))


if __name__ == "__main__":
    main()
