"""Check the legacy scalar-function form against an actual temporary fork."""

from .fixture import COMPATIBILITY, query, require, run


INSTALLER = COMPATIBILITY / "plsql" / "scalar_function.py"


SOURCE = """CREATE FUNCTION quantity_for(p_key IN BIGINT) RETURN BIGINT AS
BEGIN
    RETURN (SELECT i.quantity FROM items AS i WHERE i.id = :p_key);
END;
"""

DECIMAL_SOURCE = """CREATE FUNCTION cost_for(p_key IN DECIMAL(22,4)) RETURN DECIMAL(30,5) AS
BEGIN
    RETURN (SELECT i.cost FROM prices AS i WHERE i.id = :p_key);
END;
"""


def verify(fixture):
    with fixture.scratch("plsql") as root:
        routine = root / "function.sql"
        routine.write_text(SOURCE, encoding="utf-8")
        with fixture.disposable(root / "server") as server:
            query(server, """CREATE SCHEMA sdp_plsql_acceptance;
                CREATE TABLE sdp_plsql_acceptance.items (id BIGINT PRIMARY KEY, quantity BIGINT) ENGINE=InnoDB;
                INSERT INTO sdp_plsql_acceptance.items VALUES (1,9007199254740993);
                CREATE TABLE sdp_plsql_acceptance.prices (
                    id DECIMAL(22,4) PRIMARY KEY, cost DECIMAL(30,5)) ENGINE=InnoDB;
                INSERT INTO sdp_plsql_acceptance.prices VALUES (9007199254740993.0001,999999999999999999.12345);""")
            env, classic, cli = server
            command = [*cli, "--schema=sdp_plsql_acceptance", "--file", str(INSTALLER), str(routine)]
            result = run(command, env, 15)
            require(result.returncode == 0 and "SDP_PLSQL installed" in result.stdout, result.stdout + result.stderr)
            values = query(server, """USE sdp_plsql_acceptance;
                SELECT quantity_for(1), quantity_for(2);
                START TRANSACTION;
                UPDATE items SET quantity = 40 WHERE id = 1;
                SELECT quantity_for(1);
                ROLLBACK;
                SELECT quantity_for(1);""")
            require(values.splitlines() == ["9007199254740993\tNULL", "40", "9007199254740993"], values)
            query(server, """CREATE USER 'sdp_pl_reader'@'localhost' REQUIRE SSL;
                GRANT EXECUTE ON FUNCTION sdp_plsql_acceptance.quantity_for TO 'sdp_pl_reader'@'localhost';""")
            caller = ["--user=sdp_pl_reader" if option == "--user=root" else option for option in classic]
            select = [*caller, "--execute=SELECT sdp_plsql_acceptance.quantity_for(1)"]
            denied = run(select, env, 10)
            require(denied.returncode != 0 and "1142" in denied.stderr, denied.stdout + denied.stderr)
            query(server, "GRANT SELECT ON sdp_plsql_acceptance.items TO 'sdp_pl_reader'@'localhost'")
            allowed = run(select, env, 10)
            require(allowed.returncode == 0 and allowed.stdout.strip() == "9007199254740993", allowed.stderr)
            routine.write_text(SOURCE.replace(":p_key", ":missing"), encoding="utf-8")
            bad = run(command, env, 15)
            require(bad.returncode != 0 and "must match" in bad.stderr, bad.stdout + bad.stderr)

            routine.write_text(DECIMAL_SOURCE, encoding="utf-8")
            decimal = run(command, env, 15)
            require(decimal.returncode == 0, decimal.stdout + decimal.stderr)
            require(query(server, "SELECT sdp_plsql_acceptance.cost_for(9007199254740993.0001), "
                                  "sdp_plsql_acceptance.cost_for(9007199254740993.0000)")
                    == "999999999999999999.12345\tNULL", "exact DECIMAL input or return scale was lost")
            routine.write_text(DECIMAL_SOURCE.replace("22,4", "39,4"), encoding="utf-8")
            invalid_type = run(command, env, 15)
            require(invalid_type.returncode != 0 and "invalid DECIMAL" in invalid_type.stderr,
                    invalid_type.stdout + invalid_type.stderr)
            print("PASS PL/SQL scalar functions: BIGINT, exact DECIMAL, NULL, invoker permissions and caller transaction")
