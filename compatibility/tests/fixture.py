"""Manage fresh local StarDataParadise Server instances for compatibility tests."""

from __future__ import annotations

from contextlib import contextmanager
from dataclasses import dataclass
import os
from pathlib import Path
import socket
import subprocess
import sys
import tempfile
import time


REPOSITORY = Path(__file__).resolve().parents[2]
COMPATIBILITY = REPOSITORY / "compatibility"
OUT = REPOSITORY / "out"
MIGRATION = OUT / "mysql-migration"
BASEDIR = MIGRATION / "bfast"
BIN = BASEDIR / "runtime_output_directory" / "RelWithDebInfo"
SHELL = MIGRATION / "cli-b" / "RelWithDebInfo" / "bin" / "mysqlsh.exe"
WORKER = MIGRATION / "ml-b" / "Release" / "sdp_mysql_model_runner.exe"
HOST = "127.0.0.1"
HIDDEN = getattr(subprocess, "CREATE_NO_WINDOW", 0)


def require(condition, message):
    if not condition:
        raise RuntimeError(message)


def run(command, env, timeout, script=None):
    stdin = {"stdin": subprocess.DEVNULL} if script is None else {"input": script}
    return subprocess.run(command, env=env, **stdin, timeout=timeout, capture_output=True,
                          text=True, encoding="utf-8", errors="replace", creationflags=HIDDEN)


def local_port():
    with socket.socket(socket.AF_INET, socket.SOCK_STREAM) as listener:
        listener.bind((HOST, 0))
        return listener.getsockname()[1]


def mysql_client(binary, connection):
    return [str(binary), "--no-defaults", "--protocol=TCP", *connection, "--skip-password",
            "--connect-timeout=2", "--batch", "--raw", "--skip-column-names"]


def wait_ready(process, classic, root, env, timeout=90):
    deadline = time.monotonic() + timeout
    pid = root / "mysqld.pid"
    while time.monotonic() < deadline:
        require(process.poll() is None, f"test Server exited with code {process.returncode}")
        if pid.is_file():
            require(pid.read_text(encoding="ascii").strip() == str(process.pid),
                    "PID file does not identify the test Server")
            try:
                status = run([
                    *classic, "--execute=SELECT @@datadir,@@version; SHOW SESSION STATUS LIKE 'Ssl_cipher';",
                ], env, 5)
            except subprocess.TimeoutExpired:
                continue
            if status.returncode == 0:
                rows = [line.split("\t") for line in status.stdout.splitlines()]
                require(len(rows) == 2 and len(rows[0]) == 2 and len(rows[1]) == 2
                        and Path(rows[0][0]).resolve() == root / "data"
                        and rows[0][1].endswith("-StarDataParadise")
                        and rows[1][0] == "Ssl_cipher" and rows[1][1],
                        "unexpected test Server datadir, product version or TLS status")
                return
        time.sleep(0.2)
    raise RuntimeError("test Server did not become ready")


def stop(process, classic, env, ready):
    try:
        if ready:
            require(process.poll() is None, f"test Server stopped unexpectedly ({process.returncode})")
            result = run([*classic, "--execute=SHUTDOWN;"], env, 15)
            require(result.returncode == 0, f"test Server shutdown failed: {result.stderr}")
            require(process.wait(timeout=30) == 0, f"test Server exit code was {process.returncode}")
            print("PASS disposable fork shut down cleanly", flush=True)
    finally:
        if process.poll() is None:
            process.kill()
            process.wait(timeout=15)


def query(instance, sql):
    env, classic, _ = instance
    result = run([*classic, f"--execute={sql}"], env, 20)
    require(result.returncode == 0, result.stdout + result.stderr)
    return result.stdout.strip()


@dataclass(frozen=True)
class Fixture:
    mysqld: Path
    mysql: Path
    basedir: Path
    shell: Path
    worker: Path | None

    @contextmanager
    def scratch(self, name):
        OUT.mkdir(exist_ok=True)
        require(OUT.resolve() == OUT, "test output must be inside the repository out directory")
        out = OUT / "compatibility-tests"
        out.mkdir(exist_ok=True)
        require(out.resolve() == out,
                "test output must be inside the repository out directory")
        with tempfile.TemporaryDirectory(prefix=f"{name}-", dir=out) as scratch:
            root = Path(scratch).resolve()
            require(root.parent == out, "unexpected test scratch directory")
            yield root

    def environment(self, root):
        clean = {"MYSQL_PWD", "MYSQL_HOME", "MYSQL_TCP_PORT", "MYSQL_TEST_LOGIN_FILE", "PAGER"}
        env = {key: value for key, value in os.environ.items() if key.upper() not in clean}
        profile = root / "shell-profile"
        profile.mkdir()
        env["MYSQLSH_USER_CONFIG_HOME"] = str(profile)
        env["MYSQL_TEST_LOGIN_FILE"] = str(root / "disabled-login.cnf")
        if self.mysqld.parent.parent == self.basedir / "runtime_output_directory":
            libs = self.basedir / "library_output_directory" / self.mysqld.parent.name
            if libs.is_dir():
                env["PATH"] = os.pathsep.join(filter(None, (str(libs), env.get("PATH"))))
        return env

    @contextmanager
    def disposable(self, root, *, plugin_dir=None, tls_dir=None, server_options=()):
        require(root.parent.parent == OUT / "compatibility-tests", "unexpected test Server directory")
        root.mkdir()
        env = self.environment(root)
        data = root / "data"
        common = [str(self.mysqld), "--no-defaults", f"--basedir={self.basedir}", f"--datadir={data}"]
        if os.name == "nt":
            common.extend(["--console", "--no-monitor"])
        initialized = run([*common, "--initialize-insecure"], env, 120)
        require(initialized.returncode == 0, initialized.stdout + initialized.stderr)

        port = local_port()
        ca = (tls_dir if tls_dir is not None else data) / "ca.pem"
        connection = [f"--host={HOST}", f"--port={port}", "--user=root",
                      "--ssl-mode=VERIFY_CA", f"--ssl-ca={ca}"]
        classic = mysql_client(self.mysql, connection)
        cli = [str(self.shell), "--mysql", *connection, "--no-password", "--py",
               "--quiet-start=2", "--disable-plugins"]
        command = [*common, f"--bind-address={HOST}", f"--port={port}",
                   f"--pid-file={root / 'mysqld.pid'}", f"--tmpdir={root}",
                   "--skip-log-bin", "--loose-mysqlx=OFF", "--local-infile=ON",
                   "--require-secure-transport=ON"]
        if plugin_dir is not None:
            command.append(f"--plugin-dir={plugin_dir}")
        if tls_dir is not None:
            command.extend([f"--ssl-ca={ca}", f"--ssl-cert={tls_dir / 'server.pem'}",
                            f"--ssl-key={tls_dir / 'server.key'}"])
        command.extend(server_options)
        log = root / "mysqld.log"
        try:
            with log.open("wb") as output:
                process = subprocess.Popen(command, env=env, stdin=subprocess.DEVNULL,
                                           stdout=output, stderr=subprocess.STDOUT, creationflags=HIDDEN)
                ready = False
                try:
                    wait_ready(process, classic, root, env)
                    ready = True
                    yield env, classic, cli
                finally:
                    stop(process, classic, env, ready)
        except Exception:
            if log.is_file():
                lines = log.read_text(encoding="utf-8", errors="replace").splitlines()
                print("\n".join(lines[-40:]), file=sys.stderr)
            raise
