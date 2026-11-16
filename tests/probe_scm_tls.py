"""Prove a running, isolated Windows Service fixture via normal TLS RPCs."""
import hashlib
import hmac
import json
from pathlib import Path
import ssl
import sys

from setup_lifecycle_fixture import marker, seed
from sql_rpc import client

def main():
    port, ca, operation, verifier_path = sys.argv[1:]
    password = sys.stdin.buffer.readline().removeprefix(b"\xef\xbb\xbf").rstrip(b"\r\n")
    if len(password) != 44:
        raise RuntimeError("Fixture password was damaged during the test process handoff")
    credentials = json.loads(Path(verifier_path).read_text(encoding="utf-8"))
    expected = bytes.fromhex(credentials["verifier"])
    salt = bytes.fromhex(credentials["salt"])
    iterations = credentials["iterations"]
    if not hmac.compare_digest(hashlib.pbkdf2_hmac("sha256", password, salt, iterations), expected):
        if hmac.compare_digest(hashlib.pbkdf2_hmac("sha256", b"\xef\xbb\xbf" + password, salt, iterations), expected):
            raise RuntimeError("Bootstrap included a UTF-8 byte order mark in the stored password")
        raise RuntimeError("Bootstrap password does not match the recorded verifier")
    context = ssl.create_default_context(cafile=ca)
    with client(int(port), context, password.decode("ascii")) as session:
        {"seed": seed, "verify": marker}[operation](session)
    print("TLS, authentication, SQL marker and History verified")

if __name__ == "__main__":
    main()

