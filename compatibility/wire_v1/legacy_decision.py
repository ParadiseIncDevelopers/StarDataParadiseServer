"""Validate the supported legacy Decision grammar against caller-visible MySQL metadata."""

from pathlib import Path
import sys

from sql import Fault, require

# service.py can run directly from a working directory outside the source tree.
sys.path.insert(1, str(Path(__file__).resolve().parents[2]))
from compatibility.decision.provision import parse_rule


def validate(session, body, operators):
    require(operators, 1004)
    require(session.result is None and not session.db.in_transaction, 1013)
    require(session.identity[1] in operators, 1006)
    source = body.get("sql")
    require(body.keys() == {"sql"} and isinstance(source, str) and 0 < len(source) <= 64 * 1024)
    try:
        rule = parse_rule(source, session.config.schema)
    except ValueError:
        raise Fault(1012) from None

    referenced = {rule.column}
    if rule.calculation is not None:
        referenced.update(rule.calculation.columns)
    with session.db.cursor(buffered=True) as cursor:
        cursor.execute("""
            SELECT c.column_name
            FROM information_schema.tables AS t
            JOIN information_schema.columns AS c
              ON c.table_schema = t.table_schema AND c.table_name = t.table_name
            WHERE BINARY t.table_schema = %s AND BINARY t.table_name = %s
              AND t.table_type = 'BASE TABLE' AND t.engine = 'InnoDB'
              AND c.data_type = 'bigint' AND c.column_type NOT LIKE '%unsigned%'
              AND c.extra NOT LIKE '%GENERATED%'
        """, (rule.schema, rule.table))
        eligible = {column.lower() for column, in cursor.fetchall()}
    require(referenced <= eligible)
    return {"valid": True}
