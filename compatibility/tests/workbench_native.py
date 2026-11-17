"""Start classic Windows Workbench and verify its native Connector/C++ against a disposable StarDataParadise fork."""

import argparse
import json
import os
from pathlib import Path
import secrets
import subprocess
import sys
import time

from .fixture import BASEDIR, BIN, HIDDEN, MIGRATION, REPOSITORY, SHELL, Fixture, query, require, run

sys.path.insert(1, str(REPOSITORY / "tests"))
from service_smoke import certificates


INSTALLED = Path("C:/Program Files/MySQL/MySQL Workbench 8.0/MySQLWorkbench.exe")
PROBE = Path(__file__).with_name("workbench_gui_probe.py")
WORKSHEET = """SELECT @@version AS server_version,
       id,
       cost,
       HEX(payload) AS payload_hex
FROM sdp_wb_gui.sample;
"""
ENVIRONMENT = {"ALLUSERSPROFILE", "APPDATA", "COMPUTERNAME", "HOMEDRIVE", "HOMEPATH", "LOCALAPPDATA",
               "NUMBER_OF_PROCESSORS", "OS", "PATH", "PROCESSOR_ARCHITECTURE", "PROCESSOR_IDENTIFIER",
               "PROGRAMDATA", "PROGRAMFILES", "PROGRAMFILES(X86)", "SYSTEMDRIVE", "SYSTEMROOT",
               "USERDOMAIN", "USERNAME", "USERPROFILE", "WINDIR"}


def backend_certificate(openssl, root, name):
    location = root / name
    location.mkdir()
    certificates(str(openssl), location)
    return location


def wait_for_result(process, result):
    deadline = time.monotonic() + 90
    while time.monotonic() < deadline:
        if result.is_file():
            try:
                return json.loads(result.read_text(encoding="utf-8"))
            except (json.JSONDecodeError, PermissionError):
                pass
        if process.poll() is not None:
            raise RuntimeError(f"Workbench exited before the GRT result ({process.returncode})")
        time.sleep(0.2)
    raise RuntimeError("the native Workbench did not produce its GRT result")


def run_gui(executable, root, settings, show):
    profile = root / "profile"
    profile.mkdir()
    temporary = root / "tmp"
    temporary.mkdir()
    result = root / "result.json"
    config = root / "gui-fixture.json"
    config.write_text(json.dumps({**settings, "result": str(result), "show": show}), encoding="utf-8")
    environment = {key: value for key, value in os.environ.items() if key.upper() in ENVIRONMENT}
    environment.update(SDP_WB_GUI_FIXTURE=str(config), TEMP=str(temporary), TMP=str(temporary))
    command = [str(executable), f"--configdir={profile}", f"--run-script={PROBE}", "--log-level=warning"]
    if not show:
        command.append("--quit-when-done")
    out = root / "workbench-process.log"
    try:
        with out.open("xb") as output, subprocess.Popen(command, cwd=executable.parent, env=environment,
                                                        stdout=output, stderr=subprocess.STDOUT,
                                                        stdin=subprocess.DEVNULL,
                                                        creationflags=0 if show else HIDDEN) as process:
            try:
                detail = wait_for_result(process, result)
                require(detail.get("ok") is True, f"Workbench GRT failed: {detail.get('error')}")
                print("PASS native Workbench GRT DbMySQLQuery using VERIFY_IDENTITY TLS on " + settings["version"],
                      flush=True)
                print("PASS wrong CA rejected, exact BIGINT/DECIMAL/BLOB, legacy ANALYZE and rollback", flush=True)
                print(f"WORKBENCH_NATIVE_GUI_PID {process.pid}", flush=True)
                config.unlink()
                if show:
                    worksheet = profile / "StarDataParadise Test.sql"
                    worksheet.write_text(WORKSHEET, encoding="utf-8")
                    print(f"WORKBENCH_NATIVE_SAMPLE_SQL {worksheet}", flush=True)
                    print("Workbench is open on a disposable TLS instance with a read-only sample account", flush=True)
                    process.wait()
                else:
                    require(process.wait(timeout=25) == 0, "native Workbench did not exit cleanly")
            finally:
                if process.poll() is None:
                    process.terminate()
                    process.wait(timeout=15)
    except (OSError, ValueError, RuntimeError, subprocess.SubprocessError):
        if out.is_file():
            for line in out.read_text(encoding="utf-8", errors="replace").splitlines()[-22:]:
                print(line.replace(settings["password"], "<redacted>"), file=sys.stderr)
        if result.is_file():
            print(result.read_text(encoding="utf-8", errors="replace"), file=sys.stderr)
        raise


def smoke(args, fixture):
    with fixture.scratch("native-gui") as root:
        cert = backend_certificate(args.openssl, root, "cert")
        wrong = backend_certificate(args.openssl, root, "other")
        with fixture.disposable(root / "database", tls_dir=cert) as source:
            env, classic, _ = source
            port, uuid, version = query(source, "SELECT @@port, @@server_uuid, @@version;").split("\t")
            password = secrets.token_hex(24)
            schema, user = "sdp_wb_gui", "wb_gui_smoke"
            initialization = f"""
                CREATE SCHEMA {schema};
                CREATE TABLE {schema}.sample(id BIGINT PRIMARY KEY, cost DECIMAL(18,4), payload BLOB) ENGINE=InnoDB;
                INSERT INTO {schema}.sample VALUES (9007199254740993,123.4500,0x00FF);
                CREATE USER '{user}'@'localhost' IDENTIFIED BY '{password}' REQUIRE SSL;
                GRANT SELECT, INSERT, UPDATE ON {schema}.sample TO '{user}'@'localhost';
            """
            if args.keep_visible:
                initialization += f"""
                    CREATE USER 'wb_gui_view'@'localhost' IDENTIFIED BY '' REQUIRE SSL;
                    GRANT SELECT ON {schema}.sample TO 'wb_gui_view'@'localhost';
                """
            setup = run(classic, env, 20, script=initialization)
            require(setup.returncode == 0, "disposable Workbench fixture initialization failed")
            run_gui(args.exe, root, {"port": int(port), "uuid": uuid, "version": version, "password": password,
                                    "schema": schema, "user": user, "viewer": "wb_gui_view", "ca": str(cert / "ca.pem"),
                                    "foreign_ca": str(wrong / "ca.pem")}, args.keep_visible)
            require(query(source, f"SELECT id,cost,HEX(payload) FROM {schema}.sample;")
                    == "9007199254740993\t123.4500\t00FF", "Workbench changed committed source content")


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--exe", type=Path, default=INSTALLED)
    parser.add_argument("--mysqld", type=Path, default=BIN / "mysqld.exe")
    parser.add_argument("--mysql", type=Path, default=BIN / "mysql.exe")
    parser.add_argument("--basedir", type=Path, default=BASEDIR)
    parser.add_argument("--openssl", type=Path, default=MIGRATION / "toolchain/openssl-firedaemon/x64/bin/openssl.exe")
    parser.add_argument("--keep-visible", action="store_true", help="keep native Workbench and scratch Server until closed")
    args = parser.parse_args()
    args.exe = args.exe.resolve(strict=True)
    args.openssl = args.openssl.resolve(strict=True)
    fixture = Fixture(args.mysqld.resolve(strict=True), args.mysql.resolve(strict=True),
                      args.basedir.resolve(strict=True), SHELL, None)
    smoke(args, fixture)


if __name__ == "__main__":
    main()
