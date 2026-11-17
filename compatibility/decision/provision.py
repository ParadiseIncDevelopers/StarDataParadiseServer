"""Translate static row-local legacy Decision rules to one MySQL trigger."""

from __future__ import annotations

import argparse
from collections.abc import Iterable
from dataclasses import dataclass
import hashlib
from pathlib import Path
import re


IDENTIFIER = r"[a-z][a-z0-9_]{0,63}"
CREATE_RULE = re.compile(
    rf"\s*CREATE\s+DECISION\s+RULE\s+(?P<name>{IDENTIFIER})"
    rf"\s+ON\s+(?:(?P<schema>{IDENTIFIER})\s*\.\s*)?(?P<table>{IDENTIFIER})"
    rf"\s+FOR\s+(?P<event>INSERT|UPDATE)(?:\s+PRIORITY\s+(?P<priority>[0-9]{{1,10}}))?"
    r"\s+WHEN\s*\(\s*NEW\s*\.\s*"
    rf"(?P<column>{IDENTIFIER})\s*(?P<op><=|>=|<>|!=|=|<|>)\s*"
    r"(?P<value>[+-]?[0-9]{1,19})\s*\)\s+THEN\s+"
    rf"(?:(?P<effect>ALLOW|DENY)|SET\s+(?P<target>{IDENTIFIER})\s*=\s*"
    rf"NEW\s*\.\s*(?P<left>{IDENTIFIER})\s*(?P<arithmetic>[+*-])\s*"
    rf"(?:NEW\s*\.\s*(?P<right_field>{IDENTIFIER})|(?P<right_number>[+-]?[0-9]{{1,19}})))\s*;?\s*",
    re.IGNORECASE | re.ASCII,
)
SYSTEM_SCHEMAS = {"information_schema", "mysql", "performance_schema", "sys"}


def new_field(name: str) -> str:
    return f"NEW.`{name}`"


def bigint_literal(value: int) -> str:
    return f"CAST('{value}' AS SIGNED)"


@dataclass(frozen=True)
class Calculation:
    target: str
    left: str
    op: str
    right: str | int

    def __post_init__(self):
        if any(not re.fullmatch(IDENTIFIER, name) for name in self.columns):
            raise ValueError("invalid Decision calculation column")
        if self.op not in {"+", "-", "*"}:
            raise ValueError("unsupported Decision calculation")
        if not isinstance(self.right, str) and (type(self.right) is not int or not -(1 << 63) <= self.right < (1 << 63)):
            raise ValueError("calculation literal is outside signed BIGINT")

    @property
    def columns(self):
        return (self.target, self.left, self.right) if isinstance(self.right, str) else (self.target, self.left)

    @property
    def sql(self):
        right = new_field(self.right) if isinstance(self.right, str) else bigint_literal(self.right)
        return f"{new_field(self.left)} {self.op} {right}"


@dataclass(frozen=True)
class Rule:
    name: str
    schema: str
    table: str
    event: str
    column: str
    op: str
    value: int
    priority: int = 0
    effect: str = "allow"
    calculation: Calculation | None = None

    def __post_init__(self):
        for name in (self.name, self.schema, self.table, self.column):
            if not re.fullmatch(IDENTIFIER, name):
                raise ValueError(f"expected a lowercase MySQL identifier, got {name!r}")
        if self.schema in SYSTEM_SCHEMAS:
            raise ValueError("a Decision rule must target an application schema")
        if self.event not in {"insert", "update"} or self.op not in {"=", "<>", "!=", "<", "<=", ">", ">="}:
            raise ValueError("unsupported Decision event or comparison")
        if type(self.value) is not int or not -(1 << 63) <= self.value < (1 << 63):
            raise ValueError("comparison literal is outside signed BIGINT")
        if (type(self.priority) is not int or not 0 <= self.priority < (1 << 32)
                or self.effect not in {"allow", "deny", "set"}):
            raise ValueError("unsupported Decision priority or effect")
        if (self.effect == "set") != (self.calculation is not None):
            raise ValueError("Decision SET requires one calculation")

    @property
    def predicate(self):
        return f"{new_field(self.column)} {self.op} {bigint_literal(self.value)}"

    @property
    def trigger_name(self):
        identity = hashlib.sha256(f"{self.table}\0{self.event}".encode("ascii")).hexdigest()[:24]
        return f"sdp_decision_{identity}"


def parse_rule(source: str, default_schema: str | None = None):
    match = CREATE_RULE.fullmatch(source)
    if match is None:
        raise ValueError("expected INSERT or UPDATE [PRIORITY number] with a BIGINT comparison and ALLOW, DENY or SET")
    data = match.groupdict()
    schema = data["schema"] or default_schema
    if schema is None:
        raise ValueError("an unqualified target requires --schema")
    calculation = None
    if data["target"] is not None:
        right = data["right_field"]
        calculation = Calculation(data["target"].lower(), data["left"].lower(), data["arithmetic"],
                                  right.lower() if right is not None else int(data["right_number"]))
    return Rule(data["name"].lower(), schema.lower(), data["table"].lower(), data["event"].lower(),
                data["column"].lower(), data["op"], int(data["value"]),
                int(data["priority"] or 0), (data["effect"] or "set").lower(), calculation)


def parse_rules(source: str, default_schema: str | None = None):
    # This grammar permits neither quoted strings nor comments containing delimiters.
    statements = source.strip().removesuffix(";").split(";")
    return tuple(parse_rule(statement, default_schema) for statement in statements)


def guard_check(rule: Rule) -> str:
    action = ("LEAVE evaluate_rules;" if rule.effect == "allow"
              else "SIGNAL SQLSTATE '45000' SET MESSAGE_TEXT = 'decision_denied';")
    return f"""    IF {new_field(rule.column)} IS NULL THEN
        SIGNAL SQLSTATE '45000' SET MESSAGE_TEXT = 'decision_unknown';
    END IF;
    IF {rule.predicate} THEN
        {action}
    END IF;"""


def calculations_sql(rules: list[Rule]):
    calculations = [rule for rule in rules if rule.calculation is not None]
    targets = {name: index for index, name in enumerate(sorted({rule.calculation.target for rule in calculations}))}
    declarations = "\n".join(
        f"    DECLARE value_{index} BIGINT;\n    DECLARE chosen_{index} BOOLEAN DEFAULT FALSE;"
        for index in targets.values()
    )
    candidates = []
    for rule in calculations:
        index = targets[rule.calculation.target]
        candidates.append(f"""    IF NOT chosen_{index} AND ({rule.predicate}) THEN
        SET value_{index} = {rule.calculation.sql};
        SET chosen_{index} = TRUE;
    END IF;""")
    # All candidates above read the proposed NEW values. Assign them only after that pass.
    updates = [
        f"    IF chosen_{index} THEN SET {new_field(name)} = value_{index}; END IF;"
        for name, index in targets.items()
    ]
    return declarations, "\n".join(candidates + updates)


def provision(rule: Rule | Iterable[Rule]):
    """Create one fail-closed ordered guard trigger for a single table and event."""
    rules = (rule,) if isinstance(rule, Rule) else tuple(rule)
    if not rules:
        raise ValueError("expected at least one Decision rule")
    rule = rules[0]
    if any((item.schema, item.table, item.event) != (rule.schema, rule.table, rule.event) for item in rules):
        raise ValueError("a Decision rule set must target one table and event")
    if len({item.name for item in rules}) != len(rules):
        raise ValueError("duplicate Decision rule name")
    target = f"`{rule.schema}`.`{rule.table}`"
    columns = {item.column for item in rules}
    for item in rules:
        if item.calculation is not None:
            columns.update(item.calculation.columns)
    column_names = ", ".join(f"'{column}'" for column in sorted(columns))
    ordered = sorted(rules, key=lambda item: (-item.priority, item.name.encode("ascii")))
    declarations, calculations = calculations_sql(ordered)
    guards = [guard_check(item) for item in ordered if item.calculation is None]
    if guards:
        guards.append("    SIGNAL SQLSTATE '45000' SET MESSAGE_TEXT = 'decision_denied';")
    rules_sql = "\n".join(filter(None, (calculations, *guards)))
    return f"""DELIMITER $$
CREATE TRIGGER `{rule.schema}`.`{rule.trigger_name}`
BEFORE {rule.event.upper()} ON {target}
FOR EACH ROW
evaluate_rules: BEGIN
{declarations}
    IF (
        SELECT COUNT(*) FROM information_schema.tables AS target_table
        JOIN information_schema.columns AS target_column
          ON target_column.table_schema = target_table.table_schema
         AND target_column.table_name = target_table.table_name
        WHERE target_table.table_schema = '{rule.schema}' AND target_table.table_name = '{rule.table}'
          AND target_table.engine = 'InnoDB' AND target_column.column_name IN ({column_names})
          AND target_column.data_type = 'bigint' AND target_column.column_type NOT LIKE '%unsigned%'
          AND target_column.extra NOT LIKE '%GENERATED%'
    ) <> {len(columns)} THEN
        SIGNAL SQLSTATE '45000' SET MESSAGE_TEXT = 'decision_dependency_unavailable';
    END IF;
{rules_sql}
END evaluate_rules$$
DELIMITER ;
"""


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("rule", type=Path, help="UTF-8 file with CREATE DECISION RULE statements for one table and event")
    parser.add_argument("--schema", help="default MySQL schema for an unqualified table")
    args = parser.parse_args()
    try:
        rules = parse_rules(args.rule.read_text(encoding="utf-8-sig"), args.schema)
        sql = provision(rules)
    except (OSError, ValueError) as error:
        parser.error(str(error))
    print(sql, end="")


if __name__ == "__main__":
    main()
