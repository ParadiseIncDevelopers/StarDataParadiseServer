"""Fit and reuse the original CPU models from a disposable MySQL-backed schema."""

import json

from .fixture import COMPATIBILITY, query, require, run


MODEL = COMPATIBILITY / "ml" / "cpu_model.py"


def invoke(instance, *args, failure=None):
    env, _, cli = instance
    result = run([*cli, "--file", str(MODEL), *map(str, args)], env, 45)
    if failure is not None:
        require(result.returncode != 0 and failure in result.stderr, result.stdout + result.stderr)
        return
    require(result.returncode == 0, result.stdout + result.stderr)
    line = next((line for line in result.stdout.splitlines() if line.startswith("SDP_MODEL ")), None)
    require(line is not None, result.stdout + result.stderr)
    return json.loads(line.removeprefix("SDP_MODEL "))


def unsupervised(server, worker, source):
    invoke(server, "train", "clusters", "--algorithm", "kmeans", "--options", '{"k":2}', *source)
    left = invoke(server, "predict", "clusters", 1, '{"x":-2}', "--worker", worker)["prediction"]
    right = invoke(server, "predict", "clusters", 1, '{"x":2}', "--worker", worker)["prediction"]
    require({left, right} == {0, 1}, "wrong K-Means cluster assignments")

    invoke(server, "train", "dense", "--algorithm", "dbscan", "--options", '{"eps":1.1,"min_points":2}', *source)
    partition = query(server, """SELECT JSON_EXTRACT(artifact,'$.model.clusters'),
        JSON_EXTRACT(artifact,'$.model.noise'), JSON_LENGTH(artifact,'$.model.labels')
        FROM sdp_models.model_versions WHERE name='dense'""")
    require(partition == "2\t0\t6", f"wrong DBSCAN training partition: {partition}")
    invoke(server, "predict", "dense", 1, '{"x":2}', "--worker", worker,
           failure="does not predict new records")

    invoke(server, "train", "components", "--algorithm", "pca", "--options", '{"components":1}', *source)
    scores = invoke(server, "predict", "components", 1, '{"x":2}', "--worker", worker)["prediction"]
    require(len(scores) == 1 and abs(abs(scores[0]) - 2) < 1e-6, f"wrong PCA score: {scores}")
    print("PASS original CPU K-Means, DBSCAN and PCA on a real MySQL snapshot")


def neural_network(server, worker, source):
    arguments = ("train", "network", "--algorithm", "mlp", "--label", "class_label", *source)
    invoke(server, *arguments, failure="provide a split column")
    trained = invoke(server, *arguments, "--split-column", "phase", "--options",
                     '{"hidden_layers":[4],"epochs":100,"batch_size":4,"learning_rate":0.05}')
    version = ("predict", "network", trained["version"])
    left = invoke(server, *version, '{"x":-2}', "--worker", worker)["prediction"]
    right = invoke(server, *version, '{"x":2}', "--worker", worker)["prediction"]
    require(left < 0.5 < right, f"wrong MLP probabilities: {left}, {right}")
    require(query(server, """SELECT (JSON_EXTRACT(artifact,'$.model.validation_loss') <
         JSON_EXTRACT(artifact,'$.model.initial_validation_loss')) FROM sdp_models.model_versions
         WHERE name='network'""") == "1", "the original MLP did not report an improved validation loss")
    print("PASS original CPU MLP trained on separate MySQL train and validation rows")


def activation(server, worker, source):
    before = invoke(server, "status", "fit")
    require(before["active_version"] is None and [item["version"] for item in before["versions"]] == [1, 2],
            "training unexpectedly activated a version")
    require(invoke(server, "activate", "fit", 2)["active_version"] == 2, "activation did not persist")
    args = ("predict", "fit", "active", '{"x":4}', "--worker", worker)
    require(abs(invoke(server, *args)["prediction"] - 14) < 1e-6, "active inference did not use version 2")
    invoke(server, "rollback", "fit", 2, failure="older than the active version")
    require(invoke(server, "rollback", "fit", 1)["active_version"] == 1, "model rollback did not persist")
    restored = invoke(server, *args)
    require(restored["version"] == 1 and abs(restored["prediction"] - 9) < 1e-6,
            "active inference did not use restored version 1")

    alternate = (*source[:-1], "y")
    third = invoke(server, "train", "fit", "--algorithm", "linear", "--label", "x", *alternate)
    invoke(server, "activate", "fit", third["version"], failure="feature order differs")
    require(invoke(server, "deactivate", "fit")["active_version"] is None, "model deactivation did not persist")
    invoke(server, *args, failure="model version missing")
    print("PASS model activation, older-version rollback, incompatible features and deactivation")


def caller_source_access(server, worker, source):
    query(server, """CREATE USER 'sdp_ml_user'@'localhost' REQUIRE SSL;
        GRANT SELECT ON sdp_ml_acceptance.samples TO 'sdp_ml_user'@'localhost';
        GRANT SELECT, INSERT ON sdp_models.model_versions TO 'sdp_ml_user'@'localhost';""")
    env, classic, cli = server
    limited = (env, classic, ["--user=sdp_ml_user" if argument == "--user=root" else argument for argument in cli])
    trained = invoke(limited, "train", "restricted", "--algorithm", "linear", "--label", "y", *source)
    arguments = ("predict", "restricted", trained["version"], '{"x":4}', "--worker", worker)
    require(abs(invoke(limited, *arguments)["prediction"] - 14) < 1e-6, "caller could not use own trained model")
    query(server, "REVOKE SELECT ON sdp_ml_acceptance.samples FROM 'sdp_ml_user'@'localhost'")
    invoke(limited, *arguments, failure="SELECT command denied")
    print("PASS model inference rechecks the training source on the current MySQL account")


def verify(fixture):
    with fixture.scratch("models") as root:
        with fixture.disposable(root / "server") as server:
            query(server, """CREATE SCHEMA sdp_ml_acceptance;
                CREATE TABLE sdp_ml_acceptance.samples (id INT PRIMARY KEY,x DOUBLE,y DOUBLE,
                    class_label TINYINT, phase VARCHAR(16)) ENGINE=InnoDB;
                INSERT INTO sdp_ml_acceptance.samples VALUES
                    (1,-3,-5,0,'train'),(2,-2,-3,0,'train'),(3,-1,-1,0,'validation'),
                    (4,1,3,1,'validation'),(5,2,5,1,'train'),(6,3,7,1,'train');""")
            invoke(server, "install")
            source = ("--worker", fixture.worker, "--schema", "sdp_ml_acceptance", "--table", "samples",
                      "--order-key", "id", "--feature", "x")
            v1 = invoke(server, "train", "fit", "--algorithm", "linear", "--label", "y", *source)
            require(v1["version"] == 1 and len(v1["sha256"]) == 64, "missing version or artifact digest")
            query(server, "UPDATE sdp_ml_acceptance.samples SET y = y + 5")
            v2 = invoke(server, "train", "fit", "--algorithm", "linear", "--label", "y", *source)
            require(v2["version"] == 2 and v1["sha256"] != v2["sha256"], "model was not versioned")
            predict = ("predict", "fit")
            p1 = invoke(server, *predict, 1, '{"x":4}', "--worker", fixture.worker)["prediction"]
            p2 = invoke(server, *predict, 2, '{"x":4}', "--worker", fixture.worker)["prediction"]
            require(abs(p1 - 9) < 1e-6 and abs(p2 - 14) < 1e-6, f"bad regression values: {p1}, {p2}")
            for algorithm in ("logistic", "tree", "forest"):
                classifier = invoke(server, "train", algorithm, "--algorithm", algorithm, "--label", "class_label", *source)
                arguments = ("predict", algorithm, classifier["version"])
                left = invoke(server, *arguments, '{"x":-2}', "--worker", fixture.worker)["prediction"]
                right = invoke(server, *arguments, '{"x":2}', "--worker", fixture.worker)["prediction"]
                require(left < 0.5 < right, f"wrong {algorithm} probabilities: {left}, {right}")
            invoke(server, "train", "fit", "--algorithm", "tree", "--label", "class_label", *source,
                   failure="different algorithm")
            invoke(server, *predict, 1, '{"unknown":4}', "--worker", fixture.worker, failure="trained feature names")
            activation(server, fixture.worker, source)
            query(server, "UPDATE sdp_models.model_versions SET sha256 = REPEAT('0',64) WHERE name='fit' AND version=1")
            invoke(server, *predict, 1, '{"x":4}', "--worker", fixture.worker, failure="checksum differs")
            print("PASS original CPU linear, logistic, tree and forest: immutable versions, prediction, checksum validation")
            unsupervised(server, fixture.worker, source)
            neural_network(server, fixture.worker, source)
            caller_source_access(server, fixture.worker, source)
