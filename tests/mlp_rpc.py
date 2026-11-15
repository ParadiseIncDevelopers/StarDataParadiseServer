"""Verify MLP learning, worker isolation and optional availability over TLS."""

import argparse
import math
import os
from pathlib import Path
import secrets
import signal
import ssl
import tempfile

from jobs_rpc import terminal, wait_for
from model_rpc import activate, create_dataset
from service_smoke import certificates, execute, listening, require, save_config
from sql_rpc import client, i64, token, write


NAME = "xor_model"
FEATURE_UNAVAILABLE = 1004
VALIDATION_ERROR = 1012
OPTIONS = {
    "hidden_layers": [8], "activation": "tanh", "loss": "bce", "optimizer": "adam",
    "learning_rate": 0.03, "batch_size": 4, "epochs": 120, "patience": 30,
}
LONG_JOB = {
    "hidden_layers": [32, 32], "activation": "tanh", "loss": "bce", "optimizer": "sgd",
    "learning_rate": 0.00001, "batch_size": 4, "epochs": 300, "patience": 300, "min_delta": 0,
}
FEATURES = [{"column": field, "transform": "standard", "impute": "reject"} for field in ("axis_a", "axis_b")]


def create_xor_dataset(session, count):
    table = f"xor_input_{count}"
    write(session, f"CREATE TABLE {table} (sample_key BIGINT PRIMARY KEY, axis_a BIGINT, axis_b BIGINT, class_name TEXT)", 0)
    records = []
    for variation in range(count):
        scale_a = 2 + variation // 8
        scale_b = 2 + variation % 8
        for sign_a, sign_b in ((-1, -1), (-1, 1), (1, -1), (1, 1)):
            category = "equal" if sign_a == sign_b else "mixed"
            records.append(f"({len(records) + 1},{sign_a * scale_a},{sign_b * scale_b},'{category}')")
    write(session, f"INSERT INTO {table} VALUES " + ",".join(records), len(records))
    return create_dataset(session, f"SELECT axis_a,axis_b,class_name FROM {table} ORDER BY sample_key",
                          {"features": FEATURES, "target": "class_name", "target_kind": "categorical",
                           "split": {"mode": "stratified", "seed": 49, "train": 60, "validation": 20}})


def training_request(dataset, options, seed=2027):
    return {"request_token": token(), "name": NAME, "dataset_id": dataset["dataset_id"], "seed": seed,
            "options": options}


def reserve_training(session, dataset, options, seed=2027):
    return session.call("model_train", **training_request(dataset, options, seed))["job_id"]


def prediction(session, a, b):
    return session.call("model_predict", name=NAME, features={"axis_a": a, "axis_b": b})


def check_loss_and_predictions(session, dataset, report):
    classes = dataset["prepared"]["target_classes"]
    require(classes == ["equal", "mixed"], "MLP fixture lost its categorical outcome order")
    rows = dataset["source"]["rows"]
    artifact = report["artifact"]
    require(0 < artifact["best_epoch"] <= artifact["epochs_completed"] <= OPTIONS["epochs"]
            and artifact["optimizer_steps"] > 0 and type(artifact["early_stopped"]) is bool
            and artifact["training_loss"] < 0.25 * artifact["initial_training_loss"]
            and artifact["validation_loss"] < 0.25 * artifact["initial_validation_loss"],
            "recorded MLP losses or learning metadata did not reflect improving training")
    for split, indices in dataset["prepared"]["split"].items():
        losses = []
        matches = 0
        targets = set()
        for index in indices:
            a, b, category = rows[index]
            expected = classes.index(category)
            targets.add(expected)
            result = prediction(session, a, b)
            probability = result["probability"]
            require(result["version"] == report["version"] and 0 < probability < 1
                    and result["label"] == result["prediction"] == int(probability >= 0.5)
                    and result.get("label_value", classes[result["label"]]) == classes[result["label"]],
                    "MLP inference label, confidence or artifact version is inconsistent")
            matches += int(result["label"] == expected)
            losses.append(-math.log(probability if expected else 1 - probability))
        mean_loss = math.fsum(losses) / len(indices)
        metrics = report["metrics"][split]
        require(targets == {0, 1} and matches == len(indices) and mean_loss < 0.15
                and metrics["count"] == len(indices) and metrics["accuracy"] == 1,
                f"MLP {split} did not learn the fixture's nonlinear XOR decision")
        if split in ("train", "validation"):
            saved = artifact["training_loss" if split == "train" else "validation_loss"]
            require(math.isclose(saved, mean_loss, abs_tol=1e-9),
                    f"MLP {split} stored loss disagrees with independent inference")


def accepted_model(session):
    dataset = create_xor_dataset(session, 12)
    session.call("model_create", request_token=token(), name=NAME, algorithm="mlp")
    trained = wait_for(session, reserve_training(session, dataset, OPTIONS), terminal)
    result = trained["result"]
    require(trained["state"] == "succeeded" and result and result["version"] == 1
            and result["dataset_id"] == dataset["dataset_id"], f"MLP worker failed ({trained.get('reason')})")
    require(session.call("model_status", name=NAME)["active_version"] is None,
            "training unexpectedly activated the candidate")
    activate(session, NAME, result["version"])
    report = session.call("model_evaluate", name=NAME)
    require(report["version"] == 1 and report["algorithm"] == "mlp" and report["seed"] == 2027
            and report["hardware"]["backend"] == "cpu" and report["checksum"] == result["checksum"]
            and report["options"] == OPTIONS, "published MLP metadata does not identify actual CPU training")
    check_loss_and_predictions(session, dataset, report)
    return report


def worker_failure_and_cancel(session, reader, report, scratch):
    dataset = create_xor_dataset(session, 44)
    before = session.call("model_status", name=NAME)
    malformed = session.raw("model_train", **training_request(dataset, OPTIONS | {"hidden_layers": [0]}))
    if malformed["ok"]:
        outcome = wait_for(session, malformed["body"]["job_id"], terminal)
        require(outcome["state"] == "failed" and outcome["result"] is None and outcome["reason"],
                "malformed network candidate did not fail safely")
    else:
        require(malformed["error"]["code"] == VALIDATION_ERROR, "malformed network was rejected for an unrelated reason")
    for failure in ("crash", "cancel"):
        job_id = reserve_training(session, dataset, LONG_JOB)
        live = wait_for(session, job_id, lambda status: status["worker_pid"] is not None or terminal(status))
        require(live["state"] == "running" and isinstance(live["worker_pid"], int), "MLP worker never ran")
        require(reader.query("SELECT 42")[0] == [[i64(42)]], "isolated MLP worker blocked independent SQL")
        if failure == "crash":
            os.kill(live["worker_pid"], signal.SIGTERM)
        else:
            receipt = session.call("job_cancel", job_id=job_id)
            require(receipt["cancel_requested"], "running MLP cancellation was not acknowledged")
        final = wait_for(session, job_id, terminal)
        require(final["state"] == ("failed" if failure == "crash" else "cancelled") and final["result"] is None,
                f"MLP worker {failure} produced an invalid terminal result")
        require(reader.query("SELECT 43")[0] == [[i64(43)]] and not any(scratch.iterdir())
                and session.call("model_status", name=NAME) == before
                and session.call("model_evaluate", name=NAME) == report,
                f"MLP worker {failure} affected SQL, leaked resources or damaged the active artifact")


def disabled_case(session, table):
    features = session.call("capabilities")["features"]
    require(features.get("mlp_cpu_v1") is False and features.get("decision_v1") is True,
            "minimal runtime misreported installed modules")
    session.error("model_create", FEATURE_UNAVAILABLE, request_token=token(), name="disabled_model", algorithm="mlp")
    session.error("model_predict", FEATURE_UNAVAILABLE, name=NAME, features={"axis_a": 3, "axis_b": -3})
    write(session, f"CREATE TABLE {table} (id BIGINT PRIMARY KEY)", 0)
    rule = f"{table}_positive_only"
    write(session, f"CREATE DECISION RULE {rule} ON {table} FOR INSERT PRIORITY 1 WHEN (NEW.id > 0) THEN ALLOW", 0)
    write(session, f"INSERT INTO {table} VALUES (3)", 1)
    require(session.query(f"SELECT id FROM {table}")[0] == [[i64(3)]]
            and session.call("history_verify")["verified"], "minimal runtime lost SQL, Decision or History")


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    for argument in ("server", "openssl", "scratch"):
        parser.add_argument("--" + argument, type=Path, required=True)
    parser.add_argument("--minimal-server", type=Path,
                        help="also verify a Server built with SDP_WITH_CPU_MLP=OFF against the same storage")
    args = parser.parse_args()
    server = str(args.server.resolve(strict=True))
    openssl = str(args.openssl.resolve(strict=True))
    args.scratch.mkdir(parents=True, exist_ok=True)
    with tempfile.TemporaryDirectory(prefix="s27-", dir=args.scratch.resolve()) as directory:
        fixture = Path(directory)
        for name in ("data", "log", "temp"):
            (fixture / name).mkdir()
        certificates(openssl, fixture)
        config = save_config(fixture, "config.json", timeout_ms=20000, max_connections=8)
        require(execute(server, config, "--initialize-storage").returncode == 0, "MLP storage setup failed")
        password = secrets.token_urlsafe(32)
        require(execute(server, config, "--bootstrap-admin", "local-admin",
                        stdin=((password + "\n") * 2).encode("ascii")).returncode == 0, "MLP administrator setup failed")
        context = ssl.create_default_context(cafile=str(fixture / "ca.pem"))
        with listening(server, config) as (_, port), client(port, context, password) as owner:
            require(owner.call("capabilities")["features"].get("mlp_cpu_v1") is True,
                    "MLP CPU capability was unavailable")
            report = accepted_model(owner)
            with client(port, context, password) as reader:
                worker_failure_and_cancel(owner, reader, report, fixture / "temp")
        minimal = save_config(fixture, "minimal.json", timeout_ms=20000, mlp_cpu_enabled=False)
        with listening(server, minimal) as (_, port), client(port, context, password) as owner:
            disabled_case(owner, "runtime_disabled_demo")
        if args.minimal_server:
            minimal_server = str(args.minimal_server.resolve(strict=True))
            with listening(minimal_server, config) as (_, port), client(port, context, password) as owner:
                disabled_case(owner, "compiled_minimal_demo")
        with listening(server, config) as (_, port), client(port, context, password) as owner:
            require(owner.call("model_evaluate", name=NAME) == report and prediction(owner, -4, 4)["label"] == 1,
                    "MLP artifact changed after disabled and enabled restarts")
        require(execute(server, config, "--check-storage").returncode == 0, "MLP storage integrity failed")
    print("MLP CPU TLS XOR held-out inference, loss, isolated worker crash/cancel and optional runtime passed")


if __name__ == "__main__":
    main()
