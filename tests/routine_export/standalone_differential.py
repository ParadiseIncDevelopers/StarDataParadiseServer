#!/usr/bin/env python3
"""Differentially run verified Server routines and an independently compiled Java project."""

import argparse
import hashlib
import json
from pathlib import Path
import secrets
import ssl
import subprocess
import sys
import tempfile

sys.dont_write_bytecode = True

TEST_DIR = Path(__file__).resolve().parent
FIXTURE = TEST_DIR
TOOLS = TEST_DIR.parents[1] / "tools" / "routine_export"
if not (FIXTURE / "vectors.json").is_file():
    FIXTURE = TEST_DIR.parent / "export-fixture"
    TOOLS = TEST_DIR
sys.path[:0] = [str(TOOLS), str(FIXTURE)]

from emit_java import Emitter, export, literal, string
from server_oracle import certificates, client, execute, listening, observe, require, save_config, write


def dump(path, data):
    path.write_text(json.dumps(data, ensure_ascii=False, indent=2) + "\n", encoding="utf-8")


def offline_snapshots(metadata, names):
    objects = {str(item["id"]): item for item in metadata["objects"]}
    routines = {item["qualified_name"][-1]: item for item in objects.values() if item["kind"] == "routine"}

    def record(item):
        body = json.loads(item["definition"])
        dependencies = []
        for id_, version in body["dependencies"]:
            dependency = objects[str(id_)]
            require(version == dependency["schema_version"], "Stored dependency version differs from catalog")
            dependencies.append({
                "id": str(id_), "qualified_name": dependency["qualified_name"],
                "schema_version": version, "kind": dependency["kind"],
            })
        return {
            "id": str(item["id"]), "qualified_name": item["qualified_name"],
            "schema_version": item["schema_version"], "ir": body["ir"], "dependencies": dependencies,
        }

    def snapshot(entry):
        seen = {}

        def visit(item):
            id_ = str(item["id"])
            if id_ in seen:
                return
            result = record(item)
            seen[id_] = result
            for dependency in result["dependencies"]:
                if dependency["kind"] == "routine":
                    visit(objects[dependency["id"]])

        visit(entry)
        return {"format": 1, "mode": "standalone", "entries": [str(entry["id"])],
                "routines": list(seen.values())}

    return [snapshot(routines[name]) for name in names]


def get_java_source(vectors, observed):
    lines = ["""package stardataparadise.exported;

import java.math.*;
import java.time.*;
import java.util.*;
import static stardataparadise.exported.RoutineRuntime.*;

public final class Differential {
    static Object normalize(Object value) {
        if (value instanceof byte[] binary) return HexFormat.of().formatHex(binary);
        if (value instanceof RecordValue record)
            return record.fields().stream()
                .map(field -> Arrays.asList(field.name(), normalize(field.value()))).toList();
        if (value instanceof CollectionValue collection)
            return collection.items().stream().map(Differential::normalize).toList();
        return value;
    }

    static void equal(String test, Object expected, Object actual) {
        if (!Objects.equals(normalize(expected), normalize(actual))) {
            throw new AssertionError(test + ": expected " + expected + ", observed " + actual);
        }
    }

    public static void main(String[] args) {"""]
    for vector in vectors:
        name = vector["id"]
        outcome = observed[name]
        entries = list(vector["parameters"].items())
        values = ", ".join(literal(value, vector["parameter_types"][key]["type"]) for key, value in entries)
        fn = f'Routines.invoke("main.public.{vector["routine"]}", new Object[]{{{values}}})'
        lines.append(f"        // {name}")
        lines.append("        {")
        if "rows" in outcome:
            require(len(outcome["rows"]) == 1, f"{name}: unexpected Server row cardinality")
            success = outcome["rows"][0]
            lines.append(f"            var result = {fn};")
            if vector["sql"].startswith("SELECT "):
                require(len(success) == 1, f"{name}: unexpected Server column cardinality")
                lines.append(f'            equal("{name}", {literal(success[0])}, result.returned());')
                lines.append(f'            equal("{name} outputs", 0, result.out().size());')
            else:
                columns = vector["expected"]["columns"]
                names = ", ".join(string(column["name"]) for column in columns)
                lines.append(f'            equal("{name} outputs", List.of({names}), new ArrayList<>(result.out().keySet()));')
                for column, value in zip(columns, success, strict=True):
                    field = string(column["name"])
                    lines.append(f'            equal("{name}", {literal(value, column["type"])}, result.out().get({field}));')
        else:
            lines.extend([
                "            try {",
                f"                {fn};",
                f'                throw new AssertionError("{name}: missing Server-compatible failure");',
                "            } catch (Failure error) {",
            ])
            for attr, actual in [("code", "error.code()"), ("line", "error.site().line()"),
                                 ("column", "error.site().column()"), ("pl_exception", "error.plException()")]:
                lines.append(f'                equal("{name} {attr}", {json.dumps(outcome[attr])}, {actual});')
            lines.append("            }")
        lines.append("        }")
    lines.extend([
        "        try {",
        '            Routines.invoke("main.public.export_material", 1L);',
        '            throw new AssertionError("Private dependency was exported as an entry point");',
        "        } catch (IllegalArgumentException expected) {",
        "            // The dependency is available only to other exported routines.",
        "        }",
    ])
    lines.append(f'        System.out.println("Stage 35 standalone JDK oracle: {len(vectors)} vectors matched");')
    lines += ["    }", "}"]
    return "\n".join(lines) + "\n"


def merged(snapshots):
    entries, routines = [], {}
    for snapshot in snapshots:
        require(len(snapshot["entries"]) == 1, "Unexpected routine export entry count")
        entries.append(snapshot["entries"][0])
        for record in snapshot["routines"]:
            prior = routines.setdefault(record["id"], record)
            require(record == prior, "Routine revision/IR changed across export requests")
    return {"format": 1, "mode": "standalone", "entries": entries, "routines": list(routines.values())}


def expect_export_rejection(script, snapshot, work, name):
    path = work / f"bad-{name}.json"
    out = work / f"bad-{name}"
    dump(path, snapshot)
    reply = subprocess.run([sys.executable, str(script), "--snapshot", str(path), "--output", str(out)],
                           capture_output=True, text=True, encoding="utf-8", check=False)
    require(reply.returncode == 2 and not out.exists(),
            f"Invalid snapshot unexpectedly published files: {reply.stdout} {reply.stderr}")
    return json.loads(reply.stderr)


def audit_manifest(manifest_path):
    manifest = json.loads(manifest_path.read_text(encoding="utf-8"))
    actual = {str(member.relative_to(manifest_path.parent).as_posix()) for member in
              manifest_path.parent.rglob("*") if member.is_file()}
    require(actual == {item["path"] for item in manifest["files"]} | {"manifest.json"},
            "Manifest does not list the complete project")
    for member in manifest["files"]:
        content = (manifest_path.parent / member["path"]).read_bytes()
        require(len(content) == member["bytes"] and hashlib.sha256(content).hexdigest() == member["sha256"],
                f"Incorrect SHA-256/size for {member['path']}")


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    for name in ("server", "openssl", "scratch", "jdk"):
        parser.add_argument(f"--{name}", required=True, type=Path)
    parser.add_argument("--gradle", type=Path)
    parser.add_argument("--rpc", action="store_true")
    options = parser.parse_args()
    server, openssl, jdk = [item.resolve(strict=True) for item in (options.server, options.openssl, options.jdk)]
    options.scratch.mkdir(parents=True, exist_ok=True)
    work = Path(tempfile.mkdtemp(prefix="rpc-" if options.rpc else "catalog-", dir=options.scratch.resolve()))
    fixture = json.loads((FIXTURE / "vectors.json").read_text(encoding="utf-8"))
    vectors = [vector for vector in fixture["vectors"] if not vector["id"].startswith("connected-")]
    names = list(dict.fromkeys(vector["routine"] for vector in vectors))
    observed = {}
    with tempfile.TemporaryDirectory(prefix="server-", dir=work) as directory:
        root = Path(directory)
        for child in ("data", "log", "temp"):
            (root / child).mkdir()
        certificates(str(openssl), root)
        config = save_config(root, timeout_ms=15_000, max_connections=3)
        initialized = execute(str(server), config, "--initialize-storage")
        require(initialized.returncode == 0, f"Storage failed: {initialized.stderr}")
        secret = secrets.token_urlsafe(32)
        bootstrap = execute(str(server), config, "--bootstrap-admin", "local-admin",
                            stdin=(f"{secret}\n" * 2).encode("ascii"))
        require(bootstrap.returncode == 0, f"Fixture administrator failed: {bootstrap.stderr}")
        tls = ssl.create_default_context(cafile=str(root / "ca.pem"))
        with listening(str(server), config) as (_, port), client(port, tls, secret) as connection:
            for definition in fixture["fixtures"]:
                write(connection, definition["sql"], definition["affected_rows"])
            for vector in vectors:
                actual = observe(connection, vector)
                for key in ("rows", "pl_exception"):
                    if key in vector["expected"]:
                        require(actual.get(key) == vector["expected"][key], f'{vector["id"]}: wrong oracle {actual}')
                observed[vector["id"]] = actual

            if options.rpc:
                snapshots = [connection.call("routine_export", routine=name, mode="standalone") for name in names]
                denied = connection.raw("routine_export", routine="export_stock_for", mode="standalone")
                require(not denied["ok"], "Hosted SQL accepted in standalone RPC")
                error = denied["error"]
                require(error["code"] == 1004 and error["line"] == 3 and error["column"] > 1,
                        f"Hosted RPC rejection has incorrect source {error}")
                dump(work / "hosted-rejection.json", error)
        # The server process has exited. Use its persistent verified compilation
        # only for baseline diagnosis when exercising the published 0.3.0 binary.
        if not options.rpc:
            listing = execute(str(server), config, "--catalog-info")
            require(listing.returncode == 0, f"Cannot read baseline catalog: {listing.stderr}")
            *snapshots, hosted = offline_snapshots(json.loads(listing.stdout), [*names, "export_stock_for"])
        else:
            # Exercise the independent Java emitter fail-closed path with an injected
            # hosted opcode at a known, valid program location.
            hosted = json.loads(json.dumps(snapshots[0]))
            entry = hosted["entries"][0]
            ir = next(record["ir"] for record in hosted["routines"] if record["id"] == entry)
            ir["body"][0] = {"op": "open_cursor", "at": [60, 3, 5]}

    combined = merged(snapshots)
    dump(work / "observed.json", observed)
    dump(work / "snapshot.json", combined)
    script = TOOLS / "emit_java.py"
    out = work / "generated"
    manifest = export(combined, out)
    audit_manifest(manifest)
    # Checkout-independent: use only the exporter CLI's project output below.
    isolated = subprocess.run([sys.executable, str(script), "--snapshot", str(work / "snapshot.json"),
                               "--output", str(work / "repeat")], check=True, capture_output=True)
    require((work / "repeat" / "manifest.json").read_bytes() == manifest.read_bytes(),
            f"Deterministic export failed: {isolated.stdout}")
    contradictory = json.loads(json.dumps(snapshots[0]))
    entry = contradictory["entries"][0]
    record = next(routine for routine in contradictory["routines"] if routine["id"] == entry)
    record["dependencies"][0]["schema_version"] += 1
    reject = expect_export_rejection(script, contradictory, work, "stale-dependency")
    require(reject.get("diagnostic") == "standalone_unsupported" and reject.get("op") == "call",
            f"Revision mismatched dependency lacks a correct diagnostic {reject}")
    fail = expect_export_rejection(script, hosted, work, "hosted")
    require(fail["diagnostic"] == "standalone_unsupported" and fail["at"][1] == 3 and
            fail["at"][2] > 1 and fail["op"] in {"subquery", "open_cursor"}, f"Incorrect source failure {fail}")
    dump(work / "emitter-rejection.json", fail)
    # A transitive dependency may be compiled into the project without becoming an entry point.
    check = Emitter(snapshots[0])
    check.java()
    require(len(check.reached) == 2 and len(check.entries) == 1, "Material closure was lost")

    java_tests = work / "Differential.java"
    java_tests.write_text(get_java_source(vectors, observed), encoding="utf-8")
    dependencies = out / "src" / "main" / "java"
    javac = jdk / "bin" / ("javac.exe" if sys.platform == "win32" else "javac")
    java = jdk / "bin" / ("java.exe" if sys.platform == "win32" else "java")
    classes = work / "classes"
    subprocess.run([str(javac), "--release", "17", "-encoding", "UTF-8", "-d", str(classes),
                    *map(str, dependencies.rglob("*.java")), str(java_tests)], check=True, cwd=work)
    subprocess.run([str(java), "-cp", str(classes), "stardataparadise.exported.Differential"],
                   check=True, cwd=work)
    if options.gradle:
        subprocess.run([str(options.gradle.resolve(strict=True)), "--offline", "--no-daemon", "build"],
                       check=True, cwd=out)
    print(f"Stage 35 server binary SHA-256: {hashlib.sha256(server.read_bytes()).hexdigest()}")
    print(f"Stage 35 standalone evidence: {work}")


if __name__ == "__main__":
    main()
