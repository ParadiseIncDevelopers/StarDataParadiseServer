"""Verify Decision inspection on a real TLS Server with isolated storage."""

import argparse
import json
from pathlib import Path
import secrets
import ssl
import tempfile
import time

from decision_rpc import rejection
from history_restore_rpc import head, read_at, select
from security_rpc import receive_reply
from service_smoke import certificates, connection, exchange, execute, files_in, listening, require, save_config
from sql_rpc import Client, NULL, client, i64, text, token, write


PRIVATE_NOTE = "fixture-payment-proof-never-in-inspection-output"
INSPECTED_TABLE = "inspect_entries"
RANK_TABLE = "inspect_rank"
RANK_RULE = "inspect_threshold"


class Reader(Client):
    def __init__(self, sock, password):
        self.sock = sock
        self.id = 2
        require(exchange(sock, 1, "hello")["ok"], "inspection reader hello failed")
        require(exchange(sock, 2, "auth", {"username": "inspection_reader", "password": password})["ok"],
                "inspection reader authentication failed")


def scenario(table, stable_id, **proposed):
    return {"target": table, "event": "update", "row_id": stable_id, "proposed": proposed}


def stable_rows(client_, table, revision):
    rows = read_at(client_, table, revision)["rows"]
    return {int(row["values"]["id"]["v"]): row["row_id"] for row in rows}


def install_preview(client_):
    write(client_, f"CREATE TABLE {INSPECTED_TABLE} (id BIGINT PRIMARY KEY, amount BIGINT, doubled BIGINT)", 0)
    write(client_, "CREATE TABLE inspection_payment (id BIGINT PRIMARY KEY, note TEXT)", 0)
    write(client_, "CREATE TABLE inspection_audit (id BIGINT PRIMARY KEY)", 0)
    inserted = write(client_, f"INSERT INTO {INSPECTED_TABLE} VALUES (1,2,0),(2,3,0)", 2)["lsn"]
    write(client_, f"INSERT INTO inspection_payment VALUES (1,'{PRIVATE_NOTE}')", 1)
    write(client_, f"""CREATE DECISION RULE amount_calc ON {INSPECTED_TABLE} FOR UPDATE
        PRIORITY 30 WHEN (NEW.amount > 0) THEN SET doubled = NEW.amount * 2""", 0)
    installed = write(client_, f"""CREATE DECISION RULE inspect_gate ON {INSPECTED_TABLE} FOR UPDATE
        PRIORITY 20 WHEN (NEW.amount > 0 AND EXISTS (
            SELECT id FROM inspection_payment WHERE id = :NEW.id AND note = '{PRIVATE_NOTE}'
        )) THEN ALLOW""", 0)["lsn"]
    write(client_, f"""CREATE TRIGGER inspection_event AFTER UPDATE ON {INSPECTED_TABLE} FOR EACH ROW
BEGIN
    INSERT INTO inspection_audit VALUES (:NEW.id);
END;""", 0)
    return stable_rows(client_, INSPECTED_TABLE, inserted), read_at(client_, "inspect_gate", installed)["object_id"]


def find_guard(result, rule_id):
    for rule in result["rules"]:
        if str(rule["object_id"]) == rule_id:
            return rule
    raise RuntimeError("Decision inspection omitted the expected rule identity")


def require_failed_guard(result, rule_id, exists_state):
    require(result["allowed"] is False and result["reason"] == "denied", "a failed guard was not explained")
    guard = find_guard(result, rule_id)
    require(guard["state"] == "evaluated" and guard["truth"] == "false" and guard["version"] == 1 and
            type(guard["elapsed_ns"]) is int and guard["elapsed_ns"] >= 0, "guard truth, version or timing was lost")
    nodes = guard["nodes"]
    require(len({node["id"] for node in nodes}) == len(nodes), "Decision node IDs are ambiguous")
    node_ids = {node["id"] for node in nodes}
    require(all(node["parent_id"] == 0 or node["parent_id"] in node_ids for node in nodes),
            "Decision node references a nonexistent parent")
    root = next((node for node in nodes if node["op"] == "and"), None)
    require(root and root["state"] == "evaluated" and root["truth"] == "false", "AND trace did not report actual truth")
    matches = [node for node in nodes if node["op"] == "exists"]
    require(len(matches) == 1 and matches[0]["state"] == exists_state, "EXISTS short-circuit was misreported")
    expected_truth = None if exists_state == "skipped" else "false"
    require(matches[0]["truth"] == expected_truth, "EXISTS trace did not report its actual evaluation state")
    require(PRIVATE_NOTE not in json.dumps(result), "Decision inspection leaked a literal or dependency value")


def test_preview(client_, rows, rule_id):
    baseline = head(client_)
    skipped = scenario(INSPECTED_TABLE, rows[1], amount=i64(0))
    denied = scenario(INSPECTED_TABLE, rows[2], amount=i64(2))
    accepted = scenario(INSPECTED_TABLE, rows[1], amount=i64(9))
    explain = client_.call("decision_explain", **skipped)
    whynot = client_.call("decision_whynot", **denied)
    require(explain["revision"] == baseline and whynot["revision"] == baseline,
            "inspection mixed incompatible snapshots")
    require_failed_guard(explain, rule_id, "skipped")
    require_failed_guard(whynot, rule_id, "evaluated")
    original = client_.call("decision_whatif", **accepted)
    recheck = client_.call("decision_recheck", **accepted)
    for result in (original, recheck):
        require(result["allowed"] is True and result["computed"] == {"doubled": i64(18)},
                "Decision preview/recheck did not produce the same typed computation as the write path")
    body = json.dumps(denied, ensure_ascii=False, separators=(",", ":")).replace("'", "''")
    through_sql = client_.mutate(f"DECISION WHYNOT USING '{body}'")["decision"]
    require_failed_guard(through_sql, rule_id, "evaluated")
    candidate = f"""CREATE DECISION RULE observe_only ON {INSPECTED_TABLE}
        FOR UPDATE PRIORITY 1 WHEN (NEW.amount > 0) THEN DENY"""
    require(client_.call("decision_validate", sql=candidate)["valid"] is True,
            "Decision validate did not accept a pure candidate")
    client_.error("decision_validate", 1012, sql="SELECT 1")
    client_.error("decision_whatif", 1012, **{**accepted, "proposed": {"unrecognized": i64(1)}})
    with client_.prepared("DECISION WHATIF USING '[]'") as statement:
        malformed = client_.raw("execute", statement_id=statement)
        require(not malformed["ok"] and malformed["error"]["code"] in (1001, 1012),
                "Decision SQL accepted a JSON array in place of an object")
    require(head(client_) == baseline, "Decision inspection preview, recheck or validation published a revision")
    require(select(client_, f"SELECT id,amount,doubled FROM {INSPECTED_TABLE} ORDER BY id") ==
            [[i64(1), i64(2), i64(0)], [i64(2), i64(3), i64(0)]] and
            select(client_, "SELECT id FROM inspection_audit") == [], "Decision preview modified rows or ran a trigger")


def apply_real_update(client_):
    write(client_, f"UPDATE {INSPECTED_TABLE} SET amount=9 WHERE id=1", 1)
    require(select(client_, f"SELECT doubled FROM {INSPECTED_TABLE} WHERE id=1") == [[i64(18)]] and
            select(client_, "SELECT id FROM inspection_audit") == [[i64(1)]],
            "real write did not recompute the requested Decision and fire its installed trigger")


def test_permissions(owner, socket_context, port, rows, rule_id):
    password = secrets.token_urlsafe(32)
    write(owner, "CREATE USER inspection_reader", 0)
    owner.call("password_set", username="inspection_reader", new_password=password, request_token=token())
    write(owner, f"GRANT SELECT ON TABLE {INSPECTED_TABLE} TO USER inspection_reader", 0)
    version = head(owner)
    request = scenario(INSPECTED_TABLE, rows[2], amount=i64(2))
    with connection(port, socket_context) as socket:
        viewer = Reader(socket, password)
        reply = viewer.raw("decision_whynot", **request)
        require(not reply["ok"] and reply["error"]["code"] == 1006 and "body" not in reply and
                PRIVATE_NOTE not in json.dumps(reply), "missing dependency SELECT did not protect the decision")
        viewer.error("decision_validate", 1006, sql=f"""CREATE DECISION RULE viewer_rule ON {INSPECTED_TABLE}
                     FOR UPDATE PRIORITY 1 WHEN (TRUE) THEN ALLOW""")
        require(head(owner) == version, "unauthorized inspection published a change")
        write(owner, "GRANT SELECT ON TABLE inspection_payment TO USER inspection_reader", 0)
        require_failed_guard(viewer.call("decision_whynot", **request), rule_id, "evaluated")
        viewer.error("decision_whynot", 1006, **request, revision=version)


def wait_for_blocked_inspection(controller, request_id):
    deadline = time.monotonic() + 1.25
    while time.monotonic() < deadline:
        sessions = controller.call("session_list")["sessions"]
        if any(entry["request_id"] == request_id and entry["operation"] == "decision_explain" and
               entry["lock_wait_ms"] > 0 for entry in sessions):
            return
        time.sleep(0.005)
    raise RuntimeError("inspection never waited on the active Decision transaction")


def test_versioned_corpus(owner, port, socket_context, password):
    write(owner, f"CREATE TABLE {RANK_TABLE} (id BIGINT PRIMARY KEY, amount BIGINT)", 0)
    inserted = write(owner, f"INSERT INTO {RANK_TABLE} VALUES (1,7)", 1)["lsn"]
    stable_id = stable_rows(owner, RANK_TABLE, inserted)[1]
    version_a = write(owner, f"""CREATE DECISION RULE {RANK_RULE} ON {RANK_TABLE}
        FOR UPDATE PRIORITY 10 WHEN (NEW.amount > 5) THEN ALLOW""", 0)["lsn"]
    rule_id = read_at(owner, RANK_RULE, version_a)["object_id"]
    write(owner, f"""CREATE DECISION RULE {RANK_RULE} ON {RANK_TABLE}
        FOR UPDATE PRIORITY 10 WHEN (NEW.amount > 10) THEN ALLOW""", 0)
    old = scenario(RANK_TABLE, stable_id, amount=i64(7))
    current = head(owner)
    cases = [{"event": "update", "row_id": stable_id, "proposed": {"amount": val}}
             for val in (i64(5), i64(7), i64(11), NULL)]
    compared = owner.call("decision_test", target=RANK_TABLE, rule=RANK_RULE, from_version=1, to_version=2, cases=cases)
    require(compared["revision"] == current and compared["changed"] == 1 and len(compared["cases"]) == 4,
            "Decision test did not compare both versions against one bounded snapshot")
    expected = [(False, False), (True, False), (True, True), (False, False)]
    for pair, flags in zip(compared["cases"], expected, strict=True):
        prior, candidate = pair["prior"], pair["candidate"]
        require((prior["allowed"], candidate["allowed"]) == flags and
                find_guard(prior, rule_id)["version"] == 1 and find_guard(candidate, rule_id)["version"] == 2,
                "Decision corpus returned incorrect alternatives or mixed policy versions")
    require(head(owner) == current and select(owner, f"SELECT amount FROM {RANK_TABLE}") == [[i64(7)]],
            "comparing a candidate changed the active pointer or rows")

    require(owner.call("decision_explain", **old)["allowed"] is True, "creating a candidate activated it early")
    with client(port, socket_context, password) as peer, client(port, socket_context, password) as controller:
        owner.call("tx_begin")
        owner.mutate(f"ALTER DECISION RULE {RANK_RULE} ON {RANK_TABLE} ACTIVATE VERSION 2")
        peer.send_without_receiving("decision_explain", **old)
        wait_for_blocked_inspection(controller, peer.id)
        committed = owner.call("tx_commit", request_token=token())["lsn"]
        reply = receive_reply(peer)
        require(reply["ok"] and reply["body"]["revision"] == committed and reply["body"]["allowed"] is False
                and find_guard(reply["body"], rule_id)["version"] == 2,
                "concurrent inspection did not observe the committed activation as a single snapshot")
    historic = owner.call("decision_explain", **old, revision=version_a)
    require(historic["allowed"] is True and historic["revision"] == version_a
            and find_guard(historic, rule_id)["version"] == 1,
            "retained row-local Decision inspection used today's active version")
    owner.call("history_purge", revision=committed)
    rejection(owner.raw("decision_explain", **old, revision=version_a),
              "dependency_snapshot_unavailable", 1007)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--server", type=Path, required=True)
    parser.add_argument("--openssl", type=Path, required=True)
    parser.add_argument("--scratch", type=Path, required=True)
    args = parser.parse_args()
    server = str(args.server.resolve(strict=True))
    openssl = str(args.openssl.resolve(strict=True))
    args.scratch.mkdir(parents=True, exist_ok=True)
    with tempfile.TemporaryDirectory(prefix="s22-", dir=args.scratch) as directory:
        fixture = Path(directory)
        for name in ("data", "log", "temp"):
            (fixture / name).mkdir()
        certificates(openssl, fixture)
        config = save_config(fixture, timeout_ms=20_000, max_connections=6)
        require(execute(server, config, "--initialize-storage").returncode == 0,
                "Decision inspection fixture initialization failed")
        password = secrets.token_urlsafe(32)
        require(execute(server, config, "--bootstrap-admin", "local-admin",
                        stdin=((password + "\n") * 2).encode("ascii")).returncode == 0,
                "Decision inspection fixture administrator setup failed")
        context = ssl.create_default_context(cafile=str(fixture / "ca.pem"))
        with listening(server, config) as (_, port), client(port, context, password) as owner:
            rows, rule_id = install_preview(owner)
        data_before = files_in(fixture / "data")
        temp_before = files_in(fixture / "temp")
        with listening(server, config) as (_, port), client(port, context, password) as owner:
            test_preview(owner, rows, rule_id)
        require(files_in(fixture / "data") == data_before and files_in(fixture / "temp") == temp_before,
                "read-only Decision previews changed storage, WAL or temporary files")
        with listening(server, config) as (_, port), client(port, context, password) as owner:
            apply_real_update(owner)
            test_permissions(owner, context, port, rows, rule_id)
            test_versioned_corpus(owner, port, context, password)
            require(owner.call("history_verify")["verified"], "inspection or activation corrupted History")
        require(execute(server, config, "--check-storage").returncode == 0,
                "Decision inspection fixture storage did not verify offline")

    print("Decision inspection trace, dependency access, no preview writes, single-snapshot version corpus, "
          "concurrent activation and historical unavailability passed")


if __name__ == "__main__":
    main()
