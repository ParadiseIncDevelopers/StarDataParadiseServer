"""Train and run the original CPU models on a MySQL snapshot."""

import argparse
from contextlib import contextmanager
from decimal import Decimal
import hashlib
import json
import math
from pathlib import Path
import re
import subprocess
import sys


TABLE = "sdp_models.model_versions"
ACTIVE = "sdp_models.active_models"
ALGORITHMS = ("linear", "logistic", "tree", "forest", "kmeans", "dbscan", "pca", "mlp")
UNSUPERVISED = {"kmeans", "dbscan", "pca"}
SAFE_NAME = re.compile(r"[A-Za-z_][A-Za-z0-9_]{0,63}")


def identifier(value):
    if SAFE_NAME.fullmatch(value) is None:
        raise argparse.ArgumentTypeError("expected a simple SQL identifier")
    return value


def canonical(value):
    return json.dumps(value, sort_keys=True, separators=(",", ":"), allow_nan=False)


def sha(text):
    return hashlib.sha256(text.encode("utf-8")).hexdigest()


def run_model(worker, payload):
    result = subprocess.run([str(worker.resolve(strict=True))], input=canonical(payload),
                            capture_output=True, text=True, encoding="utf-8", timeout=30,
                            creationflags=getattr(subprocess, "CREATE_NO_WINDOW", 0))
    if result.returncode:
        raise ValueError("the original CPU model engine rejected the input")
    return json.loads(result.stdout)


def install(session):
    session.run_sql("CREATE SCHEMA IF NOT EXISTS sdp_models")
    session.run_sql(f"""CREATE TABLE IF NOT EXISTS {TABLE} (
        owner VARCHAR(288) CHARACTER SET utf8mb4 COLLATE utf8mb4_bin NOT NULL,
        name VARCHAR(64) CHARACTER SET ascii COLLATE ascii_bin NOT NULL,
        version INT UNSIGNED NOT NULL,
        algorithm ENUM('linear','logistic','tree','forest','kmeans','dbscan','pca','mlp') NOT NULL,
        artifact LONGTEXT CHARACTER SET utf8mb4 NOT NULL,
        sha256 CHAR(64) CHARACTER SET ascii NOT NULL,
        PRIMARY KEY(owner,name,version), CHECK (JSON_VALID(artifact))
    ) ENGINE=InnoDB""")
    session.run_sql(f"""CREATE TABLE IF NOT EXISTS {ACTIVE} (
        owner VARCHAR(288) CHARACTER SET utf8mb4 COLLATE utf8mb4_bin NOT NULL,
        name VARCHAR(64) CHARACTER SET ascii COLLATE ascii_bin NOT NULL,
        version INT UNSIGNED NOT NULL,
        PRIMARY KEY(owner,name), FOREIGN KEY(owner,name,version) REFERENCES {TABLE}(owner,name,version)
    ) ENGINE=InnoDB""")


def numeric(value):
    if type(value) not in (int, float, Decimal) or not math.isfinite(float(value)):
        raise ValueError("training and prediction require finite numeric values")
    return float(value)


def source_columns(args):
    columns = [args.order_key, *args.feature]
    if args.label is not None:
        columns.append(args.label)
    if args.split_column is not None:
        columns.append(args.split_column)
    return columns


def training_rows(session, args, source):
    quoted = ",".join(f"`{name}`" for name in source["columns"])
    result = session.run_sql(f"SELECT {quoted} FROM `{args.schema}`.`{args.table}` "
                             f"ORDER BY `{args.order_key}` LIMIT 1025")
    rows = result.fetch_all()
    if not 1 <= len(rows) <= 1024:
        raise ValueError("training requires between 1 and 1024 rows")
    width = len(args.feature)
    keys = {row[0] for row in rows}
    if None in keys or len(keys) != len(rows):
        raise ValueError("training row order requires unique non-NULL keys")
    features = [[numeric(row[index]) for index in range(1, width + 1)] for row in rows]
    labels = [numeric(row[width + 1]) for row in rows] if args.label is not None else None
    if args.split_column is None:
        return {"features": features, "labels": labels}

    partition = [row[width + 2] for row in rows]
    if set(partition) != {"train", "validation"}:
        raise ValueError("MLP requires nonempty train and validation partitions")
    return {
        "features": [row for row, split in zip(features, partition) if split == "train"],
        "labels": [label for label, split in zip(labels, partition) if split == "train"],
        "validation": [row for row, split in zip(features, partition) if split == "validation"],
        "validation_labels": [label for label, split in zip(labels, partition) if split == "validation"],
    }


@contextmanager
def model_lock(session, name):
    account = session.run_sql("SELECT CURRENT_USER()").fetch_one()[0]
    lock = sha(canonical(["sdp-cpu-model", account, name]))
    if session.run_sql("SELECT GET_LOCK(?, 10)", [lock]).fetch_one()[0] != 1:
        raise ValueError("could not lock the model registry")
    try:
        yield
    finally:
        session.run_sql("SELECT RELEASE_LOCK(?)", [lock])


def train(session, args):
    if not 1 <= len(args.feature) <= 64 or len({name.casefold() for name in args.feature}) != len(args.feature):
        raise ValueError("select between 1 and 64 distinct features")
    if not 0 <= args.seed < (1 << 32):
        raise ValueError("seed must be an unsigned 32-bit integer")
    if (args.algorithm in UNSUPERVISED) != (args.label is None):
        raise ValueError("provide a label column only for supervised training")
    if (args.algorithm == "mlp") != (args.split_column is not None):
        raise ValueError("provide a split column for MLP only")
    options = json.loads(args.options)
    if not isinstance(options, dict):
        raise ValueError("training options must be a JSON object")
    source = {"schema": args.schema, "table": args.table, "columns": source_columns(args)}
    rows = training_rows(session, args, source)
    model = run_model(args.worker, {"action": "train", "algorithm": args.algorithm,
                                    **rows, "seed": args.seed, "options": options})
    artifact = canonical({"features": args.feature, "model": model, "seed": args.seed, "options": options,
                          "source": source})
    digest = sha(artifact)
    with model_lock(session, args.name):
        previous = session.run_sql(f"""SELECT version, algorithm FROM {TABLE}
            WHERE owner = CURRENT_USER() AND name = ? ORDER BY version DESC LIMIT 1""", [args.name]).fetch_one()
        if previous is not None and previous[1] != args.algorithm:
            raise ValueError("model name is already bound to a different algorithm")
        version = 1 if previous is None else previous[0] + 1
        session.run_sql(f"""INSERT INTO {TABLE}(owner,name,version,algorithm,artifact,sha256)
            VALUES (CURRENT_USER(),?,?,?,?,?)""", [args.name, version, args.algorithm, artifact, digest])
    return {"name": args.name, "version": version, "sha256": digest}


def load_version(session, name, version, *, required=True):
    if version == "active":
        row = session.run_sql(f"""SELECT v.version, v.algorithm, v.artifact, v.sha256
            FROM {TABLE} AS v JOIN {ACTIVE} AS a USING (owner,name,version)
            WHERE v.owner=CURRENT_USER() AND v.name=?""", [name]).fetch_one()
    else:
        row = session.run_sql(f"""SELECT version, algorithm, artifact, sha256 FROM {TABLE}
            WHERE owner=CURRENT_USER() AND name=? AND version=?""", [name, version]).fetch_one()
    if row is None and not required:
        return None
    if row is None or sha(row[2]) != row[3]:
        raise ValueError("model version missing or artifact checksum differs")
    artifact = json.loads(row[2])
    if not isinstance(artifact, dict):
        raise ValueError("invalid model artifact")
    return row[0], row[1], artifact


def check_source_access(session, artifact):
    source = artifact.get("source")
    if not isinstance(source, dict) or not isinstance(source.get("columns"), list) or not source["columns"]:
        raise ValueError("model source metadata is missing")
    names = (source.get("schema"), source.get("table"), *source["columns"])
    if any(not isinstance(name, str) or SAFE_NAME.fullmatch(name) is None for name in names):
        raise ValueError("invalid model source metadata")
    schema, table, *columns = names
    selected = ",".join(f"`{name}`" for name in columns)
    session.run_sql(f"SELECT {selected} FROM `{schema}`.`{table}` LIMIT 0")


def set_active(session, name, version, *, rollback):
    with model_lock(session, name):
        selected, _, next_artifact = load_version(session, name, version)
        active = load_version(session, name, "active", required=False)
        if rollback and (active is None or selected >= active[0]):
            raise ValueError("rollback requires a version older than the active version")
        if active is not None and next_artifact["features"] != active[2]["features"]:
            raise ValueError("selected feature order differs from the active model")
        check_source_access(session, next_artifact)
        session.run_sql(f"""INSERT INTO {ACTIVE}(owner,name,version) VALUES(CURRENT_USER(),?,?)
            ON DUPLICATE KEY UPDATE version=?""", [name, selected, selected])
    return {"name": name, "active_version": selected}


def deactivate(session, name):
    with model_lock(session, name):
        session.run_sql(f"DELETE FROM {ACTIVE} WHERE owner=CURRENT_USER() AND name=?", [name])
    return {"name": name, "active_version": None}


def status(session, name):
    active = session.run_sql(f"SELECT version FROM {ACTIVE} WHERE owner=CURRENT_USER() AND name=?",
                             [name]).fetch_one()
    result = session.run_sql(f"""SELECT version, algorithm, sha256 FROM {TABLE}
        WHERE owner=CURRENT_USER() AND name=? ORDER BY version""", [name])
    versions = [{"version": row[0], "algorithm": row[1], "sha256": row[2]} for row in result.fetch_all()]
    if not versions:
        raise ValueError("model not found")
    return {"name": name, "active_version": None if active is None else active[0], "versions": versions}


def predict(session, args):
    version, algorithm, artifact = load_version(session, args.name, args.version)
    if algorithm == "dbscan":
        raise ValueError("DBSCAN does not predict new records")
    check_source_access(session, artifact)
    supplied = json.loads(args.features)
    if not isinstance(supplied, dict) or set(supplied) != set(artifact["features"]):
        raise ValueError("prediction requires the trained feature names")
    result = run_model(args.worker, {"action": "predict", "algorithm": algorithm, "model": artifact["model"],
                                    "features": [numeric(supplied[name]) for name in artifact["features"]]})
    return {"name": args.name, "version": version, "prediction": result}


def positive_version(value):
    if not value.isascii() or not value.isdecimal() or not 1 <= int(value) <= 0xffffffff:
        raise argparse.ArgumentTypeError("expected an unsigned, nonzero model version")
    return int(value)


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__)
    subparsers = parser.add_subparsers(dest="command", required=True)
    subparsers.add_parser("install")
    fit = subparsers.add_parser("train")
    estimate = subparsers.add_parser("predict")
    for action in ("activate", "rollback", "deactivate", "status"):
        command = subparsers.add_parser(action)
        command.add_argument("name", type=identifier)
        if action in ("activate", "rollback"):
            command.add_argument("version", type=positive_version)
    for command in (fit, estimate):
        command.add_argument("name", type=identifier)
        command.add_argument("--worker", type=Path, required=True)
    fit.add_argument("--algorithm", choices=ALGORITHMS, required=True)
    fit.add_argument("--seed", type=int, default=42)
    fit.add_argument("--options", default="{}", help="algorithm options as a JSON object")
    fit.add_argument("--label", type=identifier)
    fit.add_argument("--split-column", type=identifier, help="MLP partition column with train or validation")
    for option in ("schema", "table", "order-key"):
        fit.add_argument(f"--{option}", type=identifier, required=True)
    fit.add_argument("--feature", type=identifier, action="append", required=True)
    estimate.add_argument("version", type=lambda value: value if value == "active" else positive_version(value))
    estimate.add_argument("features", help="JSON object keyed by feature name")
    args = parser.parse_args(argv)
    from mysqlsh import globals as shell_globals

    try:
        session = shell_globals.shell.get_session()
        if session is None or not session.is_open():
            raise ValueError("connect MySQL Shell before running the model command")
        if session.run_sql("SELECT @@autocommit").fetch_one()[0] != 1:
            raise ValueError("use an autocommit connection")
        result = {"action": args.command}
        if args.command == "install":
            install(session)
        elif args.command == "train":
            result.update(train(session, args))
        elif args.command == "predict":
            result.update(predict(session, args))
        elif args.command in ("activate", "rollback"):
            result.update(set_active(session, args.name, args.version, rollback=args.command == "rollback"))
        elif args.command == "deactivate":
            result.update(deactivate(session, args.name))
        else:
            result.update(status(session, args.name))
        print("SDP_MODEL " + canonical(result))
    except (OSError, ValueError, subprocess.TimeoutExpired) as error:
        print(f"model: {error}", file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
