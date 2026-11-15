"""Verify trigger execution, statement rollback and cursor cardinality over real loopback TLS."""

import argparse
from pathlib import Path
import secrets
import ssl
import tempfile

from service_smoke import certificates, connection, exchange, execute, files_in, listening, require, save_config
from sql_rpc import Client, INVALID_ARGUMENT, NULL, client, i64, text, token, write


PERMISSION_DENIED = 1006
RESOURCE_LIMIT = 1014
BEFORE_INSERT = """CREATE TRIGGER trace_before_insert BEFORE INSERT ON items FOR EACH ROW AS
    seen BIGINT;
BEGIN
    SELECT COUNT(*) INTO seen FROM items WHERE id = :NEW.id;
    INSERT INTO audit_log VALUES ('insert_before', NULL, :NEW.id, :seen);
END;"""
AFTER_INSERT = """CREATE TRIGGER trace_after_insert AFTER INSERT ON items FOR EACH ROW AS
    seen BIGINT;
BEGIN
    SELECT COUNT(*) INTO seen FROM items WHERE id = :NEW.id;
    INSERT INTO audit_log VALUES ('insert_after', NULL, :NEW.id, :seen);
    IF NEW.quantity < 0 THEN RAISE blocked_quantity; END IF;
END;"""
BEFORE_UPDATE = """CREATE TRIGGER trace_before_update BEFORE UPDATE ON items FOR EACH ROW AS
    seen BIGINT;
BEGIN
    SELECT quantity INTO seen FROM items WHERE id = :OLD.id;
    INSERT INTO audit_log VALUES ('update_before', :OLD.id, :NEW.id, :seen);
END;"""
AFTER_DELETE = """CREATE TRIGGER trace_after_delete AFTER DELETE ON items FOR EACH ROW AS
    seen BIGINT;
BEGIN
    SELECT COUNT(*) INTO seen FROM items WHERE id = :OLD.id;
    INSERT INTO audit_log VALUES ('delete_after', :OLD.id, NULL, :seen);
END;"""
AUDIT_RELAY = """CREATE TRIGGER audit_relay AFTER INSERT ON audit_log FOR EACH ROW
BEGIN
    INSERT INTO shadow_log VALUES (:NEW.phase, :NEW.seen);
END;"""
RECURSIVE_TRIGGER = """CREATE TRIGGER recursion_guard AFTER INSERT ON recursions FOR EACH ROW
BEGIN
    INSERT INTO recursions VALUES (:NEW.number_value);
END;"""
SINGLE_MATCH = """CREATE FUNCTION single_match(p_key IN BIGINT) RETURN BIGINT AS
    result BIGINT;
BEGIN
    SELECT id INTO result FROM matching_rows WHERE id >= :p_key;
    RETURN result;
END;"""
PROTECTED_LOOKUP = """CREATE PROCEDURE protected_lookup(p_key IN BIGINT, found_id OUT BIGINT) AS
BEGIN
    SELECT id INTO found_id FROM matching_rows WHERE id = :p_key;
END;"""
AUDIT_ROWS = [
    [text("delete_after"), i64(2), NULL, i64(0)],
    [text("insert_after"), NULL, i64(1), i64(1)],
    [text("insert_before"), NULL, i64(1), i64(0)],
    [text("update_before"), i64(1), i64(2), i64(10)],
]


def rows(connection, sql):
    return connection.query(sql)[0]


def audit_is_intact(connection):
    require(rows(connection, "SELECT phase, old_id, new_id, seen FROM audit_log ORDER BY phase") == AUDIT_ROWS,
            "trigger timing, row image or committed audit data changed")
    require(rows(connection, "SELECT phase, seen FROM shadow_log ORDER BY phase")
            == [[row[0], row[3]] for row in AUDIT_ROWS], "nested trigger effects changed")
    require(rows(connection, "SELECT * FROM items") == [], "main relation kept a deleted or rejected row")


def install_fixture(connection):
    for sql in (
        "CREATE TABLE items (id BIGINT PRIMARY KEY, quantity BIGINT NOT NULL)",
        "CREATE TABLE audit_log (phase TEXT, old_id BIGINT, new_id BIGINT, seen BIGINT)",
        "CREATE TABLE shadow_log (phase TEXT, seen BIGINT)",
        "CREATE TABLE recursions (number_value BIGINT)",
        "CREATE TABLE matching_rows (id BIGINT)",
        BEFORE_INSERT, AFTER_INSERT, BEFORE_UPDATE, AFTER_DELETE, AUDIT_RELAY, RECURSIVE_TRIGGER, SINGLE_MATCH,
    ):
        write(connection, sql, 0)

    write(connection, "INSERT INTO items VALUES (1,10)", 1)
    write(connection, "UPDATE items SET id=2, quantity=20 WHERE id=1", 1)
    require(rows(connection, "SELECT id, quantity FROM items") == [[i64(2), i64(20)]],
            "updates with triggers did not persist their new row image")
    write(connection, "DELETE FROM items WHERE id=2", 1)
    write(connection, "INSERT INTO matching_rows VALUES (1),(2)", 2)
    audit_is_intact(connection)


class RoutineReader(Client):
    def __init__(self, sock, password):
        self.sock = sock
        self.id = 2
        require(exchange(sock, 1, "hello")["ok"], "routine reader hello failed")
        require(exchange(sock, 2, "auth", {"username": "routine_reader", "password": password})["ok"],
                "limited routine reader authentication failed")


def test_invoker_permissions(port, context, administrator):
    password = secrets.token_urlsafe(32)
    with client(port, context, administrator) as owner:
        write(owner, PROTECTED_LOOKUP, 0)
        write(owner, "CREATE USER routine_reader", 0)
        owner.call("password_set", username="routine_reader", new_password=password, request_token=token())

        with connection(port, context) as sock:
            reader = RoutineReader(sock, password)
            call = "CALL protected_lookup(2,NULL)"
            reader.error("query.prepare", PERMISSION_DENIED, sql=call)
            write(owner, "GRANT EXECUTE ON ROUTINE protected_lookup TO USER routine_reader", 0)
            with reader.prepared(call) as statement:
                reader.error("execute", PERMISSION_DENIED, statement_id=statement)
                write(owner, "GRANT SELECT ON TABLE matching_rows TO USER routine_reader", 0)
                result = reader.call("execute", statement_id=statement)
                require(result["columns"] == [{"name": "found_id", "type": "BIGINT"}],
                        "authorized invoker lost the OUT type")
                returned, _ = reader.collect(result, 1, 1)
                require(returned == [[i64(2)]], "granted routine could not read its typed OUT value")


def execute_error(connection, sql, code, *, exception=None, request_token=None):
    with connection.prepared(sql) as statement_id:
        fields = {"statement_id": statement_id}
        if request_token is not None:
            fields["request_token"] = request_token
        response = connection.raw("execute", **fields)
        error = response.get("error", {})
        require(not response["ok"] and error.get("code") == code,
                f"expected execution error {code}, got {error.get('code')}")
        if exception is not None:
            require(error.get("pl_exception") == exception,
                    f"expected named PL/SQL exception {exception}, got {error.get('pl_exception')}")


def test_failed_mutations(connection):
    failed = token()
    execute_error(connection, "INSERT INTO items VALUES (3,-1)", INVALID_ARGUMENT,
                  exception="blocked_quantity", request_token=failed)
    require(connection.call("tx_outcome", request_token=failed) == {"status": "unknown"},
            "rejected trigger mutation produced a committed outcome")
    audit_is_intact(connection)

    connection.call("tx_begin")
    execute_error(connection, "INSERT INTO items VALUES (4,-2)", INVALID_ARGUMENT, exception="blocked_quantity")
    audit_is_intact(connection)
    connection.call("tx_rollback")

    # The table has no uniqueness constraint. The nested INSERT reaches the trigger recursion limit.
    recursive = token()
    execute_error(connection, "INSERT INTO recursions VALUES (7)", RESOURCE_LIMIT, request_token=recursive)
    require(connection.call("tx_outcome", request_token=recursive) == {"status": "unknown"},
            "recursive trigger failure produced a committed outcome")
    require(rows(connection, "SELECT * FROM recursions") == [], "trigger recursion left partial rows")
    require(connection.call("health")["status"] == "ready", "trigger recursion made the session unavailable")
    audit_is_intact(connection)


def test_select_into(connection):
    require(rows(connection, "SELECT single_match(2)") == [[i64(2)]], "SELECT INTO did not bind its single row")
    execute_error(connection, "SELECT single_match(3)", INVALID_ARGUMENT, exception="no_data_found")
    execute_error(connection, "SELECT single_match(0)", INVALID_ARGUMENT, exception="too_many_rows")


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--server", type=Path, required=True)
    parser.add_argument("--openssl", type=Path, required=True)
    parser.add_argument("--scratch", type=Path, required=True)
    args = parser.parse_args()
    server = str(args.server.resolve(strict=True))
    openssl = str(args.openssl.resolve(strict=True))
    args.scratch.mkdir(parents=True, exist_ok=True)
    with tempfile.TemporaryDirectory(prefix="s17-", dir=args.scratch) as directory:
        fixture = Path(directory)
        for name in ("data", "log", "temp"):
            (fixture / name).mkdir()
        certificates(openssl, fixture)
        config = save_config(fixture, timeout_ms=15_000, max_connections=4)
        require(execute(server, config, "--initialize-storage").returncode == 0, "trigger storage setup failed")
        password = secrets.token_urlsafe(32)
        password_input = ((password + "\n") * 2).encode("ascii")
        require(execute(server, config, "--bootstrap-admin", "local-admin", stdin=password_input).returncode == 0,
                "trigger fixture administrator setup failed")
        context = ssl.create_default_context(cafile=str(fixture / "ca.pem"))

        with listening(server, config) as (_, port):
            with client(port, context, password) as connection:
                install_fixture(connection)
            test_invoker_permissions(port, context, password)
        committed = files_in(fixture / "data")
        require(any(".wal." in path.name for path in committed), "successful trigger audit did not produce a WAL")

        with listening(server, config) as (_, port):
            with client(port, context, password) as connection:
                audit_is_intact(connection)
                test_failed_mutations(connection)
                test_select_into(connection)

        require(execute(server, config, "--check-storage").returncode == 0, "trigger fixture failed WAL/storage verification")
        require(files_in(fixture / "data") == committed, "trigger failure changed committed data or WAL history")

    print("Triggers, nested rollback, WAL stability, recursion, SELECT INTO cardinality and invoker rights passed")


if __name__ == "__main__":
    main()
