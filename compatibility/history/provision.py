"""Emit MySQL DDL for a fresh InnoDB table and its protected History journal."""

from __future__ import annotations

import argparse
from dataclasses import dataclass
import re


IDENTIFIER = re.compile(r"[a-z][a-z0-9_]{0,63}\Z")
SYSTEM_SCHEMAS = {"information_schema", "mysql", "performance_schema", "sys"}
COLUMN_TYPES = {"bigint": "BIGINT", "text": "LONGTEXT", "blob": "LONGBLOB"}


def identifier(name: str) -> str:
    if not IDENTIFIER.fullmatch(name):
        raise ValueError(f"expected a lowercase MySQL identifier, got {name!r}")
    return f"`{name}`"


@dataclass(frozen=True)
class Column:
    name: str
    kind: str

    def __post_init__(self):
        identifier(self.name)
        if self.kind not in COLUMN_TYPES:
            raise ValueError(f"unsupported History column kind {self.kind!r}")

    def image(self, alias: str) -> str:
        field = f"{alias}.{identifier(self.name)}"
        if self.kind == "bigint":
            value = f"JSON_OBJECT('t', 'i64', 'v', CAST({field} AS CHAR CHARACTER SET ascii))"
        elif self.kind == "text":
            value = f"JSON_OBJECT('t', 'text', 'v', {field})"
        else:
            value = (
                f"JSON_OBJECT('type', 'BLOB', 'scope', 'value', 'size', OCTET_LENGTH({field}), "
                f"'sha256', SHA2({field}, 256))"
            )
        return f"'{self.name}', IF({field} IS NULL, JSON_OBJECT('t', 'null'), {value})"


@dataclass(frozen=True)
class Table:
    source_schema: str
    source_table: str
    journal_schema: str
    key: str
    columns: tuple[Column, ...]

    def __post_init__(self):
        for name in (self.source_schema, self.source_table, self.journal_schema, self.key):
            identifier(name)
        if self.source_schema == self.journal_schema or {self.source_schema, self.journal_schema} & SYSTEM_SCHEMAS:
            raise ValueError("source and journal must be distinct, non-system schemas")
        names = {column.name for column in self.columns}
        if len(names) != len(self.columns) or Column(self.key, "bigint") not in self.columns:
            raise ValueError("columns must have unique names and include the BIGINT primary key")

    @property
    def source(self):
        return f"{identifier(self.source_schema)}.{identifier(self.source_table)}"

    @property
    def journal(self):
        return identifier(self.journal_schema)

    def row_image(self, alias):
        values = ",\n            ".join(column.image(alias) for column in self.columns)
        return f"JSON_OBJECT('schema_version', 1, 'values', JSON_OBJECT(\n            {values}))"


def trigger(table: Table, operation: str) -> str:
    source_schema = identifier(table.source_schema)
    key = identifier(table.key)
    journal = table.journal
    before = "NULL" if operation == "insert" else table.row_image("OLD")
    after = "NULL" if operation == "delete" else table.row_image("NEW")
    if operation == "insert":
        identity = f"""    INSERT INTO {journal}.row_identity (source_key) VALUES (NEW.{key});
    SET tracked_row = LAST_INSERT_ID();"""
    else:
        identity = f"""    SELECT row_id INTO tracked_row FROM {journal}.row_identity
        WHERE source_key = OLD.{key} FOR UPDATE;
    IF tracked_row IS NULL THEN
        SIGNAL SQLSTATE '45000' SET MESSAGE_TEXT = 'History row identity is missing';
    END IF;"""
        if operation == "update":
            identity += f"""
    UPDATE {journal}.row_identity SET source_key = NEW.{key} WHERE row_id = tracked_row;"""
        else:
            identity += f"""
    DELETE FROM {journal}.row_identity WHERE row_id = tracked_row;"""
    return f"""CREATE TRIGGER {source_schema}.sdp_history_{operation}
AFTER {operation.upper()} ON {table.source}
FOR EACH ROW
BEGIN
    DECLARE tracked_row BIGINT UNSIGNED DEFAULT NULL;
    DECLARE event_id BIGINT UNSIGNED;
{identity}
    UPDATE {journal}.revision_counter SET head = head + 1 WHERE singleton = 1;
    SELECT head INTO event_id FROM {journal}.revision_counter WHERE singleton = 1;
    INSERT INTO {journal}.events
        (revision, row_id, actor, observed_utc, before_image, after_image)
    VALUES (event_id, tracked_row, USER(), UNIX_TIMESTAMP(), {before}, {after});
END$$
"""


def read_routines(journal: str) -> str:
    return f"""CREATE PROCEDURE {journal}.history_log(
    IN from_revision BIGINT UNSIGNED, IN to_revision BIGINT UNSIGNED,
    IN target_row BIGINT UNSIGNED, IN since_utc BIGINT, IN until_utc BIGINT,
    IN page_offset INT UNSIGNED, IN page_limit INT UNSIGNED)
SQL SECURITY INVOKER READS SQL DATA
BEGIN
    DECLARE latest_revision BIGINT UNSIGNED;
    SELECT head INTO latest_revision FROM {journal}.revision_counter WHERE singleton = 1;
    IF page_offset IS NULL OR page_limit IS NULL OR page_limit NOT BETWEEN 1 AND 256
       OR from_revision > to_revision OR since_utc > until_utc
       OR from_revision > latest_revision OR to_revision > latest_revision THEN
        SIGNAL SQLSTATE '22023' SET MESSAGE_TEXT = 'Invalid History log range or page';
    END IF;
    SELECT revision, revision - 1 AS parent, row_id, actor, observed_utc, before_image, after_image
    FROM {journal}.events
    WHERE (from_revision IS NULL OR revision >= from_revision)
      AND revision <= COALESCE(to_revision, latest_revision)
      AND (target_row IS NULL OR row_id = target_row)
      AND (since_utc IS NULL OR observed_utc >= since_utc)
      AND (until_utc IS NULL OR observed_utc <= until_utc)
    ORDER BY revision LIMIT page_limit OFFSET page_offset;
END$$

CREATE PROCEDURE {journal}.history_read(
    IN at_revision BIGINT UNSIGNED, IN target_row BIGINT UNSIGNED,
    IN page_offset INT UNSIGNED, IN page_limit INT UNSIGNED)
SQL SECURITY INVOKER READS SQL DATA
BEGIN
    IF at_revision IS NULL OR at_revision > (SELECT head FROM {journal}.revision_counter WHERE singleton = 1)
       OR page_offset IS NULL OR page_limit IS NULL OR page_limit NOT BETWEEN 1 AND 256 THEN
        SIGNAL SQLSTATE '22023' SET MESSAGE_TEXT = 'Invalid History revision or page';
    END IF;
    WITH ranked AS (
        SELECT row_id, after_image, ROW_NUMBER() OVER (PARTITION BY row_id ORDER BY revision DESC) AS rank_in_row
        FROM {journal}.events
        WHERE revision <= at_revision AND (target_row IS NULL OR row_id = target_row)
    )
    SELECT row_id, after_image AS image
    FROM ranked WHERE rank_in_row = 1 AND after_image IS NOT NULL
    ORDER BY row_id LIMIT page_limit OFFSET page_offset;
END$$

CREATE PROCEDURE {journal}.history_diff(
    IN from_revision BIGINT UNSIGNED, IN to_revision BIGINT UNSIGNED,
    IN target_row BIGINT UNSIGNED, IN page_offset INT UNSIGNED, IN page_limit INT UNSIGNED)
SQL SECURITY INVOKER READS SQL DATA
BEGIN
    IF from_revision IS NULL OR to_revision IS NULL OR from_revision >= to_revision
       OR to_revision > (SELECT head FROM {journal}.revision_counter WHERE singleton = 1)
       OR page_offset IS NULL OR page_limit IS NULL OR page_limit NOT BETWEEN 1 AND 256 THEN
        SIGNAL SQLSTATE '22023' SET MESSAGE_TEXT = 'Invalid History revision range or page';
    END IF;
    WITH earlier AS (
        SELECT row_id, after_image, ROW_NUMBER() OVER (PARTITION BY row_id ORDER BY revision DESC) AS rank_in_row
        FROM {journal}.events
        WHERE revision <= from_revision AND (target_row IS NULL OR row_id = target_row)
    ), later AS (
        SELECT row_id, after_image, ROW_NUMBER() OVER (PARTITION BY row_id ORDER BY revision DESC) AS rank_in_row
        FROM {journal}.events
        WHERE revision <= to_revision AND (target_row IS NULL OR row_id = target_row)
    )
    SELECT later.row_id, earlier.after_image AS before_image, later.after_image
    FROM later LEFT JOIN earlier ON earlier.row_id = later.row_id AND earlier.rank_in_row = 1
    WHERE later.rank_in_row = 1 AND NOT (earlier.after_image <=> later.after_image)
    ORDER BY later.row_id LIMIT page_limit OFFSET page_offset;
END$$
"""


def provision(table: Table) -> str:
    """Create an empty source schema and a separate, initially ungranted journal."""
    journal = table.journal
    key = identifier(table.key)
    definitions = [
        f"    {identifier(column.name)} {COLUMN_TYPES[column.kind]}"
        + (" NOT NULL" if column.name == table.key else "")
        for column in table.columns
    ]
    definitions.append(f"    PRIMARY KEY ({key})")
    columns_ddl = ",\n".join(definitions)
    triggers = "\n".join(trigger(table, operation) for operation in ("insert", "update", "delete"))
    return f"""CREATE SCHEMA {identifier(table.source_schema)} DEFAULT CHARACTER SET utf8mb4;
CREATE SCHEMA {journal} DEFAULT CHARACTER SET utf8mb4;
CREATE TABLE {table.source} (
{columns_ddl}
) ENGINE=InnoDB;

CREATE TABLE {journal}.row_identity (
    row_id BIGINT UNSIGNED NOT NULL AUTO_INCREMENT PRIMARY KEY,
    source_key BIGINT NOT NULL UNIQUE
) ENGINE=InnoDB;

CREATE TABLE {journal}.revision_counter (
    singleton TINYINT UNSIGNED NOT NULL PRIMARY KEY CHECK (singleton = 1),
    head BIGINT UNSIGNED NOT NULL
) ENGINE=InnoDB;
INSERT INTO {journal}.revision_counter VALUES (1, 0);

CREATE TABLE {journal}.events (
    revision BIGINT UNSIGNED NOT NULL PRIMARY KEY,
    row_id BIGINT UNSIGNED NOT NULL,
    actor VARCHAR(288) NOT NULL,
    observed_utc BIGINT NOT NULL,
    before_image JSON,
    after_image JSON,
    KEY by_row (row_id, revision),
    CHECK (before_image IS NOT NULL OR after_image IS NOT NULL)
) ENGINE=InnoDB;

DELIMITER $$
{triggers}
{read_routines(journal)}DELIMITER ;
"""


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--source-schema", required=True)
    parser.add_argument("--table", required=True)
    parser.add_argument("--journal-schema", required=True)
    parser.add_argument("--key", required=True)
    parser.add_argument("--column", metavar="NAME:TYPE", action="append", required=True)
    parser.add_argument("--restore", action="store_true",
                        help="add live-row restore and field revert for BIGINT/TEXT tables")
    args = parser.parse_args()
    try:
        columns = tuple(Column(*value.split(":", 1)) for value in args.column)
        table = Table(args.source_schema, args.table, args.journal_schema, args.key, columns)
        sql = provision(table)
        if args.restore:
            if __package__:
                from .restore import provision_restore
            else:
                from restore import provision_restore
            sql += provision_restore(table)
    except (TypeError, ValueError) as error:
        parser.error(str(error))
    print(sql, end="")


if __name__ == "__main__":
    main()
