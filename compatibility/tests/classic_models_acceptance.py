"""Verify Classic model SQL on a disposable, TLS-protected StarDataParadise Server."""

import argparse
from concurrent.futures import ThreadPoolExecutor
import json
import math
from pathlib import Path

from .fixture import BASEDIR, BIN, MIGRATION, REPOSITORY, SHELL, Fixture, query, require, run
from .workbench_native import backend_certificate


MODEL_COMMANDS = {"create_model", "train_model", "predict_model"}
INSTALL = REPOSITORY / "compatibility/ml/classic_model.sql"


def statement(verb, payload, native=True):
    model_json = json.dumps(payload, ensure_ascii=True, separators=(",", ":"))
    literal = "'" + model_json.replace("\\", "\\\\").replace("'", "''") + "'"
    if native:
        return f"{verb} MODEL USING {literal};"
    return f"CALL sdp_models.{verb.lower()}_model({literal});"


def execute(instance, script, expected_error=None):
    environment, mysql, _ = instance
    result = run(mysql, environment, 45, script=script)
    if expected_error is not None:
        require(result.returncode != 0 and expected_error in result.stderr, result.stdout + result.stderr)
    else:
        require(result.returncode == 0, result.stdout + result.stderr)
    return result.stdout.strip()


def account(instance, user):
    environment, mysql, cli = instance
    return environment, [f"--user={user}" if flag == "--user=root" else flag for flag in mysql], cli


def exercise(instance, dll, *, native=True):
    if native:
        commands = json.loads(query(instance, "SELECT STARDATAPARADISE_CAPABILITIES()"))["commands"]
        require(set(commands) == MODEL_COMMANDS, f"unexpected capabilities: {commands}")
        legacy = query(instance, "SET SESSION sdp_compatibility_mode='legacy'; SELECT STARDATAPARADISE_CAPABILITIES()")
        require(set(json.loads(legacy)["commands"]) == MODEL_COMMANDS | {"analyze_shorthand"}, "legacy capabilities differ")
        execute(instance, "CREATE MODEL USING 1;", "ERROR 1064")
    execute(instance, f"CREATE FUNCTION sdp_model_compute RETURNS STRING SONAME '{dll.name}';\n" +
            INSTALL.read_text(encoding="utf-8"))
    execute(instance, """
        CREATE SCHEMA training;
        CREATE SCHEMA private_models;
        CALL sdp_models.install_model_registry('private_models');
        CREATE TABLE training.samples(id INT PRIMARY KEY, x DECIMAL(10,2), y DECIMAL(10,2));
        INSERT INTO training.samples VALUES (4,4,9),(2,2,5),(1,1,3),(3,3,7);
        CREATE USER model_owner@localhost IDENTIFIED BY '' REQUIRE SSL;
        GRANT EXECUTE ON sdp_models.* TO model_owner@localhost;
        GRANT SELECT, INSERT ON private_models.sdp_model_definitions TO model_owner@localhost;
        GRANT SELECT, INSERT ON private_models.sdp_model_versions TO model_owner@localhost;
        GRANT SELECT(id,x,y) ON training.samples TO model_owner@localhost;
    """)
    owner = account(instance, "model_owner")
    model = {"registry": "private_models", "name": "sales"}
    train = {**model, "request_token": "1" * 32, "schema": "training", "table": "samples",
             "order_key": "id", "features": ["x"], "label": "y"}

    def command(verb, arguments, error=None):
        return execute(owner, statement(verb, arguments, native), error)

    command("CREATE", {"name": "other", "algorithm": "linear"}, "denied")
    command("TRAIN", train, "Use CREATE MODEL")
    require(command("CREATE", {**model, "algorithm": "linear"}) == "sales\tlinear", "model definition differs")
    command("CREATE", {**model, "algorithm": "logistic"}, "different algorithm")
    command("TRAIN", {**train, "dataset_id": "legacy"}, "source table")
    with ThreadPoolExecutor(max_workers=2) as pool:
        trained, parallel = pool.map(lambda _: command("TRAIN", train).split("\t"), range(2))
    require(trained == parallel, "parallel training retries published different versions")
    first_version = int(trained[1])
    require(trained[0] == "sales" and len(trained[2]) == 64, f"unexpected version: {trained}")
    require(command("TRAIN", train).split("\t") == trained, "retry returned a different version")
    command("TRAIN", {**train, "seed": 99}, "different training request")
    command("PREDICT", {**model, "features": {"y": 4}}, "trained feature names")

    def prediction(expected, *, version=None):
        request = {**model, "features": {"x": 4}}
        if version is not None:
            request["version"] = version
        _, result_version, raw = command("PREDICT", request).split("\t")
        require(math.isclose(json.loads(raw), expected, abs_tol=1e-9), f"prediction differs: {raw}")
        return int(result_version)

    require(prediction(9) == first_version, "unexpected default version")
    execute(instance, "UPDATE training.samples SET y=y+5;")
    second = int(command("TRAIN", {**train, "request_token": "2" * 32}).split("\t")[1])
    require(second > first_version and prediction(14) == second, "latest version was not used")
    require(prediction(9, version=first_version) == first_version, "older artifact changed")
    execute(owner, "START TRANSACTION;\n" +
            statement("CREATE", {**model, "name": "rolled_back", "algorithm": "linear"}, native) + "\nROLLBACK;")
    require(query(instance, "SELECT COUNT(*) FROM private_models.sdp_model_definitions WHERE name='rolled_back'") == "0",
            "CREATE MODEL committed the caller's transaction")
    execute(instance, "REVOKE SELECT(y) ON training.samples FROM model_owner@localhost;")
    command("PREDICT", {**model, "features": {"x": 4}}, "denied")
    command("TRAIN", {**train, "request_token": "3" * 32}, "denied")
    if native:
        require(query(instance, "SELECT 1 AS model, 2 AS train, 3 AS predict") == "1\t2\t3",
                "model keywords broke ordinary identifiers")
    print(f"PASS {'Classic SQL' if native else 'model routines'} create/train/predict, retry, versioning, rollback and source ACL")


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--mysqld", type=Path, default=BIN / "mysqld.exe")
    parser.add_argument("--mysql", type=Path, default=BIN / "mysql.exe")
    parser.add_argument("--basedir", type=Path, default=BASEDIR)
    parser.add_argument("--dll", type=Path, default=MIGRATION / "ml-b/Release/sdp_mysql_model_udf.dll")
    parser.add_argument("--openssl", type=Path, default=MIGRATION / "toolchain/openssl-firedaemon/x64/bin/openssl.exe")
    args = parser.parse_args()
    fixture = Fixture(args.mysqld.resolve(strict=True), args.mysql.resolve(strict=True), args.basedir.resolve(strict=True),
                      SHELL, None)
    dll = args.dll.resolve(strict=True)
    with fixture.scratch("classic-model") as root:
        cert = backend_certificate(args.openssl.resolve(strict=True), root, "cert")
        with fixture.disposable(root / "disabled", tls_dir=cert) as disabled:
            capabilities = json.loads(query(disabled, "SELECT STARDATAPARADISE_CAPABILITIES()"))
            require(not (set(capabilities["commands"]) & MODEL_COMMANDS), "model SQL should default to off")
            execute(disabled, statement("CREATE", {"name": "sales", "algorithm": "linear"}), "ERROR 1290")
        with fixture.disposable(root / "enabled", plugin_dir=dll.parent, tls_dir=cert,
                                server_options=("--sdp-model-sql=ON",)) as enabled:
            exercise(enabled, dll)


if __name__ == "__main__":
    main()
