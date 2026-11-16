"""Run the same StarDataParadise SQL fixture against each installed SDK."""
from __future__ import annotations

import argparse
from contextlib import contextmanager
import hashlib
import json
import math
import os
from pathlib import Path
import re
import secrets
import socket
import socketserver
import ssl
import struct
import subprocess
import sys
import tempfile
import threading
from zipfile import ZipFile

from jobs_rpc import terminal, wait_for
from model_rpc import REGRESSION_OPTIONS, activate, create_dataset, train
from service_smoke import certificates, connection, exchange, execute, listening, require, save_config
from sql_rpc import client, i64, text, token, write


CONTRACT_SHA256 = "e6500900a3fa34910781ea92d5e41fe8716511fac6f6d6c689ff9cce15d578ab"
GUEST = "sdk_guest"
FRAME_LIMIT = 1024 ** 2
DECIMAL = {"t": "decimal", "unscaled": "-123456789012345678901234567890123400", "scale": 4}
STAMP = {"t": "timestamp", "seconds": "-1", "nanos": 999999999, "offset_minutes": 180}
BINARY = {"t": "binary", "hex": "00017f80feff"}
NOTE = "İstanbul'un notu; a\0b"
BULK_ROWS = 313
OBSERVED_PREFIX = b"SDP_STAGE47_RESULT "


def seed(port, ctx, password, guest_password):
    with client(port, ctx, password) as owner:
        write(owner, "CREATE TABLE sdk_seed (id BIGINT PRIMARY KEY,note TEXT,amount DECIMAL(38,4),"
                     "stamp TIMESTAMP WITH TIME ZONE,payload BLOB)", 0)
        revision = write(owner, "INSERT INTO sdk_seed VALUES (1,:note,:amount,:stamp,:payload),"
                                "(2,NULL,NULL,NULL,NULL)", 2,
                         types={"note": {"type": "TEXT"}, "amount": {"type": "DECIMAL", "precision": 38, "scale": 4},
                                "stamp": {"type": "TIMESTAMP WITH TIME ZONE"}, "payload": {"type": "BLOB"}},
                         values={"note": text(NOTE), "amount": DECIMAL, "stamp": STAMP, "payload": BINARY})["lsn"]
        got, _ = owner.query("SELECT note,amount,stamp,payload FROM sdk_seed WHERE id=1")
        require(got == [[text(NOTE), DECIMAL, STAMP, BINARY]], "fixture typed storage did not round trip")

        write(owner, "CREATE TABLE sdk_bulk (id BIGINT PRIMARY KEY, reading BIGINT, outcome_value BIGINT)", 0)
        owner.call("tx_begin")
        for first in range(1, BULK_ROWS + 1, 32):
            values = [f"({number},{number},{3 * number + 7})"
                      for number in range(first, min(BULK_ROWS + 1, first + 32))]
            result = owner.mutate("INSERT INTO sdk_bulk VALUES " + ",".join(values))
            require(result["affected_rows"] == len(values), "fixture bulk insert lost rows")
        owner.call("tx_commit", request_token=token())

        write(owner, "CREATE PROCEDURE sdk_list(p_key IN BIGINT,p_value OUT BIGINT) AS DECLARE c REFCURSOR; "
                     "d REFCURSOR; BEGIN SELECT id INTO p_value FROM sdk_seed WHERE id = :p_key; "
                     "OPEN c FOR SELECT id FROM sdk_seed WHERE id = :p_key; EMIT c AS found; "
                     "OPEN d FOR SELECT id FROM sdk_seed ORDER BY id; EMIT d AS all_rows; END;", 0)
        write(owner, "CREATE PROCEDURE sdk_echo(p_rec IN OUT RECORD,p_values IN OUT COLLECTION) AS "
                     "BEGIN p_rec := p_rec; p_values := p_values; END;", 0)
        write(owner, "CREATE TABLE sdk_guard (id BIGINT PRIMARY KEY, amount BIGINT NOT NULL)", 0)
        write(owner, "CREATE DECISION RULE sdk_positive_amount ON sdk_guard FOR INSERT "
                     "PRIORITY 1 WHEN (NEW.amount < 0) THEN DENY", 0)
        write(owner, "CREATE USER sdk_guest", 0)
        owner.call("password_set", username=GUEST, new_password=guest_password, request_token=token())
        write(owner, "GRANT SELECT ON TABLE sdk_seed TO USER sdk_guest", 0)

        data = create_dataset(owner, "SELECT reading,outcome_value FROM sdk_bulk ORDER BY id", REGRESSION_OPTIONS)
        owner.call("model_create", request_token=token(), name="sdk_forecast", algorithm="linear")
        artifact = train(owner, "sdk_forecast", data)
        activate(owner, "sdk_forecast", artifact["version"])
        prediction = owner.call("model_predict", name="sdk_forecast", features={"reading": 17})
        require(math.isclose(prediction["prediction"], 58, abs_tol=1e-4), "fixture model inference was unexpected")
        job = owner.call("job_submit", request_token=token(), kind="compute", count=10_000)
        state = wait_for(owner, job["job_id"], terminal)
        require(state["state"] == "succeeded", "fixture compute job failed")
        return {"SDP_SDK_SEED_LSN": str(revision), "SDP_SDK_BULK_ROWS": str(BULK_ROWS),
                "SDP_SDK_MODEL": "sdk_forecast", "SDP_SDK_DATASET_ID": data["dataset_id"],
                "SDP_SDK_JOB_ID": job["job_id"]}


def receive(sock, length):
    result = bytearray()
    while len(result) < length:
        packet = sock.recv(length - len(result))
        if not packet:
            raise EOFError
        result.extend(packet)
    return bytes(result)


def frame(sock):
    header = receive(sock, 4)
    size, = struct.unpack("!I", header)
    if size < 1 or size > FRAME_LIMIT:
        raise ValueError("invalid fixture frame length")
    payload = receive(sock, size)
    return header + payload, json.loads(payload)


class DropReply(socketserver.ThreadingTCPServer):
    daemon_threads = True

    def __init__(self, port, context, fixture, op):
        super().__init__(("127.0.0.1", 0), Relay)
        self.port = port
        self.context = context
        self.server_tls = ssl.SSLContext(ssl.PROTOCOL_TLS_SERVER)
        self.server_tls.minimum_version = ssl.TLSVersion.TLSv1_2
        self.server_tls.load_cert_chain(fixture / "server.pem", fixture / "server.key")
        self.operation = op
        self.lock = threading.Lock()
        self.receipt = None
        self.failure = False

    def should_drop(self, request, reply):
        require(request["id"] == reply["id"] and request["v"] == reply["v"] == 1, "proxy correlation mismatch")
        if request["op"] != self.operation or not reply["ok"]:
            return False
        if self.operation == "query.prepare":
            if not request["body"].get("sql", "").lstrip().upper().startswith("SELECT "):
                return False
            receipt = True
        else:
            key, lsn = request["body"].get("request_token"), reply["body"].get("lsn")
            if not key or not lsn:
                return False
            receipt = (key, lsn)
        with self.lock:
            if self.receipt is not None:
                return False
            self.receipt = receipt
            return True

    def handle_error(self, request, client_address):
        # A diagnostic trace could include an arbitrary client URL or request.
        self.failure = True


class Relay(socketserver.BaseRequestHandler):
    def handle(self):
        self.request.settimeout(15)
        try:
            with self.server.server_tls.wrap_socket(self.request, server_side=True) as peer:
                with connection(self.server.port, self.server.context) as upstream:
                    upstream.settimeout(15)
                    while True:
                        unmodified, request = frame(peer)
                        upstream.sendall(unmodified)
                        unmodified, reply = frame(upstream)
                        if self.server.should_drop(request, reply):
                            # The real Server acknowledged upstream. Close before sending its reply.
                            peer.shutdown(socket.SHUT_RDWR)
                            return
                        peer.sendall(unmodified)
        except (EOFError, ssl.SSLEOFError, ConnectionResetError, BrokenPipeError):
            return


@contextmanager
def relay(port, context, fixture, op):
    if not op:
        yield None
        return
    with DropReply(port, context, fixture, op) as proxy:
        runner = threading.Thread(target=proxy.serve_forever, daemon=True)
        runner.start()
        try:
            yield proxy
        finally:
            proxy.shutdown()
            runner.join(timeout=3)


def configure(server, openssl, fixture, password, expiry_helper):
    for folder in ("data", "log", "temp", "snapshots"):
        (fixture / folder).mkdir()
    certificates(openssl, fixture)
    config = save_config(fixture, instance_name="cross-sdk-fixture", max_connections=16, timeout_ms=12_000,
                         backup="snapshots")
    expired = None
    if expiry_helper:
        helper = expiry_helper.resolve(strict=True)
        store = fixture / "data" / "store.sdp"
        require(not store.exists(), "expiry helper must initialize a fresh store")
        output = subprocess.run([helper, "--seed-sdk-expiry", store], capture_output=True, timeout=20)
        require(output.returncode == 0, "test-only persisted expiry fixture setup failed")
        expired = output.stdout.decode("ascii").strip()
        require(re.fullmatch(r"[0-9a-f]{32}", expired) is not None, "fixture returned an invalid expiry token")
    else:
        require(execute(server, config, "--initialize-storage").returncode == 0, "fixture storage init failed")
    require(execute(server, config, "--bootstrap-admin", "local-admin",
                    stdin=((password + "\n") * 2).encode("ascii")).returncode == 0, "fixture bootstrap failed")
    context = ssl.create_default_context(cafile=str(fixture / "ca.pem"))
    context.minimum_version = ssl.TLSVersion.TLSv1_2
    return config, context, expired


def load_golden(archive, fixture):
    require(hashlib.sha256(archive.read_bytes()).hexdigest() == CONTRACT_SHA256, "contract archive checksum mismatch")
    with ZipFile(archive) as packaged:
        vectors = packaged.read("golden/values.json")
    require(json.loads(vectors)["body"]["values"][0] == DECIMAL, "unexpected golden decimal")
    path = fixture / "golden-values.json"
    path.write_bytes(vectors)
    return path


def clean_output(raw, passwords):
    if not raw:
        return ""
    result = raw.decode("utf-8", errors="replace")
    for password in passwords:
        result = result.replace(password, "<redacted>")
    return result[:20_000]


def acceptance(args):
    args.scratch.mkdir(parents=True, exist_ok=True)
    with tempfile.TemporaryDirectory(prefix="managed-sdk-", dir=args.scratch.resolve()) as directory:
        fixture = Path(directory)
        golden = load_golden(args.contract.resolve(strict=True), fixture)
        password, guest_password = secrets.token_urlsafe(32), secrets.token_urlsafe(32)
        server, openssl = str(args.server.resolve(strict=True)), str(args.openssl.resolve(strict=True))
        config, context, expired = configure(server, openssl, fixture, password, args.expiry_helper)
        with listening(server, config) as (_, port):
            with connection(port, context) as peer:
                incompatible = exchange(peer, 1, "hello", version=2)
                require(not incompatible["ok"] and incompatible["error"]["code"] == 1003,
                        "unsupported protocol major did not receive an explicit version error")
            print("Real Server rejected unsupported protocol major 2 with error 1003")
            env = dict(os.environ)
            for flag in ("FAULT_PORT", "READ_FAULT_PORT", "EXPIRED_TOKEN"):
                env.pop("SDP_SDK_" + flag, None)
            env.update(SDP_SDK_HOST="127.0.0.1", SDP_SDK_PORT=str(port), SDP_SDK_TLS_NAME="localhost",
                       SDP_SDK_CA=str(fixture / "ca.pem"), SDP_SDK_USER="local-admin", SDP_SDK_GUEST=GUEST,
                       SDP_SDK_GOLDEN=str(golden), SDP_SDK_CONTRACT_SHA256=CONTRACT_SHA256)
            env.update(seed(port, context, password, guest_password))
            if expired:
                env["SDP_SDK_EXPIRED_TOKEN"] = expired
                with client(port, context, password) as proof:
                    require(proof.call("tx_outcome", request_token=expired) == {"status": "unknown"},
                            "actual Server did not report the persisted expired outcome as unknown")
                print("Real Server reports unknown for a persisted, expired outcome")
            read_fault = "query.prepare" if args.drop_read else None
            with (relay(port, context, fixture, args.drop_op) as proxy,
                  relay(port, context, fixture, read_fault) as read_proxy):
                if proxy is not None:
                    env["SDP_SDK_FAULT_PORT"] = str(proxy.server_address[1])
                if read_proxy is not None:
                    env["SDP_SDK_READ_FAULT_PORT"] = str(read_proxy.server_address[1])
                work = args.client_cwd.resolve(strict=True) if args.client_cwd else fixture / "consumer"
                work.mkdir(exist_ok=True)
                try:
                    result = subprocess.run(args.consumer, input=f"{password}\n{guest_password}\n".encode("ascii"),
                                            cwd=work, env=env, capture_output=True, timeout=args.timeout)
                except subprocess.TimeoutExpired as error:
                    print(clean_output(error.stdout, (password, guest_password)))
                    print(clean_output(error.stderr, (password, guest_password)), file=sys.stderr)
                    raise RuntimeError("installed SDK consumer timed out") from None
                print(clean_output(result.stdout, (password, guest_password)), end="")
                print(clean_output(result.stderr, (password, guest_password)), file=sys.stderr, end="")
                require(result.returncode == 0, f"installed SDK consumer exited {result.returncode}")
                require(args.success_marker.encode("ascii") in result.stdout.splitlines(),
                        "installed SDK consumer did not provide its acceptance marker")
                if args.require_observed:
                    records = [line[len(OBSERVED_PREFIX):] for line in result.stdout.splitlines()
                               if line.startswith(OBSERVED_PREFIX)]
                    required = {"decision_denied": 1006, "history_rows": 2, "prediction": 58}
                    if expired:
                        required["expired_outcome"] = "unknown"
                    require(len(records) == 1 and json.loads(records[0]) == required,
                            "SDK did not report the common observed Decision, History, model and expiry results")
                    with client(port, context, password) as proof:
                        rows, _ = proof.query("SELECT id FROM sdk_guard ORDER BY id")
                        require(not rows, "denied SDK mutation left a persisted row")
                    print("Matched the common SDK results and verified the denied write left no rows")
                if proxy is not None:
                    require(not proxy.failure and proxy.receipt is not None, "the expected commit reply was not dropped")
                    key, lsn = proxy.receipt
                    with client(port, context, password) as proof:
                        outcome = proof.call("tx_outcome", request_token=key)
                    require(outcome == {"status": "committed", "lsn": lsn}, "durable outcome changed after lost response")
                    print("Injected lost response and proved commit through the real Server ledger")
                if read_proxy is not None:
                    require(not read_proxy.failure and read_proxy.receipt is True, "the read reply was not dropped")
                    print("Injected loss before exposing a read result")
        require(execute(server, config, "--check-storage").returncode == 0, "isolated fixture storage verification failed")


def main():
    usage = argparse.ArgumentParser(description=__doc__)
    for path in ("server", "openssl", "contract", "scratch"):
        usage.add_argument("--" + path, type=Path, required=True)
    usage.add_argument("--client-cwd", type=Path)
    usage.add_argument("--drop-op", choices=("tx_commit", "execute"))
    usage.add_argument("--drop-read", action="store_true")
    usage.add_argument("--timeout", type=int, default=90)
    usage.add_argument("--success-marker", default="SDP_MANAGED_OK")
    usage.add_argument("--require-observed", action="store_true")
    usage.add_argument("--expiry-helper", type=Path)
    usage.add_argument("consumer", nargs=argparse.REMAINDER)
    args = usage.parse_args()
    if args.consumer[:1] == ["--"]:
        args.consumer.pop(0)
    if not args.consumer:
        usage.error("supply an installed consumer command after --")
    acceptance(args)


if __name__ == "__main__":
    main()
