"""Exercise persisted PL/SQL routines against a real, temporary TLS Server."""

import argparse
import json
from pathlib import Path
import secrets
import ssl
import tempfile

from service_smoke import certificates, execute, files_in, listening, require, save_config
from sql_rpc import BIGINT, INVALID_ARGUMENT, NULL, client, i64, write


RESOURCE_LIMIT = 1014
QUANTITY_SQL = "SELECT i.quantity FROM items AS i WHERE i.id = :p_key"
PROCEDURE = """CREATE PROCEDURE accumulate(p_count IN BIGINT, total IN OUT BIGINT, changes OUT BIGINT) AS
BEGIN
    changes := 0;
    FOR i IN 1..p_count LOOP
        CONTINUE WHEN i = 2;
        total := total + i;
        changes := changes + 1;
    END LOOP;
    IF p_count < 0 THEN changes := -1; END IF;
END;"""


def definition(replace=False):
    verb = "CREATE OR REPLACE" if replace else "CREATE"
    adjustment = " + 1" if replace else ""
    return (
        f"{verb} FUNCTION quantity_for(p_key IN BIGINT) RETURN BIGINT AS\n"
        f"BEGIN\n    RETURN ({QUANTITY_SQL}){adjustment};\nEND;"
    )


def read_quantity(connection, key):
    rows, _ = connection.query(
        "SELECT quantity_for(:key)", types={"key": BIGINT}, values={"key": i64(key)},
    )
    require(len(rows) == 1 and len(rows[0]) == 1, "function did not return a scalar value")
    return rows[0][0]


def object_named(metadata, name):
    qualified_name = ["main", "public", name]
    found = [obj for obj in metadata["objects"] if obj["qualified_name"] == qualified_name]
    require(len(found) == 1, "routine fixture did not yield one catalog object")
    return found[0]


def procedure_out(connection):
    types = {"count": BIGINT, "initial": BIGINT}
    values = {"count": i64(4), "initial": i64(10)}
    with connection.prepared("CALL accumulate(:count,:initial,NULL)", types) as statement:
        result = connection.call("execute", statement_id=statement, parameters=values)
        require(result["columns"] == [{"name": "total", "type": "BIGINT"}, {"name": "changes", "type": "BIGINT"}],
                "procedure OUT metadata did not preserve names and types")
        rows, _ = connection.collect(result, 1, 1)
        require(rows == [[i64(18), i64(3)]], "procedure arguments, loop or OUT values changed")


def failure_limits(connection):
    write(connection, """CREATE FUNCTION checked_add(p IN BIGINT) RETURN BIGINT AS
BEGIN
    RETURN p + 1;
EXCEPTION WHEN OTHERS THEN
    RETURN -5;
END;""", 0)
    rows, _ = connection.query("SELECT checked_add(4)")
    require(rows == [[i64(5)]], "ordinary BIGINT routine arithmetic changed")
    rows, _ = connection.query("SELECT checked_add(9223372036854775807)")
    require(rows == [[i64(-5)]], "overflow did not reach the routine exception handler")

    write(connection, "CREATE PROCEDURE spin() AS BEGIN LOOP END LOOP; END;", 0)
    with connection.prepared("CALL spin()") as statement:
        connection.error("execute", RESOURCE_LIMIT, statement_id=statement)
    require(connection.call("health")["status"] == "ready", "instruction limit stopped the session")

    write(connection, """CREATE FUNCTION recur_forever(p IN BIGINT) RETURN BIGINT AS
BEGIN
    RETURN recur_forever(p);
END;""", 0)
    with connection.prepared("SELECT recur_forever(1)") as statement:
        connection.error("execute", RESOURCE_LIMIT, statement_id=statement)
    require(connection.call("health")["status"] == "ready", "call depth limit stopped the session")


def create_and_replace(connection):
    write(connection, "CREATE TABLE items (id BIGINT PRIMARY KEY, quantity BIGINT)", 0)
    write(connection, "INSERT INTO items VALUES (1,7),(2,9)", 2)
    write(connection, definition(), 0)
    write(connection, PROCEDURE, 0)
    base = object_named(connection.call("metadata"), "quantity_for")
    require(base["schema_version"] == 1, "new routine revision did not start at one")
    require(read_quantity(connection, 1) == i64(7), "named routine argument did not bind to SQL")
    require(read_quantity(connection, 404) == NULL, "empty scalar SQL did not yield a typed NULL")
    procedure_out(connection)
    failure_limits(connection)

    connection.call("tx_begin")
    connection.mutate("UPDATE items SET quantity = 40 WHERE id = 1")
    require(read_quantity(connection, 1) == i64(40), "routine could not see its caller's uncommitted row")
    procedure_out(connection)
    connection.call("tx_rollback")
    require(read_quantity(connection, 1) == i64(7), "routine execution committed its caller's update")

    write(connection, "ALTER TABLE items ADD COLUMN note TEXT", 0)
    connection.error("query.prepare", INVALID_ARGUMENT, sql="SELECT quantity_for(1)")
    write(connection, definition(replace=True), 0)
    current = object_named(connection.call("metadata"), "quantity_for")
    require(current["id"] == base["id"] and current["schema_version"] == 2,
            "replacing a stale routine lost its object ID or revision")
    require(read_quantity(connection, 1) == i64(8), "replacement code did not use the new routine revision")
    return int(current["id"])


def inspect_ir(server, config, routine_id):
    catalog = execute(server, config, "--catalog-info")
    require(catalog.returncode == 0, "offline catalog inspection failed")
    metadata = json.loads(catalog.stdout)
    stored = object_named(metadata, "quantity_for")
    table = object_named(metadata, "items")
    require(stored["id"] == routine_id and stored["schema_version"] == 2 and stored["kind"] == "routine",
            "offline catalog did not retain the same routine revision")
    record = json.loads(stored["definition"])
    source = definition(replace=True)
    require(record["format"] == 1 and record["sql"] == source, "persistent routine source changed")
    require([table["id"], table["schema_version"]] in record["dependencies"],
            "recompiled routine lost its new table schema dependency")
    ir = record["ir"]
    require(ir["ir_version"] == 1 and ir["revision"] == "2" and ir["parameters"] == 1
            and ir["returns"] == {"name": "BIGINT"} and ir["uses_sql"],
            "stored typed IR header, return type or revision is incorrect")
    require(ir["slots"][0]["name"] == "p_key" and ir["slots"][0]["type"] == {"name": "BIGINT"},
            "stored IR lost the lexical slot name or type")
    instruction = ir["body"][0]
    source_bytes = record["sql"].encode("utf-8")
    require(instruction["op"] == "return" and instruction["at"] == [source_bytes.index(b"    RETURN") + 4, 3, 5],
            "stored RETURN instruction lost its exact source position")
    expression = instruction["first"]
    nested = expression["args"][0]
    require(expression["op"] == "add" and nested["op"] == "subquery"
            and nested["sql"] == QUANTITY_SQL and nested["type"] == {"name": "BIGINT"}
            and nested["bindings"] == [{"name": "p_key", "slot": 0, "type": {"name": "BIGINT"}}],
            "typed SQL IR or lexical binding did not survive serialization")
    return record


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--server", type=Path, required=True)
    parser.add_argument("--openssl", type=Path, required=True)
    parser.add_argument("--scratch", type=Path, required=True)
    args = parser.parse_args()
    server = str(args.server.resolve(strict=True))
    openssl = str(args.openssl.resolve(strict=True))
    args.scratch.mkdir(parents=True, exist_ok=True)
    with tempfile.TemporaryDirectory(prefix="s16-", dir=args.scratch) as directory:
        fixture = Path(directory)
        for name in ("data", "log", "temp"):
            (fixture / name).mkdir()
        certificates(openssl, fixture)
        config = save_config(fixture, timeout_ms=15_000, max_connections=4)
        require(execute(server, config, "--initialize-storage").returncode == 0, "routine storage setup failed")
        password = secrets.token_urlsafe(32)
        password_input = ((password + "\n") * 2).encode("ascii")
        require(execute(server, config, "--bootstrap-admin", "local-admin", stdin=password_input).returncode == 0,
                "routine administrator setup failed")
        context = ssl.create_default_context(cafile=str(fixture / "ca.pem"))

        with listening(server, config) as (_, port):
            with client(port, context, password) as connection:
                routine_id = create_and_replace(connection)
        before = files_in(fixture / "data")
        ir_record = inspect_ir(server, config, routine_id)

        with listening(server, config) as (_, port):
            with client(port, context, password) as reopened:
                require(read_quantity(reopened, 1) == i64(8) and read_quantity(reopened, 2) == i64(10),
                        "restarted routine executed with stale source or data")
                procedure_out(reopened)
        require(inspect_ir(server, config, routine_id) == ir_record, "reopening the routine changed persisted IR")
        require(execute(server, config, "--check-storage").returncode == 0, "routine fixture failed WAL/storage check")
        require(files_in(fixture / "data") == before, "read-only routine calls changed data or WAL after restart")

    print("PL/SQL TLS function, OUT values, caller rollback, limits, dependency invalidation and stored typed IR passed")


if __name__ == "__main__":
    main()
