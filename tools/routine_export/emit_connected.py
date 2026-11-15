"""Generate a connected JDK 17 wrapper from an authenticated Server snapshot."""
from pathlib import Path
import argparse
import base64
import hashlib
import json
import re

TYPES = {
    "BIGINT": "BIGINT", "BOOLEAN": "BOOLEAN", "DECIMAL": "DECIMAL",
    "TEXT": "VARCHAR", "BLOB": "VARBINARY", "DATE": "DATE", "TIME": "TIME",
    "TIMESTAMP WITH TIME ZONE": "TIMESTAMP_WITH_TIMEZONE", "RECORD": "STRUCT",
    "COLLECTION": "ARRAY",
}
GSON_SHA256 = "dd0ce1b55a3ed2080cb70f9c655850cda86c206862310009dcb5e5c95265a5e0"
TEMPLATES = Path(__file__).resolve().parent

def encoded(text):
    return base64.b64encode(text.encode("utf-8")).decode("ascii")

def sql_name(parts):
    if not isinstance(parts, list) or len(parts) != 3 or any(
        not isinstance(p, str) or not p or "\x00" in p or len(p.encode("utf-8")) > 256 for p in parts
    ):
        raise ValueError("Invalid qualified routine name")
    return ".".join('"' + p.replace('"', '""') + '"' for p in parts)

def add_revision(revisions, item):
    key, version = item["id"], item["schema_version"]
    if not isinstance(key, str) or not re.fullmatch(r"[1-9][0-9]{0,18}", key):
        raise ValueError("Invalid catalog identity")
    if type(version) is not int or not 1 <= version <= 0xffff_ffff:
        raise ValueError("Invalid immutable routine version")
    if revisions.setdefault(key, version) != version:
        raise ValueError("Conflicting immutable routine versions")

def generate(snapshot, output, jar_hash):
    if snapshot.get("format") != 1 or snapshot.get("mode") != "connected":
        raise ValueError("Expected a connected routine snapshot v1")
    if not re.fullmatch(r"[0-9a-f]{64}", jar_hash):
        raise ValueError("Expected the installed SDK JAR SHA-256")
    entries, routines = snapshot["entries"], snapshot["routines"]
    if len(entries) != 1 or not 1 <= len(routines) <= 64:
        raise ValueError("Invalid routine closure")
    indexed = {item["id"]: item for item in routines}
    if len(indexed) != len(routines) or entries[0] not in indexed:
        raise ValueError("Invalid routine identity")
    entry = indexed[entries[0]]
    revisions = {}
    for routine in routines:
        add_revision(revisions, routine)
        for dependency in routine["dependencies"]:
            add_revision(revisions, dependency)
    slots = entry["parameters"]
    if len(slots) > 128:
        raise ValueError("Too many parameters")
    bindings, marks = [], []
    for slot in slots:
        if slot["mode"] == "out":
            marks.append("NULL")
        elif slot["mode"] in ("in", "in_out"):
            marks.append("?")
            kind = slot["type"]["name"]
            if kind not in TYPES:
                at = slot["at"]
                raise ValueError(f"Unsupported parameter {kind} at {at[1]}:{at[2]}")
            scale = slot["type"].get("scale") if kind == "DECIMAL" else None
            if scale is not None and (type(scale) is not int or not 0 <= scale <= 38):
                raise ValueError("Invalid DECIMAL scale")
            scale_expr = "null" if scale is None else str(scale)
            bindings.append(f"        new Parameter({len(bindings)+1}, Types.{TYPES[kind]}, {scale_expr})")
        else:
            raise ValueError("Invalid parameter mode")
    call = "SELECT " if entry["returns"] is not None else "CALL "
    sql = call + sql_name(entry["qualified_name"]) + "(" + ", ".join(marks) + ")"
    values = {
        "@SQL@": encoded(sql),
        "@FUNCTION@": "true" if entry["returns"] is not None else "false",
        "@REVISIONS@": ",\n".join(f'        Map.entry("{key}", {ver}L)' for key, ver in sorted(revisions.items())),
        "@INPUT@": ",\n".join(bindings),
    }
    source = (TEMPLATES / "ConnectedRoutine.java.in").read_text(encoding="utf-8-sig")
    for key, value in values.items():
        source = source.replace(key, value)
    output.mkdir(parents=False, exist_ok=False)
    name = "src/main/java/stardataparadise/exported/ConnectedRoutine.java"
    (output / name).parent.mkdir(parents=True)
    files = {
        name: source,
        "settings.gradle": "rootProject.name = 'stardataparadise-connected-export'\n",
        "build.gradle": (TEMPLATES / "build.gradle.in").read_text(encoding="utf-8-sig").replace("@SDK@", jar_hash),
        "README.md": """# StarDataParadise connected routine

The Java wrapper binds typed inputs through the pinned JDBC package. The routine,
its dependencies and all SQL run on StarDataParadise Server. Use a caller-owned
JDBC transaction with autocommit disabled. Metadata revision checks and execution
share that transaction. Commit or roll back through the connection when finished.

Build on JDK 17 using Gradle with -PsdpSdkJar and -PsdpGsonJar pointing to the exact
JARs in manifest.json. Call ConnectedRoutine.invoke with an SdpConnection and the
IN and IN OUT arguments. Each returned result set contains its columns and rows.
""",
    }
    for relative, body in files.items():
        (output / relative).write_text(body, encoding="utf-8", newline="\n")
    manifest = {
        "format": 1, "exporter_version": "0.4.0", "mode": "connected", "ir_version": 1,
        "java": 17, "server_required": True, "server_retains": ["routine", "dependencies", "SQL effects"],
        "sdk": {"coordinate": "com.stardataparadise:stardataparadise-jdbc:0.2.0", "sha256": jar_hash,
                "gson_sha256": GSON_SHA256},
        "snapshot": snapshot,
        "files": [{"path": path, "sha256": hashlib.sha256((output / path).read_bytes()).hexdigest()}
                  for path in sorted(files)]
    }
    (output / "manifest.json").write_text(json.dumps(manifest, ensure_ascii=False, indent=2) + "\n",
                                        encoding="utf-8", newline="\n")

if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--snapshot", required=True, type=Path)
    parser.add_argument("--output", required=True, type=Path)
    parser.add_argument("--sdk-sha256", required=True)
    options = parser.parse_args()
    try:
        generate(json.loads(options.snapshot.read_text(encoding="utf-8-sig")), options.output, options.sdk_sha256)
    except (OSError, ValueError, KeyError, TypeError) as error:
        parser.exit(2, f"Connected export rejected: {error}\n")
