"""Verify the Decision validation RPC and its authorization on a disposable wire bridge."""

import argparse
from pathlib import Path
import ssl
import sys

from acceptance import Client, connection, require


def candidate(table, expression="NEW.id >= 0", effect="ALLOW"):
    return f"CREATE DECISION RULE test_guard ON {table} FOR INSERT WHEN ({expression}) THEN {effect}"


def verify(port, tls, operator, reader):
    # Fixture tables expose an InnoDB signed BIGINT, an unsigned BIGINT and a MyISAM table.
    with connection(port, tls) as operator_socket, connection(port, tls) as reader_socket:
        owner = Client(operator_socket, *operator)
        visitor = Client(reader_socket, *reader)
        sql = candidate("durable_sample")
        require(owner.call("decision_validate", sql=sql) == {"valid": True}, "Decision guard validation differs")
        calculation = candidate("durable_sample", effect="SET id = NEW.id + 1")
        require(owner.call("decision_validate", sql=calculation) == {"valid": True},
                "Decision calculation validation differs")
        owner.error("decision_validate", 1012, sql="SELECT 1")
        owner.error("decision_validate", 1012, sql=sql, unexpected=True)
        owner.error("decision_validate", 1012, sql=candidate("durable_sample", "NEW.missing > 0"))
        owner.error("decision_validate", 1012, sql=candidate("durable_sample", effect="SET missing = NEW.id + 1"))
        owner.error("decision_validate", 1012, sql=candidate("unsigned_sample"))
        owner.error("decision_validate", 1012, sql=candidate("nontransactional_direct"))
        owner.call("tx_begin")
        owner.error("decision_validate", 1013, sql=sql)
        owner.call("tx_rollback")
        visitor.error("decision_validate", 1006, sql=sql)
        owner.error("decision_create", 1004, sql=sql, request_token="1" * 32)
        require(not owner.call("capabilities")["features"]["decision_v1"], "advertised broader Decision support")
    print("PASS Decision grammar and native dependency validation, no creation and exact operator gating", flush=True)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--port", type=int, required=True)
    parser.add_argument("--ca", type=Path, required=True)
    parser.add_argument("--operator", required=True)
    parser.add_argument("--reader", required=True)
    args = parser.parse_args()
    passwords = [sys.stdin.readline().rstrip("\r\n") for _ in range(2)]
    verify(args.port, ssl.create_default_context(cafile=str(args.ca)),
           (args.operator, passwords[0]), (args.reader, passwords[1]))


if __name__ == "__main__":
    main()
