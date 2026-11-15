"""Verify real clustering, PCA and experiment comparisons over TLS."""

import argparse
import math
from pathlib import Path
import secrets
import ssl
import statistics
import tempfile

from jobs_rpc import submit, terminal, wait_for
from model_rpc import create_dataset
from service_smoke import certificates, execute, listening, require, save_config
from sql_rpc import client, token, write


INVALID_ARGUMENT = 1012
FEATURE_UNAVAILABLE = 1004
RESOURCE_LIMIT = 1014
FEATURES = [{"column": column, "transform": "numeric", "impute": "reject"} for column in ("coordinate_a", "coordinate_b")]
OPTIONS = {"features": FEATURES, "split": {"mode": "time", "time": "sample_key", "train": 60, "validation": 20}}
LEFT = [(-20, 0), (-21, 0), (-20, 1), (-19, 0), (-20, -1)]
RIGHT = [(20, 0), (21, 0), (20, 1), (19, 0), (20, -1)]
HELD_OUT = [(-19, 0), (-20, 1), (19, 0), (20, 1), (-21, 0), (-20, -1), (21, 0), (20, -1)]


def install_data(session, name, points):
    write(session, f"CREATE TABLE {name} (sample_key BIGINT PRIMARY KEY, coordinate_a BIGINT, coordinate_b BIGINT)", 0)
    values = ",".join(f"({index},{a},{b})" for index, (a, b) in enumerate(points, 1))
    write(session, f"INSERT INTO {name} VALUES " + values, len(points))
    return create_dataset(session, f"SELECT sample_key,coordinate_a,coordinate_b FROM {name} ORDER BY sample_key", OPTIONS)


def training_request(name, dataset, options, seed=57):
    return {"request_token": token(), "name": name, "dataset_id": dataset["dataset_id"], "seed": seed, "options": options}


def train_model(session, name, dataset, options, seed=57):
    queued = session.call("model_train", **training_request(name, dataset, options, seed))
    finished = wait_for(session, queued["job_id"], terminal)
    require(finished["state"] == "succeeded" and finished["result"], f"{name} training failed ({finished.get('reason')})")
    version = finished["result"]["version"]
    report = session.call("model_evaluate", name=name, version=version)
    require(report["version"] == version and report["dataset_id"] == dataset["dataset_id"]
            and finished["result"]["checksum"] == report["checksum"] and report["seed"] == seed,
            "unsupervised artifact did not retain worker identity, dataset or seed")
    return report


def create_model(session, name, algorithm):
    return session.call("model_create", request_token=token(), name=name, algorithm=algorithm)


def activate(session, name, report):
    session.call("model_activate", request_token=token(), name=name, version=report["version"])


def predict(session, name, point):
    a, b = point
    return session.call("model_predict", name=name, features={"coordinate_a": a, "coordinate_b": b})


def point_at(dataset, index):
    return dataset["source"]["rows"][index][1:]


def own_silhouette(points, labels):
    values = []
    for current, label in enumerate(labels):
        per_group = {}
        for neighbor, other in enumerate(labels):
            if current != neighbor:
                per_group.setdefault(other, []).append(math.dist(points[current], points[neighbor]))
        inner = statistics.mean(per_group.pop(label))
        outer = min(statistics.mean(group) for group in per_group.values())
        values.append((outer - inner) / max(outer, inner))
    return statistics.mean(values)


def kmeans_case(session, dataset):
    name = "cluster_model"
    create_model(session, name, "kmeans")
    report = train_model(session, name, dataset, {"k": 2})
    activate(session, name, report)
    left = predict(session, name, (-20, 0))
    right = predict(session, name, (20, 0))
    require(left["version"] == right["version"] == report["version"]
            and left["cluster"] == left["prediction"] and right["cluster"] == right["prediction"]
            and left["cluster"] != right["cluster"], "K-Means assignment did not separate the two known groups")
    centers = sorted(report["artifact"]["centers"])
    require(len(centers) == 2 and math.dist(centers[0], (-20, 0)) < 1e-9 and math.dist(centers[1], (20, 0)) < 1e-9,
            "K-Means centers did not match independently known training centroids")
    labels = report["artifact"]["labels"]
    training = dataset["prepared"]["split"]["train"]
    require(labels == [left["cluster"] if point_at(dataset, i)[0] < 0 else right["cluster"] for i in training],
            "K-Means training labels did not match cluster membership")
    for partition, indices in dataset["prepared"]["split"].items():
        points = [point_at(dataset, i) for i in indices]
        groups = [predict(session, name, point)["cluster"] for point in points]
        metric = report["metrics"][partition]
        require(metric["count"] == len(points) and metric["silhouette_reason"] is None
                and math.isclose(metric["silhouette"], own_silhouette(points, groups), abs_tol=1e-10),
                f"K-Means {partition} silhouette differs from the independent Euclidean calculation")
    alternate = train_model(session, name, dataset, {"k": 2}, seed=93)
    require(session.call("model_status", name=name)["active_version"] == 1 and alternate["version"] == 2,
            "training a comparison run changed the active model")
    return report, alternate


def dbscan_case(session, dataset):
    name = "density_model"
    create_model(session, name, "dbscan")
    report = train_model(session, name, dataset, {"eps": 2, "min_points": 3})
    activate(session, name, report)
    labels = report["artifact"]["labels"]
    require(len(labels) == 12 and len(set(labels[:5])) == 1 and len(set(labels[5:10])) == 1
            and labels[0] >= 0 and labels[5] >= 0 and labels[0] != labels[5] and labels[10:] == [-1, -1],
            "DBSCAN did not discover both compact groups and the two known points of noise")
    metric = report["metrics"]["train"]
    require(metric["silhouette_samples"] == len(LEFT + RIGHT) and metric["silhouette_reason"] is None
            and math.isclose(metric["silhouette"], own_silhouette(LEFT + RIGHT, labels[:10]), abs_tol=1e-10),
            "DBSCAN training silhouette disagrees with the independent computation excluding noise")
    for partition in ("validation", "test"):
        metric = report["metrics"][partition]
        require(metric["silhouette"] is None and metric["silhouette_reason"] == "out_of_sample_unavailable",
                "DBSCAN returned a speculative held-out silhouette")
    session.error("model_predict", FEATURE_UNAVAILABLE, name=name, features={"coordinate_a": -20, "coordinate_b": 0})
    alternate = train_model(session, name, dataset, {"eps": 8, "min_points": 3})
    expanded = alternate["artifact"]["labels"]
    require(expanded[10] == expanded[0] and expanded[11] == -1,
            "the larger DBSCAN radius did not include the nearby point and preserve the far noise")
    comparison = session.call("model_compare", runs=[{"name": name, "version": model["version"]}
                                                       for model in (report, alternate)], split="train")
    require(not comparison["comparable"] and comparison["reason"] == "different_sample_scope",
            "DBSCAN runs scored over different non-noise samples were compared")
    return report


def pca_case(session, dataset):
    name = "projection_model"
    create_model(session, name, "pca")
    report = train_model(session, name, dataset, {"components": 1})
    activate(session, name, report)
    artifact = report["artifact"]
    component, = artifact["components"]
    require(len(component) == 2 and math.isclose(math.hypot(*component), 1, abs_tol=1e-9)
            and math.isclose(abs((component[0] + 2 * component[1]) / math.sqrt(5)), 1, abs_tol=1e-9)
            and math.isclose(artifact["explained_variance_ratio"][0], 1, abs_tol=1e-9)
            and artifact["explained_variance"][0] > 0,
            "PCA did not learn the unit direction or variance of the known affine line")
    training = [point_at(dataset, index) for index in dataset["prepared"]["split"]["train"]]
    center = [statistics.mean(column) for column in zip(*training, strict=True)]
    require(math.dist(center, artifact["center"]) < 1e-9, "PCA centered using holdout data")
    point = (19, 44)
    predicted = predict(session, name, point)
    expected_score = sum((value - mean) * coordinate for value, mean, coordinate in zip(point, center, component, strict=True))
    projected_x = (point[0] + 2 * (point[1] - 1)) / 5
    require(predicted["version"] == report["version"] and len(predicted["prediction"]) == 1
            and math.isclose(predicted["prediction"][0], expected_score, abs_tol=1e-9)
            and math.dist(predicted["reconstruction"], (projected_x, 2 * projected_x + 1)) < 1e-9,
            "PCA projection or reconstruction disagrees with the independent geometric result")
    for partition, indices in dataset["prepared"]["split"].items():
        metric = report["metrics"][partition]
        require(metric["count"] == len(indices) and metric["rmse"] < 1e-9,
                "rank-one source data could not be reconstructed from one fitted principal component")
    return report


def experiments(session, clean, dbscan):
    def run(name, version=1):
        return {"name": name, "version": version}

    compared = session.call("model_compare", runs=[run("cluster_model"), run("cluster_model", 2)], split="test")
    require(compared["comparable"] and compared["reason"] is None and compared["split"] == "test"
            and [result["version"] for result in compared["runs"]] == [1, 2]
            and all(result["dataset_id"] == clean["dataset_id"] and result["metric"] == "silhouette_all_samples"
                    and result["metrics"]["silhouette"] is not None for result in compared["runs"]),
            "comparable K-Means runs from the same dataset were refused")
    incomparable = session.call("model_compare", runs=[run("cluster_model"), run("density_model", dbscan["version"])],
                                split="test")
    require(not incomparable["comparable"] and incomparable["reason"] == "different_dataset",
            "K-Means and DBSCAN from different datasets were compared")
    other_snapshot = create_dataset(session, "SELECT sample_key,coordinate_a,coordinate_b FROM cluster_points ORDER BY sample_key",
                                    OPTIONS)
    create_model(session, "other_snapshot_model", "kmeans")
    train_model(session, "other_snapshot_model", other_snapshot, {"k": 2})
    different = session.call("model_compare", runs=[run("cluster_model"), run("other_snapshot_model")], split="test")
    require(not different["comparable"] and different["reason"] == "different_dataset",
            "different dataset snapshots of the same rows were incorrectly comparable")

    create_model(session, "another_projection", "pca")
    train_model(session, "another_projection", clean, {"components": 1})
    mismatch = session.call("model_compare", runs=[run("cluster_model"), run("another_projection")], split="test")
    require(not mismatch["comparable"] and mismatch["reason"] == "different_metric",
            "different metric families were compared on the same dataset")

    single = train_model(session, "cluster_model", clean, {"k": 1})
    require(all(metric["silhouette"] is None and metric["silhouette_reason"] == "insufficient_clusters"
                for metric in single["metrics"].values()), "a one-cluster result received a numeric silhouette")
    undefined = session.call("model_compare", runs=[run("cluster_model"), run("cluster_model", single["version"])],
                             split="test")
    require(not undefined["comparable"] and undefined["reason"] == "undefined_metric",
            "experiment comparison included an undefined silhouette")


def verify_invalid(session, clean, noisy):
    for name, dataset, options in (
        ("cluster_model", clean, {"k": 0}),
        ("cluster_model", clean, {"k": 40}),
        ("density_model", noisy, {"eps": 0, "min_points": 3}),
        ("projection_model", clean, {"components": 3}),
    ):
        before = session.call("model_status", name=name)
        answer = session.raw("model_train", **training_request(name, dataset, options))
        if answer["ok"]:
            invalid = wait_for(session, answer["body"]["job_id"], terminal)
            require(invalid["state"] == "failed" and invalid["result"] is None and invalid["reason"],
                    "invalid unsupervised training produced a result")
        else:
            require(answer["error"]["code"] == INVALID_ARGUMENT, "invalid training returned an unrelated error")
        require(session.call("model_status", name=name) == before, "failed training changed the model registry")
    session.error("dataset_create", INVALID_ARGUMENT, request_token=token(),
                  sql="SELECT sample_key,coordinate_a,coordinate_b FROM cluster_points WHERE sample_key=0",
                  options=OPTIONS)


def verify_cancel(session, clean, temp):
    before = session.call("model_status", name="cluster_model")
    session.error("model_train", RESOURCE_LIMIT,
                  **training_request("cluster_model", clean, {"k": 2, "oversize": "x" * 1024}))
    require(not any(temp.iterdir()) and session.call("model_status", name="cluster_model") == before,
            "an over-quota request changed the model or left temporary resources")
    blocker = submit(session, 1_000_000_000, timeout_ms=60000)
    active = wait_for(session, blocker, lambda status: status["worker_pid"] is not None or terminal(status))
    require(active["state"] == "running", "cancellation fixture could not occupy the isolated worker")
    staged = session.call("model_train", **training_request("cluster_model", clean, {"k": 2}, seed=112))
    require(staged["state"] == "queued", "candidate bypassed shared job scheduling")
    session.call("job_cancel", job_id=staged["job_id"])
    cancelled = wait_for(session, staged["job_id"], terminal)
    require(cancelled["state"] == "cancelled" and cancelled["result"] is None
            and session.call("model_status", name="cluster_model") == before,
            "cancelled candidate published a partial artifact")
    session.call("job_cancel", job_id=blocker)
    require(wait_for(session, blocker, terminal)["state"] == "cancelled" and not any(temp.iterdir()),
            "cancelling isolated work leaked temporary resources")


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    for argument in ("server", "openssl", "scratch"):
        parser.add_argument("--" + argument, type=Path, required=True)
    args = parser.parse_args()
    server = str(args.server.resolve(strict=True))
    openssl = str(args.openssl.resolve(strict=True))
    args.scratch.mkdir(parents=True, exist_ok=True)
    with tempfile.TemporaryDirectory(prefix="s26-", dir=args.scratch.resolve()) as directory:
        fixture = Path(directory)
        for name in ("data", "log", "temp"):
            (fixture / name).mkdir()
        certificates(openssl, fixture)
        config = save_config(fixture, "config.json", timeout_ms=20000, max_connections=8)
        require(execute(server, config, "--initialize-storage").returncode == 0, "unsupervised storage setup failed")
        password = secrets.token_urlsafe(32)
        require(execute(server, config, "--bootstrap-admin", "local-admin",
                        stdin=((password + "\n") * 2).encode("ascii")).returncode == 0,
                "unsupervised administrator setup failed")
        context = ssl.create_default_context(cafile=str(fixture / "ca.pem"))
        with listening(server, config) as (_, port), client(port, context, password) as owner:
            require(owner.call("capabilities")["features"].get("unsupervised_ml_v1") is True,
                    "unsupervised ML capability was unavailable")
            clean = install_data(owner, "cluster_points", LEFT + RIGHT + [(-20, 0), (20, 0)] + HELD_OUT)
            noisy = install_data(owner, "noise_points", LEFT + RIGHT + [(-20, 8), (0, 45)] + HELD_OUT)
            line = install_data(owner, "projection_points", [(value, 2 * value + 1) for value in range(-10, 10)])
            original, alternate = kmeans_case(owner, clean)
            dbscan = dbscan_case(owner, noisy)
            pca = pca_case(owner, line)
            experiments(owner, clean, dbscan)
            verify_invalid(owner, clean, noisy)
            verify_cancel(owner, clean, fixture / "temp")
        with listening(server, config) as (_, port), client(port, context, password) as reopened:
            for name, report in (("cluster_model", original), ("density_model", dbscan), ("projection_model", pca)):
                require(reopened.call("model_evaluate", name=name) == report,
                        f"restart lost the {name} artifact, metric or active version")
            require(reopened.call("model_evaluate", name="cluster_model", version=2) == alternate
                    and reopened.call("history_verify")["verified"], "restart lost immutable comparison or journal")
        require(execute(server, config, "--check-storage").returncode == 0, "unsupervised storage integrity failed")
    print("Unsupervised ML TLS training clusters/noise, silhouette, PCA reconstruction, compare, quota/cancel and restart passed")


if __name__ == "__main__":
    main()
