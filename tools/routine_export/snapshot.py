"""Download a verified PL/SQL routine closure over the TLS wire v1."""
import argparse
import getpass
import json
from pathlib import Path
import socket
import ssl
import struct
import sys

MAX_FRAME = 1024 * 1024

class ServerError(Exception):
    def __init__(self, detail):
        self.code = detail.get("code")
        self.line = detail.get("line")
        self.column = detail.get("column")
        self.correlation = detail.get("correlation_id")
        location = f" at {self.line}:{self.column}" if self.line else ""
        super().__init__(f"Server error {self.code}{location}")

def take(sock, length):
    buffer = bytearray(length)
    view = memoryview(buffer)
    while view:
        count = sock.recv_into(view)
        if not count:
            raise OSError("Server closed the TLS frame")
        view = view[count:]
    return buffer

def exchange(sock, request_id, operation, body):
    data = json.dumps({"v": 1, "id": request_id, "op": operation, "body": body},
                      separators=(",", ":")).encode("utf-8")
    if not 1 <= len(data) <= MAX_FRAME:
        raise ValueError("Request exceeds the wire frame limit")
    sock.sendall(struct.pack("!I", len(data)) + data)
    length, = struct.unpack("!I", take(sock, 4))
    if not 1 <= length <= MAX_FRAME:
        raise ValueError("Server returned an invalid wire frame")
    reply = json.loads(take(sock, length))
    if not isinstance(reply, dict) or reply.get("v") != 1 or reply.get("id") != request_id:
        raise ValueError("Server returned an unexpected response")
    if reply.get("ok") is not True:
        raise ServerError(reply.get("error", {}))
    if not isinstance(reply.get("body"), dict):
        raise ValueError("Server returned an invalid response body")
    return reply["body"]

def snapshot(host, port, tls_name, ca, user, secret, mode, name):
    tls = ssl.create_default_context(cafile=ca)
    tls.minimum_version = ssl.TLSVersion.TLSv1_2
    with socket.create_connection((host, port), timeout=20) as raw:
        with tls.wrap_socket(raw, server_hostname=tls_name or host) as sock:
            exchange(sock, 1, "hello", {})
            exchange(sock, 2, "auth", {"username": user, "password": secret})
            features = exchange(sock, 3, "capabilities", {})
            if features.get("features", {}).get("routine_export_v1") is not True:
                raise ValueError("Server lacks routine_export_v1")
            return exchange(sock, 4, "routine_export", {"routine": name, "mode": mode})

def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--host", default="127.0.0.1")
    parser.add_argument("--port", type=int, default=7443)
    parser.add_argument("--tls-name")
    parser.add_argument("--ca", type=Path)
    parser.add_argument("--user", required=True)
    parser.add_argument("--routine", required=True)
    parser.add_argument("--mode", choices=["standalone", "connected"], required=True)
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--password-stdin", action="store_true")
    args = parser.parse_args()
    if not 1 <= args.port <= 65535:
        parser.error("Invalid port")
    try:
        if args.password_stdin:
            password = sys.stdin.readline(1026).rstrip("\r\n")
        elif sys.stdin.isatty():
            password = getpass.getpass("Password: ")
        else:
            parser.error("Use --password-stdin with redirected input")
        if not 1 <= len(password.encode("utf-8")) <= 1024:
            parser.error("Invalid password length")
        result = snapshot(args.host, args.port, args.tls_name, args.ca, args.user, password,
                          args.mode, args.routine)
        with args.output.open("x", encoding="utf-8", newline="\n") as out:
            out.write(json.dumps(result, ensure_ascii=False, indent=2) + "\n")
    except (ValueError, OSError, ServerError) as error:
        parser.exit(1, f"Snapshot failed: {error}\n")

if __name__ == "__main__":
    main()
