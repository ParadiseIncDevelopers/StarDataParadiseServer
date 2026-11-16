"""Exercise the Windows offline update entry point against a released Server."""

import argparse
import base64
from contextlib import contextmanager
from hashlib import file_digest
import json
import os
from pathlib import Path
import secrets
import shutil
import ssl
import subprocess
import tempfile
import time

from history_restore_rpc import select
from service_smoke import certificates, execute, listening, require, save_config, send_ctrl_c
from setup_lifecycle_fixture import marker, seed
from sql_rpc import client, i64, write


PAUSE_POINTS = ("verified", "preflight", "stopped", "staged", "verified_target", "started")
COMPATIBILITY = dict.fromkeys(
    ("protocol", "storage", "wal", "checkpoint", "history", "plsql_ir", "model", "native_abi"), 1
)
COMPATIBILITY["qt"] = "6.11.3"


SIGN_FIXTURE = """
$ErrorActionPreference = 'Stop'
if ($PSVersionTable.PSEdition -eq 'Desktop') { Add-Type -AssemblyName System.Security }
else { Add-Type -AssemblyName System.Security.Cryptography.Pkcs }
$path = $env:SDP_UPDATE_TEST_MANIFEST
$raw = [IO.File]::ReadAllBytes($path)
$rsa = [Security.Cryptography.RSACng]::new(2048)
try {
    $req = [Security.Cryptography.X509Certificates.CertificateRequest]::new(
        'CN=StarDataParadise integration fixture', $rsa, [Security.Cryptography.HashAlgorithmName]::SHA256,
        [Security.Cryptography.RSASignaturePadding]::Pkcs1)
    $now = [DateTimeOffset]::UtcNow
    $cert = $req.CreateSelfSigned($now.AddMinutes(-5), $now.AddHours(1))
    try {
        $content = [Security.Cryptography.Pkcs.ContentInfo]::new($raw)
        $cms = [Security.Cryptography.Pkcs.SignedCms]::new($content, $true)
        $signer = [Security.Cryptography.Pkcs.CmsSigner]::new($cert)
        $signer.IncludeOption = [Security.Cryptography.X509Certificates.X509IncludeOption]::EndCertOnly
        $signer.DigestAlgorithm = [Security.Cryptography.Oid]::new('2.16.840.1.101.3.4.2.1')
        $cms.ComputeSignature($signer, $true)
        [IO.File]::WriteAllBytes($path + '.p7s', $cms.Encode())
        $hash = [Security.Cryptography.SHA256]::Create()
        try { [Console]::WriteLine([BitConverter]::ToString($hash.ComputeHash($cert.RawData)).Replace('-', '')) }
        finally { $hash.Dispose() }
    } finally { $cert.Dispose() }
} finally { $rsa.Dispose() }
"""


HOLD_AT_BOUNDARY = """
$ErrorActionPreference = 'Stop'
$invocation = ConvertFrom-Json -InputObject $env:SDP_UPDATE_TEST_INVOCATION
$params = @{}
for ($index = 0; $index -lt $invocation.arguments.Count; $index += 2) {
    $key = $invocation.arguments[$index].Substring(1)
    $params[$key] = $invocation.arguments[$index + 1]
}
& $invocation.update @params
if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }
[IO.File]::WriteAllText($invocation.signal, 'paused')
Start-Sleep -Seconds 150
"""


def digest(path):
    with path.open("rb") as file:
        return file_digest(file, "sha256").hexdigest()


def storage_hashes(directory):
    return {path.name: digest(path) for path in directory.iterdir() if path.is_file() and path.name != ".sdp.lock"}


def powershell(program):
    return [str(program), "-NoProfile", "-NonInteractive", "-ExecutionPolicy", "RemoteSigned"]


def encoded(script):
    return ["-EncodedCommand", base64.b64encode(script.encode("utf-16-le")).decode("ascii")]


def run(command, *, env=None):
    return subprocess.run(command, text=True, encoding="utf-8", errors="replace", capture_output=True,
                          stdin=subprocess.DEVNULL, timeout=150, env=env)


def require_ok(result, operation):
    require(result.returncode == 0, f"{operation} failed with {result.returncode}: {result.stderr.strip()}")
    return result.stdout


def make_bundle(args, root):
    bundle = root / "bundle"
    bundle.mkdir()
    candidate = bundle / "StarDataParadiseServer.exe"
    shutil.copy2(args.candidate, candidate)
    manifest = {
        "schema": 1,
        "channel": "production",
        "component": "server",
        "from_version": "0.3.0",
        "to_version": "0.4.0",
        "compatibility": COMPATIBILITY,
        "artifacts": [{"name": candidate.name, "sha256": digest(candidate), "bytes": candidate.stat().st_size}],
    }
    manifest_path = bundle / "update.json"
    manifest_path.write_text(json.dumps(manifest), encoding="utf-8")
    signer = run(powershell(args.powershell) + encoded(SIGN_FIXTURE),
                 env={**os.environ, "SDP_UPDATE_TEST_MANIFEST": str(manifest_path)})
    pin = require_ok(signer, "generate in-memory fixture signature").strip().lower()
    require(len(pin) == 64 and all(char in "0123456789abcdef" for char in pin), "unexpected certificate pin")
    return bundle, pin


@contextmanager
def sql_instance(server, config, context, password):
    with listening(str(server), config) as (process, port):
        with client(port, context, password) as connection:
            yield connection
        send_ctrl_c(process.pid)
        require(process.wait(timeout=12) == 0, "fixture Server shutdown was not graceful")


def make_source(args, root):
    source = root / "source"
    source.mkdir()
    certificates(str(args.openssl), source)
    for folder in ("data", "log", "temp", "backup"):
        (source / folder).mkdir()
    config = save_config(source, backup="backup", instance_name="upgrade-source")
    password = secrets.token_urlsafe(32)
    initialize = execute(str(args.old_server), config, "--initialize-storage")
    require(initialize.returncode == 0, "could not initialize released Server storage")
    bootstrap = execute(str(args.old_server), config, "--bootstrap-admin", "local-admin",
                        stdin=(password + "\n" + password + "\n").encode("ascii"))
    require(bootstrap.returncode == 0, "could not bootstrap released Server")
    context = ssl.create_default_context(cafile=str(source / "ca.pem"))
    with sql_instance(args.old_server, config, context, password) as connection:
        seed(connection)
    return config, password, context, storage_hashes(source / "data")


def update_args(args, journal, bundle, target, source, pin, action="Apply"):
    return [
        "-Action", action, "-JournalPath", str(journal), "-BundleRoot", str(bundle), "-Component", "server",
        "-TargetRoot", str(target), "-SourceConfig", str(source), "-SourceProgram", str(args.old_server),
        "-TrustedSignerSha256", pin,
    ]


def run_update(args, arguments):
    return run(powershell(args.powershell) + ["-File", str(args.update)] + arguments)


def journal_state(args, journal):
    report = run_update(args, ["-Action", "Status", "-JournalPath", str(journal)])
    return json.loads(require_ok(report, "read durable update status"))


def kill_at_boundary(args, arguments, point, root):
    signal = root / "checkpoint-reached.flag"
    env = {**os.environ, "SDP_UPDATE_TEST_INVOCATION": json.dumps({
        "update": str(args.update), "arguments": arguments + ["-PauseAfter", point], "signal": str(signal),
    })}
    with (root / "interrupted.stdout.log").open("wb") as stdout, (root / "interrupted.stderr.log").open("wb") as stderr:
        process = subprocess.Popen(powershell(args.powershell) + encoded(HOLD_AT_BOUNDARY), env=env,
                                   stdin=subprocess.DEVNULL, stdout=stdout, stderr=stderr)
        try:
            deadline = time.monotonic() + 90
            while not signal.exists() and process.poll() is None and time.monotonic() < deadline:
                time.sleep(0.05)
            if not signal.exists() or process.poll() is not None:
                output = (root / "interrupted.stderr.log").read_text(encoding="utf-8", errors="replace")
                raise RuntimeError(f"update did not pause at {point}: {output}")
            process.kill()
            require(process.wait(timeout=10) != 0, "paused update did not record killed process")
        finally:
            if process.poll() is None:
                process.kill()
                process.wait(timeout=10)


def test_tampered(args, root, bundle, source, pin):
    for change in ("manifest", "artifact"):
        attempt = root / f"tampered-{change}"
        attempt.mkdir()
        bad_bundle = attempt / "bundle"
        shutil.copytree(bundle, bad_bundle)
        target = attempt / "target"
        journal = attempt / "journal.json"
        if change == "manifest":
            path = bad_bundle / "update.json"
            path.write_bytes(path.read_bytes() + b"\n")
            expected = "signature does not validate"
        else:
            with (bad_bundle / "StarDataParadiseServer.exe").open("r+b") as file:
                previous = file.read(1)
                file.seek(0)
                file.write(bytes([previous[0] ^ 1]))
            expected = "SHA-256 does not match"
        result = run_update(args, update_args(args, journal, bad_bundle, target, source, pin))
        require(result.returncode != 0 and expected in result.stderr, f"wrong {change} refusal: {result.stderr}")
        require(not target.exists() and not journal.exists(), f"tampered {change} reached staging")
        print(f"PASS rejects tampered signed {change} before staging", flush=True)


def test_restart(args, root, source, bundle, pin, baseline):
    for point in PAUSE_POINTS:
        attempt = root / f"pause-{point}"
        attempt.mkdir()
        target = attempt / "target"
        journal = attempt / "journal.json"
        arguments = update_args(args, journal, bundle, target, source, pin)
        kill_at_boundary(args, arguments, point, attempt)
        state = journal_state(args, journal)
        require(state["last_complete"] == point and state["phase"] == point, f"wrong durable boundary for {point}")
        require(storage_hashes(source.parent / "data") == baseline, f"source changed while pausing at {point}")
        resumed = json.loads(require_ok(run_update(args, arguments), f"resume after {point}"))
        require(resumed["phase"] == "complete" and resumed["id"] == state["id"], f"resume from {point} did not finish")
        require(storage_hashes(source.parent / "data") == baseline, f"source changed after resuming {point}")
        print(f"PASS kill after {point}, resume same journal and preserve source storage", flush=True)
    return target, journal


def test_rollback(args, root, bundle, source, pin, baseline, context, password, target, journal):
    updated = target / "StarDataParadiseServer.exe"
    for leaf in ("backup", "data"):
        with sql_instance(updated, target / f"{leaf}.json", context, password) as connection:
            marker(connection)
            require(select(connection, "SELECT value FROM lifecycle_data WHERE id=2") == [], "copied rolled-back row")
    print("PASS pinned offline update copied SQL and History storage from genuine Server 0.3.0", flush=True)

    clean_target = root / "pause-verified" / "target"
    clean_journal = root / "pause-verified" / "journal.json"
    clean = run_update(args, update_args(args, clean_journal, bundle, clean_target, source, pin, "Rollback"))
    restored = json.loads(require_ok(clean, "mark rollback without later writes"))
    require(restored["phase"] == "rolled_back" and restored["rollback_boundary"] == "rollback_complete",
            "clean rollback did not record its boundary")

    with sql_instance(updated, target / "data.json", context, password) as connection:
        write(connection, "INSERT INTO lifecycle_data VALUES (3,819)", 1)
        require(select(connection, "SELECT value FROM lifecycle_data WHERE id=3") == [[i64(819)]],
                "candidate-only write was not committed")
    rollback = run_update(args, update_args(args, journal, bundle, target, source, pin, "Rollback"))
    require(rollback.returncode != 0 and "new data" in rollback.stderr, f"rollback unexpectedly accepted writes: {rollback.stderr}")
    status = journal_state(args, journal)
    require(status["phase"] == "safe_stop" and status["rollback_boundary"] == "has_new_writes",
            "rollback risk did not become durable")
    require(storage_hashes(source.parent / "data") == baseline, "source storage changed during the update")
    with sql_instance(updated, target / "data.json", context, password) as connection:
        require(select(connection, "SELECT value FROM lifecycle_data WHERE id=3") == [[i64(819)]],
                "refused rollback discarded target writes")
    with sql_instance(args.old_server, source, context, password) as connection:
        marker(connection)
        require(select(connection, "SELECT value FROM lifecycle_data WHERE id=3") == [], "candidate write reached original data")
    print("PASS clean rollback records its boundary and later writes force a safe stop without data loss", flush=True)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    for flag in ("old-server", "candidate", "openssl", "powershell", "update", "scratch"):
        parser.add_argument("--" + flag, type=Path, required=True)
    args = parser.parse_args()
    for key, path in vars(args).items():
        setattr(args, key, path.resolve(strict=key != "scratch"))
    args.scratch.mkdir(parents=True, exist_ok=True)
    root = Path(tempfile.mkdtemp(prefix="workflow-", dir=args.scratch))
    print(f"Update integration fixture {root}", flush=True)
    require(require_ok(run([str(args.old_server), "--version"]), "release version").strip()
            == "StarDataParadiseServer 0.3.0", "wrong old release binary")
    require(require_ok(run([str(args.candidate), "--version"]), "candidate version").strip()
            == "StarDataParadiseServer 0.4.0", "wrong candidate binary")
    print(f"Source Server 0.3.0 SHA-256 {digest(args.old_server)}", flush=True)
    print(f"Candidate Server 0.4.0 SHA-256 {digest(args.candidate)}", flush=True)
    bundle, pin = make_bundle(args, root)
    source, password, context, baseline = make_source(args, root)
    test_tampered(args, root, bundle, source, pin)
    target, journal = test_restart(args, root, source, bundle, pin, baseline)
    test_rollback(args, root, bundle, source, pin, baseline, context, password, target, journal)


if __name__ == "__main__":
    main()
