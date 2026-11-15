#!/usr/bin/env python3
"""Generate standalone Java 17 source from a verified StarDataParadise routine snapshot."""

import argparse
import base64
from contextlib import contextmanager
from dataclasses import dataclass
import hashlib
import json
from pathlib import Path
import re
import shutil
import sys
import tempfile
import time

PACKAGE = "stardataparadise.exported"
SIMPLE_TYPES = {
    "NULL", "BIGINT", "BOOLEAN", "TEXT", "BLOB", "DATE", "TIME",
    "TIMESTAMP WITH TIME ZONE", "RECORD", "COLLECTION",
}
ARITHMETIC = {"add", "subtract", "multiply", "divide"}
COMPARISON = {"eq", "ne", "lt", "le", "gt", "ge"}
UNARY = {"is_null", "is_not_null", "not", "negate"}
BINARY = {"and", "or"} | ARITHMETIC | COMPARISON


class ExportError(ValueError):
    def __init__(self, diagnostic, message, routine=None, node=None):
        super().__init__(message)
        self.diagnostic = {"diagnostic": diagnostic, "message": message}
        if routine is not None:
            self.diagnostic["routine"] = list(routine.name)
        if node is not None:
            self.diagnostic["at"] = node["at"]
            if "op" in node:
                self.diagnostic["op"] = node["op"]


def check(condition, message):
    if not condition:
        raise ExportError("invalid_snapshot", message)


def string(value):
    return 'utf8("' + base64.b64encode(value.encode("utf-8")).decode("ascii") + '")'


def site(node):
    offset, line, column = node["at"]
    check(all(type(n) is int for n in (offset, line, column)) and
          0 <= offset < 2**63 and 0 < line < 2**31 and 0 < column < 2**31,
          "Invalid typed IR source location")
    return f"new Site({offset}L, {line}, {column})"


def type_source(value):
    name = value["name"]
    if name in SIMPLE_TYPES:
        return f'new Type("{name}", 0, 0)'
    precision, scale = value["precision"], value["scale"]
    check(type(precision) is int and type(scale) is int and 0 <= scale <= precision <= 38 and
          precision > 0 and name == f"DECIMAL({precision},{scale})", "Invalid SQL type in typed IR")
    return f'new Type("DECIMAL", {precision}, {scale})'


def long_source(value):
    check(isinstance(value, str) and re.fullmatch(r"-?(0|[1-9][0-9]{0,18})", value) is not None,
          "Invalid integer value in typed IR")
    check(-(2**63) <= int(value) < 2**63, "Integer outside BIGINT range")
    return value + "L"


def literal(value, value_type=None, depth=0):
    check(depth <= 16, "Overly nested typed literal")
    tag = value["t"]
    if tag == "null":
        return "null"
    if tag == "i64":
        return long_source(value["v"])
    if tag == "bool":
        check(type(value["v"]) is bool, "Invalid BOOLEAN value in typed IR")
        return str(value["v"]).lower()
    if tag == "decimal":
        coefficient, scale = value["unscaled"], value["scale"]
        check(re.fullmatch(r"-?(0|[1-9][0-9]{0,37})", coefficient) is not None and
              type(scale) is int and 0 <= scale <= 38, "Invalid DECIMAL value in typed IR")
        return f'new BigDecimal(new BigInteger("{coefficient}"), {scale})'
    if tag == "text":
        text = string(value["v"])
        if value_type == "DATE":
            return f"LocalDate.parse({text})"
        if value_type == "TIME":
            return f"LocalTime.parse({text})"
        return text
    if tag == "binary":
        check(re.fullmatch("(?:[0-9a-f]{2})*", value["hex"]) is not None, "Invalid BLOB value in typed IR")
        return f'HexFormat.of().parseHex("{value["hex"]}")'
    if tag == "timestamp":
        nanos, offset = value["nanos"], value["offset_minutes"]
        check(type(nanos) is int and 0 <= nanos < 1_000_000_000 and type(offset) is int and
              -840 <= offset <= 840, "Invalid TIMESTAMP value in typed IR")
        return f'new Timestamp({long_source(value["seconds"])}, {nanos}, {offset})'
    if tag == "record":
        fields = ", ".join(
            f"new Field({string(field['name'])}, {literal(field['value'], depth=depth + 1)})"
            for field in value["fields"])
        return f"new RecordValue(List.of({fields}))"
    if tag == "collection":
        items = ", ".join(literal(item, depth=depth + 1) for item in value["items"])
        return f"new CollectionValue(Arrays.asList(new Object[]{{{items}}}))"
    raise ExportError("invalid_snapshot", f"Unknown typed literal tag {tag}")


def qualified(parts):
    check(isinstance(parts, list) and 1 <= len(parts) <= 3 and
          all(isinstance(part, str) and bool(part) for part in parts), "Invalid routine name")
    return tuple([["main", "public"], ["main"], []][len(parts) - 1] + parts)


def catalog_id(value):
    check(isinstance(value, str) and re.fullmatch(r"[1-9][0-9]{0,19}", value) is not None and
          int(value) < 2**64, "Invalid routine catalog ID")
    return value


@dataclass
class Routine:
    identifier: str
    name: tuple
    ordinal: int
    version: int
    ir: dict
    dependencies: dict

    @property
    def parameters(self):
        return self.ir["slots"][:self.ir["parameters"]]

    @property
    def returned(self):
        return self.ir["returns"]

    @property
    def method(self):
        return f"routine{self.ordinal}"

    @property
    def types(self):
        return f"TYPES_{self.ordinal}"


class Source:
    def __init__(self):
        self.lines = []
        self.depth = 0

    def line(self, text=""):
        self.lines.append("    " * self.depth + text if text else "")

    @contextmanager
    def block(self, header, tail="}"):
        self.line(header + " {" if header else "{")
        self.depth += 1
        try:
            yield
        finally:
            self.depth -= 1
        if tail:
            self.line(tail)

    def text(self):
        return "\n".join(self.lines) + "\n"


class Emitter:
    def __init__(self, snapshot):
        check(snapshot["format"] == 1 and snapshot["mode"] == "standalone",
              "Expected a format-1 standalone routine snapshot")
        records = snapshot["routines"]
        check(isinstance(records, list) and 0 < len(records) <= 128, "Invalid snapshot routine count")
        self.by_id = {}
        self.by_name = {}
        for ordinal, record in enumerate(records):
            identifier = catalog_id(record["id"])
            name = qualified(record["qualified_name"])
            check(len(record["qualified_name"]) == 3 and name not in self.by_name and
                  identifier not in self.by_id, "Duplicate or unqualified routine identity")
            ir = record["ir"]
            version = record["schema_version"]
            check(ir["ir_version"] == 1 and type(version) is int and 0 < version < 2**64 and
                  str(version) == ir["revision"] and qualified(ir["routine"]) == name,
                  "Typed IR version or routine identity differs from its catalog identity")
            check(type(ir["parameters"]) is int and 0 <= ir["parameters"] <= len(ir["slots"]) <= 256,
                  "Invalid typed IR slot count")
            modes = [slot["mode"] for slot in ir["slots"]]
            check(all(mode in {"in", "in_out", "out"} for mode in modes[:ir["parameters"]]) and
                  all(mode == "local" for mode in modes[ir["parameters"]:]), "Invalid typed IR slot modes")
            dependencies = {}
            for dependency in record["dependencies"]:
                referenced = catalog_id(dependency["id"])
                revision = dependency["schema_version"]
                check(referenced not in dependencies and type(revision) is int and 0 < revision < 2**64,
                      "Invalid or duplicate dependency revision")
                dependencies[referenced] = dependency
            routine = Routine(identifier, name, ordinal, version, ir, dependencies)
            self.by_id[identifier] = self.by_name[name] = routine
        entries = snapshot["entries"]
        check(isinstance(entries, list) and entries and all(entry in self.by_id for entry in entries) and
              len(entries) == len(set(entries)), "Invalid snapshot entry points")
        self.entries = [self.by_id[entry] for entry in entries]
        self.reached = {}
        self.used_dependencies = {}
        self.source = Source()
        self.label = 0

    def unsupported(self, routine, node, message):
        raise ExportError("standalone_unsupported", message, routine, node)

    def slot(self, routine, node):
        number = node["slot"]
        check(type(number) is int and 0 <= number < len(routine.ir["slots"]), "Invalid typed IR slot number")
        return number

    def called(self, routine, node):
        name = qualified(node["routine"])
        target = self.by_name.get(name)
        if target is None:
            self.unsupported(routine, node, "Called routine is missing from the verified dependency closure")
        if target is not routine:
            dependency = routine.dependencies.get(target.identifier)
            if dependency is None or dependency["schema_version"] != target.version:
                self.unsupported(routine, node, "Called routine revision is absent from the dependency closure")
            if dependency["kind"] != "routine" or qualified(dependency["qualified_name"]) != name:
                self.unsupported(routine, node, "Called routine identity differs from the dependency closure")
            self.used_dependencies[routine.identifier].add(target.identifier)
        if target.returned is None or any(slot["mode"] != "in" for slot in target.parameters):
            self.unsupported(routine, node, "A standalone call requires a scalar function with IN arguments")
        return target

    def validate(self, routine):
        if routine.identifier in self.reached:
            return
        self.reached[routine.identifier] = routine
        self.used_dependencies[routine.identifier] = set()

        def expression(node):
            op = node["op"]
            site(node)
            type_source(node["type"])
            args = node.get("args", [])
            if op == "call":
                target = self.called(routine, node)
                check(len(args) == len(target.parameters), "Invalid routine argument count in typed IR")
                self.validate(target)
            elif op in BINARY:
                check(len(args) == 2, "Invalid binary expression in typed IR")
                if op in COMPARISON and any(arg["type"]["name"] in {"RECORD", "COLLECTION"} for arg in args):
                    self.unsupported(routine, node, "Compound comparison requires Server")
            elif op in UNARY:
                check(len(args) == 1, "Invalid unary expression in typed IR")
            elif op in {"load", "literal"}:
                check(not args, "Unexpected arguments in typed IR")
                if op == "load":
                    self.slot(routine, node)
                else:
                    literal(node["value"], node["type"]["name"])
            else:
                self.unsupported(routine, node, f"Expression {op} requires Server")
            for child in args:
                expression(child)

        def statements(nodes, in_loop=False):
            for node in nodes:
                op = node["op"]
                site(node)
                if op not in {"assign", "if", "block", "for", "while", "loop",
                              "exit", "continue", "return", "raise"}:
                    self.unsupported(routine, node, f"Instruction {op} requires Server")
                if op in {"assign", "for"}:
                    self.slot(routine, node)
                if op in {"exit", "continue"}:
                    check(in_loop, "Loop control outside a typed IR loop")
                for key in ("first", "second"):
                    if key in node:
                        expression(node[key])
                for key in ("body", "alternate", "handlers"):
                    statements(node.get(key, []), in_loop or op in {"for", "while", "loop"})

        site(routine.ir)
        for slot in routine.ir["slots"]:
            site(slot)
            type_source(slot["type"])
        if routine.returned:
            type_source(routine.returned)
        statements(routine.ir["body"])
        statements(routine.ir["handlers"])
        if routine.ir.get("writes_sql") or routine.ir.get("cursors") or routine.ir.get("out_cursors"):
            self.unsupported(routine, routine.ir, "Routine has SQL writes or cursor declarations")
        if set(routine.dependencies) - self.used_dependencies[routine.identifier] - {routine.identifier}:
            self.unsupported(routine, routine.ir, "Routine has a non-local catalog dependency")

    def expression(self, routine, node, depth=0):
        op, at = node["op"], site(node)
        args = [self.expression(routine, arg, depth + 1) for arg in node.get("args", [])]
        if op == "literal":
            value = literal(node["value"], node["type"]["name"])
        elif op == "load":
            value = f"f.get({self.slot(routine, node)})"
        elif op in {"and", "or"}:
            value = f"{op}({args[0]}, () -> {args[1]})"
        elif op in {"is_null", "is_not_null"}:
            operator = "==" if op == "is_null" else "!="
            value = f"({args[0]} {operator} null)"
        elif op == "not":
            value = f"not({args[0]})"
        elif op == "negate":
            value = f"negate({args[0]}, {at})"
        elif op in ARITHMETIC | COMPARISON:
            call = "arithmetic" if op in ARITHMETIC else "compare"
            value = f'{call}("{op}", {", ".join(args)}, {at})'
        else:
            target = self.called(routine, node)
            value = (f"call{target.ordinal}(c, {at}, {type_source(node['type'])}, "
                     f"new Object[]{{{', '.join(args)}}})")
        return f"c.evaluate({at}, {depth}, () -> {value})"

    def new_label(self):
        self.label += 1
        return self.label

    def statements(self, routine, nodes, depth, loop=None):
        out = self.source
        for node in nodes:
            op, at = node["op"], site(node)
            out.line(f"c.step({at}, {depth});")
            first = node.get("first")
            value = self.expression(routine, first) if first is not None else None
            body = node.get("body", [])
            if op == "assign":
                out.line(f"f.put({self.slot(routine, node)}, {value}, {site(first)});")
            elif op == "if":
                with out.block(f"if (truth({value}))"):
                    self.statements(routine, body, depth + 1, loop)
                if node.get("alternate"):
                    with out.block("else"):
                        self.statements(routine, node["alternate"], depth + 1, loop)
            elif op == "block":
                with out.block(""):
                    self.guarded(routine, body, node.get("handlers", []), depth + 1, loop)
            elif op in {"for", "while", "loop"}:
                number = self.new_label()
                label, inner = f"loop{number}", f"body{number}"
                if op == "for":
                    bound = self.expression(routine, node["second"])
                    out.line(f"long lower{number} = integer({value}, {at});")
                    out.line(f"long upper{number} = integer({bound}, {at});")
                    control = f"for (long i{number} = lower{number}; i{number} <= upper{number};)"
                else:
                    control = "while (truth(true))"
                with out.block(f"{label}: {control}"):
                    out.line(f"c.step({at}, {depth});")
                    if op == "for":
                        out.line(f"f.put({self.slot(routine, node)}, i{number}, {at});")
                    elif op == "while":
                        out.line(f"if (!truth({value})) break;")
                    with out.block(f"{inner}:"):
                        self.statements(routine, body, depth + 1, (label, inner))
                    if op == "for":
                        out.line(f"if (i{number} == upper{number}) break;")
                        out.line(f"++i{number};")
            elif op in {"exit", "continue"}:
                # Java's reachability analysis treats unguarded loop control as abrupt.
                condition = f"truth({value})" if value is not None else "truth(true)"
                label = loop[0 if op == "exit" else 1]
                out.line(f"if ({condition}) break {label};")
            elif op == "return":
                if first is not None:
                    value = f"fit({value}, {type_source(routine.returned)}, {site(first)})"
                out.line(f"returnValue({value if value is not None else 'null'});")
            else:
                out.line(f"throwException({string(node['exception'])}, {at}, c);")

    def guarded(self, routine, nodes, handlers, depth, loop=None):
        if not handlers:
            self.statements(routine, nodes, depth, loop)
            return
        out = self.source
        number = self.new_label()
        with out.block("try"):
            self.statements(routine, nodes, depth, loop)
        with out.block(f"catch (Failure failure{number})"):
            out.line(f"if (failure{number}.plException() == null) throw failure{number};")
            out.line(f"Failure previous{number} = c.activeException;")
            out.line(f"c.activeException = failure{number};")
            with out.block("try"):
                self.statements(routine, handlers, depth, loop)
            with out.block("finally"):
                out.line(f"c.activeException = previous{number};")

    def emit_routine(self, routine):
        out, at = self.source, site(routine.ir)
        out.line(f"// Routine catalog ID {routine.identifier}, schema revision {routine.version}.")
        type_array = ", ".join(type_source(slot["type"]) for slot in routine.ir["slots"])
        out.line(f"private static final Type[] {routine.types} = {{{type_array}}};")
        with out.block(f"private static RoutineResult {routine.method}(Context c, Object[] arguments)"):
            with out.block(f"try (Frame f = c.enter({routine.types}, {at}))"):
                params = routine.parameters
                inputs = sum(slot["mode"] != "out" for slot in params)
                out.line(f"boolean full = arguments.length == {len(params)};")
                with out.block(f"if (!full && arguments.length != {inputs})"):
                    out.line(f'throw invalidArgument({at}, "Routine argument count does not match declaration");')
                out.line("int position = 0;")
                for index, slot in enumerate(params):
                    if slot["mode"] != "out":
                        at_parameter = site(slot)
                        out.line(f"f.put({index}, arguments[position++], {at_parameter});")
                    else:
                        with out.block("if (full && arguments[position++] != null)"):
                            out.line(f'throw invalidArgument({site(slot)}, "OUT argument position requires NULL");')
                out.line("Object returned = null;")
                if routine.returned is not None:
                    out.line("boolean didReturn = false;")
                with out.block("try"):
                    self.guarded(routine, routine.ir["body"], routine.ir["handlers"], 0)
                with out.block("catch (Return result)"):
                    out.line("returned = result.value;")
                    if routine.returned is not None:
                        out.line("didReturn = true;")
                if routine.returned is not None:
                    out.line(f"if (!didReturn) throw programError({at});")
                out.line("Map<String, Object> outputs = new LinkedHashMap<>();")
                for index, slot in enumerate(params):
                    if slot["mode"] != "in":
                        out.line(f"outputs.put({string(slot['name'])}, f.get({index}));")
                out.line("return new RoutineResult(returned, outputs);")
        if routine.returned is not None and all(slot["mode"] == "in" for slot in routine.parameters):
            out.line()
            with out.block(f"private static Object call{routine.ordinal}(Context c, Site at, Type resultType, Object[] args)"):
                for i in range(len(routine.parameters)):
                    out.line(f"args[{i}] = fit(args[{i}], {routine.types}[{i}], at);")
                out.line(f"Object result = {routine.method}(c, args).returned();")
                out.line("c.step(at, 0);")
                out.line("return fit(result, resultType, at);")

    def java(self):
        for entry in self.entries:
            self.validate(entry)
        check(len(self.reached) == len(self.by_id), "Snapshot contains routines outside the requested closure")
        out = self.source
        out.line(f"package {PACKAGE};")
        out.line()
        out.line("import java.math.BigDecimal;")
        out.line("import java.math.BigInteger;")
        out.line("import java.time.LocalDate;")
        out.line("import java.time.LocalTime;")
        out.line("import java.util.*;")
        out.line()
        out.line(f"import static {PACKAGE}.RoutineRuntime.*;")
        out.line()
        out.line("/** Generated StarDataParadise typed routines. */")
        with out.block("public final class Routines"):
            out.line("private Routines() {}")
            out.line()
            with out.block("public static RoutineResult invoke(String qualifiedName, Object... inputs)"):
                out.line('return invoke(List.of(qualifiedName.split("\\\\.", -1)), inputs);')
            out.line()
            with out.block("public static RoutineResult invoke(List<String> qualifiedName, Object... inputs)"):
                out.line("Objects.requireNonNull(qualifiedName, \"qualifiedName\");")
                out.line("Objects.requireNonNull(inputs, \"inputs\");")
                for routine in self.entries:
                    name = ", ".join(string(part) for part in routine.name)
                    with out.block(f"if (qualifiedName.equals(List.of({name})))"):
                        out.line(f"return {routine.method}(new Context(), inputs);")
                out.line('throw new IllegalArgumentException("Routine is not a generated entry point");')
            for routine in self.reached.values():
                out.line()
                self.emit_routine(routine)
        return out.text()

    def manifest(self, files):
        return {
            "format": 1, "exporter_version": 1, "mode": "standalone", "standalone_eligible": True,
            "server_required": False, "sql_effects": "none",
            "java_release": 17, "entry_class": PACKAGE + ".Routines",
            "entries": [routine.identifier for routine in self.entries],
            "routines": [
                {
                    "id": routine.identifier, "qualified_name": list(routine.name),
                    "schema_version": routine.version, "ir_version": routine.ir["ir_version"],
                    "deterministic": routine.ir["deterministic"],
                    "ir_sha256": hashlib.sha256(json.dumps(
                        routine.ir, sort_keys=True, separators=(",", ":"), ensure_ascii=False
                    ).encode("utf-8")).hexdigest(),
                    "dependencies": [routine.dependencies[key] for key in sorted(routine.dependencies, key=int)],
                }
                for routine in self.reached.values()
            ],
            "files": [
                {"path": path, "bytes": len(content), "sha256": hashlib.sha256(content).hexdigest()}
                for path, content in sorted(files.items())
            ],
        }


def smoke_test(entries):
    names = ", ".join("List.of(" + ", ".join(string(part) for part in entry.name) + ")" for entry in entries)
    return f"""package {PACKAGE};

import java.util.List;

import static {PACKAGE}.RoutineRuntime.utf8;

public final class ExportSmoke {{
    public static void main(String[] args) {{
        for (var name : List.of({names})) {{
            try {{
                Routines.invoke(name, new Object[257]);
                throw new AssertionError("Accepted more arguments than the typed IR permits");
            }} catch (RoutineRuntime.Failure error) {{
                if (error.code() != 1012 || error.site().line() < 1 || error.site().column() < 1) {{
                    throw new AssertionError("Missing typed argument diagnostic", error);
                }}
            }}
        }}
        try {{
            Routines.invoke(List.of());
            throw new AssertionError("Accepted an unknown entry point");
        }} catch (IllegalArgumentException expected) {{
            // The dispatcher rejects unknown entry points before execution.
        }}
    }}
}}
""".encode("utf-8")


def export(snapshot, output):
    emitter = Emitter(snapshot)
    generated = emitter.java()
    package_path = "src/main/java/" + PACKAGE.replace(".", "/") + "/"
    files = {
        package_path + "Routines.java": generated.encode("utf-8"),
        package_path + "RoutineRuntime.java": Path(__file__).with_name("RoutineRuntime.java").read_bytes(),
        "src/test/java/" + PACKAGE.replace(".", "/") + "/ExportSmoke.java": smoke_test(emitter.entries),
        "settings.gradle": b"rootProject.name = 'sdp-exported-routines'\n",
        "build.gradle": b"""plugins {
    id 'java-library'
}
java {
    sourceCompatibility = JavaVersion.VERSION_17
    targetCompatibility = JavaVersion.VERSION_17
}
tasks.withType(JavaCompile).configureEach {
    options.encoding = 'UTF-8'
    options.release = 17
}
tasks.register('exportSmoke', JavaExec) {
    dependsOn testClasses
    classpath = sourceSets.test.runtimeClasspath
    mainClass = 'stardataparadise.exported.ExportSmoke'
}
tasks.named('test') { enabled = false }
tasks.named('check') { dependsOn 'exportSmoke' }
""",
        "README.md": ("""# Exported StarDataParadise routines

This project contains Java 17 source generated from a verified catalog snapshot.
Build it with a local JDK 17 and Gradle using gradle --offline build.
You can also compile the files under src/main/java directly with javac --release 17.

Call stardataparadise.exported.Routines.invoke("main.public.routine_name", inputs).
Use the List<String> overload for any name part containing a period. Pass Long
(or Integer/Short/Byte), BigDecimal, Boolean, String, LocalDate, LocalTime,
byte[], RoutineRuntime.Timestamp, RecordValue or CollectionValue according
to the SQL type. null is SQL NULL. The returned object exposes returned()
and out() with the ordered OUT and IN OUT values. To pass one NULL through
varargs, use (Object) null. Supply only IN/IN OUT values or supply every
argument and place NULL in each OUT position.

RoutineRuntime.Failure exposes the server-compatible numeric code(), optional
plException() and typed-IR site() (byte offset, line and column).
manifest.json records the exact verified routine revisions, dependency identities,
IR fingerprints and SHA-256 hashes of generated files. The project has no SDK or
database dependency at build or execution time. Gradle check runs the generated
entry point and argument-validation smoke test with no external test dependencies.
""").encode("utf-8"),
    }
    files["manifest.json"] = (json.dumps(emitter.manifest(files), indent=2, ensure_ascii=False) + "\n").encode("utf-8")
    output = output.absolute()
    check(output.parent.is_dir() and not output.exists() and not output.is_symlink(),
          "Choose a new output path in an existing directory")

    with tempfile.TemporaryDirectory(prefix=".sdp-java-", dir=output.parent) as temporary:
        ready = Path(temporary)
        for name, content in files.items():
            target = ready / name
            target.parent.mkdir(parents=True, exist_ok=True)
            target.write_bytes(content)
        # POSIX rename may replace an empty directory. Reserve the name first.
        if sys.platform == "win32":
            # Antivirus and indexers can briefly hold newly generated files.
            for delay in (0.02, 0.05, 0.1, 0.25, 0.5, 1, None):
                try:
                    ready.rename(output)
                    break
                except OSError as error:
                    if (delay is None or error.winerror not in {5, 32, 33} or
                            output.exists() or output.is_symlink()):
                        raise
                    time.sleep(delay)
        else:
            output.mkdir()
            try:
                ready.replace(output)
            except OSError:
                output.rmdir()
                raise
    return output / "manifest.json"


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--snapshot", type=Path, required=True)
    parser.add_argument("--output", type=Path, required=True)
    args = parser.parse_args()
    try:
        check(args.snapshot.stat().st_size <= 8 * 1024 * 1024, "Snapshot exceeds the allowed size")
        snapshot = json.loads(args.snapshot.read_text(encoding="utf-8"))
        manifest = export(snapshot, args.output)
    except ExportError as error:
        print(json.dumps(error.diagnostic, ensure_ascii=False), file=sys.stderr)
        return 2
    except (KeyError, TypeError, ValueError, RecursionError, OSError) as error:
        print(json.dumps({"diagnostic": "export_failed", "message": str(error)}, ensure_ascii=False), file=sys.stderr)
        return 2
    print(manifest)
    return 0


if __name__ == "__main__":
    sys.exit(main())
