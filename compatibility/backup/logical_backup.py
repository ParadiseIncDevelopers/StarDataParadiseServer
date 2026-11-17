"""Create, check and restore a local schema archive using the active MySQL Shell."""

import argparse
import hashlib
import json
import os
from pathlib import Path
import re
import sys
import tempfile


FORMAT = "sdp-mysql-logical-v1"
REQUIRED_FILES = {"@.json", "@.done.json", "@.checksums.json"}
SYSTEM_SCHEMAS = {"mysql", "information_schema", "performance_schema", "sys"}


def local_path(path, directory):
    if path.is_symlink() or path.is_junction():
        raise ValueError(f"linked archive paths are unsupported: {path.name}")
    if not (path.is_dir() if directory else path.is_file()):
        raise ValueError(f"expected {'directory' if directory else 'file'}: {path.name}")
    return path


def index_dump(directory):
    entries = {}
    for path in sorted(local_path(directory, True).iterdir()):
        with local_path(path, False).open("rb") as stream:
            length = os.fstat(stream.fileno()).st_size
            digest = hashlib.file_digest(stream, "sha256").hexdigest()
        entries[path.name] = {"bytes": length, "sha256": digest}
    if not REQUIRED_FILES <= entries.keys():
        raise ValueError("the dump is missing completion or checksum metadata")
    return entries


def read_manifest(archive):
    local_path(archive, True)
    path = local_path(archive / "manifest.json", False)
    manifest = json.loads(path.read_text(encoding="utf-8"))
    if (not isinstance(manifest, dict) or manifest.get("format") != FORMAT
            or manifest.get("id") != archive.name
            or not isinstance(manifest.get("source_uuid"), str)
            or not isinstance(manifest.get("schema"), str)):
        raise ValueError("unexpected archive manifest")
    schema_name(manifest["schema"])
    if manifest.get("files") != index_dump(archive / "dump"):
        raise ValueError("archive files do not match the manifest")
    return manifest


def schema_name(value):
    if not value or len(value) > 64 or value.casefold() in SYSTEM_SCHEMAS:
        raise ValueError("an application schema is required")
    return value


def archive_id(value):
    if not re.fullmatch(r"[0-9a-f]{32}", value):
        raise ValueError("the archive ID must be 32 lowercase hexadecimal characters")
    return value


def shell_session():
    from mysqlsh import globals as shell_globals

    session = shell_globals.shell.get_session()
    if session is None or not session.is_open():
        raise ValueError("connect MySQL Shell to the server before running this command")
    row = session.run_sql("SHOW SESSION STATUS LIKE 'Ssl_cipher'").fetch_one()
    if row is None or not row[1]:
        raise ValueError("a TLS connection is required")
    return session, session.run_sql("SELECT @@server_uuid").fetch_one()[0]


def create(root, archive, schema):
    from mysqlsh import globals as shell_globals

    _, source_uuid = shell_session()

    def existing():
        manifest = read_manifest(archive)
        if manifest["schema"] != schema or manifest["source_uuid"] != source_uuid:
            raise ValueError("archive ID is already bound to a different source or schema")
        return manifest

    if archive.exists() or archive.is_symlink():
        return existing()

    with tempfile.TemporaryDirectory(prefix=".backup-", dir=root) as scratch:
        pending = Path(scratch) / archive.name
        pending.mkdir()
        dump = pending / "dump"
        shell_globals.util.dump_schemas([schema], str(dump), {
            "consistent": True, "checksum": True, "showProgress": False, "threads": 1,
        })
        manifest = {"format": FORMAT, "id": archive.name, "schema": schema,
                    "source_uuid": source_uuid, "files": index_dump(dump)}
        with (pending / "manifest.json").open("x", encoding="utf-8", newline="\n") as stream:
            json.dump(manifest, stream, indent=2, sort_keys=True)
            stream.write("\n")
        try:
            pending.rename(archive)
        except OSError:
            if not archive.exists():
                raise
            return existing()
    return manifest


def restore(archive):
    from mysqlsh import globals as shell_globals

    manifest = read_manifest(archive)
    schema = manifest["schema"]
    session, target_uuid = shell_session()
    if target_uuid == manifest["source_uuid"]:
        raise ValueError("restore requires an independent target server")

    lock = hashlib.sha256(f"sdp-logical-restore:{schema.casefold()}".encode()).hexdigest()
    if session.run_sql("SELECT GET_LOCK(?, 0)", [lock]).fetch_one()[0] != 1:
        raise ValueError("another restore holds the target schema lock")
    try:
        if session.run_sql("SELECT 1 FROM INFORMATION_SCHEMA.SCHEMATA WHERE SCHEMA_NAME = ?",
                           [schema]).fetch_one() is not None:
            raise ValueError("the target schema already exists")
        shell_globals.util.load_dump(str(archive / "dump"), {
            "checksum": True, "loadUsers": False, "showProgress": False, "threads": 1,
            "progressFile": "",
        })
    finally:
        session.run_sql("SELECT RELEASE_LOCK(?)", [lock])
    return target_uuid


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--archive-root", type=Path, required=True)
    parser.add_argument("command", choices=("create", "verify", "restore"))
    parser.add_argument("archive_id")
    parser.add_argument("--schema", help="application schema for create")
    args = parser.parse_args(argv)
    try:
        root = local_path(args.archive_root.absolute(), True).resolve(strict=True)
        archive = root / archive_id(args.archive_id)
        if (args.command == "create") != (args.schema is not None):
            raise ValueError("--schema is required for create only")
        result = {"action": args.command, "archive_id": archive.name}
        if args.command == "create":
            result["schema"] = create(root, archive, schema_name(args.schema))["schema"]
        elif args.command == "verify":
            result["schema"] = read_manifest(archive)["schema"]
        else:
            result["target_uuid"] = restore(archive)
        print("SDP_BACKUP " + json.dumps(result, ensure_ascii=False, sort_keys=True))
    except (OSError, ValueError) as error:
        print(f"backup: {error}", file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
