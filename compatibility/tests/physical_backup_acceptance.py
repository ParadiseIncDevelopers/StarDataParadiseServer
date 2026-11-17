"""Test a physical archive and independent restore against disposable Windows forks."""

from __future__ import annotations

import argparse
from contextlib import contextmanager
import json
from pathlib import Path
import subprocess
import sys
import time
from uuid import uuid4

from compatibility.backup.physical_backup import Runtime, start_probe
from .fixture import BASEDIR, BIN, COMPATIBILITY, HIDDEN, SHELL, Fixture, query, require, run


TOOL = COMPATIBILITY / "backup" / "physical_backup.py"
PLUGIN = BASEDIR / "plugin_output_directory" / "RelWithDebInfo" / "mysql_clone.dll"
TYPED = """SELECT id, cost, HEX(payload), HEX(label), stamp, CAST(details AS CHAR)
    FROM sdp_physical.sample ORDER BY id;"""
METADATA = """SELECT COLUMN_NAME, COLUMN_TYPE, IS_NULLABLE FROM information_schema.columns
    WHERE TABLE_SCHEMA='sdp_physical' AND TABLE_NAME='sample' ORDER BY ORDINAL_POSITION;"""
ROUTINE = "CALL sdp_physical.total_cost();"
HISTORY = "SELECT revision, HEX(payload) FROM sdp_physical.history ORDER BY revision;"
WRITER_OPEN = "SELECT IS_USED_LOCK('sdp_physical_pending_writer') IS NOT NULL;"


def invoke(env, archives, action, archive_id, options=(), *, expect=None):
    command = [sys.executable, "-B", str(TOOL), "--archive-root", str(archives),
               action, archive_id, *options]
    result = run(command, env, 180)
    output = result.stdout + result.stderr
    if expect is not None:
        require(result.returncode != 0 and expect in output, f"expected {expect!r}: {output}")
        return None
    require(result.returncode == 0, output)
    line = next((line for line in result.stdout.splitlines() if line.startswith("SDP_PHYSICAL_BACKUP ")), None)
    require(line is not None, output)
    return json.loads(line.removeprefix("SDP_PHYSICAL_BACKUP "))


@contextmanager
def pending_transaction(source):
    env, classic, _ = source
    command = [argument for argument in classic if argument != "--skip-password"]
    with subprocess.Popen([*command, "--unbuffered"], env=env, stdin=subprocess.PIPE,
                          stdout=subprocess.DEVNULL, text=True, creationflags=HIDDEN) as client:
        try:
            client.stdin.write("START TRANSACTION;\nINSERT INTO sdp_physical.history VALUES (4,0x04);\n"
                               "SELECT GET_LOCK('sdp_physical_pending_writer',0);\n")
            client.stdin.flush()
            deadline = time.monotonic() + 10
            while query(source, WRITER_OPEN) != "1":
                require(client.poll() is None and time.monotonic() < deadline,
                        "uncommitted writer did not reach the snapshot")
                time.sleep(0.1)
            yield
            require(query(source, WRITER_OPEN) == "1", "the writer closed during the snapshot")
            client.stdin.write("ROLLBACK;\n")
            client.stdin.close()
            require(client.wait(timeout=10) == 0, "pending test writer failed")
        finally:
            if client.poll() is None:
                client.kill()
                client.wait(timeout=10)


def verify(fixture, plugin):
    require(plugin.is_file(), f"missing Clone plugin: {plugin}")
    with fixture.scratch("phys") as root:
        archives = root / "archives"
        archives.mkdir()
        archive_id = uuid4().hex
        archive = archives / archive_id
        target = root / "restored"
        source_root = root / "source"
        with fixture.disposable(source_root, plugin_dir=plugin.parent) as source:
            env, _, _ = source
            runtime_options = ["--mysqld", str(fixture.mysqld), "--mysql", str(fixture.mysql),
                               "--basedir", str(fixture.basedir), "--plugin-dir", str(plugin.parent),
                               "--user", "root"]
            source_port = query(source, "SELECT @@port;")
            source_ca = source_root / "data" / "ca.pem"
            create = [*runtime_options, "--source-port", source_port, "--source-ca", str(source_ca)]
            restore = [*runtime_options, "--target", str(target)]

            query(source, """INSTALL PLUGIN clone SONAME 'mysql_clone.dll';
                CREATE SCHEMA sdp_physical;
                CREATE TABLE sdp_physical.sample(
                    id BIGINT PRIMARY KEY, cost DECIMAL(18,4), payload BLOB,
                    label VARCHAR(20) CHARACTER SET utf8mb4, stamp DATETIME(6), details JSON
                ) ENGINE=InnoDB;
                CREATE TABLE sdp_physical.history(revision BIGINT PRIMARY KEY, payload BLOB) ENGINE=InnoDB;
                CREATE PROCEDURE sdp_physical.total_cost() SELECT SUM(cost) FROM sdp_physical.sample;
                START TRANSACTION;
                INSERT INTO sdp_physical.sample VALUES
                    (9007199254740993,123.4500,0x00FF,CONVERT(0xC3B6 USING utf8mb4),
                     '2026-11-17 10:02:03.123456',JSON_OBJECT('quantity',3)),
                    (2,NULL,NULL,'plain',NULL,NULL);
                INSERT INTO sdp_physical.history VALUES (1,0xAB00FF),(2,0x01);
                COMMIT;
                START TRANSACTION;
                INSERT INTO sdp_physical.history VALUES (3,0xFF);
                ROLLBACK;""")
            source_uuid = query(source, "SELECT @@server_uuid;")
            originals = [query(source, statement) for statement in (TYPED, METADATA, ROUTINE, HISTORY)]
            require("9007199254740993\t123.4500\t00FF\tC3B6\t2026-11-17 10:02:03.123456" in originals[0],
                    "source typed data was not initialized as expected")
            require(originals[3] == "1\tAB00FF\n2\t01", "source transaction boundaries failed")

            query(source, "CREATE TABLE sdp_physical.bad(id INT) ENGINE=MyISAM;")
            invoke(env, archives, "create", archive_id, create, expect="application tables must use InnoDB")
            require(not list(archives.iterdir()), "unsupported source left a published or staged archive")
            query(source, "DROP TABLE sdp_physical.bad;")

            with pending_transaction(source):
                created = invoke(env, archives, "create", archive_id, create)
            require(created["source_uuid"] == source_uuid and created["probe_uuid"] != source_uuid
                    and created["checked_tables"] == 2, f"independent archive verification failed: {created}")
            require(invoke(env, archives, "create", archive_id, create) == created, "retry changed archive result")
            require(invoke(env, archives, "verify", archive_id) == created, "published archive verification changed metadata")
            require(query(source, "SELECT @@server_uuid;") == source_uuid, "source was interrupted by backup")
            print("PASS live native InnoDB clone with open writer and publish after independent TLS startup "
                  "and CHECK TABLE", flush=True)

            tablespace = archive / "data" / "sdp_physical" / "sample.ibd"
            with tablespace.open("r+b") as file:
                byte = file.read(1)
                file.seek(0)
                file.write(bytes((byte[0] ^ 0xff,)))
            invoke(env, archives, "restore", archive_id, restore, expect="SHA-256")
            require(not target.exists(), "rejected archive published a restore target")
            with tablespace.open("r+b") as file:
                file.write(byte)

            target.mkdir()
            sentinel = target / "sentinel"
            sentinel.write_text("keep", encoding="utf-8")
            invoke(env, archives, "restore", archive_id, restore, expect="already exists")
            require(sentinel.read_text(encoding="utf-8") == "keep", "restore changed an existing target")
            sentinel.unlink()
            target.rmdir()

            restored = invoke(env, archives, "restore", archive_id, restore)
            require(restored["target_uuid"] not in (source_uuid, created["probe_uuid"]),
                    "restored Server reused another Server identity")
            require(invoke(env, archives, "verify", archive_id) == created, "restore changed the archive")
            print("PASS SHA-256 damage rejected before restore and existing target preserved", flush=True)

            probe = root / "reopen"
            probe.mkdir()
            runtime = Runtime(fixture.mysqld, fixture.mysql, fixture.basedir, plugin.parent, "root", None)
            with start_probe(runtime, target, probe, created["settings"], source_uuid) as (port, ca, uuid):
                rows = ["\n".join("\t".join(row) for row in runtime.sql(port, ca, statement))
                        for statement in (TYPED, METADATA, ROUTINE, HISTORY)]
                require(rows == originals, f"typed physical restore differs: {rows!r}")
                require(uuid == restored["target_uuid"], "published datadir changed identity after restart")
                require(ca.read_bytes() != source_ca.read_bytes(), "restore reused source TLS certificate")
            print("PASS reopened published restore independently with identical typed rows, metadata, "
                  "procedure and transaction history", flush=True)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--mysqld", type=Path, default=BIN / "mysqld.exe")
    parser.add_argument("--mysql", type=Path, default=BIN / "mysql.exe")
    parser.add_argument("--basedir", type=Path, default=BASEDIR)
    parser.add_argument("--plugin", type=Path, default=PLUGIN)
    args = parser.parse_args()
    fixture = Fixture(args.mysqld.resolve(strict=True), args.mysql.resolve(strict=True),
                      args.basedir.resolve(strict=True), SHELL, None)
    verify(fixture, args.plugin.resolve(strict=True))


if __name__ == "__main__":
    main()
