"""Verify legacy CPU model RPCs on a provisioned disposable fork."""

import math
from acceptance import require
from durable_acceptance import client, token


def verify(port, tls, owner, stranger, revoke_source):
    with (client(port, tls, *owner) as model,
          client(port, tls, *stranger) as someone_else):
        name = "forecast"
        model.error("model_predict", 1013, name=name, features={"reading": 7})
        model.error("model_activate", 1012, name=name, version=0, request_token=token())
        someone_else.error("model_activate", 1006, name=name, version=1, request_token=token())
        someone_else.error("model_predict", 1006, name=name, features={"reading": 7})
        model.error("model_status", 1004, name=name)
        model.error("model_compare", 1004, runs=[], split="test")
        model.call("tx_begin")
        model.error("model_predict", 1013, name=name, features={"reading": 7})
        model.call("tx_rollback")

        first = token()
        enabled = model.call("model_activate", name=name, version=1, request_token=first)
        require(enabled == {"version": 1, "active_version": 1, "lsn": enabled["lsn"]}
                and enabled["lsn"].isdigit(), "activation legacy shape")
        require(model.call("tx_outcome", request_token=first) == {"status": "committed", "lsn": enabled["lsn"]},
                "activation has no durable outcome")
        require(someone_else.call("tx_outcome", request_token=first) == {"status": "unknown"},
                "other account visibility into activation receipt")
        model.error("model_activate", 1013, name=name, version=2, request_token=first)
        result = model.call("model_predict", name=name, features={"reading": 7})
        require(result.keys() == {"version", "algorithm", "prediction"} and result["version"] == 1
                and result["algorithm"] == "linear" and math.isclose(result["prediction"], 15, abs_tol=1e-9),
                "linear prediction legacy shape/duplicate token safety")
        model.error("model_predict", 1012, name=name, features={"wrong": 7})
        model.error("model_predict", 1012, name=name, features={"reading": True})
        model.error("model_predict", 1012, name=name, version=1, features={"reading": 7})
        model.call("model_activate", name=name, version=2, request_token=token())
        result = model.call("model_predict", name=name, features={"reading": 7})
        require(result["version"] == 2 and math.isclose(result["prediction"], 20, abs_tol=1e-9),
                "second active version prediction")
        model.error("model_rollback", 1013, name=name, version=2, request_token=token())
        restored = token()
        result = model.call("model_rollback", name=name, version=1, request_token=restored)
        require(result == {"version": 1, "active_version": 1, "lsn": result["lsn"]}
                and model.call("tx_outcome", request_token=restored)["lsn"] == result["lsn"],
                "rollback legacy shape/durability")
        model.call("model_activate", name="classifier", version=1, request_token=token())
        result = model.call("model_predict", name="classifier", features={"reading": 3})
        require(result.keys() == {"version", "algorithm", "prediction", "label", "probability"}
                and result["algorithm"] == "logistic" and result["version"] == 1
                and result["label"] == result["prediction"] == 1 and 0.5 < result["probability"] <= 1,
                "classifier legacy shape")
        model.call("model_activate", name="clusters", version=1, request_token=token())
        result = model.call("model_predict", name="clusters", features={"reading": 3})
        require(result.keys() == {"version", "algorithm", "prediction", "cluster"}
                and result["algorithm"] == "kmeans" and result["version"] == 1
                and result["cluster"] == result["prediction"] in (0, 1), "K-Means legacy shape")

        removed = token()
        disabled = model.call("model_deactivate", name=name, request_token=removed)
        require(disabled == {"active_version": None, "lsn": disabled["lsn"]}
                and model.call("tx_outcome", request_token=removed)["lsn"] == disabled["lsn"],
                "deactivation shape/durability")
        model.error("model_predict", 1013, name=name, features={"reading": 7})
        model.call("model_activate", name=name, version=1, request_token=token())
        revoke_source()
        model.error("model_predict", 1006, name=name, features={"reading": 7})
        denied = token()
        model.error("model_activate", 1006, name=name, version=2, request_token=denied)
        require(model.call("tx_outcome", request_token=denied) == {"status": "unknown"},
                "failed activation left a receipt")

    print("PASS legacy model activation, rollback, deactivation, exact prediction shapes and durable receipts",
          flush=True)
    print("PASS model actor ownership, source SELECT revocation, duplicate token and transaction guards", flush=True)
