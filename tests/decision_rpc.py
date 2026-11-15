"""Exercise persisted Decision rules through the real loopback TLS data paths."""

import argparse
import json
from pathlib import Path
import secrets
import ssl
import tempfile
import time

from history_restore_rpc import head, preview, read_at, select
from security_rpc import receive_reply
from service_smoke import certificates, connection, exchange, execute, listening, require, save_config
from sql_rpc import client, i64, text, token, write


PERMISSION_DENIED = 1006
MODULE_UNAVAILABLE = 1007
INVALID_STATE = 1013
APPROVE_ORDER = "UPDATE guarded_orders SET phase='approved' WHERE id=1"
ORDER_RULE = """CREATE DECISION RULE approval_guard ON guarded_orders FOR UPDATE
PRIORITY 100 WHEN (NOT (NEW.phase = 'approved')
    OR (OLD.phase = 'draft' AND EXISTS (SELECT id FROM order_payments WHERE id = :NEW.id))) THEN ALLOW"""
COMPUTE_RULE = """CREATE DECISION RULE calculate_total ON decision_totals FOR INSERT
PRIORITY 100 WHEN (ACTOR = 'local-admin' AND NEW.amount >= 0) THEN SET doubled = twice_amount(NEW.amount)"""


def rejection(reply, reason, code=PERMISSION_DENIED):
    error = reply.get("error", {})
    require(not reply["ok"] and error.get("code") == code and error.get("decision_reason") == reason,
            f"expected Decision {reason}/{code}, got {error.get('decision_reason')}/{error.get('code')}")


def denied_write(session, sql, reason="denied", code=PERMISSION_DENIED):
    request_token = token()
    revision = head(session)
    with session.prepared(sql) as statement:
        rejection(session.raw("execute", statement_id=statement, request_token=request_token), reason, code)
    require(session.call("tx_outcome", request_token=request_token) == {"status": "unknown"}
            and head(session) == revision, "a denied write published an outcome or History revision")


def verify_order_status(session, order, expected):
    require(select(session, f"SELECT phase FROM guarded_orders WHERE id={order}") == [[text(expected)]],
            f"unexpected order status for id {order}")


def install_orders(session):
    write(session, "CREATE TABLE guarded_orders (id BIGINT PRIMARY KEY, phase TEXT NOT NULL)", 0)
    write(session, "CREATE TABLE order_payments (id BIGINT PRIMARY KEY)", 0)
    write(session, "CREATE TABLE approval_queue (id BIGINT PRIMARY KEY)", 0)
    write(session, "CREATE TABLE regular_writes (id BIGINT PRIMARY KEY)", 0)
    prior = write(session, "INSERT INTO guarded_orders VALUES (1,'approved')", 1)["lsn"]
    row = read_at(session, "guarded_orders", prior)["rows"][0]
    write(session, "UPDATE guarded_orders SET phase='draft' WHERE id=1", 1)
    write(session, "INSERT INTO guarded_orders VALUES (2,'draft')", 1)
    write(session, "INSERT INTO order_payments VALUES (2)", 1)
    write(session, ORDER_RULE, 0)
    write(session, """CREATE PROCEDURE request_approval() AS
BEGIN
    UPDATE guarded_orders SET phase='approved' WHERE id=1;
END;""", 0)
    write(session, """CREATE TRIGGER approve_queued_order AFTER INSERT ON approval_queue FOR EACH ROW
BEGIN
    UPDATE guarded_orders SET phase='approved' WHERE id=:NEW.id;
END;""", 0)
    return prior, row["row_id"]


def prove_all_write_paths(session, prior, identity):
    denied_write(session, APPROVE_ORDER)
    denied_write(session, "CALL request_approval()")
    denied_write(session, "INSERT INTO approval_queue VALUES (1)")
    require(select(session, "SELECT id FROM approval_queue") == [],
            "a trigger-induced denial left the source statement applied")

    plan = preview(session, "restore", "guarded_orders", prior, row_id=identity)
    revision = head(session)
    request_token = token()
    rejection(session.raw("history_restore", plan_id=plan["plan_id"], request_token=request_token), "denied")
    require(session.call("tx_outcome", request_token=request_token) == {"status": "unknown"}
            and head(session) == revision, "a denied restore left a committed result")
    verify_order_status(session, 1, "draft")

    write(session, "INSERT INTO order_payments VALUES (1)", 1)
    write(session, APPROVE_ORDER, 1)
    verify_order_status(session, 1, "approved")


def rule_definition_at(session, name, revision):
    historical = read_at(session, name, revision)
    raw = historical["object"]["definition"]
    definition = json.loads(raw)
    require(type(definition["active"]) is int and isinstance(definition["versions"], list),
            "History omitted the rule version or active pointer")
    return historical["object_id"], definition


def prove_rule_order_and_history(session):
    write(session, "CREATE TABLE decision_rank (id BIGINT PRIMARY KEY, score BIGINT)", 0)
    write(session, """CREATE DECISION RULE low_veto ON decision_rank FOR INSERT
PRIORITY 10 WHEN (NEW.score > 0) THEN DENY""", 0)
    write(session, """CREATE DECISION RULE zulu_gate ON decision_rank FOR INSERT
PRIORITY 100 WHEN (NEW.score > 0) THEN ALLOW""", 0)
    write(session, "INSERT INTO decision_rank VALUES (1,10)", 1)

    create_revision = write(session, """CREATE DECISION RULE alpha_gate ON decision_rank FOR INSERT
PRIORITY 100 WHEN (NEW.score > 0) THEN DENY""", 0)["lsn"]
    denied_write(session, "INSERT INTO decision_rank VALUES (2,10)")
    denied_write(session, "INSERT INTO decision_rank VALUES (3,NULL)", reason="unknown")
    denied_write(session, "INSERT INTO decision_rank VALUES (4,0)")
    version_revision = write(session, """CREATE DECISION RULE alpha_gate ON decision_rank FOR INSERT
PRIORITY 100 WHEN (NEW.score > 0) THEN ALLOW""", 0)["lsn"]
    denied_write(session, "INSERT INTO decision_rank VALUES (2,10)")
    activate_revision = write(session, "ALTER DECISION RULE alpha_gate ON decision_rank ACTIVATE VERSION 2", 0)["lsn"]
    write(session, "INSERT INTO decision_rank VALUES (2,10)", 1)

    first_id, first = rule_definition_at(session, "alpha_gate", create_revision)
    second_id, second = rule_definition_at(session, "alpha_gate", version_revision)
    third_id, third = rule_definition_at(session, "alpha_gate", activate_revision)
    require(first_id == second_id == third_id and first["active"] == 1 and second["active"] == 1
            and third["active"] == 2 and len(first["versions"]) == 1 and len(second["versions"]) == 2
            and first["versions"][0] == second["versions"][0] and second["versions"] == third["versions"],
            "Decision rule version or stable object identity changed unexpectedly")
    require("THEN DENY" in first["versions"][0]["source"] and "THEN ALLOW" in second["versions"][1]["source"],
            "History did not retain both rule definitions")
    activation = session.call("history_log", target="alpha_gate",
                              **{"from": activate_revision, "to": activate_revision})["entries"]
    require(len(activation) == 1 and activation[0]["kind"] == "decision" and activation[0]["object_id"] == first_id
            and activation[0]["revision"] == activate_revision and activation[0]["actor"] == "local-admin",
            "activation and its History metadata were not written as one actor-attributed revision")
    require(session.call("history_verify")["verified"], "Decision catalog writes broke History verification")
    return first_id


def prove_computation(session):
    write(session, "CREATE TABLE decision_totals (id BIGINT PRIMARY KEY, amount BIGINT, doubled BIGINT)", 0)
    write(session, """CREATE FUNCTION twice_amount(input_amount IN BIGINT) RETURN BIGINT AS
BEGIN
    RETURN input_amount * 2;
END;""", 0)
    write(session, COMPUTE_RULE, 0)
    write(session, """CREATE DECISION RULE authorize_total ON decision_totals FOR INSERT
PRIORITY 100 WHEN (ACTOR = 'local-admin') THEN ALLOW""", 0)
    result = write(session, "INSERT INTO decision_totals VALUES (1,12,999)", 1)
    require(select(session, "SELECT doubled FROM decision_totals WHERE id=1") == [[i64(24)]],
            "the typed Decision output did not replace the proposed value")
    after = read_at(session, "decision_totals", result["lsn"])["rows"][0]["values"]
    require(after["doubled"] == i64(24), "Decision output and History were not published atomically")


def prove_side_effect_rejected(session):
    write(session, "CREATE SEQUENCE decision_sequence", 0)
    write(session, """CREATE FUNCTION hosted_gate(input_id BIGINT) RETURN BOOLEAN AS
BEGIN
    RETURN EXISTS (SELECT 1 FROM order_payments WHERE id = :input_id);
END;""", 0)
    revision = head(session)
    for expression in ("EXISTS (SELECT NEXT VALUE FOR decision_sequence)", "hosted_gate(NEW.id)"):
        rule = f"""CREATE DECISION RULE invalid_gate ON decision_totals FOR UPDATE
PRIORITY 100 WHEN ({expression}) THEN ALLOW"""
        reply = session.raw("query.prepare", sql=rule)
        if reply["ok"]:
            statement = reply["body"]["statement_id"]
            try:
                reply = session.raw("execute", statement_id=statement, request_token=token())
            finally:
                session.call("statement_close", statement_id=statement)
        require(not reply["ok"] and reply["error"]["code"] == 1004 and head(session) == revision,
                "a non-pure Decision was accepted or produced a side effect")


def prove_actor_authorization(owner, port, context):
    password = secrets.token_urlsafe(32)
    write(owner, "CREATE USER decision_actor", 0)
    owner.call("password_set", username="decision_actor", new_password=password, request_token=token())
    write(owner, "GRANT INSERT ON TABLE decision_totals TO USER decision_actor", 0)
    write(owner, "GRANT DDL ON GLOBAL TO USER decision_actor", 0)
    revision = head(owner)
    with connection(port, context) as actor:
        require(exchange(actor, 1, "hello")["ok"], "Decision actor hello failed")
        require(exchange(actor, 2, "auth", {"username": "decision_actor", "password": password})["ok"],
                "Decision actor authentication failed")
        prepared = exchange(actor, 3, "query.prepare", {"sql": "INSERT INTO decision_totals VALUES (2,12,0)"})
        require(prepared["ok"], "the Decision actor's INSERT grant was not recognized")
        rejection(exchange(actor, 4, "execute", {"statement_id": prepared["body"]["statement_id"],
                                                 "request_token": token()}), "denied")
        rule = """CREATE DECISION RULE bypass_actor ON decision_totals FOR INSERT
PRIORITY 200 WHEN (NEW.amount >= 0) THEN ALLOW"""
        reply = exchange(actor, 5, "query.prepare", {"sql": rule})
        if reply["ok"]:
            reply = exchange(actor, 6, "execute", {"statement_id": reply["body"]["statement_id"],
                                                  "request_token": token()})
        require(not reply["ok"] and reply["error"]["code"] == PERMISSION_DENIED,
                "general DDL grant allowed the user to bypass an administrator-owned guard")
    require(head(owner) == revision and select(owner, "SELECT id FROM decision_totals") == [[i64(1)]],
            "a rejected actor changed policy or computed rows")


def prove_dependency_pinning(session):
    prior = head(session)
    previous = read_at(session, "decision_totals", prior)["rows"][0]
    write(session, """CREATE OR REPLACE FUNCTION twice_amount(input_amount IN BIGINT) RETURN BIGINT AS
BEGIN
    RETURN input_amount * 3;
END;""", 0)
    proposed = "INSERT INTO decision_totals VALUES (2,12,0)"
    denied_write(session, proposed, "policy_unavailable", INVALID_STATE)
    write(session, COMPUTE_RULE, 0)
    denied_write(session, proposed, "policy_unavailable", INVALID_STATE)
    write(session, "ALTER DECISION RULE calculate_total ON decision_totals ACTIVATE VERSION 2", 0)
    write(session, proposed, 1)
    require(select(session, "SELECT doubled FROM decision_totals ORDER BY id") == [[i64(24)], [i64(36)]],
            "activating the validated dependency changed a prior row or retained the old result")
    removed = write(session, "DELETE FROM decision_totals WHERE id=1", 1)["lsn"]
    plan = preview(session, "restore", "decision_totals", prior, row_id=previous["row_id"])
    request_token = token()
    reply = session.raw("history_restore", plan_id=plan["plan_id"], request_token=request_token)
    rejection(reply, "history_image_changed", INVALID_STATE)
    require(head(session) == removed and session.call("tx_outcome", request_token=request_token) == {"status": "unknown"}
            and select(session, "SELECT id FROM decision_totals") == [[i64(2)]],
            "a restore silently substituted its recalculated Decision value or published a partial result")


def wait_for_decision_writer(controller, request_id):
    deadline = time.monotonic() + 1.3
    while time.monotonic() < deadline:
        sessions = controller.call("session_list")["sessions"]
        if any(session["request_id"] == request_id and session["operation"] == "execute"
               and session["lock_wait_ms"] > 0 for session in sessions):
            return
        time.sleep(0.005)
    raise RuntimeError("contending Decision write was not observed in the transaction lease queue")


def prove_concurrency(port, context, password):
    with client(port, context, password) as holder, client(port, context, password) as contender, \
            client(port, context, password) as controller:
        with contender.prepared("UPDATE guarded_orders SET phase='approved' WHERE id=2") as approval:
            holder.call("tx_begin")
            holder.mutate("DELETE FROM order_payments WHERE id=2")
            blocked_token = token()
            contender.send_without_receiving("execute", statement_id=approval, request_token=blocked_token)
            wait_for_decision_writer(controller, contender.id)
            holder.call("tx_commit", request_token=token())
            rejection(receive_reply(contender), "denied")
            require(contender.call("tx_outcome", request_token=blocked_token) == {"status": "unknown"},
                    "the contending approval committed against the removed payment")
            verify_order_status(contender, 2, "draft")

            write(holder, "INSERT INTO order_payments VALUES (2)", 1)
            holder.call("tx_begin")
            holder.mutate("DELETE FROM order_payments WHERE id=2")
            accepted_token = token()
            contender.send_without_receiving("execute", statement_id=approval, request_token=accepted_token)
            wait_for_decision_writer(controller, contender.id)
            holder.call("tx_rollback")
            reply = receive_reply(contender)
            require(reply["ok"] and reply["body"]["affected_rows"] == 1,
                    "the contending approval did not observe the retained payment")
            verify_order_status(contender, 2, "approved")


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--server", type=Path, required=True)
    parser.add_argument("--openssl", type=Path, required=True)
    parser.add_argument("--scratch", type=Path, required=True)
    args = parser.parse_args()
    server = str(args.server.resolve(strict=True))
    openssl = str(args.openssl.resolve(strict=True))
    args.scratch.mkdir(parents=True, exist_ok=True)
    with tempfile.TemporaryDirectory(prefix="s21-", dir=args.scratch) as directory:
        fixture = Path(directory)
        for name in ("data", "log", "temp"):
            (fixture / name).mkdir()
        certificates(openssl, fixture)
        enabled = save_config(fixture, "enabled.json", timeout_ms=20_000, max_connections=6)
        disabled = save_config(fixture, "disabled.json", timeout_ms=20_000, max_connections=6, decision_enabled=False)
        require(execute(server, enabled, "--initialize-storage").returncode == 0, "Decision fixture initialization failed")
        password = secrets.token_urlsafe(32)
        require(execute(server, enabled, "--bootstrap-admin", "local-admin",
                        stdin=((password + "\n") * 2).encode("ascii")).returncode == 0,
                "Decision fixture administrator setup failed")
        context = ssl.create_default_context(cafile=str(fixture / "ca.pem"))

        with listening(server, enabled) as (_, port):
            with client(port, context, password) as owner:
                require(owner.call("capabilities")["features"].get("decision_v1") is True,
                        "enabled Server did not advertise Decision v1")
                prior, stable_id = install_orders(owner)
                prove_all_write_paths(owner, prior, stable_id)
                rule_id = prove_rule_order_and_history(owner)
                prove_computation(owner)
                prove_side_effect_rejected(owner)
                prove_actor_authorization(owner, port, context)
                prove_dependency_pinning(owner)
            prove_concurrency(port, context, password)

        with listening(server, disabled) as (_, port), client(port, context, password) as owner:
            require(owner.call("capabilities")["features"].get("decision_v1") is False,
                    "disabled Server advertised Decision runtime availability")
            denied_write(owner, "UPDATE guarded_orders SET phase='draft' WHERE id=1",
                         "module_unavailable", MODULE_UNAVAILABLE)
            verify_order_status(owner, 1, "approved")
            write(owner, "INSERT INTO regular_writes VALUES (1)", 1)
            persisted_id, persisted = rule_definition_at(owner, "alpha_gate", head(owner))
            require(persisted_id == rule_id and persisted["active"] == 2,
                    "disabled runtime lost or modified the active Decision rule")

        enabled = save_config(fixture, "enabled.json", timeout_ms=20_000, max_connections=6, decision_enabled=True)
        with listening(server, enabled) as (_, port), client(port, context, password) as owner:
            require(owner.call("capabilities")["features"].get("decision_v1") is True,
                    "explicitly re-enabled Server did not advertise Decision v1")
            require(select(owner, "SELECT id FROM regular_writes") == [[i64(1)]],
                    "disabling Decision prevented an unrelated write")
            write(owner, "INSERT INTO decision_rank VALUES (5,10)", 1)
            denied_write(owner, "INSERT INTO decision_rank VALUES (6,NULL)", reason="unknown")
            require(owner.call("history_verify")["verified"], "Decision revision did not verify after restart")
        require(execute(server, enabled, "--check-storage").returncode == 0, "Decision storage did not verify offline")

    print("Decision TLS SQL/procedure/trigger/restore, UNKNOWN and priority, immutable History versions, "
          "serial lease and disabled-runtime enforcement passed")


if __name__ == "__main__":
    main()
