"""The typed values and named bindings supported by the MySQL wire bridge."""

from decimal import Decimal
import re

from mysql.connector.constants import FieldType


NAME = re.compile(r"[A-Za-z_][A-Za-z_0-9]*")
INTEGER = re.compile(r"(?:0|-?[1-9][0-9]*)")
HEX = re.compile(r"[0-9a-f]*")
DECLARATIONS = {"BIGINT": "i64", "BOOLEAN": "bool", "TEXT": "text", "BLOB": "binary",
                "DECIMAL": "decimal"}
INT_FIELDS = {FieldType.TINY, FieldType.SHORT, FieldType.INT24, FieldType.LONG, FieldType.LONGLONG}
DECIMAL_FIELDS = {FieldType.DECIMAL, FieldType.NEWDECIMAL}
STRING_FIELDS = {FieldType.STRING, FieldType.VAR_STRING, FieldType.VARCHAR, FieldType.ENUM, FieldType.SET,
                 FieldType.TINY_BLOB, FieldType.MEDIUM_BLOB, FieldType.LONG_BLOB, FieldType.BLOB}


class Fault(Exception):
    def __init__(self, code, *, close=False, uncertain=False):
        super().__init__(code)
        self.code = code
        self.close = close
        self.uncertain = uncertain


def require(condition, code=1012):
    if not condition:
        raise Fault(code)


def object_field(body, key):
    value = body.get(key, {})
    require(isinstance(value, dict))
    return value


def integer_field(body, key, default, maximum):
    value = body.get(key, default)
    require(type(value) is int and 1 <= value <= maximum)
    return value


def name_field(body, key):
    value = body.get(key)
    require(isinstance(value, str) and len(value) <= 64 and NAME.fullmatch(value))
    return value


def literal_end(text, index):
    quote = text[index]
    while index + 1 < len(text):
        index += 1
        if text[index] != quote:
            continue
        if index + 1 == len(text) or text[index + 1] != quote:
            return index + 1
        index += 1
    raise Fault(1012)


def prepare_sql(statement, declared):
    require(isinstance(statement, str) and 0 < len(statement) <= 64 * 1024 and "\0" not in statement)
    statement = statement.strip()
    first = NAME.match(statement)
    verb = first[0].upper() if first else ""
    require(verb in {"SELECT", "INSERT", "UPDATE", "DELETE"}, 1004)
    sql, names = [], []
    index = 0
    while index < len(statement):
        char = statement[index]
        if char in "'\"`":
            end = literal_end(statement, index)
            sql.append(statement[index:end])
            index = end
            continue
        # The backend runs with NO_BACKSLASH_ESCAPES. Restrict comments and raw markers
        # so the statement the gateway classifies is the one prepared by MySQL.
        if char in "?#" or statement.startswith(("--", "/*"), index):
            raise Fault(1004)
        if char == ";":
            require(index == len(statement) - 1, 1004)
            break
        if char == ":":
            match = NAME.match(statement, index + 1)
            require(match and len(match[0]) <= 64)
            sql.append("?")
            names.append(match[0])
            index = match.end()
            continue
        word = NAME.match(statement, index)
        if word:
            if verb == "SELECT":
                require(word[0].upper() not in {"INTO", "FOR", "LOCK"}, 1004)
            sql.append(word[0])
            index = word.end()
            continue
        sql.append(char)
        index += 1
    require(len(names) <= 128 and len(declared) <= 64 and set(names) == set(declared))
    for descriptor in declared.values():
        require(isinstance(descriptor, dict))
        kind = descriptor.get("type")
        require(isinstance(kind, str))
        require(kind in DECLARATIONS, 1004)
        if kind == "DECIMAL":
            precision, scale = descriptor.get("precision"), descriptor.get("scale")
            require(type(precision) is int and type(scale) is int and 0 <= scale <= precision <= 38
                    and precision >= 1)
    return verb, "".join(sql).encode("utf-8"), names


def parse_binding(value, descriptor):
    require(isinstance(value, dict))
    tag = value.get("t")
    if tag == "null":
        require(len(value) == 1)
        return None
    require(tag == DECLARATIONS[descriptor["type"]])
    raw = value.get("v")
    if tag == "bool":
        require(type(raw) is bool)
        return int(raw)
    if tag == "i64":
        require(isinstance(raw, str) and len(raw) <= 20 and INTEGER.fullmatch(raw))
        result = int(raw)
        require(-(1 << 63) <= result < (1 << 63))
        return result
    if tag == "text":
        require(isinstance(raw, str))
        return raw
    if tag == "binary":
        raw = value.get("hex")
        require(isinstance(raw, str) and len(raw) % 2 == 0 and HEX.fullmatch(raw))
        return bytes.fromhex(raw)
    digits, scale = value.get("unscaled"), value.get("scale")
    require(isinstance(digits, str) and len(digits) <= 39 and INTEGER.fullmatch(digits))
    precision, target_scale = descriptor["precision"], descriptor["scale"]
    require(type(scale) is int and 0 <= scale <= target_scale)
    require(digits == "0" or len(digits.lstrip("-")) - scale <= precision - target_scale)
    # Decimal from a tuple bypasses the current Python precision context.
    return Decimal((digits.startswith("-"), tuple(map(int, digits.lstrip("-"))), -scale))


def binding_values(body, declarations, names):
    values = object_field(body, "parameters")
    require(values.keys() == declarations.keys())
    decoded = {key: parse_binding(value, declarations[key]) for key, value in values.items()}
    return tuple(decoded[name] for name in names)


def column_type(column):
    code = column[1]
    if code in INT_FIELDS or code == FieldType.NULL:
        return "BIGINT"
    if code in DECIMAL_FIELDS:
        return "DECIMAL"
    if code in STRING_FIELDS:
        return "BLOB" if column[8] == 63 else "TEXT"
    raise Fault(1004)


def wire_value(value, kind):
    if value is None:
        return {"t": "null"}
    if kind == "BIGINT" and type(value) is int and -(1 << 63) <= value < (1 << 63):
        return {"t": "i64", "v": str(value)}
    if kind == "TEXT" and isinstance(value, str):
        return {"t": "text", "v": value}
    if kind == "BLOB" and isinstance(value, bytes):
        return {"t": "binary", "hex": value.hex()}
    if kind == "DECIMAL" and isinstance(value, Decimal):
        plain = format(value, "f")
        integer, _, fraction = plain.partition(".")
        unscaled = str(int(integer + fraction))
        if len(unscaled.lstrip("-")) <= 38 and len(fraction) <= 38:
            return {"t": "decimal", "unscaled": unscaled, "scale": len(fraction)}
    raise Fault(1004)
