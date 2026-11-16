"""Run a persisted retraining recipe over TLS as its original database actor."""

import argparse
from pathlib import Path
import secrets
import ssl
import tempfile
import time

from dataset_rpc import DatasetReader
from jobs_rpc import terminal, wait_for
from model_rpc import REGRESSION_OPTIONS, REGRESSION_SQL, create_dataset, replace_regression_labels, verified_artifact
from service_smoke import certificates, connection, execute, listening, require, save_config
from sql_rpc import client, token, write


PERMISSION_DENIED = 1006
INVALID_STATE = 1013


def next_occurrence(session, schedule):
    end = time.monotonic() + 15
    while time.monotonic() < end:
        state = session.call("job_schedule_status", schedule_id=schedule)
        if state["last_job_id"]:
            session.call("job_schedule_cancel", schedule_id=schedule)
            job_id = state["last_job_id"]
            try:
                return wait_for(session, job_id, terminal)
            except RuntimeError as error:
                status = session.call("job_status", job_id=job_id)
                capabilities = session.call("job_capabilities")
                raise RuntimeError(f"retraining occurrence stalled in {status['state']}, "
                                   f"reason={status['reason']}, scheduler_paused={capabilities['scheduler_paused']}") from error
        time.sleep(.04)
    raise RuntimeError("retraining schedule never became due")


def exercise(port, context, password, guest_password):
    with client(port, context, password) as owner:
        kinds = owner.call("job_capabilities")
        require("retrain" in kinds["kinds"] and "retrain" not in kinds["unavailable_kinds"],
                "retrain capability was not available")
        write(owner, "CREATE TABLE regression_input (sample_key BIGINT PRIMARY KEY, reading BIGINT, outcome_value BIGINT)", 0)
        rows = ",".join(f"({i + 1},{x},{3 * x + 7})" for i, x in enumerate(range(-15, 15)))
        write(owner, "INSERT INTO regression_input VALUES " + rows, 30)
        write(owner, "CREATE USER dataset_reader", 0)
        owner.call("password_set", username="dataset_reader", new_password=guest_password, request_token=token())
        write(owner, "GRANT SELECT ON TABLE regression_input TO USER dataset_reader", 0)
        write(owner, "GRANT DDL ON GLOBAL TO USER dataset_reader", 0)
        owner.call("model_create", request_token=token(), name="foreign_model", algorithm="linear")
        foreign_dataset = create_dataset(owner, REGRESSION_SQL, REGRESSION_OPTIONS)

        with connection(port, context) as socket:
            actor = DatasetReader(socket, guest_password)
            actor.call("model_create", request_token=token(), name="scheduled_model", algorithm="linear")
            snapshot = create_dataset(actor, REGRESSION_SQL, REGRESSION_OPTIONS)
            recipe = {"kind": "retrain", "name": "scheduled_model", "dataset_id": snapshot["dataset_id"], "seed": 23}
            actor.error("job_schedule_create", PERMISSION_DENIED,
                        **dict(recipe, name="foreign_model", request_token=token(), rule={"every_seconds": 60}))
            actor.error("job_schedule_create", PERMISSION_DENIED,
                        **dict(recipe, dataset_id=foreign_dataset["dataset_id"], request_token=token(),
                               rule={"every_seconds": 60}))

            actor.call("tx_begin")
            staged = actor.call("job_schedule_create", **recipe, request_token=token(), rule={"every_seconds": 60})
            actor.call("tx_rollback")
            actor.error("job_schedule_status", PERMISSION_DENIED, schedule_id=staged["schedule_id"])

            far_future = dict(recipe, request_token=token(), rule={"every_seconds": 3600})
            pinned = actor.call("job_schedule_create", **far_future)
            require(actor.call("job_schedule_create", **far_future) == pinned, "retried schedule duplicated work")
            actor.error("job_schedule_create", INVALID_STATE, **dict(far_future, seed=24))
            actor.error("dataset_delete", INVALID_STATE, dataset_id=snapshot["dataset_id"])
            actor.call("job_schedule_cancel", schedule_id=pinned["schedule_id"])

            replace_regression_labels(owner)
            scheduled = actor.call("job_schedule_create", **recipe, request_token=token(),
                                   rule={"every_seconds": 4, "misfire": "run_once"})
            require(scheduled["last_job_id"] is None, "new schedule did not remain pending until its due time")

        finished = next_occurrence(owner, scheduled["schedule_id"])
        require(finished["state"] == "succeeded" and finished["attempt"] == 1 and finished["result"],
                f"scheduled retraining did not finish after its owner disconnected: {finished['reason']}")
        artifact = finished["result"]
        report = verified_artifact(owner, "scheduled_model", "linear", artifact["version"], snapshot, seed=23)
        require(report["run_id"] == finished["job_id"] and report["checksum"] == artifact["checksum"],
                "the scheduled job and the published model version have different identities")
        require(abs(report["artifact"]["intercept"] - 7) < 1e-4 and
                abs(report["artifact"]["coefficients"][0] - 3) < 1e-4,
                "retrain read edited source rows instead of the pinned dataset")
        registry = owner.call("model_status", name="scheduled_model")
        require(len(registry["versions"]) == 1 and registry["active_version"] is None,
                "scheduled publication duplicated a version or unexpectedly activated it")

        with connection(port, context) as socket:
            actor = DatasetReader(socket, guest_password)
            blocked = actor.call("job_schedule_create", **recipe, request_token=token(),
                                 rule={"every_seconds": 4, "misfire": "run_once"}, retries=1, backoff_seconds=1)
        write(owner, "REVOKE SELECT ON TABLE regression_input FROM USER dataset_reader", 0)
        rejected = next_occurrence(owner, blocked["schedule_id"])
        require(rejected["state"] == "failed" and rejected["reason"] == "permission_denied" and
                rejected["attempt"] == 1 and rejected["result"] is None,
                "revoking the scheduled actor's source access did not deny retraining")
        require(owner.call("model_status", name="scheduled_model") == registry,
                "a denied scheduled run changed the published registry")
        return scheduled["schedule_id"], finished["job_id"], rejected["job_id"], registry


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    for argument in ("server", "openssl", "scratch"):
        parser.add_argument("--" + argument, type=Path, required=True)
    args = parser.parse_args()
    server = str(args.server.resolve(strict=True))
    openssl = str(args.openssl.resolve(strict=True))
    args.scratch.mkdir(parents=True, exist_ok=True)
    with tempfile.TemporaryDirectory(prefix="retrain-", dir=args.scratch.resolve()) as directory:
        fixture = Path(directory)
        for name in ("data", "log", "temp"):
            (fixture / name).mkdir()
        certificates(openssl, fixture)
        config = save_config(fixture, "config.json", timeout_ms=20000, max_connections=8)
        require(execute(server, config, "--initialize-storage").returncode == 0, "retrain database initialization failed")
        password, guest_password = secrets.token_urlsafe(32), secrets.token_urlsafe(32)
        require(execute(server, config, "--bootstrap-admin", "local-admin",
                        stdin=((password + "\n") * 2).encode("ascii")).returncode == 0,
                "retrain administrator bootstrap failed")
        context = ssl.create_default_context(cafile=str(fixture / "ca.pem"))
        with listening(server, config) as (_, port):
            schedule, completed, rejected, registry = exercise(port, context, password, guest_password)
        with listening(server, config) as (_, port), client(port, context, password) as owner:
            state = owner.call("job_schedule_status", schedule_id=schedule)
            require(not state["enabled"] and state["last_job_id"] == completed, "restart changed the cancelled schedule")
            require(owner.call("job_status", job_id=completed)["state"] == "succeeded" and
                    owner.call("job_status", job_id=rejected)["state"] == "failed" and
                    owner.call("model_status", name="scheduled_model") == registry,
                    "restart lost or duplicated a scheduled result")
            require(owner.call("history_verify")["verified"], "scheduled publication damaged History")
    print("TLS scheduled retrain: immutable dataset, rights revocation, actual model publication and restart passed")


if __name__ == "__main__":
    main()
