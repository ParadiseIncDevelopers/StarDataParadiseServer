"""Install the legacy read-only scalar function form as a MySQL routine."""

import argparse
from pathlib import Path
import re
import sys


IDENTIFIER = r"[a-z_][a-z0-9_]{0,63}"
QUALIFIED = rf"{IDENTIFIER}(?:\.{IDENTIFIER})?"
TYPE = r"(?:BIGINT|DECIMAL\s*\(\s*[0-9]{1,2}\s*,\s*[0-9]{1,2}\s*\))"
FUNCTION = re.compile(
    rf"\s*CREATE\s+FUNCTION\s+(?P<function>{QUALIFIED})\s*"
    rf"\(\s*(?P<parameter>{IDENTIFIER})\s+IN\s+(?P<parameter_type>{TYPE})\s*\)\s*"
    rf"RETURN\s+(?P<result_type>{TYPE})\s+AS\s+BEGIN\s+RETURN\s*\(\s*"
    rf"SELECT\s+(?P<projection_alias>{IDENTIFIER})\.(?P<projection>{IDENTIFIER})\s+"
    rf"FROM\s+(?P<table>{QUALIFIED})\s+AS\s+(?P<alias>{IDENTIFIER})\s+"
    rf"WHERE\s+(?P<key_alias>{IDENTIFIER})\.(?P<key>{IDENTIFIER})\s*=\s*:(?P<binding>{IDENTIFIER})\s*"
    rf"\)\s*;\s*END\s*;?\s*",
    flags=re.ASCII | re.IGNORECASE,
)


def quoted(name):
    return "`" + name.replace("`", "``") + "`"


def qualified(name, schema):
    parts = name.split(".")
    if len(parts) == 1:
        parts.insert(0, schema)
    return ".".join(map(quoted, parts))


def scalar_type(declaration):
    if declaration.upper() == "BIGINT":
        return "BIGINT"
    precision, scale = map(int, re.findall(r"[0-9]+", declaration))
    if not 1 <= precision <= 38 or not 0 <= scale <= precision:
        raise ValueError("invalid DECIMAL precision or scale")
    return f"DECIMAL({precision},{scale})"


def compile_function(source, schema):
    match = FUNCTION.fullmatch(source)
    if match is None:
        raise ValueError("unsupported PL/SQL scalar function definition")
    fields = match.groupdict()
    if (fields["parameter"].lower() != fields["binding"].lower()
            or fields["projection_alias"].lower() != fields["alias"].lower()
            or fields["key_alias"].lower() != fields["alias"].lower()):
        raise ValueError("qualified columns and the bound parameter must match their declarations")
    function = qualified(fields["function"], schema)
    table = qualified(fields["table"], schema)
    alias, projection, key = (quoted(fields[name]) for name in ("alias", "projection", "key"))
    parameter_type, result_type = (scalar_type(fields[name]) for name in ("parameter_type", "result_type"))
    return (f"CREATE FUNCTION {function} (`_sdp_value` {parameter_type}) RETURNS {result_type} "
            "READS SQL DATA SQL SECURITY INVOKER "
            f"RETURN (SELECT {alias}.{projection} FROM {table} AS {alias} "
            f"WHERE {alias}.{key} = `_sdp_value`)")


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("definition", type=Path)
    args = parser.parse_args(argv)
    from mysqlsh import globals as shell_globals

    try:
        session = shell_globals.shell.get_session()
        if session is None or not session.is_open():
            raise ValueError("connect MySQL Shell to the server before installing a function")
        schema = session.run_sql("SELECT DATABASE()").fetch_one()[0]
        if not schema:
            raise ValueError("select the target schema before installing a function")
        source = args.definition.read_text(encoding="utf-8-sig")
        session.run_sql(compile_function(source, schema))
        print("SDP_PLSQL installed scalar function")
    except (OSError, ValueError) as error:
        print(f"PL/SQL: {error}", file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
