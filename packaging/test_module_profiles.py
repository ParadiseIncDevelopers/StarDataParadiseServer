"""Verify the package's Server feature flags on one disposable TLS instance."""

import argparse
import json
from pathlib import Path
import secrets
import ssl
import sys
import tempfile

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "tests"))
from service_smoke import certificates, execute, listening, require, save_config
from sql_rpc import client, i64, token, write


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    for name in ("server", "openssl", "scratch", "defaults", "optional"):
        parser.add_argument("--" + name, type=Path, required=True)
    args = parser.parse_args()
    args.scratch.mkdir(parents=True, exist_ok=True)
    defaults = json.loads(args.defaults.read_text(encoding="utf-8-sig"))
    modules = {}
    for module in ("cpu-learning", "deep-learning", "decision"):
        modules.update(json.loads((args.optional / (module + ".json")).read_text(encoding="utf-8-sig")))
    feature_names = ("decision_v1", "dataset_v1", "supervised_ml_v1", "unsupervised_ml_v1",
                     "model_lifecycle_v1", "mlp_cpu_v1", "scheduled_retrain_v1")

    with tempfile.TemporaryDirectory(prefix="package-modules-", dir=args.scratch.resolve()) as folder:
        root = Path(folder)
        for name in ("data", "log", "temp"):
            (root / name).mkdir()
        certificates(str(args.openssl.resolve()), root)
        base = save_config(root, "core.json", **defaults, timeout_ms=20000)
        complete = save_config(root, "complete.json", **{**defaults, **modules}, timeout_ms=20000)
        server = str(args.server.resolve())
        password = secrets.token_urlsafe(32)
        require(execute(server, base, "--initialize-storage").returncode == 0, "Core initialize failed")
        require(execute(server, base, "--bootstrap-admin", "local-admin",
                        stdin=((password + "\n") * 2).encode("ascii")).returncode == 0, "Core bootstrap failed")
        context = ssl.create_default_context(cafile=str(root / "ca.pem"))
        with listening(server, base) as (_, port), client(port, context, password) as owner:
            features = owner.call("capabilities")["features"]
            require(all(features.get(name) is False for name in feature_names), "Core exposed an optional feature")
            require(features["history_v1"] and features["scheduled_sql_v1"] and not features["ml_gpu_v1"],
                    "Core capabilities were incorrect")
            write(owner, "CREATE TABLE installed_state (id BIGINT PRIMARY KEY)", 0)
            owner.call("tx_begin")
            owner.mutate("INSERT INTO installed_state VALUES (7)")
            owner.call("tx_commit", request_token=token())
            require(owner.call("history_verify")["verified"], "Core History verification failed")
            owner.error("model_create", 1007, request_token=token(), name="optional_model", algorithm="linear")
            require("retrain" in owner.call("job_capabilities")["unavailable_kinds"], "Core could schedule retraining")
        with listening(server, complete) as (_, port), client(port, context, password) as owner:
            features = owner.call("capabilities")["features"]
            require(all(features.get(name) is True for name in feature_names), "Complete did not enable optional features")
            require(not features["ml_gpu_v1"], "Complete enabled an unavailable GPU backend")
            require(owner.query("SELECT id FROM installed_state")[0] == [[i64(7)]], "Optional modules altered existing data")
            owner.call("model_create", request_token=token(), name="optional_model", algorithm="linear")
            require("retrain" in owner.call("job_capabilities")["schedule_kinds"], "Optional ML did not enable retraining")
            require(owner.call("history_verify")["verified"], "Optional modules damaged History")
        with listening(server, base) as (_, port), client(port, context, password) as owner:
            owner.error("model_status", 1007, name="optional_model")
            require(owner.query("SELECT id FROM installed_state")[0] == [[i64(7)]], "Core changed data after module disable")
        with listening(server, complete) as (_, port), client(port, context, password) as owner:
            require(owner.call("model_status", name="optional_model")["algorithm"] == "linear",
                    "Optional model was lost across module disable/enable")
    print("Core SQL/transaction/History, optional model and Decision capabilities, and model retention passed")


if __name__ == "__main__":
    main()
