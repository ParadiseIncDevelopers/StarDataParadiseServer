"""Verify materialized datasets, training-only preprocessing and typed PL/SQL jobs over TLS."""

import argparse
import math
from pathlib import Path
import secrets
import ssl
import statistics
import tempfile

from history_restore_rpc import head, select
from jobs_rpc import submit, terminal, wait_for
from service_smoke import certificates, connection, exchange, execute, listening, require, save_config
from sql_rpc import BIGINT, Client, client, i64, token, write


PERMISSION_DENIED = 1006
INVALID_STATE = 1013
INVALID_ARGUMENT = 1012
SOURCE = """SELECT sample_id,input_metric,category,target_value,time_axis,group_key
FROM dataset_measurements WHERE sample_id >= :min_id ORDER BY sample_id"""
DATASET_OPTIONS = {
    "features": [
        {"column": "input_metric", "transform": "standard", "impute": "mean"},
        {"column": "category", "transform": "onehot", "impute": "reject"},
    ],
    "target": "target_value",
    "split": {"mode": "time", "time": "time_axis", "group": "group_key", "train": 60, "validation": 20},
}


class DatasetReader(Client):
    def __init__(self, sock, password):
        self.sock = sock
        self.id = 2
        require(exchange(sock, 1, "hello")["ok"], "dataset reader TLS hello failed")
        require(exchange(sock, 2, "auth", {"username": "dataset_reader", "password": password})["ok"],
                "dataset reader authentication failed")


def create_dataset(session, request_token=None, **overrides):
    fields = {"request_token": request_token or token(), "sql": SOURCE, "parameter_types": {"min_id": BIGINT},
              "parameters": {"min_id": i64(1)}, "options": DATASET_OPTIONS}
    fields.update(overrides)
    return session.call("dataset_create", **fields)


def install_source(session):
    write(session, """CREATE TABLE dataset_measurements (sample_id BIGINT PRIMARY KEY, input_metric BIGINT,
category TEXT, target_value BIGINT, time_axis BIGINT, group_key BIGINT)""", 0)
    write(session, """INSERT INTO dataset_measurements VALUES
    (0,-900000,'excluded',0,0,0),(1,2,'known_a',10,1,1),(2,4,'known_b',20,2,1),
    (3,NULL,'known_a',30,3,2),(4,8,'known_b',40,4,2),(5,10,'known_a',50,5,3),(6,12,'known_b',60,6,3),
    (7,70,'known_a',70,7,4),(8,80,'known_b',80,8,4),(9,800000,'held_only',90,9,5),
    (10,900000,'held_only',100,10,5)""", 11)
    write(session, """CREATE FUNCTION twice_retained(input_value IN BIGINT) RETURN BIGINT AS
BEGIN
    RETURN input_value * 2;
END;""", 0)
    write(session, """CREATE PROCEDURE retained_stats(input_value IN BIGINT, scaled_value OUT BIGINT, large_value OUT BOOLEAN) AS
BEGIN
    scaled_value := input_value * 2;
    large_value := input_value > 10;
END;""", 0)


def verify_training_fit(snapshot):
    source = snapshot["source"]
    require(source["columns"] == ["sample_id", "input_metric", "category", "target_value", "time_axis", "group_key"],
            "dataset did not preserve projected source names")
    rows = source["rows"]
    require(snapshot["row_count"] == len(rows) == 10 and snapshot["byte_count"] > 0
            and [row[0] for row in rows] == list(range(1, 11)),
            "parameterized extraction changed row count or included the excluded source row")
    prepared = snapshot["prepared"]
    split = prepared["split"]
    train, validation, test = (split[key] for key in ("train", "validation", "test"))
    require([len(train), len(validation), len(test)] == [6, 2, 2]
            and sorted(train + validation + test) == list(range(len(rows))),
            "split omitted a row, overlapped, or ignored the requested percentages")
    require(max(rows[i][4] for i in train) < min(rows[i][4] for i in validation)
            and max(rows[i][4] for i in validation) < min(rows[i][4] for i in test),
            "time split leaked a later observation into earlier training")
    grouped_train, grouped_validation, grouped_test = ({rows[i][5] for i in part}
                                                       for part in (train, validation, test))
    require(not grouped_train & grouped_validation and not grouped_train & grouped_test
            and not grouped_validation & grouped_test, "group membership leaked across data splits")

    numeric, categorical = prepared["transforms"]["features"]
    samples = [rows[i][1] for i in train if rows[i][1] is not None]
    fit_mean = statistics.mean(samples)
    train_filled = [fit_mean if rows[i][1] is None else rows[i][1] for i in train]
    require(math.isclose(numeric["fill"], fit_mean) and math.isclose(numeric["offset"], fit_mean)
            and math.isclose(numeric["scale"], statistics.pstdev(train_filled))
            and max(rows[i][1] for i in test) >= 800000,
            "held-out outliers affected numeric imputation or centering")
    require(categorical["categories"] == ["known_a", "known_b"]
            and "held_only" not in categorical["categories"],
            "one-hot encoding fit on validation or test data")
    require(prepared["transforms"]["width"] == 4 and len(prepared["features"]) == len(rows)
            and all(len(row) == 4 and all(math.isfinite(cell) for cell in row) for row in prepared["features"]),
            "dataset transform matrix dimensions or numeric values changed")
    require(all(prepared["features"][i][1:] == [0, 0, 1] for i in test if rows[i][2] == "held_only"),
            "a previously unseen category did not use the reserved inference bucket")
    require(prepared["target"] == [row[3] for row in rows], "regression targets did not match the retained rows")


def verify_split_variants(session):
    options = {**DATASET_OPTIONS, "split": {"mode": "random", "seed": 42, "group": "group_key"}}
    grouped = []
    for _ in range(2):
        dataset = create_dataset(session, options=options)["dataset_id"]
        snapshot = session.call("dataset_get", dataset_id=dataset)
        grouped.append(snapshot["prepared"])
        groups = [{snapshot["source"]["rows"][i][5] for i in snapshot["prepared"]["split"][part]}
                  for part in ("train", "validation", "test")]
        require(not groups[0] & groups[1] and not groups[0] & groups[2] and not groups[1] & groups[2],
                "seeded random split separated a group across partitions")
        session.call("dataset_delete", dataset_id=dataset)
    require(grouped[0] == grouped[1], "same source, seed and build changed the split or fitted artifact")

    options = {"features": [DATASET_OPTIONS["features"][0]], "target": "category", "target_kind": "categorical",
               "split": {"mode": "stratified", "seed": 17}}
    dataset = create_dataset(session, options=options)["dataset_id"]
    snapshot = session.call("dataset_get", dataset_id=dataset)
    prepared = snapshot["prepared"]
    require(set(prepared["target_classes"]) == {"known_a", "known_b", "held_only"}
            and {snapshot["source"]["rows"][i][2] for i in prepared["split"]["train"]} ==
            set(prepared["target_classes"])
            and sorted(index for part in prepared["split"].values() for index in part) == list(range(10)),
            "stratification lost a class in training or overlapped partitions")
    session.call("dataset_delete", dataset_id=dataset)

    before = head(session)
    request_token = token()
    session.error("dataset_create", INVALID_ARGUMENT, request_token=request_token, sql=SOURCE,
                  parameter_types={"min_id": BIGINT}, parameters={"min_id": i64(1000)}, options=DATASET_OPTIONS)
    require(head(session) == before
            and session.call("tx_outcome", request_token=request_token) == {"status": "unknown"},
            "rejected empty dataset produced an artifact or commit")


def inspect_concurrent_preparation(session, snapshot):
    dataset_id = snapshot["dataset_id"]
    blocking = submit(session, 1_000_000_000, timeout_ms=60000)
    live = wait_for(session, blocking, lambda state: state["worker_pid"] is not None or terminal(state))
    require(live["state"] == "running", "dataset fixture did not acquire a running isolated worker")
    request = {"request_token": token(), "dataset_id": dataset_id, "routine": "twice_retained",
               "arguments": {"input_value": "input_metric"}, "indices": [0, 1, 8, 9]}
    queued = session.call("dataset_plsql_submit", **request)
    require(queued["state"] == "queued", "PL/SQL dataset worker did not join the existing job queue")
    session.error("dataset_delete", INVALID_STATE, dataset_id=dataset_id)
    write(session, "UPDATE dataset_measurements SET input_metric=6000 WHERE sample_id >= 1", 10)
    require(select(session, "SELECT input_metric FROM dataset_measurements WHERE sample_id=1") == [[i64(6000)]]
            and session.call("dataset_get", dataset_id=dataset_id) == snapshot,
            "source write blocked the OLTP lease or altered the materialized dataset")
    session.call("job_cancel", job_id=blocking)
    require(wait_for(session, blocking, terminal)["state"] == "cancelled",
            "dataset queue fixture could not stop its competing worker")
    finished = wait_for(session, queued["job_id"], terminal)
    require(finished["state"] == "succeeded" and finished["result"]["kind"] == "result_set"
            and finished["result"]["rows"] == [[i64(4)], [i64(8)], [i64(1600000)], [i64(1800000)]],
            f"isolated PL/SQL did not use the pre-mutation typed rows ({finished.get('reason')})")
    for routine, expected_kind, expected in (
        ("twice_retained", "scalar", [[i64(24)]]),
        ("retained_stats", "record", [[i64(24), {"t": "bool", "v": True}]]),
    ):
        case = session.call("dataset_plsql_submit", request_token=token(), dataset_id=dataset_id, routine=routine,
                            arguments={"input_value": "input_metric"}, indices=[5])
        result = wait_for(session, case["job_id"], terminal)
        require(result["state"] == "succeeded" and result["result"]["kind"] == expected_kind
                and result["result"]["rows"] == expected, f"dataset routine lost its typed {expected_kind} output")
    session.error("dataset_delete", INVALID_STATE, dataset_id=dataset_id)
    return queued["job_id"]


def verify_owner_and_revocation(port, context, owner, root_dataset):
    write(owner, "CREATE USER dataset_reader", 0)
    password = secrets.token_urlsafe(32)
    owner.call("password_set", username="dataset_reader", new_password=password, request_token=token())
    with connection(port, context) as socket:
        reader = DatasetReader(socket, password)
        reader.error("dataset_get", PERMISSION_DENIED, dataset_id=root_dataset)
        write(owner, "GRANT SELECT ON TABLE dataset_measurements TO USER dataset_reader", 0)
        write(owner, "GRANT EXECUTE ON ROUTINE twice_retained TO USER dataset_reader", 0)
        reader.error("dataset_get", PERMISSION_DENIED, dataset_id=root_dataset)
        owned = create_dataset(reader)["dataset_id"]
        require(reader.call("dataset_get", dataset_id=owned)["row_count"] == 10, "owner could not sample its dataset")
        completed = reader.call("dataset_plsql_submit", request_token=token(), dataset_id=owned,
                                routine="twice_retained", arguments={"input_value": "input_metric"}, indices=[0])
        result = wait_for(reader, completed["job_id"], terminal)
        require(result["state"] == "succeeded" and result["result"]["rows"] == [[i64(12000)]],
                "granted owner could not read its own derived values")
        write(owner, "REVOKE SELECT ON TABLE dataset_measurements FROM USER dataset_reader", 0)
        reader.error("dataset_get", PERMISSION_DENIED, dataset_id=owned)
        for operation in ("job_status", "job_watch", "job_cancel"):
            reader.error(operation, PERMISSION_DENIED, job_id=completed["job_id"])
        reader.error("dataset_plsql_submit", PERMISSION_DENIED, request_token=token(), dataset_id=owned,
                     routine="twice_retained", arguments={"input_value": "input_metric"}, indices=[0])
        write(owner, "GRANT SELECT ON TABLE dataset_measurements TO USER dataset_reader", 0)
        require(reader.call("job_status", job_id=completed["job_id"])["result"] == result["result"],
                "restored SELECT privilege changed the retained result")
        reader.error("dataset_delete", INVALID_STATE, dataset_id=owned)
        unreferenced = create_dataset(reader)["dataset_id"]
        reader.call("dataset_delete", dataset_id=unreferenced)
        reader.error("dataset_get", PERMISSION_DENIED, dataset_id=unreferenced)


def verify_removed_dependency(owner):
    write(owner, "CREATE TABLE retired_dataset_source (sample_key BIGINT)", 0)
    write(owner, "INSERT INTO retired_dataset_source VALUES (1),(2),(3)", 3)
    created = owner.call("dataset_create", request_token=token(),
                         sql="SELECT sample_key FROM retired_dataset_source",
                         options={"features": [{"column": "sample_key", "transform": "numeric", "impute": "reject"}],
                                  "split": {"mode": "random"}})
    write(owner, "DROP TABLE retired_dataset_source", 0)
    owner.error("dataset_get", PERMISSION_DENIED, dataset_id=created["dataset_id"])


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    for argument in ("server", "openssl", "scratch"):
        parser.add_argument("--" + argument, required=True, type=Path)
    args = parser.parse_args()
    server = str(args.server.resolve(strict=True))
    openssl = str(args.openssl.resolve(strict=True))
    args.scratch.mkdir(parents=True, exist_ok=True)
    with tempfile.TemporaryDirectory(prefix="s24-", dir=args.scratch.resolve()) as directory:
        fixture = Path(directory)
        for name in ("data", "log", "temp"):
            (fixture / name).mkdir()
        certificates(openssl, fixture)
        config = save_config(fixture, "config.json", timeout_ms=20000, max_connections=8)
        require(execute(server, config, "--initialize-storage").returncode == 0, "dataset storage initialization failed")
        password = secrets.token_urlsafe(32)
        require(execute(server, config, "--bootstrap-admin", "local-admin",
                        stdin=((password + "\n") * 2).encode("ascii")).returncode == 0,
                "dataset administrator bootstrap failed")
        context = ssl.create_default_context(cafile=str(fixture / "ca.pem"))
        with listening(server, config) as (_, port), client(port, context, password) as owner:
            require(owner.call("capabilities")["features"].get("dataset_v1") is True, "dataset capability unavailable")
            install_source(owner)
            request_token = token()
            created = create_dataset(owner, request_token)
            dataset_id = created["dataset_id"]
            require(owner.call("tx_outcome", request_token=request_token) == {"status": "committed", "lsn": created["lsn"]},
                    "dataset receipt and materialized artifact did not commit together")
            snapshot = owner.call("dataset_get", dataset_id=dataset_id)
            require(snapshot["revision"] == created["revision"] and snapshot["row_count"] == created["row_count"]
                    and snapshot["owner"] == created["owner"] == "local-admin"
                    and snapshot["query"] == SOURCE and snapshot["options"] == DATASET_OPTIONS
                    and snapshot["column_types"] == ["BIGINT", "BIGINT", "TEXT", "BIGINT", "BIGINT", "BIGINT"]
                    and int(created["lsn"]) > int(snapshot["revision"]),
                    "published dataset metadata omitted owner, lineage, types, split settings or creation revision")
            verify_training_fit(snapshot)
            verify_split_variants(owner)
            job_id = inspect_concurrent_preparation(owner, snapshot)
            require(create_dataset(owner, request_token) == created,
                    "retry after source mutation materialized a new dataset or changed its receipt")
            verify_owner_and_revocation(port, context, owner, dataset_id)
        with listening(server, config) as (_, port), client(port, context, password) as reopened:
            require(reopened.call("dataset_get", dataset_id=dataset_id) == snapshot
                    and reopened.call("job_status", job_id=job_id)["state"] == "succeeded",
                    "materialized snapshot or PL/SQL result was lost across restart")
            reopened.error("dataset_delete", INVALID_STATE, dataset_id=dataset_id)
            verify_removed_dependency(reopened)
            require(reopened.call("history_verify")["verified"], "dataset commits failed History verification")
        require(execute(server, config, "--check-storage").returncode == 0, "dataset storage integrity verification failed")
        logs = b"".join(path.read_bytes() for path in (fixture / "log").rglob("*.log"))
        require(b"held_only" not in logs, "operational log contained a raw dataset value")
    print("Dataset TLS materialization, training-only fit, group/time split, typed PL/SQL job, ACL and pin protection passed")


if __name__ == "__main__":
    main()
