"""Exercise supervised model training, metrics, activation and restart over real TLS."""

import argparse
from concurrent.futures import ThreadPoolExecutor
import json
import math
from pathlib import Path
import re
import secrets
import ssl
import tempfile
from threading import Event
import time

from dataset_rpc import DatasetReader
from jobs_rpc import terminal, wait_for
from service_smoke import certificates, connection, execute, listening, require, save_config
from sql_rpc import client, token, write


PERMISSION_DENIED = 1006
INVALID_ARGUMENT = 1012
SHA256 = re.compile(r"[0-9a-f]{64}\Z")
LINEAR_TOLERANCE = 1e-4
FEATURES = [{"column": "reading", "transform": "numeric", "impute": "reject"}]
REGRESSION_OPTIONS = {"features": FEATURES, "target": "outcome_value", "target_kind": "numeric",
                      "split": {"mode": "random", "seed": 23, "train": 60, "validation": 20}}
CLASSIFICATION_OPTIONS = {"features": FEATURES, "target": "outcome_value", "target_kind": "categorical",
                          "split": {"mode": "stratified", "seed": 37, "train": 50, "validation": 25}}
REGRESSION_SQL = "SELECT reading,outcome_value FROM regression_input ORDER BY sample_key"
CLASSIFICATION_SQL = "SELECT reading,outcome_value FROM classification_input ORDER BY sample_key"


def model_sql(session, verb, fields):
    source = json.dumps(fields, separators=(",", ":")).replace("'", "''")
    return session.mutate(f"{verb} MODEL USING '{source}'")["model"]


def prepare_sources(owner):
    write(owner, "CREATE TABLE regression_input (sample_key BIGINT PRIMARY KEY, reading BIGINT, outcome_value BIGINT)", 0)
    regression = [(index, value, 3 * value + 7) for index, value in enumerate(range(-15, 15), 1)]
    values = ",".join(f"({index},{value},{expected})" for index, value, expected in regression)
    write(owner, "INSERT INTO regression_input VALUES " + values, len(regression))

    write(owner, "CREATE TABLE classification_input (sample_key BIGINT PRIMARY KEY, reading BIGINT, outcome_value TEXT)", 0)
    readings = list(range(-124, -100)) + list(range(101, 125))
    records = ",".join(f"({index},{reading},'{('cold' if reading < 0 else 'hot')}')"
                       for index, reading in enumerate(readings, 1))
    write(owner, "INSERT INTO classification_input VALUES " + records, len(readings))


def replace_regression_labels(owner):
    owner.call("tx_begin")
    for key, reading in enumerate(range(-15, 15), 1):
        result = owner.mutate(f"UPDATE regression_input SET outcome_value={-2 * reading + 101} WHERE sample_key={key}")
        require(result["affected_rows"] == 1, "retraining fixture lost a source row")
    owner.call("tx_commit", request_token=token())


def create_dataset(session, source, options):
    receipt = session.call("dataset_create", request_token=token(), sql=source, options=options)
    snapshot = session.call("dataset_get", dataset_id=receipt["dataset_id"])
    require(snapshot["row_count"] == receipt["row_count"], "model fixture dataset receipt differs from its persisted rows")
    return snapshot


def train(session, name, data, *, seed=19, via_sql=False):
    request = {"name": name, "dataset_id": data["dataset_id"], "seed": seed, "request_token": token()}
    reply = model_sql(session, "TRAIN", request) if via_sql else session.call("model_train", **request)
    job = wait_for(session, reply["job_id"], terminal)
    require(job["state"] == "succeeded" and job["attempt"] == 1 and job["result"] is not None,
            f"{name} training worker did not publish a model ({job.get('reason')})")
    artifact = job["result"]
    require(artifact["version"] > 0 and artifact["dataset_id"] == data["dataset_id"]
            and SHA256.fullmatch(artifact["checksum"]), "training result omitted its published artifact identity")
    return artifact


def activate(session, name, version, *, via_sql=False):
    request = {"name": name, "version": version, "request_token": token()}
    if via_sql:
        model_sql(session, "ALTER", request)
    else:
        session.call("model_activate", **request)
    require(session.call("model_status", name=name)["active_version"] == version,
            f"activation did not publish the {name} active version")


def prediction(session, name, reading, *, via_sql=False):
    request = {"name": name, "features": {"reading": reading}}
    reply = model_sql(session, "PREDICT", request) if via_sql else session.call("model_predict", **request)
    require(type(reply["prediction"]) in (float, int) and math.isfinite(reply["prediction"]),
            "predict did not return a finite numeric value")
    return reply


def verified_artifact(session, name, algorithm, version, dataset, *, seed=19, via_sql=False):
    request = {"name": name, "version": version}
    report = model_sql(session, "EVALUATE", request) if via_sql else session.call("model_evaluate", **request)
    require(report["version"] == version and report["algorithm"] == algorithm
            and report["dataset_id"] == dataset["dataset_id"] and SHA256.fullmatch(report["checksum"])
            and report["artifact"] and report["seed"] == seed and report["options"] == {}
            and re.fullmatch(r"[0-9a-f]{32}", report["run_id"]) and report["runtime"] == "sdp-supervised-v1"
            and report["hardware"]["backend"] == "cpu",
            "published model did not retain its artifact identity, dataset, training inputs and actual runtime")
    return report


def assert_regression(session, report, data):
    rows = data["source"]["rows"]
    for part, indices in data["prepared"]["split"].items():
        metrics = report["metrics"][part]
        errors = []
        for row in indices:
            sample, expected = rows[row]
            estimate = prediction(session, "forecast_model", sample)
            require(estimate["version"] == report["version"], "prediction did not identify its artifact version")
            errors.append(abs(estimate["prediction"] - expected))
        mean_absolute = math.fsum(errors) / len(errors)
        mean_squared = math.fsum(error ** 2 for error in errors) / len(errors)
        require(metrics["count"] == len(errors) and math.isclose(metrics["mae"], mean_absolute, abs_tol=1e-10)
                and math.isclose(metrics["mse"], mean_squared, abs_tol=1e-10)
                and math.isclose(metrics["rmse"], math.sqrt(mean_squared), abs_tol=1e-10),
                f"linear {part} metrics disagree with independent prediction errors")
        require(max(errors) < LINEAR_TOLERANCE, "linear fit drifted from the known fixture")
    at_zero = prediction(session, "forecast_model", 0, via_sql=True)
    at_ten = prediction(session, "forecast_model", 10)
    fitted = report["artifact"]
    require(math.isclose(at_zero["prediction"], 7, abs_tol=LINEAR_TOLERANCE)
            and math.isclose((at_ten["prediction"] - at_zero["prediction"]) / 10, 3, abs_tol=LINEAR_TOLERANCE)
            and math.isclose(fitted["intercept"], 7, abs_tol=LINEAR_TOLERANCE)
            and len(fitted["coefficients"]) == 1 and math.isclose(fitted["coefficients"][0], 3, abs_tol=LINEAR_TOLERANCE),
            "linear intercept or slope did not fit the known coefficients")


def assert_classification(session, name, report, data):
    rows = data["source"]["rows"]
    classes = data["prepared"]["target_classes"]
    require(classes == ["cold", "hot"], "classification fixture lost its stable class labels")
    for part, indices in data["prepared"]["split"].items():
        counts = {"tp": 0, "tn": 0, "fp": 0, "fn": 0}
        for index in indices:
            reading, category = rows[index]
            estimate = prediction(session, name, reading)
            label = estimate["label"]
            probability = estimate["probability"]
            require(estimate["version"] == report["version"] and label in (0, 1)
                    and 0 <= probability <= 1 and label == int(probability >= 0.5)
                    and estimate["prediction"] == label and estimate.get("label_value", classes[label]) == classes[label],
                    "classifier probability, encoded label, original label or version is inconsistent")
            actual = classes.index(category)
            key = ("tp" if actual else "fp") if label else ("fn" if actual else "tn")
            counts[key] += 1
        tp, tn, fp, fn = (counts[key] for key in ("tp", "tn", "fp", "fn"))
        require(tp > 0 and tn > 0, f"{name} failed to recognize both classes in the separated fixture")
        precision = tp / (tp + fp)
        recall = tp / (tp + fn)
        expected = {"accuracy": (tp + tn) / len(indices), "precision": precision, "recall": recall,
                    "f1": 2 * precision * recall / (precision + recall)}
        metrics = report["metrics"][part]
        require(metrics["count"] == len(indices) and metrics["confusion"] == counts and metrics["undefined"] == {}
                and all(math.isclose(metrics[key], value, abs_tol=1e-12) for key, value in expected.items()),
                f"{name} {part} classification metrics disagree with independent confusion counts")
        require(metrics["accuracy"] >= 0.95, f"{name} did not learn the known class separation")


def fit_models(session):
    numeric = create_dataset(session, REGRESSION_SQL, REGRESSION_OPTIONS)
    categorical = create_dataset(session, CLASSIFICATION_SQL, CLASSIFICATION_OPTIONS)
    model_sql(session, "CREATE", {"name": "forecast_model", "algorithm": "linear", "request_token": token()})
    require(session.call("model_status", name="forecast_model")["active_version"] is None,
            "empty model was reported as active")
    train(session, "forecast_model", numeric, via_sql=True)
    require(session.call("model_status", name="forecast_model")["active_version"] is None,
            "training activated a model before the requested atomic activation")
    activate(session, "forecast_model", 1)
    original = verified_artifact(session, "forecast_model", "linear", 1, numeric, via_sql=True)
    assert_regression(session, original, numeric)
    for algorithm in ("logistic", "tree", "forest"):
        name = "fixture_" + algorithm
        session.call("model_create", request_token=token(), name=name, algorithm=algorithm)
        train(session, name, categorical)
        activate(session, name, 1)
        report = verified_artifact(session, name, algorithm, 1, categorical)
        assert_classification(session, name, report, categorical)
    return numeric, original


def observe_activation(port, context, password, owner, original):
    replace_regression_labels(owner)
    second = create_dataset(owner, REGRESSION_SQL, REGRESSION_OPTIONS)
    train(owner, "forecast_model", second, seed=23)
    candidate = verified_artifact(owner, "forecast_model", "linear", 2, second, seed=23)
    require(candidate["checksum"] != original["checksum"]
            and owner.call("model_evaluate", name="forecast_model")["version"] == 1,
            "publishing a candidate changed or replaced the active artifact")

    seen_old = Event()

    def watch():
        with client(port, context, password) as observer:
            deadline = time.monotonic() + 10
            versions = []
            while time.monotonic() < deadline:
                observed = prediction(observer, "forecast_model", 4)
                version = observed["version"]
                expected = {1: 19, 2: 93}.get(version)
                require(expected is not None and math.isclose(observed["prediction"], expected, abs_tol=LINEAR_TOLERANCE),
                        "concurrent inference observed a partial artifact or mismatched version")
                versions.append(version)
                if version == 1:
                    seen_old.set()
                else:
                    return versions
            raise RuntimeError("concurrent reader did not observe the activated model")

    with ThreadPoolExecutor(max_workers=1) as pool:
        watcher = pool.submit(watch)
        require(seen_old.wait(10), "concurrent reader did not observe the original model")
        activate(owner, "forecast_model", 2, via_sql=True)
        require(watcher.result(timeout=10)[-1] == 2, "new version did not become visible as a whole")
    prior = owner.call("model_evaluate", name="forecast_model", version=1)
    require(prior["checksum"] == original["checksum"] and prior["artifact"] == original["artifact"],
            "activation rewrote the previous immutable artifact")
    return candidate


def verify_invalid_and_unauthorized(port, context, owner, existing_dataset):
    for features in ({}, {"unknown": 4}, {"reading": "not-a-number"}, {"reading": 4, "unknown": 5}):
        owner.error("model_predict", INVALID_ARGUMENT, name="forecast_model", features=features)
    owner.error("model_activate", INVALID_ARGUMENT, name="forecast_model", version=0, request_token=token())
    before = owner.call("model_status", name="forecast_model")
    malformed = owner.raw("model_train", request_token=token(), name="forecast_model",
                          dataset_id=existing_dataset["dataset_id"], options={"artifact": {"coefficients": [999]}})
    if malformed["ok"]:
        task = wait_for(owner, malformed["body"]["job_id"], terminal)
        require(task["state"] == "failed" and task["result"] is None,
                "unrecognized training input produced a published artifact")
    else:
        require(malformed["error"]["code"] == INVALID_ARGUMENT, "unknown training option returned an unrelated error")
    require(owner.call("model_status", name="forecast_model") == before,
            "rejected artifact input altered the registry")
    write(owner, "CREATE USER dataset_reader", 0)
    password = secrets.token_urlsafe(32)
    owner.call("password_set", username="dataset_reader", new_password=password, request_token=token())
    write(owner, "GRANT SELECT ON TABLE regression_input TO USER dataset_reader", 0)
    with connection(port, context) as socket:
        stranger = DatasetReader(socket, password)
        stranger.error("model_predict", PERMISSION_DENIED, name="forecast_model", features={"reading": 4})
        stranger.error("model_status", PERMISSION_DENIED, name="forecast_model")
        stranger.error("model_evaluate", PERMISSION_DENIED, name="forecast_model", version=1)
        stranger.error("model_create", PERMISSION_DENIED, request_token=token(), name="owned_model", algorithm="linear")
        write(owner, "GRANT DDL ON GLOBAL TO USER dataset_reader", 0)
        stranger.call("model_create", request_token=token(), name="owned_model", algorithm="linear")
        stranger.error("model_train", PERMISSION_DENIED, request_token=token(), name="owned_model",
                       dataset_id=existing_dataset["dataset_id"])
        private_data = create_dataset(stranger, REGRESSION_SQL, REGRESSION_OPTIONS)
        published = train(stranger, "owned_model", private_data)
        activate(stranger, "owned_model", published["version"])
        before_revoke = prediction(stranger, "owned_model", 4)
        write(owner, "REVOKE SELECT ON TABLE regression_input FROM USER dataset_reader", 0)
        stranger.error("model_evaluate", PERMISSION_DENIED, name="owned_model")
        stranger.error("model_predict", PERMISSION_DENIED, name="owned_model", features={"reading": 4})
        stranger.error("model_activate", PERMISSION_DENIED, name="owned_model", version=1, request_token=token())
        write(owner, "GRANT SELECT ON TABLE regression_input TO USER dataset_reader", 0)
        require(prediction(stranger, "owned_model", 4) == before_revoke,
                "grant restoration changed the completed owned artifact")


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    for argument in ("server", "openssl", "scratch"):
        parser.add_argument("--" + argument, type=Path, required=True)
    args = parser.parse_args()
    server = str(args.server.resolve(strict=True))
    openssl = str(args.openssl.resolve(strict=True))
    args.scratch.mkdir(parents=True, exist_ok=True)
    with tempfile.TemporaryDirectory(prefix="s25-", dir=args.scratch.resolve()) as directory:
        fixture = Path(directory)
        for name in ("data", "log", "temp"):
            (fixture / name).mkdir()
        certificates(openssl, fixture)
        config = save_config(fixture, "config.json", timeout_ms=20000, max_connections=8)
        require(execute(server, config, "--initialize-storage").returncode == 0, "model storage initialization failed")
        password = secrets.token_urlsafe(32)
        require(execute(server, config, "--bootstrap-admin", "local-admin",
                        stdin=((password + "\n") * 2).encode("ascii")).returncode == 0,
                "model administrator bootstrap failed")
        context = ssl.create_default_context(cafile=str(fixture / "ca.pem"))
        with listening(server, config) as (_, port), client(port, context, password) as owner:
            require(owner.call("capabilities")["features"].get("supervised_ml_v1") is True,
                    "server did not advertise supervised model capability")
            prepare_sources(owner)
            initial, original = fit_models(owner)
            candidate = observe_activation(port, context, password, owner, original)
            verify_invalid_and_unauthorized(port, context, owner, initial)
        with listening(server, config) as (_, port), client(port, context, password) as reopened:
            require(reopened.call("model_status", name="forecast_model")["active_version"] == 2,
                    "restart lost the active version")
            after = reopened.call("model_evaluate", name="forecast_model")
            require(after["checksum"] == candidate["checksum"] and after["artifact"] == candidate["artifact"]
                    and after["metrics"] == candidate["metrics"], "restart lost immutable artifact details")
            result = prediction(reopened, "forecast_model", 4)
            require(result["version"] == 2 and math.isclose(result["prediction"], 93, abs_tol=LINEAR_TOLERANCE),
                    "restart did not use the active artifact")
            require(reopened.call("history_verify")["verified"], "model publication broke durable History")
        require(execute(server, config, "--check-storage").returncode == 0, "supervised model storage integrity failed")
    print("Supervised ML TLS linear/logistic/tree/forest, independent held-out metrics, activation and restart passed")


if __name__ == "__main__":
    main()
