"""Create and restore a verified local physical InnoDB archive on Windows."""

from __future__ import annotations

import argparse
from contextlib import contextmanager
from dataclasses import dataclass
from datetime import datetime, timezone
import hashlib
import json
import os
from pathlib import Path
import re
import shutil
import socket
import stat
import subprocess
import sys
import tempfile
import time
from uuid import UUID


FORMAT = "sdp-mysql-physical-v1"
HOST = "127.0.0.1"
HIDDEN = getattr(subprocess, "CREATE_NO_WINDOW", 0)
APP_TABLES = """TABLE_TYPE='BASE TABLE' AND
    TABLE_SCHEMA NOT IN ('mysql','sys','information_schema','performance_schema')"""


def require(condition, message):
    if not condition:
        raise ValueError(message)


def plain_path(path, directory):
    info = path.lstat()
    require(not info.st_file_attributes & stat.FILE_ATTRIBUTE_REPARSE_POINT,
            f"linked paths are unsupported: {path}")
    require(stat.S_ISDIR(info.st_mode) if directory else stat.S_ISREG(info.st_mode),
            f"unexpected path type: {path}")
    require(directory or info.st_nlink == 1, f"hard-linked files are unsupported: {path}")
    return path


def local_directory(path):
    absolute = Path(os.path.abspath(path))
    require(bool(absolute.drive) and not absolute.drive.startswith("\\\\"),
            "a local Windows drive is required")
    for parent in reversed((absolute, *absolute.parents)):
        plain_path(parent, True)
    return absolute.resolve(strict=True)


def image_index(image):
    entries = {}
    for directory, folders, files in os.walk(plain_path(image, True), onerror=raise_walk_error):
        directory = Path(directory)
        for name in folders:
            path = plain_path(directory / name, True)
            entries[path.relative_to(image).as_posix()] = {"directory": True}
        for name in files:
            path = plain_path(directory / name, False)
            with path.open("rb") as stream:
                entries[path.relative_to(image).as_posix()] = {
                    "bytes": os.fstat(stream.fileno()).st_size,
                    "sha256": hashlib.file_digest(stream, "sha256").hexdigest(),
                }
    require({"ibdata1", "mysql.ibd", "#innodb_redo"} <= entries.keys(),
            "missing system tablespace, data dictionary or redo directory")
    require(not any(name in entries for name in ("auto.cnf", "mysqld-auto.cnf"))
            and not any(name.lower().endswith(".pem") for name in entries if "/" not in name),
            "unexpected server identity, configuration or key file in clone")
    return entries


def raise_walk_error(error):
    raise error


def manifest_read(archive, *, source_uuid=None):
    plain_path(archive, True)
    require({path.name for path in archive.iterdir()} == {"data", "manifest.json"},
            "archive layout does not match the expected physical format")
    path = plain_path(archive / "manifest.json", False)
    manifest = json.loads(path.read_text(encoding="utf-8"))
    require(isinstance(manifest, dict) and manifest.get("format") == FORMAT
            and manifest.get("id") == archive.name, "unexpected physical archive manifest")
    source_id, probe_id, checked, validated = (manifest.get(key) for key in
                                                ("source_uuid", "probe_uuid", "checked_tables", "validated_utc"))
    require(isinstance(source_id, str) and isinstance(probe_id, str) and UUID(source_id) != UUID(probe_id)
            and type(checked) is int and checked >= 0 and isinstance(validated, str)
            and datetime.fromisoformat(validated).utcoffset() == timezone.utc.utcoffset(None),
            "missing independent restore result")
    require(source_uuid is None or source_id == source_uuid, "archive belongs to a different source Server")
    settings = manifest.get("settings")
    require(isinstance(settings, dict)
            and isinstance(settings.get("version"), str)
            and settings["version"].endswith("-StarDataParadise")
            and settings.get("page_size") in (4096, 8192, 16384, 32768, 65536)
            and settings.get("lower_case_table_names") in (0, 1, 2),
            "unsupported source Server settings")
    require(manifest.get("files") == image_index(archive / "data"),
            "physical archive files do not match the SHA-256 manifest")
    return manifest


@dataclass(frozen=True)
class Runtime:
    mysqld: Path
    mysql: Path
    basedir: Path
    plugin_dir: Path
    user: str
    client_options: Path | None

    def environment(self):
        blocked = {"MYSQL_PWD", "MYSQL_HOME", "MYSQL_TCP_PORT", "MYSQL_TEST_LOGIN_FILE", "MYSQL_GROUP_SUFFIX"}
        env = {key: value for key, value in os.environ.items() if key.upper() not in blocked}
        if self.mysqld.parent.parent == self.basedir / "runtime_output_directory":
            libraries = self.basedir / "library_output_directory" / self.mysqld.parent.name
            if libraries.is_dir():
                env["PATH"] = os.pathsep.join(filter(None, (str(libraries), env.get("PATH"))))
        return env

    def sql(self, port, ca, statement, *, timeout=60):
        defaults = (f"--defaults-file={self.client_options}" if self.client_options else "--no-defaults")
        command = [str(self.mysql), defaults, "--no-login-paths", "--protocol=TCP",
                   f"--host={HOST}", f"--port={port}", f"--user={self.user}",
                   "--ssl-mode=VERIFY_CA", f"--ssl-ca={ca}", "--connect-timeout=2",
                   "--batch", "--raw", "--silent", "--skip-column-names", "--binary-mode",
                   "--default-character-set=utf8mb4"]
        result = subprocess.run(command, input=statement, env=self.environment(), capture_output=True,
                                text=True, encoding="utf-8", errors="replace", timeout=timeout,
                                creationflags=HIDDEN)
        require(result.returncode == 0, result.stderr.strip() or "classic SQL client failed")
        return [line.split("\t") for line in result.stdout.splitlines()]

    def identity(self, port, ca):
        [(datadir, version, server_uuid, page_size, case), (cipher, encrypted)] = self.sql(
            port, ca, "SELECT @@datadir,@@version,@@server_uuid,@@innodb_page_size,"
                      "@@lower_case_table_names; SHOW SESSION STATUS LIKE 'Ssl_cipher';")
        require(version.endswith("-StarDataParadise") and cipher == "Ssl_cipher" and encrypted,
                "a StarDataParadise Server with CA-verified TLS is required")
        settings = {"version": version, "page_size": int(page_size), "lower_case_table_names": int(case)}
        return Path(datadir).resolve(strict=True), str(UUID(server_uuid)), settings


def ensure_compatible_tables(runtime, port, ca):
    [(other_engines, encrypted, redo_encrypt, undo_encrypt)] = runtime.sql(port, ca, f"""
        SELECT (SELECT COUNT(*) FROM INFORMATION_SCHEMA.TABLES
                   WHERE {APP_TABLES} AND ENGINE <> 'InnoDB'),
               (SELECT COUNT(*) FROM INFORMATION_SCHEMA.INNODB_TABLESPACES WHERE ENCRYPTION='Y'),
               @@innodb_redo_log_encrypt, @@innodb_undo_log_encrypt;
    """)
    require(other_engines == "0", "application tables must use InnoDB")
    require(encrypted == redo_encrypt == undo_encrypt == "0", "a keyring-dependent snapshot is unsupported")


def ensure_compatible_layout(runtime, port, ca, datadir):
    [(files, *locations)] = runtime.sql(port, ca, """
        SELECT @@innodb_data_file_path, COALESCE(@@innodb_data_home_dir, ''),
            COALESCE(@@innodb_undo_directory, ''), COALESCE(@@innodb_doublewrite_dir, '');
    """)
    require(files == "ibdata1:12M:autoextend", "a default InnoDB system tablespace layout is required")
    require(all(not location or (datadir / location).resolve() == datadir for location in locations),
            "InnoDB system, undo and doublewrite storage must be in the source data directory")


def check_tables(runtime, port, ca):
    entries = runtime.sql(port, ca, f"""SELECT HEX(TABLE_SCHEMA), HEX(TABLE_NAME)
        FROM INFORMATION_SCHEMA.TABLES WHERE {APP_TABLES} ORDER BY TABLE_SCHEMA,TABLE_NAME;""")
    quoted = [".".join("`" + bytes.fromhex(part).decode("utf-8").replace("`", "``") + "`" for part in row)
              for row in entries]
    for start in range(0, len(quoted), 50):
        tables = quoted[start:start + 50]
        result = runtime.sql(port, ca, f"CHECK TABLE {','.join(tables)} QUICK;", timeout=180)
        require(len(result) == len(tables) and all(row[2:] == ["status", "OK"] for row in result),
                "InnoDB CHECK TABLE failed on the restored copy: " + repr(result))
    return len(quoted)


def ephemeral_port():
    with socket.socket(socket.AF_INET, socket.SOCK_STREAM) as listener:
        listener.bind((HOST, 0))
        return listener.getsockname()[1]


def startup_identity(runtime, process, pid, port, data, expected, source_uuid):
    deadline = time.monotonic() + 90
    ca = data / "ca.pem"
    detail = ""
    while time.monotonic() < deadline and process.poll() is None:
        if pid.is_file() and ca.is_file():
            require(pid.read_text(encoding="ascii").strip() == str(process.pid),
                    "unexpected restore probe PID")
            try:
                datadir, server_uuid, settings = runtime.identity(port, ca)
            except (ValueError, OSError, subprocess.TimeoutExpired) as error:
                detail = str(error)
            else:
                require(datadir == data and server_uuid != source_uuid and settings == expected,
                        "restore probe must have its own datadir and UUID with matching Server settings")
                return server_uuid
        time.sleep(0.2)
    raise ValueError(f"restored Server did not become ready ({process.poll()}): {detail}")


@contextmanager
def start_probe(runtime, data, root, settings, source_uuid):
    port = ephemeral_port()
    pid = root / "mysqld.pid"
    log = root / "mysqld.log"
    common = [str(runtime.mysqld), "--no-defaults", "--console", "--no-monitor",
              f"--basedir={runtime.basedir}", f"--plugin-dir={runtime.plugin_dir}",
              f"--datadir={data}", f"--tmpdir={root}", f"--pid-file={pid}",
              f"--bind-address={HOST}", f"--port={port}",
              f"--innodb-page-size={settings['page_size']}",
              f"--lower-case-table-names={settings['lower_case_table_names']}",
              "--skip-log-bin", "--skip-replica-start", "--loose-mysqlx=OFF", "--upgrade=NONE",
              "--loose-group-replication-start-on-boot=OFF", "--innodb-force-recovery=0",
              "--event-scheduler=OFF", "--persisted-globals-load=OFF",
              "--secure-file-priv=NULL", "--require-secure-transport=ON"]
    try:
        with log.open("xb") as output:
            process = subprocess.Popen(common, stdin=subprocess.DEVNULL, stdout=output,
                                       stderr=subprocess.STDOUT, env=runtime.environment(), creationflags=HIDDEN)
            ready = False
            try:
                server_uuid = startup_identity(runtime, process, pid, port, data, settings, source_uuid)
                ready = True
                yield port, data / "ca.pem", server_uuid
            finally:
                try:
                    if ready:
                        require(process.poll() is None, "restored Server exited before clean shutdown")
                        runtime.sql(port, data / "ca.pem", "SHUTDOWN;", timeout=20)
                        require(process.wait(timeout=30) == 0, "restored Server shutdown failed")
                finally:
                    if process.poll() is None:
                        process.kill()
                        process.wait(timeout=15)
    except (ValueError, OSError, subprocess.SubprocessError) as error:
        if log.is_file():
            tail = log.read_text(encoding="utf-8", errors="replace").splitlines()[-18:]
            raise ValueError(f"{error}\n" + "\n".join(tail)) from error
        raise


def probe_image(runtime, data, root, settings, source_uuid):
    with start_probe(runtime, data, root, settings, source_uuid) as (port, ca, uuid):
        ensure_compatible_tables(runtime, port, ca)
        return uuid, check_tables(runtime, port, ca)


def copy_image(source, destination, files):
    shutil.copytree(source, destination)
    require(image_index(destination) == files, "copied image does not match the SHA-256 manifest")


def create(runtime, archive, port, ca):
    datadir, source_uuid, settings = runtime.identity(port, ca)
    require(not archive.is_relative_to(datadir) and not datadir.is_relative_to(archive.parent),
            "archive root must be separate from the source data directory")

    def existing():
        manifest = manifest_read(archive)
        require(manifest["source_uuid"] == source_uuid and manifest["settings"] == settings,
                "archive ID belongs to a different source Server")
        return manifest

    if archive.exists() or archive.is_symlink():
        return existing()

    ensure_compatible_tables(runtime, port, ca)
    ensure_compatible_layout(runtime, port, ca, datadir)
    with tempfile.TemporaryDirectory(prefix=".physical-", dir=archive.parent) as scratch:
        pending = Path(scratch) / archive.name
        pending.mkdir()
        data = pending / "data"
        statement_path = data.as_posix().replace("'", "''")
        runtime.sql(port, ca, f"CLONE LOCAL DATA DIRECTORY '{statement_path}';", timeout=None)
        files = image_index(data)
        probe_root = Path(scratch) / "probe"
        probe_root.mkdir()
        probe_copy = probe_root / "data"
        copy_image(data, probe_copy, files)
        probe_uuid, table_count = probe_image(runtime, probe_copy, probe_root, settings, source_uuid)
        manifest = {"format": FORMAT, "id": archive.name, "source_uuid": source_uuid, "probe_uuid": probe_uuid,
                    "settings": settings, "checked_tables": table_count,
                    "validated_utc": datetime.now(timezone.utc).isoformat(), "files": files}
        with (pending / "manifest.json").open("x", encoding="utf-8", newline="\n") as stream:
            json.dump(manifest, stream, indent=2, sort_keys=True)
            stream.write("\n")
            stream.flush()
            os.fsync(stream.fileno())
        try:
            pending.rename(archive)
        except FileExistsError:
            return existing()
    return manifest


def restore(runtime, archive, target, *, source_uuid=None):
    manifest = manifest_read(archive, source_uuid=source_uuid)
    require(not target.is_relative_to(archive.parent) and not archive.is_relative_to(target),
            "restore target must be separate from the archive root")
    require(not target.exists() and not target.is_symlink(), "restore target already exists")
    require(not any((parent / "ibdata1").exists() and (parent / "mysql.ibd").exists()
                    for parent in target.parents), "restore target is inside a Server data directory")
    with tempfile.TemporaryDirectory(prefix=".restore-", dir=target.parent) as scratch:
        root = Path(scratch)
        data = root / "data"
        copy_image(archive / "data", data, manifest["files"])
        uuid, checked = probe_image(runtime, data, root, manifest["settings"], manifest["source_uuid"])
        require(checked == manifest["checked_tables"], "restored table count differs from the verified archive")
        data.rename(target)
    return {"id": archive.name, "target": str(target), "target_uuid": uuid, "checked_tables": checked}


def port_number(text):
    port = int(text)
    if not 1 <= port <= 65535:
        raise argparse.ArgumentTypeError("port must be between 1 and 65535")
    return port


def archive_id(text):
    if not re.fullmatch(r"[a-f0-9]{32}", text):
        raise argparse.ArgumentTypeError("physical archive ID must be 32 lowercase hexadecimal digits")
    return text


def parse_args():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--archive-root", type=Path, required=True)
    commands = parser.add_subparsers(dest="action", required=True)
    for action in ("create", "verify", "restore"):
        command = commands.add_parser(action)
        command.add_argument("archive_id", type=archive_id)
        if action != "verify":
            for name in ("mysqld", "mysql", "basedir", "plugin-dir"):
                command.add_argument(f"--{name}", type=Path, required=True)
            command.add_argument("--user", required=True)
            command.add_argument("--client-options", type=Path, help="MySQL option file containing the client password")
        if action == "create":
            command.add_argument("--source-port", type=port_number, required=True)
            command.add_argument("--source-ca", type=Path, required=True)
        if action == "restore":
            command.add_argument("--target", type=Path, required=True)
    return parser.parse_args()


def main():
    args = parse_args()
    try:
        require(os.name == "nt", "this physical backup tool is supported on Windows")
        archive = local_directory(args.archive_root) / args.archive_id
        if args.action == "verify":
            result = manifest_read(archive)
        else:
            runtime = Runtime(plain_path(args.mysqld.resolve(strict=True), False),
                              plain_path(args.mysql.resolve(strict=True), False),
                              local_directory(args.basedir), local_directory(args.plugin_dir),
                              args.user, args.client_options.resolve(strict=True) if args.client_options else None)
            if args.action == "create":
                result = create(runtime, archive, args.source_port, args.source_ca.resolve(strict=True))
            else:
                target = local_directory(args.target.parent) / args.target.name
                result = restore(runtime, archive, target)
        summary = {key: value for key, value in result.items() if key != "files"}
        print("SDP_PHYSICAL_BACKUP " + json.dumps(summary, ensure_ascii=False))
        return 0
    except (OSError, ValueError, TypeError, subprocess.SubprocessError) as error:
        print(f"physical backup: {error}", file=sys.stderr)
        return 1


if __name__ == "__main__":
    sys.exit(main())
