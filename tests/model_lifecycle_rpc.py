"""Test checkpoint recovery, model lifecycle and device admission over TLS."""

import argparse
from concurrent.futures import ThreadPoolExecutor
import math
from pathlib import Path
import secrets
import ssl
import tempfile
from threading import Event

from jobs_rpc import terminal, wait_for
from mlp_rpc import NAME, OPTIONS, create_xor_dataset, prediction, training_request
from model_rpc import SHA256, activate
from service_smoke import certificates, execute, listening, require, save_config
from sql_rpc import client, token


INVALID_STATE = 1013
FEATURE_UNAVAILABLE = 1004
PAUSE_EPOCH = 13


def train(session, dataset, seed=2027, **extras):
    request = training_request(dataset, OPTIONS, seed) | extras
    return session.call("model_train", **request)["job_id"]


def completed(session, job_id, dataset, version):
    result = wait_for(session, job_id, terminal)
    published = result["result"]
    require(result["state"] == "succeeded" and published and published["version"] == version
            and published["dataset_id"] == dataset["dataset_id"], f"training candidate {version} failed ({result.get('reason')})")
    report = session.call("model_evaluate", name=NAME, version=version)
    require(report["checksum"] == published["checksum"] and SHA256.fullmatch(report["checksum"]),
            "job and registry report different artifact identities")
    return report


def checkpoint(session, dataset):
    session.call("model_create", request_token=token(), name=NAME, algorithm="mlp")
    uninterrupted = completed(session, train(session, dataset), dataset, 1)
    activate(session, NAME, 1)
    paused = train(session, dataset, pause_after_epoch=PAUSE_EPOCH)
    status = wait_for(session, paused, lambda job: job["state"] == "paused" or terminal(job))
    require(status["state"] == "paused", f"checkpoint job did not pause ({status.get('reason')})")
    manifest = session.call("model_checkpoint", name=NAME, job_id=paused)
    require(manifest["job_id"] == paused and manifest["dataset_id"] == dataset["dataset_id"]
            and manifest["epoch"] == PAUSE_EPOCH and manifest["runtime"] == uninterrupted["runtime"]
            and SHA256.fullmatch(manifest["checksum"]), "checkpoint omitted its dataset, runtime, epoch or SHA256 identity")
    require(session.call("model_status", name=NAME)["active_version"] == 1,
            "pausing a training candidate changed current inference")
    return manifest, uninterrupted


def resume_after_restart(session, dataset, manifest, uninterrupted):
    paused = manifest["job_id"]
    require(session.call("job_status", job_id=paused)["state"] == "paused"
            and session.call("model_checkpoint", name=NAME, job_id=paused) == manifest,
            "restart changed the durable paused job or checkpoint manifest")
    good = manifest["checksum"]
    bad = ("1" if good[0] == "0" else "0") + good[1:]
    request = {"name": NAME, "job_id": paused, "request_token": token()}
    session.error("model_resume", INVALID_STATE, checksum=bad, **request)
    require(session.call("job_status", job_id=paused)["state"] == "paused"
            and session.call("model_status", name=NAME)["active_version"] == 1,
            "rejecting a wrong checksum changed the checkpoint or active model")
    updated = session.call("model_resume", checksum=good, **request)
    resumed = updated["job_id"]
    require(resumed != paused, "resume did not create a separately identifiable attempt")
    report = completed(session, resumed, dataset, 2)
    require(report["artifact"] == uninterrupted["artifact"] and report["metrics"] == uninterrupted["metrics"]
            and report["seed"] == uninterrupted["seed"],
            "resumed nontrivial CPU training diverged from the uninterrupted weights, optimizer metadata or splits")
    require(session.call("job_status", job_id=paused)["state"] == "paused"
            and session.call("model_status", name=NAME)["active_version"] == 1,
            "publishing an inactive resumed version changed the source pause or active model")
    return report


def device_paths(session, dataset):
    devices = session.call("model_devices")
    require(devices["cpu_available"] is True and devices["gpu_available"] is False
            and devices["gpu_verified"] is False and devices["gpu_reason"]
            and isinstance(devices.get("devices", []), list),
            "GPU device capability claimed an unavailable or unverified backend")
    gpu = {"device": "gpu", "device_index": 0, "memory_mib": 128, "allow_cpu_fallback": False}
    request = training_request(dataset, OPTIONS, seed=2100) | gpu
    session.error("model_train", FEATURE_UNAVAILABLE, **request)
    gpu["allow_cpu_fallback"] = True
    report = completed(session, train(session, dataset, seed=2100, **gpu), dataset, 3)
    hardware = report["hardware"]
    require(hardware["backend"] == "cpu" and hardware["requested_device"] == "gpu" and hardware["fallback_reason"],
            "approved fallback did not persist the actual CPU backend and requested GPU separately")
    return report


def concurrent_activation(session, port, context, password):
    old_ready = Event()
    changed = Event()
    point = (-3, 4)
    old = prediction(session, *point)
    require(old["version"] == 1, "checkpoint or GPU training unexpectedly activated its result")

    def sample():
        with client(port, context, password) as reader:
            observed = [prediction(reader, *point)]
            old_ready.set()
            for _ in range(24):
                observed.append(prediction(reader, *point))
                if changed.is_set() and observed[-1]["version"] == 3:
                    break
            require(changed.wait(timeout=12), "concurrent activation was never performed")
            observed.append(prediction(reader, *point))
            return observed

    with ThreadPoolExecutor(max_workers=1) as pool:
        samples = pool.submit(sample)
        require(old_ready.wait(timeout=12), "inference reader did not observe the active version")
        activate(session, NAME, 3)
        changed.set()
        observed = samples.result(timeout=15)
    new = prediction(session, *point)
    require(new["version"] == 3 and not math.isclose(old["probability"], new["probability"], abs_tol=1e-6),
            "concurrent activation fixture requires two measurably distinct predictions")
    require(observed[0] == old and observed[-1] == new
            and all(snapshot in (old, new) for snapshot in observed),
            "parallel inference mixed model versions, preprocessing or predictions")


def lifecycle(session, resumed):
    session.error("model_delete", INVALID_STATE, name=NAME, version=3, request_token=token())
    session.call("model_rollback", name=NAME, version=1, request_token=token())
    require(prediction(session, -3, 4)["version"] == 1, "rollback did not atomically restore the chosen version")
    revision = session.call("history_status")["current_revision"]
    pin = {"pin_kind": "snapshot", "pin_name": "stage28-artifact"}
    session.call("history_pin", revision=revision, **pin)
    session.error("model_delete", INVALID_STATE, name=NAME, version=3, request_token=token())
    require(session.call("model_evaluate", name=NAME, version=3)["version"] == 3,
            "pinned inactive version was retired")
    session.call("history_unpin", **pin)
    session.call("model_delete", name=NAME, version=3, request_token=token())
    session.error("model_evaluate", INVALID_STATE, name=NAME, version=3)
    require([version["version"] for version in session.call("model_status", name=NAME)["versions"]] == [1, 2],
            "retiring an unpinned artifact changed the remaining version identities")
    session.call("model_deactivate", name=NAME, request_token=token())
    require(session.call("model_status", name=NAME)["active_version"] is None,
            "deactivation left an artifact serving new reads")
    session.error("model_predict", INVALID_STATE, name=NAME, features={"axis_a": -3, "axis_b": 4})
    require(session.call("model_evaluate", name=NAME, version=2) == resumed, "deactivation lost immutable history")
    activate(session, NAME, resumed["version"])


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    for argument in ("server", "openssl", "scratch"):
        parser.add_argument("--" + argument, type=Path, required=True)
    args = parser.parse_args()
    server = str(args.server.resolve(strict=True))
    openssl = str(args.openssl.resolve(strict=True))
    args.scratch.mkdir(parents=True, exist_ok=True)
    with tempfile.TemporaryDirectory(prefix="s28-", dir=args.scratch.resolve()) as directory:
        fixture = Path(directory)
        for name in ("data", "log", "temp"):
            (fixture / name).mkdir()
        certificates(openssl, fixture)
        config = save_config(fixture, "config.json", timeout_ms=20000, max_connections=8)
        require(execute(server, config, "--initialize-storage").returncode == 0, "checkpoint storage setup failed")
        password = secrets.token_urlsafe(32)
        require(execute(server, config, "--bootstrap-admin", "local-admin",
                        stdin=((password + "\n") * 2).encode("ascii")).returncode == 0,
                "checkpoint administrator setup failed")
        context = ssl.create_default_context(cafile=str(fixture / "ca.pem"))
        with listening(server, config) as (_, port), client(port, context, password) as owner:
            dataset = create_xor_dataset(owner, 12)
            manifest, uninterrupted = checkpoint(owner, dataset)
        with listening(server, config) as (_, port), client(port, context, password) as owner:
            resumed = resume_after_restart(owner, dataset, manifest, uninterrupted)
            device_paths(owner, dataset)
            concurrent_activation(owner, port, context, password)
            lifecycle(owner, resumed)
            require(owner.call("history_verify")["verified"] and not any((fixture / "temp").iterdir()),
                    "model lifecycle left inconsistent History or worker scratch")
        with listening(server, config) as (_, port), client(port, context, password) as owner:
            require(owner.call("model_evaluate", name=NAME) == resumed, "restart changed the reactivated resumed artifact")
        require(execute(server, config, "--check-storage").returncode == 0, "checkpoint storage integrity failed")
    print("Model lifecycle TLS restart/resume, damaged checksum, concurrent activation and approved CPU fallback passed")


if __name__ == "__main__":
    main()
