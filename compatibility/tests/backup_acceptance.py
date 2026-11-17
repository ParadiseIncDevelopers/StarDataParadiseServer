"""Run a real logical backup and restore between two disposable Windows forks."""

import json

from .fixture import COMPATIBILITY, query, require, run


BACKUP = COMPATIBILITY / "backup" / "logical_backup.py"
SCHEMA = "sdp_backup_acceptance"
ARCHIVE_ID = "6daae84e7cdb4cbc8408116e781c6392"


def backup(instance, root, action, failure=None):
    env, _, cli = instance
    options = ["--schema", SCHEMA] if action == "create" else []
    result = run([*cli, "--file", str(BACKUP), "--archive-root", str(root), action, ARCHIVE_ID, *options], env, 90)
    output = result.stdout + result.stderr
    if failure is not None:
        require(result.returncode != 0 and failure in output, f"expected {failure!r}: {output}")
        return None
    require(result.returncode == 0, output)
    line = next((line for line in result.stdout.splitlines() if line.startswith("SDP_BACKUP ")), None)
    require(line is not None, output)
    return json.loads(line.removeprefix("SDP_BACKUP "))


def verify(fixture):
    with fixture.scratch("backup") as root:
        archives = root / "archives"
        archives.mkdir()
        with fixture.disposable(root / "source") as source, fixture.disposable(root / "target") as target:


            query(source, f"""CREATE SCHEMA {SCHEMA};
                CREATE TABLE {SCHEMA}.sample(id BIGINT PRIMARY KEY, cost DECIMAL(16,4),
                    payload BLOB, label VARCHAR(20)) ENGINE=InnoDB;
                INSERT INTO {SCHEMA}.sample VALUES (9007199254740993,123.4500,0x00FF,CONVERT(0xC3B6 USING utf8mb4)),
                    (2,NULL,NULL,'plain');""")
            contents = f"SELECT id, cost, HEX(payload), HEX(label) FROM {SCHEMA}.sample ORDER BY id"
            original = query(source, contents)
            created = backup(source, archives, "create")
            require(created == backup(source, archives, "create"), "create retry changed identity")
            backup(source, archives, "verify")
            backup(source, archives, "restore", "independent target server")
            checksum = archives / ARCHIVE_ID / "dump" / "@.checksums.json"
            untampered = checksum.read_bytes()
            checksum.write_bytes(untampered + b"\n")
            backup(target, archives, "restore", "archive files do not match")
            checksum.write_bytes(untampered)
            restore = backup(target, archives, "restore")
            require(restore["target_uuid"] != query(source, "SELECT @@server_uuid"), "restore target was not independent")
            require(query(target, contents) == original, "typed restore contents changed")
            backup(target, archives, "restore", "target schema already exists")
            print("PASS consistent logical backup, idempotent create, checksums, independent typed restore and rejections")
