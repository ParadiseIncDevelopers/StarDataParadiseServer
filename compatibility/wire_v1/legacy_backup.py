"""Read and validate physical archive aliases through the legacy admin RPC."""

from __future__ import annotations

from dataclasses import dataclass, field
from datetime import datetime
import hashlib
import json
import os
from pathlib import Path
import re
import subprocess
import sys
import tempfile
from threading import Lock

from sql import Fault, require

# service.py can be run directly from a working directory outside the source tree.
sys.path.insert(1, str(Path(__file__).resolve().parents[2]))
from compatibility.backup import physical_backup as physical


OPERATIONS = frozenset({"backup_catalog", "backup_verify", "backup_restore"})
ARCHIVE_ID = re.compile(r"[0-9a-f]{32}")
TARGET_NAME = re.compile(r"[a-z0-9_-]{1,32}")
ACCOUNT = re.compile(r"[A-Za-z0-9_.-]{1,32}@[^\x00-\x20@]{1,255}")
CATALOG_LIMIT = 16


@dataclass(frozen=True)
class BackupConfig:
    archive_root: Path
    work_root: Path
    administrators: frozenset[str]
    runtime: physical.Runtime
    allow_restore: bool
    busy: Lock = field(default_factory=Lock, repr=False)


def configured_path(value, directory):
    if not isinstance(value, str) or not Path(value).is_absolute():
        raise ValueError("backup configuration paths must be absolute")
    path = Path(value)
    if directory:
        return physical.local_directory(path)
    return physical.plain_path(physical.local_directory(path.parent) / path.name, False)


def load_config(path):
    if os.name != "nt":
        raise ValueError("the physical backup adapter requires Windows")
    path = configured_path(str(Path(path).absolute()), False)
    options = json.loads(path.read_text(encoding="utf-8"))
    required = {"archive_root", "work_root", "administrators", "runtime"}
    if not isinstance(options, dict) or not required <= options.keys() <= required | {"allow_restore"}:
        raise ValueError("invalid backup configuration fields")
    administrators = options["administrators"]
    if (not isinstance(administrators, list) or not administrators
            or any(not isinstance(account, str) or not ACCOUNT.fullmatch(account) for account in administrators)):
        raise ValueError("expected exact MySQL administrator accounts in username@host form")
    allow_restore = options.get("allow_restore", False)
    if type(allow_restore) is not bool:
        raise ValueError("allow_restore must be a boolean")
    archive_root = configured_path(options["archive_root"], True)
    work_root = configured_path(options["work_root"], True)
    if archive_root.is_relative_to(work_root) or work_root.is_relative_to(archive_root):
        raise ValueError("physical archive and work roots must be independent")
    for path in (archive_root, work_root):
        if any((parent / "ibdata1").exists() and (parent / "mysql.ibd").exists()
               for parent in (path, *path.parents)):
            raise ValueError("physical backup roots must be outside Server data directories")
    runtime = options["runtime"]
    required = {"mysqld", "mysql", "basedir", "plugin_dir", "user"}
    if (not isinstance(runtime, dict) or not required <= runtime.keys() <= required | {"client_options"}
            or not isinstance(runtime["user"], str) or not re.fullmatch(r"[A-Za-z0-9_.-]{1,32}", runtime["user"])):
        raise ValueError("invalid backup runtime configuration")
    client_options = runtime.get("client_options")
    tools = physical.Runtime(configured_path(runtime["mysqld"], False), configured_path(runtime["mysql"], False),
                             configured_path(runtime["basedir"], True), configured_path(runtime["plugin_dir"], True),
                             runtime["user"], configured_path(client_options, False) if client_options else None)
    return BackupConfig(archive_root, work_root, frozenset(administrators), tools, allow_restore)


def manifest_digest(archive):
    with physical.plain_path(archive / "manifest.json", False).open("rb") as stream:
        return hashlib.file_digest(stream, "sha256").hexdigest()


def check_archive(archive, source_uuid):
    physical.plain_path(archive, True)
    digest = manifest_digest(archive)
    manifest = physical.manifest_read(archive, source_uuid=source_uuid)
    if digest != manifest_digest(archive):
        raise ValueError("archive manifest changed during validation")
    return manifest, {"backup_id": manifest["id"], "verified": True,
                      "verified_at": int(datetime.fromisoformat(manifest["validated_utc"]).timestamp()),
                      "manifest_sha256": digest, "format": physical.FORMAT, "lsn": None}


def catalog(config, source_uuid):
    archives = []
    for archive in config.archive_root.iterdir():
        if ARCHIVE_ID.fullmatch(archive.name):
            archives.append(archive)
            require(len(archives) <= CATALOG_LIMIT, 1014)
    entries = []
    for archive in sorted(archives):
        try:
            _, reply = check_archive(archive, source_uuid)
        except (OSError, ValueError):
            reply = {"backup_id": archive.name, "verified": False, "reason": "archive_invalid"}
        entries.append(reply)
    return {"backups": entries, "pitr": False}


def reverify(config, archive, manifest):
    with tempfile.TemporaryDirectory(prefix=".reverify-", dir=config.work_root) as scratch:
        root = Path(scratch)
        data = root / "data"
        physical.copy_image(archive / "data", data, manifest["files"])
        _, tables = physical.probe_image(config.runtime, data, root, manifest["settings"], manifest["source_uuid"])
        if tables != manifest["checked_tables"]:
            raise ValueError("verified application table count changed")


def dispatch(session, operation, body, config):
    require(config is not None and operation in OPERATIONS, 1004)
    require(session.identity is not None and session.identity[1] in config.administrators, 1006)
    require(session.result is None and not session.db.in_transaction, 1013)
    restore = operation == "backup_restore"
    if operation == "backup_catalog":
        require(not body)
    else:
        require(body.keys() == ({"backup_id", "target_name"} if restore else {"backup_id"}))
        archive_id = body["backup_id"]
        require(isinstance(archive_id, str) and ARCHIVE_ID.fullmatch(archive_id))
        if restore:
            require(config.allow_restore, 1004)
            target = body["target_name"]
            require(isinstance(target, str) and TARGET_NAME.fullmatch(target) and not Path(target).is_reserved())

    require(config.busy.acquire(blocking=False), 1014)
    try:
        source_uuid = session.identity[0].lower()
        if operation == "backup_catalog":
            return catalog(config, source_uuid)
        archive = config.archive_root / archive_id
        manifest, answer = check_archive(archive, source_uuid)
        if restore:
            physical.restore(config.runtime, archive, config.work_root / target, source_uuid=source_uuid)
            return {**answer, "restored": True, "target_name": target}
        reverify(config, archive, manifest)
        return answer
    except (OSError, ValueError, subprocess.SubprocessError):
        raise Fault(1013) from None
    finally:
        config.busy.release()
