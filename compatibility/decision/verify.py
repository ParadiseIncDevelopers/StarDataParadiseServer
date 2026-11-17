"""Test fail-closed Decision enforcement with the live History and MySQL paths."""

from __future__ import annotations

import secrets

from compatibility.history.provision import Column, Table, provision as provision_history
from compatibility.history.verify import require
from .provision import parse_rule, parse_rules, provision


def rule_source(name, table, event, predicate, priority=None, effect="ALLOW"):
    order = "" if priority is None else f" PRIORITY {priority}"
    return f"CREATE DECISION RULE {name} ON {table} FOR {event}{order} WHEN ({predicate}) THEN {effect};"


def check_parser():
    unsupported = (
        ("INSERT", "NEW.score > 9223372036854775808"),
        ("DELETE", "NEW.score >= 0"),
        ("WRITE", "NEW.score >= 0"),
        ("INSERT", "OLD.score < NEW.score"),
        ("INSERT", "NEW.score > 0 OR NEW.score IS NULL"),
        ("INSERT", "NEW.score > 0); DROP TABLE orders;--"),
    )
    for event, predicate in unsupported:
        try:
            parse_rule(rule_source("reject", "qa.orders", event, predicate))
        except ValueError:
            continue
        raise RuntimeError(f"recognized unsupported Decision rule {event} {predicate}")
    minimum = parse_rule(rule_source("signed_i64", "orders", "INSERT",
                                     "NEW.score >= -9223372036854775808"), "qa")
    require(minimum.value == -(1 << 63) and minimum.schema == "qa" and minimum.priority == 0,
            "supported signed BIGINT/default schema lost")
    maximum = parse_rule(rule_source("max_priority", "qa.orders", "UPDATE", "NEW.score <> 0",
                                     4294967295, "DENY"))
    require(maximum.priority == 4294967295 and maximum.effect == "deny", "unsigned priority or DENY lost")
    valid = rule_source("same", "qa.orders", "INSERT", "NEW.score > 0")
    invalid_sets = (
        rule_source("rejected", "qa.orders", "INSERT", "NEW.score > 0", 4294967296),
        rule_source("rejected", "qa.orders", "INSERT", "NEW.score > 0", -1),
        valid + "DROP TABLE qa.orders;",
        valid + rule_source("SAME", "qa.orders", "INSERT", "NEW.score > 1"),
        valid + rule_source("other", "qa.orders", "UPDATE", "NEW.score > 1"),
        valid + rule_source("other", "qa.payments", "INSERT", "NEW.score > 1"),
        rule_source("bad_calc", "qa.orders", "INSERT", "NEW.score > 1", 0,
                    "SET score = NEW.score / 2"),
        rule_source("bad_calc", "qa.orders", "INSERT", "NEW.score > 1", 0,
                    "SET score = NEW.score * 9223372036854775808"),
        rule_source("bad_calc", "qa.orders", "INSERT", "NEW.score > 1", 0,
                    "SET score = NATURAL_FUNCTION(NEW.score)"),
    )
    for source in invalid_sets:
        try:
            provision(parse_rules(source))
        except ValueError:
            continue
        raise RuntimeError(f"recognized an unsupported Decision rule set: {source}")
    print("PASS Decision parser accepts the limited signed BIGINT grammar", flush=True)


def verify_order(run):
    suffix = secrets.token_hex(6)
    table = Table(f"sdp_rank_src_{suffix}", "scores", f"sdp_rank_hist_{suffix}", "id",
                  tuple(Column(column, "bigint") for column in ("id", "score", "risk", "lower_flag")))
    source, journal = table.source, table.journal
    name = f"{table.source_schema}.{table.source_table}"
    run(provision_history(table))
    rules = parse_rules("\n".join((
        rule_source("low_veto", name, "INSERT", "NEW.lower_flag = 1", 10, "DENY"),
        rule_source("zulu_gate", name, "INSERT", "NEW.score > 0", 100),
        rule_source("Alpha_gate", name, "INSERT", "NEW.risk = 1", 100, "DENY"),
        rule_source("top_gate", name, "INSERT", "NEW.score >= 100", 4294967295),
    )))
    require(provision(rules) == provision(reversed(rules)), "trigger depends on input order")
    run(provision(rules))
    run(provision(rules), error="already exists")
    user = f"rank_{suffix}"
    password = secrets.token_hex(32)
    run(f"CREATE USER '{user}'@'localhost' IDENTIFIED BY '{password}' REQUIRE SSL;"
        f"GRANT SELECT, INSERT, UPDATE ON {source} TO '{user}'@'localhost';")

    def write(sql, error=None):
        return run(sql, user=user, password=password, error=error)

    write(f"INSERT INTO {source} VALUES (1,10,0,1);")  # Equal-priority allow beats lower true deny.
    write(f"INSERT INTO {source} VALUES (2,10,1,0);", "decision_denied")  # Alpha precedes Zulu.
    write(f"INSERT INTO {source} VALUES (3,10,NULL,0);", "decision_unknown")
    write(f"INSERT INTO {source} VALUES (4,10,0,NULL);")  # Higher ALLOW stops lower UNKNOWN.
    write(f"INSERT INTO {source} VALUES (5,0,0,0);", "decision_denied")  # All false.
    write(f"INSERT INTO {source} VALUES (6,100,1,1);")  # UINT32_MAX outranks equal-priority guards.
    write(f"INSERT INTO {source} VALUES (7,10,0,0),(8,10,NULL,0);", "decision_unknown")
    run(f"ALTER TABLE {source} ADD CHECK (score <= 100);")
    write(f"INSERT INTO {source} VALUES (9,101,1,1);", "Check constraint")
    require(run(f"SELECT id FROM {source} ORDER BY id;") == [["1"], ["4"], ["6"]]
            and run(f"SELECT head FROM {journal}.revision_counter;") == [["3"]],
            "ranked denial, UNKNOWN or native CHECK published rows or History")

    run(provision(parse_rules("\n".join((
        rule_source("default_update", name, "UPDATE", "NEW.score >= 0"),
        rule_source("stop_zero", name, "UPDATE", "NEW.score = 0", 10, "DENY"),
    )))))
    write(f"UPDATE {source} SET score=0 WHERE id=1;", "decision_denied")
    write(f"UPDATE {source} SET score=2 WHERE id=1;")
    rows = run(f"SELECT event_manipulation, COUNT(*) FROM information_schema.triggers "
               f"WHERE event_object_schema='{table.source_schema}' "
               f"AND event_object_table='{table.source_table}' AND action_timing='BEFORE' "
               "GROUP BY event_manipulation ORDER BY event_manipulation;")
    require(rows == [["INSERT", "1"], ["UPDATE", "1"]]
            and run(f"SELECT head FROM {journal}.revision_counter;") == [["4"]],
            "expected one trigger per event and one successful UPDATE event")

    run(f"ALTER TABLE {source} MODIFY lower_flag VARCHAR(16);")
    write(f"INSERT INTO {source} VALUES (10,100,0,'1');", "decision_dependency_unavailable")
    run(f"ALTER TABLE {source} MODIFY lower_flag BIGINT;")
    print("PASS Decision orders static ALLOW/DENY by descending priority and name byte tie, stops at UNKNOWN", flush=True)
    print("PASS ranked guards preserve native CHECK, table dependencies and one trigger per event", flush=True)
    return table


def verify_calculations(run):
    suffix = secrets.token_hex(6)
    table = Table(f"sdp_calc_src_{suffix}", "numbers", f"sdp_calc_hist_{suffix}", "id",
                  tuple(Column(column, "bigint") for column in ("id", "x", "y")))
    source, journal = table.source, table.journal
    name = f"{table.source_schema}.{table.source_table}"
    run(provision_history(table))
    rules = parse_rules("\n".join((
        rule_source("shadow_x", name, "INSERT", "NEW.x > 0", 1,
                    "SET x = NEW.x * 9223372036854775807"),
        rule_source("compute_y", name, "INSERT", "NEW.y > 0", 80, "SET y = NEW.x * NEW.y"),
        rule_source("compute_x", name, "INSERT", "NEW.x > 0", 90, "SET x = NEW.y + 2"),
        rule_source("stop_large", name, "INSERT", "NEW.y > 20", 100, "DENY"),
        rule_source("allow_small", name, "INSERT", "NEW.x >= 6", 30),
    )))
    run(provision(rules))
    run(f"INSERT INTO {source} VALUES (1,3,4);")
    require(run(f"SELECT x,y FROM {source} WHERE id=1;") == [["6", "12"]],
            "Decision calculations did not use the same proposed NEW image")
    run(f"INSERT INTO {source} VALUES (2,5,5);", error="decision_denied")
    run(f"INSERT INTO {source} VALUES (3,9223372036854775807,2);", error="out of range")
    rows = run(f"SELECT JSON_UNQUOTE(JSON_EXTRACT(after_image,'$.values.x.v')), "
               "JSON_UNQUOTE(JSON_EXTRACT(after_image,'$.values.y.v')) "
               f"FROM {journal}.events;")
    require(rows == [["6", "12"]] and run(f"SELECT id FROM {source};") == [["1"]],
            "failed calculations or guards changed rows or History")

    run(provision(parse_rules("\n".join((
        rule_source("minus", name, "UPDATE", "NEW.x > 0", 50, "SET x = NEW.y - -2"),
        rule_source("fallback", name, "UPDATE", "NEW.x > 0", 0, "SET x = NEW.x + 100"),
    )))))
    run(f"UPDATE {source} SET y=NULL WHERE id=1;")
    require(run(f"SELECT x,y FROM {source} WHERE id=1;") == [["NULL", "NULL"]],
            "a successful NULL calculation did not stop the lower rule")
    run(f"UPDATE {source} SET y=8 WHERE id=1;")
    require(run(f"SELECT x,y FROM {source} WHERE id=1;") == [["NULL", "8"]]
            and run(f"SELECT head FROM {journal}.revision_counter;") == [["3"]],
            "UNKNOWN calculation condition did not skip without denying the row")
    run(f"UPDATE {source} SET x=1 WHERE id=1;")
    require(run(f"SELECT x,y FROM {source} WHERE id=1;") == [["10", "8"]],
            "signed subtraction or calculation-only write failed")
    print("PASS Decision SET uses one proposed image, first TRUE per field, and native overflow/History", flush=True)
    print("PASS Decision guards see computed NEW values and nullable SET remains deterministic", flush=True)


def run_checks(run):
    check_parser()
    suffix = secrets.token_hex(6)
    table = Table(f"sdp_dec_src_{suffix}", "scores", f"sdp_dec_hist_{suffix}", "id",
                  (Column("id", "bigint"), Column("score", "bigint")))
    source = table.source
    journal = table.journal
    name = f"{table.source_schema}.{table.source_table}"
    user = f"decision_{suffix}"
    password = secrets.token_hex(32)
    run(provision_history(table))
    run(f"CREATE USER '{user}'@'localhost' IDENTIFIED BY '{password}' REQUIRE SSL;"
        f"GRANT SELECT, INSERT, UPDATE, DELETE ON {source} TO '{user}'@'localhost';")

    def write(sql, error=None):
        return run(sql, user=user, password=password, error=error)

    insert = parse_rule(rule_source("minimum_score", name, "INSERT", "NEW.score >= 10"))
    run(provision(insert))
    duplicate = parse_rule(rule_source("replacement", name, "INSERT", "NEW.score >= 0"))
    run(provision(duplicate), error="already exists")
    write(f"INSERT INTO {source} VALUES (1,10);")
    write(f"INSERT INTO {source} VALUES (2,1);", "decision_denied")
    write(f"INSERT INTO {source} VALUES (2,NULL);", "decision_unknown")
    write(f"INSERT INTO {source} VALUES (2,12),(3,NULL);", "decision_unknown")
    write(f"INSERT IGNORE INTO {source} VALUES (4,1);", "decision_denied")
    write(f"INSERT INTO {source} (id,score) SELECT 5,2;", "decision_denied")
    require(run(f"SELECT * FROM {source};") == [["1", "10"]]
            and run(f"SELECT head FROM {journal}.revision_counter;") == [["1"]],
            "denied INSERT published data or History")
    print("PASS false and UNKNOWN INSERT rules block data and History, including multirow and IGNORE", flush=True)

    write(f"UPDATE {source} SET score=7 WHERE id=1;")
    update = parse_rule(rule_source("floor", name, "UPDATE", "NEW.score >= 5"))
    run(provision(update))
    write(f"UPDATE {source} SET score=4 WHERE id=1;", "decision_denied")
    write(f"UPDATE {source} SET score=NULL WHERE id=1;", "decision_unknown")
    write(f"UPDATE {source} SET score=5 WHERE id=1;")
    write(f"PREPARE probe FROM 'INSERT INTO {source} VALUES (?, ?)';"
          "SET @sample_id=6, @sample_score=NULL; EXECUTE probe USING @sample_id, @sample_score;",
          "decision_unknown")
    run(f"CREATE PROCEDURE `{table.source_schema}`.add_score(IN p_id BIGINT, IN p_score BIGINT) "
        f"SQL SECURITY INVOKER INSERT INTO {source} VALUES (p_id, p_score);"
        f"GRANT EXECUTE ON PROCEDURE `{table.source_schema}`.add_score TO '{user}'@'localhost';")
    write(f"CALL `{table.source_schema}`.add_score(6, NULL);", "decision_unknown")
    write(f"CALL `{table.source_schema}`.add_score(6, 12);")
    write(f"DROP TRIGGER `{table.source_schema}`.`{insert.trigger_name}`;", "denied")
    require(run(f"SELECT * FROM {source} ORDER BY id;") == [["1", "5"], ["6", "12"]]
            and run(f"SELECT head FROM {journal}.revision_counter;") == [["4"]],
            "guard scope, prepared SQL or routine execution changed protected rows or History")
    print("PASS separate UPDATE, prepared writes, stored-routine writes and trigger permissions", flush=True)

    run(f"ALTER TABLE {source} MODIFY score VARCHAR(32);")
    write(f"INSERT INTO {source} VALUES (7,'99');", "decision_dependency_unavailable")
    write(f"UPDATE {source} SET score='99' WHERE id=1;", "decision_dependency_unavailable")
    require(run(f"SELECT * FROM {source} ORDER BY id;") == [["1", "5"], ["6", "12"]]
            and run(f"SELECT head FROM {journal}.revision_counter;") == [["4"]],
            "a changed type bypassed the Decision guard")
    print("PASS incompatible dependency type stops later writes without new History", flush=True)
    ordered = verify_order(run)
    verify_calculations(run)
    return table, ordered


def check_after_restart(run, setup):
    table, ranked = setup
    run(f"INSERT INTO {table.source} VALUES (8,'99');", error="decision_dependency_unavailable")
    require(run(f"SELECT id FROM {table.source} ORDER BY id;") == [["1"], ["6"]],
            "Decision enforcement changed after restart")
    run(f"INSERT INTO {ranked.source} VALUES (2,10,1,0);", error="decision_denied")
    run(f"INSERT INTO {ranked.source} VALUES (3,10,NULL,0);", error="decision_unknown")
    require(run(f"SELECT id FROM {ranked.source} ORDER BY id;") == [["1"], ["4"], ["6"]],
            "ranked Decision enforcement changed after restart")
    print("PASS Decision guard remains fail-closed after restart", flush=True)
