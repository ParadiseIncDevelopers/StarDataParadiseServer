"""Verify opt-in live-row RESTORE and field REVERT against a disposable MySQL fork."""

import json
import secrets

from compatibility.decision.provision import parse_rule, provision as decision_sql

from .provision import Column, Table, provision
from .restore import provision_restore
from .verify import main, require


def verify_revert(run):
    suffix = secrets.token_hex(5)
    table = Table(f"sdp_revert_src_{suffix}", "items", f"sdp_revert_log_{suffix}", "id",
                  (Column("id", "bigint"), Column("score", "bigint"), Column("label", "text")))
    source, journal = table.source, table.journal
    run(provision(table) + provision_restore(table))
    run(f"INSERT INTO {source} VALUES (1,5,'initial');"
        f"UPDATE {source} SET score=9 WHERE id=1;"
        f"UPDATE {source} SET label='independent edit' WHERE id=1;")

    def preview(revision, column, error=None, **flags):
        name = "NULL" if column is None else f"'{column}'"
        rows = run(f"SELECT {journal}.history_revert_preview({revision},1,{name});", error=error, **flags)
        return json.loads(rows[0][0]) if error is None else None

    def apply(plan, token, error=None, **flags):
        rows = run(f"SELECT {journal}.history_revert_apply('{plan}', '{token}');", error=error, **flags)
        return json.loads(rows[0][0]) if error is None else None

    def state(revision, score, label):
        require(run(f"SELECT head FROM {journal}.revision_counter;") == [[str(revision)]]
                and run(f"SELECT score,label FROM {source} WHERE id=1;") == [[str(score), label]],
                "REVERT changed unrelated values or History revision")

    for field in ("id", "score ", "missing", None):
        preview(2, field, "invalid_restore_field")
    preview(1, "score", "revision_not_recoverable")  # Insert has no previous value.
    preview(2, "label", "revert_field_unchanged")
    stale = preview(2, "score")
    require(stale["mode"] == "revert" and stale["field"] == "score"
            and stale["before"]["values"]["score"]["v"] == "9"
            and stale["after"]["values"]["score"]["v"] == "5"
            and stale["after"]["values"]["label"]["v"] == "independent edit",
            "field plan changed an unrelated column")
    state(3, 9, "independent edit")
    run(f"INSERT INTO {source} VALUES (2,20,'other row');")
    apply(stale["plan_id"], secrets.token_hex(16), "stale_preview")
    state(4, 9, "independent edit")

    actor = f"reverter_{suffix}"
    run(f"CREATE USER '{actor}'@'localhost' REQUIRE SSL;"
        f"GRANT TRIGGER, SELECT, UPDATE (score) ON {source} TO '{actor}'@'localhost';"
        f"GRANT SELECT, INSERT, UPDATE, EXECUTE ON {journal}.* TO '{actor}'@'localhost';")
    plan = preview(2, "score", user=actor)["plan_id"]
    token = secrets.token_hex(16)
    apply(plan, token, "restore_plan_not_found")
    run(f"SELECT {journal}.history_restore_apply('{plan}','{token}');",
        user=actor, error="request_conflict")
    require(apply(plan, token, user=actor) == {"revision": "5", "replayed": False},
            "field apply did not complete with a single-column update grant")
    require(apply(plan, token, user=actor) == {"revision": "5", "replayed": True},
            "field apply receipt could not be replayed")
    preview(2, "score", "restore_no_change")
    state(5, 5, "independent edit")
    run(f"UPDATE {source} SET score=12 WHERE id=1;"
        f"UPDATE {source} SET label=NULL WHERE id=1;"
        f"UPDATE {source} SET score=14 WHERE id=1;")
    preview(2, "score", "revert_field_conflict")
    denied = preview(7, "label", user=actor)["plan_id"]
    apply(denied, secrets.token_hex(16), "denied", user=actor)
    state(8, 14, "NULL")

    text_plan = preview(7, "label")["plan_id"]
    text_token = secrets.token_hex(16)
    rows = run(f"START TRANSACTION; SELECT {journal}.history_revert_apply('{text_plan}','{text_token}'); ROLLBACK;")
    require(json.loads(rows[0][0]) == {"revision": "9", "replayed": False},
            "field apply did not join the caller's transaction")
    state(8, 14, "NULL")
    require(apply(text_plan, text_token) == {"revision": "9", "replayed": False},
            "rolled-back text apply could not retry")
    state(9, 14, "independent edit")
    guard = parse_rule(f"CREATE DECISION RULE floor ON {table.source_schema}.{table.source_table} "
                       "FOR UPDATE WHEN (NEW.score >= 14) THEN ALLOW;")
    run(decision_sql(guard))
    rule_plan = preview(8, "score")["plan_id"]
    apply(rule_plan, secrets.token_hex(16), "decision_denied")
    state(9, 14, "independent edit")
    run(f"CREATE TRIGGER `{table.source_schema}`.rewrite_revert BEFORE UPDATE ON {source} "
        "FOR EACH ROW SET NEW.score = OLD.score + 1;")
    rewritten = preview(9, "label")["plan_id"]
    apply(rewritten, secrets.token_hex(16), "restored_image_mismatch")
    state(9, 14, "independent edit")
    require(run(f"SELECT request_token,applied_revision FROM {journal}.restore_plans "
                f"WHERE plan_id IN (UNHEX('{rule_plan}'),UNHEX('{rewritten}'));") == [["NULL", "NULL"]] * 2,
            "failed field apply published a request token or receipt")
    print("PASS field REVERT preserves later independent values, rejects conflicting edits and enforces column grants",
          flush=True)
    print("PASS field REVERT preserves stale, owner, verb, Decision, after-image and rollback guards", flush=True)
    return table, text_plan, text_token


def run_checks(run):
    suffix = secrets.token_hex(5)
    table = Table(f"sdp_restore_src_{suffix}", "items", f"sdp_restore_log_{suffix}", "id",
                  (Column("id", "bigint"), Column("score", "bigint"), Column("label", "text")))
    source, journal = table.source, table.journal
    run(provision(table) + provision_restore(table))
    try:
        binary = Table(table.source_schema, table.source_table, table.journal_schema, table.key,
                       (*table.columns, Column("payload", "blob")))
        provision_restore(binary)
    except ValueError as error:
        require("BLOB digests cannot be restored" in str(error), "unexpected BLOB restore error")
    else:
        raise RuntimeError("RESTORE accepted BLOB digests")

    def preview(revision, row=1, error=None):
        output = run(f"SELECT {journal}.history_restore_preview({revision}, {row});", error=error)
        return json.loads(output[0][0]) if error is None else None

    def apply(plan, token, **flags):
        output = run(f"SELECT {journal}.history_restore_apply('{plan}', '{token}');", **flags)
        return json.loads(output[0][0]) if "error" not in flags else None

    def head(revision):
        require(run(f"SELECT head FROM {journal}.revision_counter;") == [[str(revision)]],
                "unexpected restore revision boundary")

    def pending(plan):
        require(run(f"SELECT request_token, applied_revision FROM {journal}.restore_plans "
                    f"WHERE plan_id=UNHEX('{plan}');") == [["NULL", "NULL"]],
                "unsuccessful apply published a token or receipt")

    # A label with Unicode, TAB, line feed and trailing spaces must reconstruct exactly.
    utf8 = "C3B6090A2020"
    run(f"INSERT INTO {source} VALUES (1,10,CONVERT(0x{utf8} USING utf8mb4));"
        f"UPDATE {source} SET id=2, score=20, label=NULL WHERE id=1;")
    before = preview(1)
    require(before["current_revision"] == "2" and before["row_id"] == "1"
            and before["before"]["values"]["id"]["v"] == "2"
            and before["after"]["values"]["id"]["v"] == "1"
            and before["after"]["values"]["label"]["v"] == bytes.fromhex(utf8).decode(),
            "Restore preview did not describe the stable row and typed values")
    require(run(f"SELECT id, score, HEX(label) FROM {source};") == [["2", "20", "NULL"]],
            "Restore preview modified live rows")
    head(2)
    preview(1, 22, error="row_identity_conflict")
    preview(3, error="revision_not_recoverable")
    run(f"INSERT INTO {source} VALUES (99,30,'other row');")
    apply(before["plan_id"], secrets.token_hex(16), error="stale_preview")
    pending(before["plan_id"])
    head(3)

    plan = preview(1)["plan_id"]
    token = secrets.token_hex(16)
    actor = f"restore_other_{suffix}"
    run(f"CREATE USER '{actor}'@'localhost' REQUIRE SSL;"
        f"GRANT TRIGGER, SELECT, UPDATE ON {source} TO '{actor}'@'localhost';"
        f"GRANT SELECT, INSERT, UPDATE, EXECUTE ON {journal}.* TO '{actor}'@'localhost';")
    apply(plan, token, user=actor, error="restore_plan_not_found")
    require(apply(plan, token) == {"revision": "4", "replayed": False}, "restore did not commit")
    require(apply(plan, token) == {"revision": "4", "replayed": True}, "restore replay changed outcome")
    apply(plan, secrets.token_hex(16), error="request_conflict")
    require(run(f"SELECT id,score,HEX(label) FROM {source} WHERE id<>99;") == [["1", "10", utf8]]
            and run(f"SELECT row_id,source_key FROM {journal}.row_identity WHERE row_id=1;") == [["1", "1"]],
            "restore changed the stable row ID or lost exact typed text")
    head(4)
    print("PASS RESTORE applies a stable row, records one event and replays its actor-bound receipt", flush=True)

    run(f"INSERT INTO {source} VALUES (2,25,'conflict');")
    preview(2, error="row_identity_conflict")
    run(f"DELETE FROM {source} WHERE id=2;")
    scratch_plan = preview(2)["plan_id"]
    apply(scratch_plan, token, error="request_conflict")
    scratch_token = secrets.token_hex(16)
    result = run(f"START TRANSACTION; "
                 f"SELECT {journal}.history_restore_apply('{scratch_plan}', '{scratch_token}'); ROLLBACK;")
    require(json.loads(result[0][0]) == {"revision": "7", "replayed": False},
            "restore did not act in the caller transaction")
    head(6)
    pending(scratch_plan)
    require(run(f"SELECT id,score FROM {source} WHERE id<>99;") == [["1", "10"]],
            "rollback retained the restored row")
    require(apply(scratch_plan, scratch_token) == {"revision": "7", "replayed": False},
            "rolled-back restore could not retry")

    child = f"`{table.source_schema}`.child_fixture"
    run(f"CREATE TABLE {child}(id BIGINT PRIMARY KEY, FOREIGN KEY (id) REFERENCES {source}(id)) ENGINE=InnoDB;"
        f"INSERT INTO {child} VALUES (2);")
    constrained = preview(1)["plan_id"]
    apply(constrained, secrets.token_hex(16), error="foreign key constraint fails")
    pending(constrained)
    head(7)
    run(f"DROP TABLE {child}; CREATE TABLE {child}(id BIGINT PRIMARY KEY, "
        f"FOREIGN KEY (id) REFERENCES {source}(id) ON UPDATE CASCADE) ENGINE=InnoDB;")
    preview(1, error="schema_conflict")
    run(f"DROP TABLE {child};")

    guard = parse_rule(f"CREATE DECISION RULE floor ON {table.source_schema}.{table.source_table} "
                       "FOR UPDATE WHEN (NEW.score >= 15) THEN ALLOW;")
    run(decision_sql(guard))
    apply(preview(1)["plan_id"], secrets.token_hex(16), error="decision_denied")
    head(7)
    run(f"DROP TRIGGER `{table.source_schema}`.`{guard.trigger_name}`;"
        f"ALTER TABLE {source} ADD CONSTRAINT limit_score CHECK (score >= 15);")
    constrained = preview(1)["plan_id"]
    apply(constrained, secrets.token_hex(16), error="Check constraint")
    pending(constrained)
    head(7)
    run(f"ALTER TABLE {source} DROP CHECK limit_score; "
        f"CREATE TRIGGER `{table.source_schema}`.rewrite_restore BEFORE UPDATE ON {source} "
        "FOR EACH ROW SET NEW.label = 'rewritten';")
    rewritten = preview(1)["plan_id"]
    apply(rewritten, secrets.token_hex(16), error="restored_image_mismatch")
    pending(rewritten)
    head(7)
    require(run(f"SELECT id,score,HEX(label) FROM {source} WHERE id<>99;") == [["2", "20", "NULL"]],
            "a post-write mismatch retained the change")
    run(f"ALTER TABLE {source} MODIFY label VARCHAR(32);")
    preview(1, error="schema_conflict")
    print("PASS RESTORE fails closed on stale plans, key conflicts, Decision, CHECK/FK and changed schema", flush=True)
    print("PASS caller rollback and rejected after-image roll back data, History and receipt", flush=True)
    return (table, plan, token), verify_revert(run)


def check_after_restart(run, state):
    restore, revert = state
    table, plan, token = restore
    output = run(f"SELECT {table.journal}.history_restore_apply('{plan}','{token}');")
    require(json.loads(output[0][0]) == {"revision": "4", "replayed": True}
            and run(f"SELECT head FROM {table.journal}.revision_counter;") == [["7"]],
            "durable restore receipt did not replay after restart")
    print("PASS RESTORE receipt persists across restart without another write", flush=True)
    table, plan, token = revert
    output = run(f"SELECT {table.journal}.history_revert_apply('{plan}','{token}');")
    require(json.loads(output[0][0]) == {"revision": "9", "replayed": True}
            and run(f"SELECT head FROM {table.journal}.revision_counter;") == [["9"]],
            "durable field revert receipt did not replay after restart")
    print("PASS REVERT receipt persists across restart without another write", flush=True)


if __name__ == "__main__":
    main(run_checks, __doc__)
