"""Install a bounded BIGINT PL/SQL procedure through MySQL Shell."""

import argparse
from dataclasses import dataclass
from pathlib import Path
import re
import sys

if __package__:
    from .scalar_function import IDENTIFIER, qualified, quoted
else:
    sys.path.insert(0, str(Path(__file__).resolve().parent))
    from scalar_function import IDENTIFIER, qualified, quoted


MAX_SOURCE = 32 * 1024
MAX_TOKENS = 1024
MAX_RUNTIME_STEPS = 100_000
NAME = re.compile(IDENTIFIER, re.ASCII | re.IGNORECASE)
TOKEN = re.compile(r"\s+|[A-Za-z_][A-Za-z0-9_]*|[0-9]+|:=|\.\.|<=|>=|<>|!=|[().,;:+*=<>-]", re.ASCII)
PRIORITY = {"+": 1, "-": 1, "*": 2}
COMPARISONS = {"=", "<>", "!=", "<", "<=", ">", ">="}


@dataclass(frozen=True)
class Expression:
    sql: str
    steps: int = 1


def indented(statements):
    return "\n".join("    " + line for statement in statements for line in statement.splitlines())


def tokenize(source):
    if len(source) > MAX_SOURCE:
        raise ValueError("PL/SQL procedure definition exceeds the size limit")
    parts = []
    index = 0
    while index < len(source):
        match = TOKEN.match(source, index)
        if match is None:
            raise ValueError("unsupported character in PL/SQL procedure definition")
        token = match[0]
        index = match.end()
        if not token.isspace():
            parts.append(token)
            if len(parts) > MAX_TOKENS:
                raise ValueError("PL/SQL procedure definition has too many tokens")
    return parts


class Procedure:
    def __init__(self, source, schema):
        self.tokens = tokenize(source)
        self.position = 0
        self.schema = schema
        self.parameters = {}
        self.internal_names = 0
        self.statements = 0
        self.loop = None

    def private_name(self):
        while True:
            self.internal_names += 1
            name = f"_sdp{self.internal_names}"
            if name not in self.parameters:
                return name

    def current(self):
        return self.tokens[self.position] if self.position < len(self.tokens) else ""

    def accept(self, value):
        if self.current().upper() == value:
            self.position += 1
            return True
        return False

    def expect(self, value):
        if not self.accept(value):
            raise ValueError(f"expected {value} in PL/SQL procedure definition")

    def identifier(self):
        value = self.current()
        if not NAME.fullmatch(value):
            raise ValueError("expected a PL/SQL identifier")
        self.position += 1
        return value

    def object_name(self):
        first = self.identifier()
        return f"{first}.{self.identifier()}" if self.accept(".") else first

    def column(self):
        alias = self.identifier()
        self.expect(".")
        return alias, self.identifier()

    def parameter(self, name, *, assigned=False):
        if self.loop is not None and name.lower() == self.loop[0]:
            if assigned:
                raise ValueError("a FOR iterator cannot be assigned")
            return self.loop[1]
        parameter = self.parameters.get(name.lower())
        if parameter is None:
            raise ValueError("expression references an undeclared parameter")
        spelling, direction = parameter
        if assigned and direction == "IN":
            raise ValueError("an IN parameter cannot be assigned")
        return quoted(spelling)

    def signature(self):
        self.expect("(")
        if self.accept(")"):
            return ""
        while True:
            name = self.identifier()
            if len(self.parameters) >= 32:
                raise ValueError("PL/SQL procedure has too many parameters")
            key = name.lower()
            if key == "null" or key in self.parameters:
                raise ValueError("invalid or repeated PL/SQL parameter name")
            if self.accept("IN"):
                direction = "INOUT" if self.accept("OUT") else "IN"
            else:
                self.expect("OUT")
                direction = "OUT"
            self.expect("BIGINT")
            self.parameters[key] = name, direction
            if not self.accept(","):
                self.expect(")")
                break
        return ", ".join(f"{direction} {quoted(name)} BIGINT"
                         for name, direction in self.parameters.values())

    def select(self):
        projection_alias, projection = self.column()
        self.expect("FROM")
        table = qualified(self.object_name(), self.schema)
        self.expect("AS")
        alias = self.identifier()
        self.expect("WHERE")
        key_alias, key = self.column()
        self.expect("=")
        self.expect(":")
        parameter = self.parameter(self.identifier())
        if alias.lower() != projection_alias.lower() or alias.lower() != key_alias.lower():
            raise ValueError("qualified columns must use the declared SELECT alias")
        alias = quoted(alias)
        return Expression(f"(SELECT {alias}.{quoted(projection)} FROM {table} AS {alias} "
                          f"WHERE {alias}.{quoted(key)} = {parameter})", 2)

    def atom(self, depth):
        if depth > 16:
            raise ValueError("PL/SQL expression exceeds the nesting limit")
        if self.accept("("):
            value = self.select() if self.accept("SELECT") else self.expression(1, depth + 1)
            self.expect(")")
            return Expression(f"({value.sql})", value.steps)
        if self.accept("NULL"):
            return Expression("NULL")
        sign = ""
        if self.current() in {"+", "-"}:
            sign = self.current()
            self.position += 1
        token = self.current()
        if token.isascii() and token.isdecimal():
            if len(token) > 19:
                raise ValueError("PL/SQL BIGINT literal is out of range")
            value = int(sign + token)
            if not -(1 << 63) <= value < (1 << 63):
                raise ValueError("PL/SQL BIGINT literal is out of range")
            self.position += 1
            return Expression(f"CAST('{value}' AS SIGNED)", 2 if sign else 1)
        if sign:
            value = self.atom(depth + 1)
            return Expression(f"({sign}{value.sql})", 1 + value.steps)
        return Expression(self.parameter(self.identifier()))

    def expression(self, lowest=1, depth=0):
        left = self.atom(depth)
        while (operator := self.current()) in PRIORITY and PRIORITY[operator] >= lowest:
            self.position += 1
            right = self.expression(PRIORITY[operator] + 1, depth + 1)
            left = Expression(f"({left.sql} {operator} {right.sql})", 1 + left.steps + right.steps)
        return left

    def condition(self):
        left = self.expression()
        operator = self.current()
        if operator not in COMPARISONS:
            raise ValueError("expected a scalar BIGINT comparison")
        self.position += 1
        right = self.expression()
        return Expression(f"{left.sql} {operator} {right.sql}", 1 + left.steps + right.steps)

    def charge(self, steps):
        return (f"IF {self.budget} < {steps} THEN\n"
                "    SIGNAL SQLSTATE '45000' SET MESSAGE_TEXT = 'PL/SQL execution step limit exceeded';\n"
                "END IF;\n"
                f"SET {self.budget} = {self.budget} - {steps};\n")

    def block(self, endings, depth):
        if depth > 8:
            raise ValueError("PL/SQL statement nesting limit exceeded")
        items = []
        while self.current().upper() not in endings:
            self.statements += 1
            if self.statements > 64:
                raise ValueError("PL/SQL procedure has too many statements")
            items.append(self.statement(depth))
            self.expect(";")
        if not items:
            raise ValueError("expected a nonempty PL/SQL statement block")
        return items

    def statement(self, depth):
        if self.position + 1 < len(self.tokens) and self.tokens[self.position + 1] == ":=":
            target = self.parameter(self.identifier(), assigned=True)
            self.expect(":=")
            value = self.expression()
            return self.charge(1 + value.steps) + f"SET {target} = {value.sql};"
        if self.accept("IF"):
            return self.if_statement(depth)
        if self.accept("FOR"):
            return self.for_statement(depth)
        if self.accept("CONTINUE"):
            if self.loop is None:
                raise ValueError("CONTINUE WHEN requires a FOR loop")
            self.expect("WHEN")
            value = self.condition()
            return self.charge(1 + value.steps) + f"IF {value.sql} THEN LEAVE {self.loop[2]}; END IF;"
        raise ValueError("unsupported PL/SQL procedure statement")

    def if_statement(self, depth):
        value = self.condition()
        self.expect("THEN")
        body = indented(self.block({"ELSE", "END"}, depth + 1))
        alternate = ""
        if self.accept("ELSE"):
            alternate = "\nELSE\n" + indented(self.block({"END"}, depth + 1))
        self.expect("END")
        self.expect("IF")
        return self.charge(1 + value.steps) + f"IF {value.sql} THEN\n{body}{alternate}\nEND IF;"

    def for_statement(self, depth):
        if self.loop is not None:
            raise ValueError("nested FOR loops are unavailable")
        name = self.identifier()
        if name.lower() == "null":
            raise ValueError("invalid FOR iterator name")
        self.expect("IN")
        start = self.expression()
        self.expect("..")
        end = self.expression()
        self.expect("LOOP")
        iterator, upper = (quoted(self.private_name()) for _ in range(2))
        loop, body_label = (self.private_name() for _ in range(2))
        self.loop = name.lower(), iterator, body_label
        body = self.block({"END"}, depth + 1)
        self.loop = None
        self.expect("END")
        self.expect("LOOP")

        iterate = (f"{loop}: LOOP\n" + indented([
            self.charge(1).rstrip(),
            f"{body_label}: BEGIN\n{indented(body)}\nEND {body_label};",
            f"IF {iterator} = {upper} THEN LEAVE {loop}; END IF;",
            f"SET {iterator} = {iterator} + 1;",
        ]) + f"\nEND LOOP {loop};")
        body = indented([
            f"DECLARE {iterator}, {upper} BIGINT;",
            self.charge(1 + start.steps + end.steps).rstrip(),
            f"SET {iterator} = {start.sql}, {upper} = {end.sql};",
            f"IF {iterator} IS NULL OR {upper} IS NULL THEN\n"
            "    SIGNAL SQLSTATE '22004' SET MESSAGE_TEXT = 'Expected non-NULL BIGINT FOR bounds';\nEND IF;",
            f"IF {iterator} <= {upper} THEN\n{indented([iterate])}\nEND IF;",
        ])
        return f"BEGIN\n{body}\nEND;"

    def compile(self):
        self.expect("CREATE")
        self.expect("PROCEDURE")
        routine = qualified(self.object_name(), self.schema)
        arguments = self.signature()
        self.expect("AS")
        self.expect("BEGIN")
        self.budget = quoted(self.private_name())
        statements = self.block({"END"}, 0)
        self.expect("END")
        self.accept(";")
        if self.current():
            raise ValueError("expected a single PL/SQL procedure")
        body = indented([f"DECLARE {self.budget} INT UNSIGNED DEFAULT {MAX_RUNTIME_STEPS};", *statements])
        return (f"CREATE PROCEDURE {routine} ({arguments}) READS SQL DATA SQL SECURITY INVOKER\n"
                f"BEGIN\n{body}\nEND")


def compile_procedure(source, schema):
    return Procedure(source, schema).compile()


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("definition", type=Path)
    args = parser.parse_args(argv)
    from mysqlsh import globals as shell_globals

    try:
        session = shell_globals.shell.get_session()
        if session is None or not session.is_open():
            raise ValueError("connect MySQL Shell to the server before installing a procedure")
        schema, mode = session.run_sql("SELECT DATABASE(), @@SESSION.sql_mode").fetch_one()
        if not schema:
            raise ValueError("select the target schema before installing a procedure")
        with args.definition.open(encoding="utf-8-sig") as definition:
            ddl = compile_procedure(definition.read(MAX_SOURCE + 1), schema)
        strict_modes = ",".join(filter(None, (mode, "STRICT_ALL_TABLES")))
        session.run_sql("SET SESSION sql_mode = ?", [strict_modes])
        try:
            session.run_sql(ddl)
        finally:
            session.run_sql("SET SESSION sql_mode = ?", [mode])
        print("SDP_PLSQL installed scalar procedure")
    except (OSError, ValueError) as error:
        print(f"PL/SQL: {error}", file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
