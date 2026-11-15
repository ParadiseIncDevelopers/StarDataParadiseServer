"""Exercise two isolated instances, measured health and redacted support bundles."""

import argparse
import hashlib
import json
from pathlib import Path
import secrets
import ssl
import tempfile

from backup_rpc import ARCHIVES, bootstrap, verified_job
from history_restore_rpc import select
from jobs_rpc import terminal, wait_for
from service_smoke import certificates, execute, listening, require, save_config
from sql_rpc import client, text, token, write


INVALID_ARGUMENT = 1012
INVALID_STATE = 1013
CHECKS = ("storage", "history", "backup", "certificate", "jobs")


def initialize(server, openssl, root, name):
    directory = root / name
    directory.mkdir()
    for child in ("data", "log", "temp", ARCHIVES):
        (directory / child).mkdir()
    certificates(openssl, directory)
    config = save_config(directory, "instance.json", instance_name=name, backup=ARCHIVES,
                         timeout_ms=20000, max_connections=8)
    require(execute(server, config, "--initialize-storage").returncode == 0,
            f"{name} storage initialization failed")
    password = secrets.token_urlsafe(32)
    bootstrap(server, config, password)
    context = ssl.create_default_context(cafile=str(directory / "ca.pem"))
    return directory, config, password, context


def save_marker(session, label):
    write(session, "CREATE TABLE isolation_marker (id BIGINT PRIMARY KEY, value TEXT NOT NULL)", 0)
    write(session, "CREATE INDEX isolation_value ON isolation_marker(value)", 0)
    write(session, f"INSERT INTO isolation_marker VALUES (1,'{label}')", 1)


def marker(session, label):
    require(select(session, "SELECT value FROM isolation_marker") == [[text(label)]],
            f"instance returned another instance's state for {label}")


def health(session):
    observed = session.call("admin_health")
    records = observed["checks"]
    require(len(observed["instance_id"]) == 32, "health did not identify its instance")
    require(all(name in records for name in CHECKS), "health omitted a required probe record")
    for name in CHECKS:
        check = records[name]
        require(check["status"] in ("UNKNOWN", "OK", "ERROR")
                and all(key in check for key in ("checked_at", "evidence", "remediation")),
                f"{name} check omitted status, timestamp, evidence or remediation")
    return records


def verify_live_metrics(session, port, context, password, instance_id):
    with client(port, context, password) as transaction:
        transaction.call("tx_begin")
        metrics = session.call("admin_status")
        require(metrics["instance_id"] == instance_id and metrics["instance_name"] == "red" and metrics["port"] == port
                and len(metrics["sessions"]) == 2 and metrics["active_transactions"] == 1
                and metrics["waiting_sessions"] == 0 and metrics["disk"]["bytes"] > 0
                and metrics["limits"]["connections"] == 8 and metrics["limits"]["job_workers"] == 1
                and metrics["deadlock_graph_available"] is False,
                "administration did not report the live instance's transaction, disk or limits evidence")
        transaction.call("tx_rollback")
    require(session.call("admin_status")["active_transactions"] == 0,
            "administration continued reporting a rolled-back transaction")


def measure_health(session):
    before = health(session)
    for name in CHECKS:
        result = before[name]
        require(result["status"] == "UNKNOWN" and result["checked_at"] is None,
                f"unchecked {name} was reported as healthy")
    session.call("admin_health_check", check="backup")
    require(health(session)["backup"]["status"] == "UNKNOWN",
            "an instance without a verified backup reported it healthy")
    for name in ("storage", "history", "certificate", "jobs"):
        receipt = session.call("admin_health_check", check=name)
        measured = health(session)[name]
        require(receipt["check"] == name and receipt["result"] == measured
                and measured["status"] == "OK" and measured["checked_at"] and measured["evidence"],
                f"{name} did not acquire measurable evidence after its check")
    queued = session.call("backup_submit", request_token=token())
    backup = verified_job(session, queued["job_id"])
    session.call("backup_verify", backup_id=backup["backup_id"])
    session.call("admin_health_check", check="backup")
    after = health(session)["backup"]
    require(after["status"] == "OK" and after["checked_at"]
            and after["evidence"]["backup_id"] == backup["backup_id"]
            and after["evidence"]["manifest_sha256"] == backup["manifest_sha256"],
            "independent verification did not update the backup health record")
    return backup


def config_and_support(session, root, config):
    original = config.read_bytes()
    preview = session.call("admin_config_preview", config={"timeout_ms": 12000, "max_connections": 7})
    require(preview["valid"] is True and preview["applied"] is False
            and preview["dynamic"] == {"timeout_ms": 12000} and preview["restart_required"] == {"max_connections": 7},
            "valid configuration preview did not classify the proposed change")
    session.error("admin_config_preview", INVALID_ARGUMENT, config={"port": 70000})
    session.error("admin_config_apply", INVALID_ARGUMENT, config={"port": 70000})
    require(config.read_bytes() == original, "invalid configuration operation overwrote the known-good file")
    updated = session.call("admin_config_apply", config={"timeout_ms": 12000})
    previous = updated["previous_config"]
    require(Path(previous).name == previous and previous.startswith(config.name + ".previous-"),
            "configuration apply did not identify its staged local previous copy")
    require(updated["applied"] is True and config.with_name(previous).read_bytes() == original
            and json.loads(config.read_bytes())["timeout_ms"] == 12000
            and session.call("admin_status")["limits"]["timeout_ms"] == 12000,
            "dynamic change did not preserve the previous configuration or publish the new timeout")
    marker(session, "red")
    secret = secrets.token_hex(32)
    unauthorized = root / "log" / "private-fixture.txt"
    unauthorized.write_text(secret, encoding="utf-8")
    correlation_secret = secrets.token_hex(16)
    records = [
        {"at_utc": 1767225600, "event": "listening", "password": secret, "correlation_id": correlation_secret},
        {"at_utc": 1767225601, "event": secret, "correlation_id": correlation_secret},
    ]
    with (root / "log" / "operational.jsonl").open("a", encoding="utf-8") as log:
        for record in records:
            log.write(json.dumps(record, separators=(",", ":")) + "\n")
    preview = session.call("admin_support_preview")
    require(preview["secret_scan_passed"] is True and preview["log_counts"]["operational"] >= len(records),
            "support preview did not inspect and redact its log input")
    target = "isolated-support-proof"
    session.call("admin_support_create", target_name=target)
    bundle = root / "temp" / "support" / target / "bundle.json"
    content = bundle.read_text(encoding="utf-8")
    document = json.loads(content)
    stamped = [record for record in document["logs"]["operational"] if record["at_utc"] in (1767225600, 1767225601)]
    require(len(stamped) == 2 and stamped[0]["event"] == "listening" and stamped[1]["event"] == "redacted_event"
            and stamped[0]["correlation"] == stamped[1]["correlation"]
            and secret not in content and correlation_secret not in content and "PRIVATE KEY" not in content
            and "admin-v1.json" not in content and "private-fixture.txt" not in content,
            "support bundle omitted operational evidence or included sensitive files/values")
    original_bundle = hashlib.sha256(bundle.read_bytes()).hexdigest()
    session.error("admin_support_create", INVALID_STATE, target_name=target)
    require(hashlib.sha256(bundle.read_bytes()).hexdigest() == original_bundle,
            "reusing a support target overwrote the first bundle")


def invalid_startup(server, root, config):
    original = config.read_bytes()
    store = root / "data" / "store.sdp"
    baseline = hashlib.sha256(store.read_bytes()).hexdigest()
    malformed = save_config(root, "port-out-of-range.json", port=70000, instance_name="red")
    check = execute(server, malformed, "--check-storage")
    require(check.returncode != 0 and config.read_bytes() == original
            and hashlib.sha256(store.read_bytes()).hexdigest() == baseline,
            "bad startup configuration replaced the live configuration or changed stored data")


def maintenance(session):
    for action in ("integrity", "statistics", "index"):
        job = session.call("admin_maintenance_submit", request_token=token(), action=action)
        result = wait_for(session, job["job_id"], terminal, seconds=45)
        report = result["result"]
        require(result["state"] == "succeeded" and report and report["action"] == action,
                f"{action} maintenance did not complete on the shared scheduler ({result.get('reason')})")
        if action == "integrity":
            require(report["verified"] is True and report["rows"] > 0 and report["history_head"] > 0,
                    "integrity maintenance did not verify populated storage and History")
        else:
            require(report["objects"] > 0 and report["lsn"] > int(job["lsn"]),
                    f"{action} did not publish changes to populated catalog objects")
            if action == "statistics":
                require(report["rows"] >= 1, "statistics did not account for the known SQL row")
        marker(session, "red")


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    for argument in ("server", "openssl", "scratch"):
        parser.add_argument("--" + argument, type=Path, required=True)
    args = parser.parse_args()
    server = str(args.server.resolve(strict=True))
    openssl = str(args.openssl.resolve(strict=True))
    args.scratch.mkdir(parents=True, exist_ok=True)
    with tempfile.TemporaryDirectory(prefix="s30-", dir=args.scratch.resolve()) as directory:
        root = Path(directory)
        a, config_a, password_a, tls_a = initialize(server, openssl, root, "red")
        _, config_b, password_b, tls_b = initialize(server, openssl, root, "blue")
        with listening(server, config_b) as (blue_process, blue_port), client(blue_port, tls_b, password_b) as blue:
            save_marker(blue, "blue")
            with listening(server, config_a) as (red_process, red_port), client(red_port, tls_a, password_a) as red:
                require(red_port != blue_port and red_process.pid != blue_process.pid,
                        "test instances shared a port or process")
                save_marker(red, "red")
                marker(red, "red")
                marker(blue, "blue")
                red_id = red.call("admin_health")["instance_id"]
                blue_id = blue.call("admin_health")["instance_id"]
                require(red_id != blue_id, "simultaneous instances reported the same durable identity")
                verify_live_metrics(red, red_port, tls_a, password_a, red_id)
                measure_health(red)
                require(health(blue)["backup"]["status"] == "UNKNOWN",
                        "health evidence was shared across isolated instances")
                config_and_support(red, a, config_a)
                maintenance(red)
            require(red_process.poll() is not None and blue_process.poll() is None,
                    "stopping one instance stopped the other")
            marker(blue, "blue")
            invalid_startup(server, a, config_a)
            marker(blue, "blue")
            with listening(server, config_a) as (_, port), client(port, tls_a, password_a) as reopened:
                require(reopened.call("admin_health")["instance_id"] == red_id,
                        "restart changed the selected instance's identity")
                marker(reopened, "red")
                marker(blue, "blue")
        for config in (config_a, config_b):
            require(execute(server, config, "--check-storage").returncode == 0,
                    "one independent instance failed an offline storage check")
    print("Admin TLS separated instances, evidenced health, invalid configuration and redacted support passed")


if __name__ == "__main__":
    main()
