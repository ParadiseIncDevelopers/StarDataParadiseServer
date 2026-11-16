"""Check a disposable Setup-created instance through its normal TLS and SQL ports."""

import argparse
from pathlib import Path
import ssl
import sys

from decision_rpc import denied_write
from history_restore_rpc import select
from model_rpc import REGRESSION_OPTIONS, REGRESSION_SQL, create_dataset, verified_artifact
from jobs_rpc import terminal, wait_for
from service_smoke import certificates, listening, require, send_ctrl_c
from sql_rpc import client, i64, token, write


def marker(session):
    require(select(session, "SELECT value FROM lifecycle_data WHERE id=1") == [[i64(917)]],
            "Setup maintenance lost committed instance rows")
    require(session.call("history_verify")["verified"], "Setup maintenance lost verified History")


def seed(session):
    write(session, "CREATE TABLE lifecycle_data (id BIGINT PRIMARY KEY, value BIGINT NOT NULL)", 0)
    session.call("tx_begin")
    require(session.mutate("INSERT INTO lifecycle_data VALUES (1,917)")["affected_rows"] == 1,
            "Setup fixture could not stage its committed row")
    session.call("tx_commit", request_token=token())
    session.call("tx_begin")
    require(session.mutate("INSERT INTO lifecycle_data VALUES (2,300)")["affected_rows"] == 1,
            "Setup fixture could not stage its rolled-back row")
    session.call("tx_rollback")
    require(select(session, "SELECT value FROM lifecycle_data WHERE id=2") == [],
            "Setup instance did not roll back a test transaction")
    marker(session)


def enable_rule(session):
    marker(session)
    write(session, "CREATE DECISION RULE lifecycle_guard ON lifecycle_data FOR INSERT "
                   "PRIORITY 100 WHEN (NEW.value < 0) THEN DENY", 0)
    denied_write(session, "INSERT INTO lifecycle_data VALUES (3,-1)")


def train(session):
    marker(session)
    write(session, "CREATE TABLE regression_input (sample_key BIGINT PRIMARY KEY, reading BIGINT, outcome_value BIGINT)", 0)
    rows = ",".join(f"({key},{x},{3 * x + 7})" for key, x in enumerate(range(-15, 15), 1))
    write(session, "INSERT INTO regression_input VALUES " + rows, 30)
    dataset = create_dataset(session, REGRESSION_SQL, REGRESSION_OPTIONS)
    session.call("model_create", request_token=token(), name="setup_probe", algorithm="linear")
    submitted = session.call("model_train", request_token=token(), name="setup_probe", dataset_id=dataset["dataset_id"],
                             seed=19)
    done = wait_for(session, submitted["job_id"], terminal)
    require(done["state"] == "succeeded" and done["result"]["version"] == 1,
            "added ML module did not train a real model")
    verified_artifact(session, "setup_probe", "linear", 1, dataset)


def verify_model(session):
    marker(session)
    versions = session.call("model_status", name="setup_probe")["versions"]
    require(len(versions) == 1 and versions[0]["version"] == 1, "ML maintenance lost the trained model")
    report = session.call("model_evaluate", name="setup_probe", version=1)
    require(abs(report["artifact"]["coefficients"][0] - 3) < 1e-4,
            "ML maintenance changed the stored artifact")


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    commands = parser.add_subparsers(dest="mode", required=True)
    generate = commands.add_parser("certificates")
    generate.add_argument("--openssl", type=Path, required=True)
    generate.add_argument("--tls", type=Path, required=True)
    operations = {"seed": seed, "verify": marker, "train": train, "model": verify_model, "rule": enable_rule}
    for mode in operations:
        command = commands.add_parser(mode)
        command.add_argument("--server", type=Path, required=True)
        command.add_argument("--config", type=Path, required=True)
        command.add_argument("--tls", type=Path, required=True)
    args = parser.parse_args()
    if args.mode == "certificates":
        args.tls.mkdir(parents=True, exist_ok=True)
        certificates(str(args.openssl.resolve(strict=True)), args.tls.resolve(strict=True))
        return

    password = sys.stdin.readline().rstrip("\r\n")
    require(bool(password), "temporary fixture administrator password must be provided on stdin")
    tls = ssl.create_default_context(cafile=str(args.tls / "ca.pem"))
    server = str(args.server.resolve(strict=True))
    config = args.config.resolve(strict=True)
    with listening(server, config) as (process, port):
        with client(port, tls, password) as session:
            operations[args.mode](session)
        send_ctrl_c(process.pid)
        require(process.wait(timeout=12) == 0, "Setup fixture Server did not stop gracefully")
    print(f"Real TLS lifecycle fixture {args.mode} passed.")


if __name__ == "__main__":
    main()
