"""Windows loopback TLS and foreground process acceptance tests."""

from contextlib import contextmanager
import ctypes
from ctypes import wintypes
import hashlib
import json
import os
from pathlib import Path
import queue
import secrets
import socket
import ssl
import struct
import subprocess
import tempfile
import threading
import time

import argparse


def require(condition, message):
    if not condition:
        raise RuntimeError(message)


def openssl(tool, root, *args):
    env = {**os.environ, "OPENSSL_CONF": str(root / "openssl.cnf")}
    result = subprocess.run([tool, *args], cwd=root, env=env, capture_output=True, timeout=20)
    require(result.returncode == 0, "test TLS material generation failed")


def certificates(tool, root):
    (root / "openssl.cnf").write_text("[ req ]\ndistinguished_name = dn\n[ dn ]\n", encoding="ascii")
    openssl(tool, root, "req", "-x509", "-newkey", "rsa:2048", "-nodes",
            "-keyout", "ca.key", "-out", "ca.pem", "-days", "2", "-subj", "/CN=SDP local test CA",
            "-addext", "basicConstraints=critical,CA:TRUE", "-addext", "keyUsage=critical,keyCertSign,cRLSign")
    openssl(tool, root, "req", "-new", "-newkey", "rsa:2048", "-nodes",
            "-keyout", "server.key", "-out", "server.csr", "-subj", "/CN=localhost")
    (root / "server.ext").write_text(
        "subjectAltName=DNS:localhost,IP:127.0.0.1\n"
        "extendedKeyUsage=serverAuth\nbasicConstraints=critical,CA:FALSE\n",
        encoding="ascii",
    )
    openssl(tool, root, "x509", "-req", "-in", "server.csr", "-CA", "ca.pem", "-CAkey", "ca.key",
            "-CAcreateserial", "-out", "server.pem", "-days", "2", "-extfile", "server.ext")


def save_config(root, file="server.json", **overrides):
    settings = dict(data_dir="data", log_dir="log", temp_dir="temp",
                    tls_certificate="server.pem", tls_private_key="server.key",
                    port=0, timeout_ms=1800, max_connections=2)
    settings.update(overrides)
    path = root / file
    path.write_text(json.dumps(settings), encoding="utf-8")
    return path


def execute(server, config, *args, stdin=None):
    return subprocess.run([server, "--config", str(config), *args], input=stdin,
                          stdout=subprocess.PIPE, stderr=subprocess.PIPE, timeout=10)


@contextmanager
def listening(server, config):
    process = subprocess.Popen([server, "--config", str(config)], stdout=subprocess.PIPE,
                               stderr=subprocess.PIPE, creationflags=subprocess.CREATE_NEW_CONSOLE)
    output = queue.Queue(maxsize=1)
    reader = threading.Thread(target=lambda: output.put(process.stdout.readline()), daemon=True)
    reader.start()
    try:
        try:
            announcement = output.get(timeout=12).decode("ascii").strip().split()
        except queue.Empty as error:
            raise RuntimeError("server did not report readiness") from error
        require(len(announcement) == 3 and announcement[:2] == ["LISTENING", "127.0.0.1"],
                "server did not bind loopback")
        yield process, int(announcement[2])
    finally:
        if process.poll() is None:
            process.kill()
        process.communicate(timeout=5)
        reader.join(timeout=5)


def send_ctrl_c(pid):
    kernel = ctypes.WinDLL("kernel32", use_last_error=True)
    kernel.AttachConsole.argtypes = [wintypes.DWORD]
    kernel.AttachConsole.restype = wintypes.BOOL
    kernel.FreeConsole.restype = wintypes.BOOL
    kernel.SetConsoleCtrlHandler.argtypes = [ctypes.c_void_p, wintypes.BOOL]
    kernel.SetConsoleCtrlHandler.restype = wintypes.BOOL
    kernel.GenerateConsoleCtrlEvent.argtypes = [wintypes.DWORD, wintypes.DWORD]
    kernel.GenerateConsoleCtrlEvent.restype = wintypes.BOOL

    kernel.FreeConsole()
    for _ in range(30):
        if kernel.AttachConsole(pid):
            break
        time.sleep(0.05)
    else:
        raise ctypes.WinError(ctypes.get_last_error())

    try:
        require(kernel.SetConsoleCtrlHandler(None, True), "test Ctrl+C handler registration failed")
        require(kernel.GenerateConsoleCtrlEvent(0, 0), "could not signal Ctrl+C to the isolated console")
        time.sleep(0.1)
    finally:
        kernel.FreeConsole()
        kernel.SetConsoleCtrlHandler(None, False)


def receive(sock, n):
    data = bytearray()
    while len(data) < n:
        chunk = sock.recv(n - len(data))
        require(chunk, "truncated response")
        data.extend(chunk)
    return bytes(data)


def exchange(sock, id_, op, body=None, version=1):
    payload = json.dumps(dict(v=version, id=id_, op=op, body=body or {}),
                         separators=(",", ":")).encode("utf-8")
    sock.sendall(struct.pack("!I", len(payload)) + payload)
    length, = struct.unpack("!I", receive(sock, 4))
    require(0 < length <= 1024**2, "invalid response length")
    response = json.loads(receive(sock, length))
    require(response["id"] == id_ and response["v"] == 1, "uncorrelated response")
    return response


def connection(port, context, hostname="localhost"):
    raw = socket.create_connection(("127.0.0.1", port), timeout=4)
    try:
        return context.wrap_socket(raw, server_hostname=hostname, suppress_ragged_eofs=False)
    except BaseException:
        raw.close()
        raise


def require_certificate_failure(port, context, host, desired_message):
    try:
        with connection(port, context, host):
            raise RuntimeError("bad server certificate accepted")
    except ssl.SSLCertVerificationError as error:
        require(desired_message in error.verify_message.lower(), "unexpected certificate rejection")


def authenticate(sock, password):
    require(exchange(sock, 1, "hello")["ok"], "hello failed")
    return exchange(sock, 2, "auth", {"username": "local-admin", "password": password})


def files_in(root):
    def digest(path):
        # Windows locks the first byte of this marker during server ownership.
        if path.name == ".sdp.lock" and path.stat().st_size == 0:
            return hashlib.sha256(b"").digest()
        return hashlib.sha256(path.read_bytes()).digest()

    return {path.relative_to(root): digest(path)
            for path in root.rglob("*") if path.is_file()}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--server", type=Path, required=True)
    parser.add_argument("--openssl", type=Path, required=True)
    parser.add_argument("--scratch", type=Path, required=True)
    args = parser.parse_args()
    server = str(args.server.resolve(strict=True))
    tool = str(args.openssl.resolve(strict=True))
    args.scratch.mkdir(parents=True, exist_ok=True)
    with tempfile.TemporaryDirectory(prefix="s04-", dir=args.scratch) as folder:
        root = Path(folder)
        for directory in ("data", "log", "temp", "other-data"):
            (root / directory).mkdir()
        certificates(tool, root)
        config = save_config(root)

        password = secrets.token_urlsafe(32)
        wrong = secrets.token_urlsafe(32)
        password_input = ((password + "\n") * 2).encode("ascii")
        initial = execute(server, config)
        require(initial.returncode == 1 and b"Administrator setup required" in initial.stderr,
                "server started without bootstrap")
        require(execute(server, config, "--bootstrap-admin", "local-admin", stdin=password_input).returncode == 0,
                "administrator bootstrap failed")
        stored = files_in(root / "data")
        duplicate = execute(server, config, "--bootstrap-admin", "second-admin", stdin=password_input)
        require(duplicate.returncode == 1 and b"already configured" in duplicate.stderr, "bootstrap accepted twice")
        require(files_in(root / "data") == stored, "second bootstrap changed the verifier")

        context = ssl.create_default_context(cafile=str(root / "ca.pem"))
        with listening(server, config) as (process, port):
            require_certificate_failure(port, ssl.create_default_context(), "localhost", "local issuer")
            require_certificate_failure(port, context, "other.invalid", "hostname mismatch")
            with connection(port, context) as sock:
                response = authenticate(sock, wrong)
                require(response["error"]["code"] == 1005, "wrong password accepted")
            with connection(port, context) as sock:
                require(exchange(sock, 1, "hello", version=2)["error"]["code"] == 1003, "major version accepted")
            with connection(port, context) as sock:
                require(authenticate(sock, password)["ok"], "correct password rejected")
                features = exchange(sock, 3, "capabilities")["body"]["features"]
                require(features["health"] and features["tls"] and features["auth"] and not features["sql"],
                        "incorrect capabilities")
                require(exchange(sock, 4, "health")["body"]["status"] == "ready", "health not ready")
                require(exchange(sock, 5, "query.prepare", {"sql": "SELECT 1"})["error"]["code"] == 1004,
                        "unimplemented SQL did not return unavailable")

                busy = execute(server, config)
                require(busy.returncode == 1 and b"already locked" in busy.stderr,
                        "second process took ownership of the data directory")
                invalid = save_config(root, "invalid.json", bind="0.0.0.0")
                snapshot = files_in(root / "data")
                refused = execute(server, invalid)
                require(refused.returncode == 1 and b"remote listening not enabled" in refused.stderr,
                        "invalid config was accepted")
                require(files_in(root / "data") == snapshot, "invalid config changed the data directory")
                require(exchange(sock, 6, "health")["ok"], "invalid config interfered with active process")

                other = save_config(root, "other.json", data_dir="other-data", port=port)
                require(execute(server, other, "--bootstrap-admin", "local-admin", stdin=password_input).returncode == 0,
                        "could not prepare isolated second instance")
                collision = execute(server, other)
                require(collision.returncode == 1 and b"Bind/listen failed" in collision.stderr,
                        "second process took ownership of the port")

            time.sleep(0.6)
            with connection(port, context) as first, connection(port, context) as second:
                require(exchange(first, 1, "hello")["ok"] and exchange(second, 1, "hello")["ok"],
                        "session limit broke allowed connections")
                try:
                    with connection(port, context):
                        raise RuntimeError("connection limit was not enforced")
                except (ssl.SSLError, ConnectionError):
                    pass
                require(first.recv(1) == b"", "inactive TLS session timed out without a close notification")
                require(second.recv(1) == b"", "second inactive TLS session did not time out")

            time.sleep(0.65)
            with connection(port, context) as active:
                require(authenticate(active, password)["ok"], "failed to authenticate before shutdown")
                send_ctrl_c(process.pid)
                require(active.recv(1) == b"", "active TLS connection was not closed orderly")
                require(process.wait(timeout=8) == 0, "Ctrl+C did not stop the server cleanly")

        restart = save_config(root, "restart.json", port=port)
        with listening(server, restart) as (process, same_port):
            require(port == same_port, "server failed to reuse released port")
            with connection(same_port, context) as sock:
                require(authenticate(sock, password)["ok"], "verifier did not survive restart")
            send_ctrl_c(process.pid)
            require(process.wait(timeout=8) == 0, "restarted server did not stop cleanly")

        logs = root / "log"
        auth = (logs / "auth.log").read_bytes()
        operational = (logs / "operational.log").read_bytes()
        require(b"authentication_failed" in auth and b"authentication_passed" in auth, "auth events missing")
        require(b"tls_handshake_failed" in operational and b"session_timeout" in operational
                and operational.count(b" stopped") == 2, "operational events missing")
        require(all(secret.encode("ascii") not in auth + operational for secret in (password, wrong)),
                "credential leaked in logs")
        require(all(path.stat().st_size <= 256 * 1024 for path in logs.iterdir()), "log cap exceeded")

    print("TLS trust, hostname, auth, capabilities, timeouts, process locks, Ctrl+C and restart passed")


if __name__ == "__main__":
    main()
