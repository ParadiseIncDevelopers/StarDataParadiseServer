"""Validate the legacy backup aliases over TLS against isolated MySQL forks."""

from __future__ import annotations

import argparse
from contextlib import contextmanager
import json
from pathlib import Path
import re
import secrets
import ssl
import subprocess
import sys
import time
from uuid import uuid4

CODE = Path(__file__).resolve().parent
sys.path.insert(1, str(CODE.parent.parent / "tests"))
sys.path.insert(2, str(CODE.parent.parent))
from acceptance import Client
from service_smoke import certificates, connection
from compatibility.backup import physical_backup as physical
from compatibility.tests.fixture import BASEDIR, BIN, HIDDEN, MIGRATION, SHELL, Fixture, query, require, run
from compatibility.tests.physical_backup_acceptance import PLUGIN, TOOL


@contextmanager
def bridge(env, root, source_port, source_uuid, config):
    log = root / "bridge.log"
    frontend, backend = root / "front", root / "back"
    command = [sys.executable, "-B", str(CODE / "service.py"), "--port", "0",
               "--cert", str(frontend / "server.pem"), "--key", str(frontend / "server.key"),
               "--mysql-port", source_port, "--mysql-ca", str(backend / "ca.pem"),
               "--mysql-uuid", source_uuid, "--schema", "sdp_backup_wire",
               "--legacy-backup-config", str(config)]
    context = ssl.create_default_context(cafile=str(frontend / "ca.pem"))
    with log.open("xb") as output, subprocess.Popen(command, env=env, cwd=root, stdin=subprocess.DEVNULL,
                                                   stdout=output, stderr=subprocess.STDOUT,
                                                   creationflags=HIDDEN) as process:
        try:
            deadline = time.monotonic() + 20
            while time.monotonic() < deadline:
                details = log.read_text(encoding="utf-8", errors="replace")
                match = re.search(r"^SDP_WIRE_V1_READY (\d+)$", details, re.MULTILINE)
                if match:
                    yield int(match[1]), context
                    return
                require(process.poll() is None, details)
                time.sleep(0.1)
            raise RuntimeError("wire backup fixture did not become ready")
        finally:
            if process.poll() is None:
                process.terminate()
                process.wait(timeout=15)


def validate(port, tls, work, owner_password, guest_password, archive, archive_id):
    with connection(port, tls) as owner_socket, connection(port, tls) as guest_socket:
        owner_socket.settimeout(120)
        owner = Client(owner_socket, "backup_compat_owner", owner_password)
        guest = Client(guest_socket, "backup_compat_guest", guest_password)
        for operation, body in (("backup_catalog", {}), ("backup_verify", {"backup_id": archive_id}),
                                ("backup_restore", {"backup_id": archive_id, "target_name": "guest"})):
            guest.error(operation, 1006, **body)
        owner.error("backup_catalog", 1012, unexpected=True)
        owner.error("backup_verify", 1012, backup_id="../unsafe")
        owner.error("backup_restore", 1012, backup_id=archive_id, target_name="../unsafe")
        owner.error("backup_restore", 1012, backup_id=archive_id, target_name="nul")
        owner.error("backup_submit", 1004, request_token=uuid4().hex)
        features = owner.call("capabilities")["features"]
        require(not features["backup_v1"], "partial backup operations advertised the complete legacy subsystem")

        catalog = owner.call("backup_catalog")
        summary = owner.call("backup_verify", backup_id=archive_id)
        require(catalog == {"backups": [summary], "pitr": False} and summary["backup_id"] == archive_id
                and summary["verified"] is True and summary["format"] == physical.FORMAT
                and summary["lsn"] is None and len(summary["manifest_sha256"]) == 64,
                "legacy physical catalog and independently verified result disagree")
        print("PASS deployed wire aliases, legacy catalog summary, and explicit effective-account permission gate",
              flush=True)

        file = archive / "manifest.json"
        original = file.read_bytes()
        try:
            modified = json.loads(original)
            modified["source_uuid"] = str(uuid4())
            file.write_text(json.dumps(modified), encoding="utf-8")
            owner.error("backup_verify", 1013, backup_id=archive_id)
            owner.error("backup_restore", 1013, backup_id=archive_id, target_name="other-source")
            require(owner.call("backup_catalog") == {"pitr": False, "backups": [{"backup_id": archive_id,
                    "reason": "archive_invalid", "verified": False}]}, "foreign source archive was disclosed as valid")
        finally:
            file.write_bytes(original)

        tablespace = archive / "data" / "sdp_backup_wire" / "sample.ibd"
        with tablespace.open("r+b") as stream:
            first = stream.read(1)
            stream.seek(0)
            stream.write(bytes((first[0] ^ 255,)))
        try:
            owner.error("backup_verify", 1013, backup_id=archive_id)
            owner.error("backup_restore", 1013, backup_id=archive_id, target_name="damaged")
            require(not tuple(work.iterdir()), "rejected aliases published output")
        finally:
            with tablespace.open("r+b") as stream:
                stream.write(first)

        target_name = "verified-copy"
        restored = owner.call("backup_restore", backup_id=archive_id, target_name=target_name)
        require(restored == {**summary, "restored": True, "target_name": target_name},
                "verified physical restore returned a different manifest or alias")
        owner.error("backup_restore", 1013, backup_id=archive_id, target_name=target_name)
        require([path.name for path in work.iterdir()] == [target_name],
                "restore did not preserve the independent target or left scratch")
        print("PASS foreign source and corrupt SHA-256 rejected, exclusive independent restore alias published", flush=True)
        return work / target_name


def acceptance(fixture, plugin, openssl):
    require(plugin.is_file(), "same-fork clone plugin is required")
    with fixture.scratch("backup-rpc") as root:
        for name in ("front", "back", "archives", "work"):
            (root / name).mkdir()
        archives, work = (root / name for name in ("archives", "work"))
        for name in ("front", "back"):
            certificates(str(openssl), root / name)
        owner, guest, operator = (secrets.token_hex(24) for _ in range(3))
        with fixture.disposable(root / "source", plugin_dir=plugin.parent, tls_dir=root / "back") as source:
            env, classic, _ = source
            query(source, """INSTALL PLUGIN clone SONAME 'mysql_clone.dll';
                CREATE SCHEMA sdp_backup_wire;
                CREATE TABLE sdp_backup_wire.sample(id BIGINT PRIMARY KEY, v DECIMAL(18,4)) ENGINE=InnoDB;
                INSERT INTO sdp_backup_wire.sample VALUES(9007199254740993,123.4500);""")
            for username, password in (("backup_compat_owner", owner), ("backup_compat_guest", guest)):
                sql = (f"CREATE USER '{username}'@'localhost' IDENTIFIED BY '{password}' REQUIRE SSL;"
                       f"GRANT SELECT ON sdp_backup_wire.* TO '{username}'@'localhost';")
                result = run(classic, env, 20, script=sql)
                require(result.returncode == 0, "disposable wire account setup failed")
            operator_sql = (f"CREATE USER 'backup_probe'@'localhost' IDENTIFIED BY '{operator}' REQUIRE SSL;"
                            "GRANT PROCESS, SELECT, SHUTDOWN ON *.* TO 'backup_probe'@'localhost';")
            require(run(classic, env, 20, script=operator_sql).returncode == 0, "probe credential setup failed")
            source_port, source_uuid = query(source, "SELECT @@port,@@server_uuid;").split("\t")
            options = {"mysqld": str(fixture.mysqld), "mysql": str(fixture.mysql), "basedir": str(fixture.basedir),
                       "plugin_dir": str(plugin.parent), "user": "root"}
            credentials = root / "probe-opt.cnf"
            credentials.write_text(f"[client]\npassword={operator}\n", encoding="ascii")
            probe_options = {**options, "user": "backup_probe", "client_options": str(credentials)}
            config = root / "backup.json"
            config.write_text(json.dumps({"archive_root": str(archives), "work_root": str(work),
                                          "administrators": ["backup_compat_owner@localhost"], "runtime": probe_options,
                                          "allow_restore": True}), encoding="utf-8")
            archive_id = uuid4().hex
            archive = archives / archive_id
            command = [sys.executable, "-B", str(TOOL), "--archive-root", str(archives), "create", archive_id]
            for key, value in options.items():
                command.extend(("--" + key.replace("_", "-"), value))
            command.extend(("--source-port", source_port, "--source-ca", str(root / "back" / "ca.pem")))
            created = run(command, env, 180)
            require(created.returncode == 0, created.stdout + created.stderr)
            with bridge(env, root, source_port, source_uuid, config) as (port, tls):
                target = validate(port, tls, work, owner, guest, archive, archive_id)
            probe = root / "target"
            probe.mkdir()
            runtime = physical.Runtime(fixture.mysqld, fixture.mysql, fixture.basedir, plugin.parent,
                                       "backup_probe", credentials)
            manifest = physical.manifest_read(archive)
            with physical.start_probe(runtime, target, probe, manifest["settings"], source_uuid) as (port, ca, _):
                rows = runtime.sql(port, ca, "SELECT id,v FROM sdp_backup_wire.sample;")
                require(rows == [["9007199254740993", "123.4500"]], "RPC restore changed typed data")
            require(query(source, "SELECT @@server_uuid;") == source_uuid, "source Server was interrupted")
            print("PASS RPC-published datadir booted independently and returned matching typed rows", flush=True)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--mysqld", type=Path, default=BIN / "mysqld.exe")
    parser.add_argument("--mysql", type=Path, default=BIN / "mysql.exe")
    parser.add_argument("--basedir", type=Path, default=BASEDIR)
    parser.add_argument("--plugin", type=Path, default=PLUGIN)
    parser.add_argument("--openssl", type=Path, default=MIGRATION / "toolchain/openssl-firedaemon/x64/bin/openssl.exe")
    args = parser.parse_args()
    fixture = Fixture(args.mysqld.resolve(strict=True), args.mysql.resolve(strict=True),
                      args.basedir.resolve(strict=True), SHELL, None)
    acceptance(fixture, args.plugin.resolve(strict=True), args.openssl.resolve(strict=True))


if __name__ == "__main__":
    main()
