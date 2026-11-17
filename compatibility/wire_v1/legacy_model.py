"""Expose the installed CPU model registry through supported legacy RPCs."""

from contextlib import suppress
import math
from pathlib import Path
import subprocess
import sys

from mysql.connector.errors import Error

from ledger import request_token
from legacy_mutations import mutate as durable_mutate
from sql import Fault, integer_field, name_field, require

# service.py can be invoked directly from its own directory.
sys.path.insert(1, str(Path(__file__).resolve().parents[1] / "ml"))
import cpu_model


OPERATIONS = {"model_activate", "model_rollback", "model_deactivate", "model_predict"}
PREDICTABLE = {"linear", "logistic", "tree", "forest", "mlp", "kmeans"}


class SqlResult:
    def __init__(self, rows):
        self.rows = rows

    def fetch_one(self):
        return self.rows[0] if self.rows else None

    def fetch_all(self):
        return self.rows


class SqlSession:
    def __init__(self, db):
        self.db = db

    def run_sql(self, sql, values=()):
        with self.db.cursor(buffered=True) as cursor:
            cursor.execute(sql.replace("?", "%s"), tuple(values))
            return SqlResult(cursor.fetchall() if cursor.with_rows else [])


def owned_model(sql, name):
    owned = sql.run_sql(f"SELECT 1 FROM {cpu_model.TABLE} WHERE owner = CURRENT_USER() AND name = ? LIMIT 1",
                        [name]).fetch_one()
    require(owned is not None, 1006)


def model_version(sql, name, selector):
    try:
        loaded = cpu_model.load_version(sql, name, selector, required=False)
    except (TypeError, ValueError):
        raise Fault(1013) from None
    require(loaded is not None, 1013)
    version, algorithm, artifact = loaded
    features = artifact.get("features")
    require(isinstance(features, list) and 1 <= len(features) <= 64
            and all(isinstance(feature, str) and cpu_model.SAFE_NAME.fullmatch(feature) for feature in features)
            and len({feature.casefold() for feature in features}) == len(features)
            and isinstance(artifact.get("model"), dict), 1013)
    return version, algorithm, artifact


def estimate(worker, algorithm, model, features):
    require(algorithm in PREDICTABLE, 1004)
    try:
        value = cpu_model.run_model(worker, {"action": "predict", "algorithm": algorithm,
                                             "model": model, "features": features})
    except subprocess.TimeoutExpired:
        raise Fault(1008) from None
    except OSError:
        raise Fault(1007) from None
    except (ValueError, OverflowError):
        raise Fault(1013) from None
    if algorithm == "kmeans":
        centers = model.get("centers")
        require(type(value) is int and isinstance(centers, list) and 0 <= value < len(centers), 1013)
    else:
        require(type(value) in (int, float) and math.isfinite(value), 1013)
        require(algorithm == "linear" or 0 <= value <= 1, 1013)
    return value


def require_source(sql, artifact):
    try:
        cpu_model.check_source_access(sql, artifact)
    except ValueError:
        raise Fault(1013) from None


def rollback(session):
    try:
        session.db.rollback()
    except (Error, OSError):
        session.close()


def predict(session, sql, name, supplied, worker):
    require(isinstance(supplied, dict))
    session.db.start_transaction(readonly=True, isolation_level="REPEATABLE READ")
    try:
        owned_model(sql, name)
        version, algorithm, artifact = model_version(sql, name, "active")
        require(algorithm in PREDICTABLE, 1004)
        require_source(sql, artifact)
    finally:
        rollback(session)
    features = artifact["features"]
    require(set(supplied) == set(features))
    try:
        values = [cpu_model.numeric(supplied[feature]) for feature in features]
    except (ValueError, OverflowError):
        raise Fault(1012) from None
    value = estimate(worker, algorithm, artifact["model"], values)
    response = {"version": version, "algorithm": algorithm, "prediction": value}
    if algorithm == "kmeans":
        response["cluster"] = value
    elif algorithm != "linear":
        label = int(value >= 0.5)
        response.update(prediction=label, label=label, probability=value)
    return response


def activate(sql, operation, name, requested, worker):
    version, algorithm, artifact = model_version(sql, name, requested)
    require(algorithm in PREDICTABLE, 1004)
    require_source(sql, artifact)
    estimate(worker, algorithm, artifact["model"], [0.0] * len(artifact["features"]))
    row = sql.run_sql(f"SELECT version FROM {cpu_model.ACTIVE} WHERE owner = CURRENT_USER() AND name = ?",
                      [name]).fetch_one()
    if operation == "model_rollback":
        require(row is not None and version < row[0], 1013)
    if row is not None and row[0] != version:
        _, _, active = model_version(sql, name, row[0])
        require_source(sql, active)
        require(artifact["features"] == active["features"], 1013)
    sql.run_sql(f"""INSERT INTO {cpu_model.ACTIVE}(owner, name, version) VALUES (CURRENT_USER(), ?, ?)
        ON DUPLICATE KEY UPDATE version = ?""", [name, version, version])
    return {"active_version": version, "version": version}


def mutate(session, sql, operation, name, body, worker):
    require(session.receipts is not None, 1004)
    request_token(body)
    version = None if operation == "model_deactivate" else integer_field(body, "version", None, 0xffffffff)
    lock = cpu_model.sha(cpu_model.canonical(["sdp-cpu-model", session.identity[1], name]))
    require(sql.run_sql("SELECT GET_LOCK(?, 10)", [lock]).fetch_one()[0] == 1, 1008)

    def apply(_token):
        if operation == "model_deactivate":
            sql.run_sql(f"DELETE FROM {cpu_model.ACTIVE} WHERE owner = CURRENT_USER() AND name = ?", [name])
            return {"active_version": None}
        return activate(sql, operation, name, version, worker)

    closing = False
    try:
        return durable_mutate(session, body, apply, authorize=lambda: owned_model(sql, name))
    except Fault as error:
        closing = error.close
        raise
    finally:
        if not closing:
            try:
                sql.run_sql("SELECT RELEASE_LOCK(?)", [lock])
            except (Error, OSError):
                session.close()


def dispatch(session, operation, body, worker):
    require(session.result is None and not session.db.in_transaction, 1013)
    name = name_field(body, "name")
    sql = SqlSession(session.db)
    if operation == "model_predict":
        require(set(body) == {"name", "features"})
        return predict(session, sql, name, body["features"], worker)
    fields = {"name", "request_token"}
    if operation != "model_deactivate":
        fields.add("version")
    require(set(body) == fields)
    return mutate(session, sql, operation, name, body, worker)
