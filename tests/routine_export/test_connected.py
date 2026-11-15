"""Exercise connected Java exports against the real TLS SQL Server and pinned JDBC JAR."""
import argparse
import hashlib
import json
import os
from pathlib import Path
import secrets
import ssl
import subprocess
import sys
import tempfile

sys.path.insert(0, str(Path(__file__).resolve().parents[2] / "tools" / "routine_export"))

from emit_connected import generate
from snapshot import exchange, snapshot, ServerError

def main():
    parser = argparse.ArgumentParser(description=__doc__)
    for flag in ("server", "openssl", "scratch", "jar", "gson", "fixture", "server-root", "jdk"):
        parser.add_argument(f"--{flag}", type=Path, required=True)
    args = parser.parse_args()
    sys.path.insert(0, str(args.server_root.resolve() / "tests"))
    from service_smoke import certificates, execute, listening, require, save_config
    from sql_rpc import client, token, write
    fixtures = json.loads(args.fixture.read_text(encoding="utf-8-sig"))
    digest = hashlib.sha256(args.jar.read_bytes()).hexdigest()
    args.scratch.mkdir(parents=True, exist_ok=True)
    run = Path(tempfile.mkdtemp(dir=args.scratch, prefix="connected-")).resolve()
    instance = run / "instance"
    for child in ("data", "log", "temp"):
        (instance / child).mkdir(parents=True)
    certificates(str(args.openssl.resolve()), instance)
    server = str(args.server.resolve(strict=True))
    config = save_config(instance, timeout_ms=15000, max_connections=6)
    require(execute(server, config, "--initialize-storage").returncode == 0, "export instance initialization failed")
    admin = secrets.token_urlsafe(32)
    secret = secrets.token_urlsafe(32)
    require(execute(server, config, "--bootstrap-admin", "local-admin",
                    stdin=(f"{admin}\n" * 2).encode("ascii")).returncode == 0, "export bootstrap failed")
    ca = instance / "ca.pem"
    tls = ssl.create_default_context(cafile=ca)
    binaries = args.jdk.resolve() / "bin"
    jar_classpath = os.pathsep.join(str(p.resolve()) for p in (args.jar, args.gson))
    loader = Path(__file__).with_name("CheckConnected.java")
    with listening(server, config) as (_, port), client(port, tls, admin) as owner:
        for row in fixtures["fixtures"]:
            write(owner, row["sql"], row["affected_rows"])
        features = owner.call("capabilities")["features"]
        require(features.get("routine_export_v1") is True, "export capability missing")
        missing = owner.raw("routine_export", routine="export_stock_for", mode="standalone")
        error = missing.get("error", {})
        require(not missing["ok"] and error.get("line") == 3 and error.get("column", 0) > 0,
                f"source position for declined standalone export: {error}")
        rejected_path = owner.raw("routine_export", routine="export_stock_for", mode="connected", path="../output")
        require(not rejected_path["ok"], "Server accepted a client file path")

        write(owner, "CREATE USER export_reader", 0)
        owner.call("password_set", username="export_reader", new_password=secret, request_token=token())
        def read(mode, name, as_guest=False):
            return snapshot("127.0.0.1", port, "localhost", ca,
                            "export_reader" if as_guest else "local-admin", secret if as_guest else admin, mode, name)
        def denied(mode, name):
            try:
                read(mode, name, True)
            except ServerError as failure:
                require(failure.code == 1006, f"permission error expected, got {failure.code}")
            else:
                raise AssertionError("unauthorized export succeeded")
        denied("connected", "export_stock_for")
        write(owner, "GRANT EXECUTE ON ROUTINE export_stock_for TO USER export_reader", 0)
        denied("connected", "export_stock_for")
        write(owner, "GRANT SELECT ON TABLE export_stock TO USER export_reader", 0)

        def run_app(name, source, mode):
            cp = str(source / "classes") + os.pathsep + jar_classpath
            subprocess.run([binaries / "java.exe", "-cp", cp, "CheckConnected", str(port), str(ca), mode],
                           check=True, input=admin + "\n", encoding="ascii", timeout=45)
            print(f"{name} {mode} JDBC verified")
        generated = {}
        for name in ("export_stock_for", "export_stock_cursor"):
            artifact = read("connected", name, name == "export_stock_for")
            require(artifact["uses_sql"] and not artifact["writes_sql"] and not artifact["standalone_eligible"],
                    "export eligibility and SQL effect report mismatch")
            require(all("ir" not in item and "sql" not in item for item in artifact["routines"]),
                    "connected snapshot exposed routine implementation")
            source = run / name
            generate(artifact, source, digest)
            java = list((source / "src").rglob("*.java"))
            classes = source / "classes"
            classes.mkdir()
            subprocess.run([binaries / "javac.exe", "--release", "17", "-encoding", "UTF-8",
                            "-cp", jar_classpath, "-d", str(classes), *(str(p) for p in java), str(loader)],
                           check=True, timeout=60)
            generated[name] = source
            run_app(name, source, "normal")
        with client(port, tls, admin) as updater:
            write(updater, "CREATE OR REPLACE FUNCTION export_stock_for(p IN BIGINT) RETURN BIGINT AS BEGIN RETURN 99; END;", 0)
        run_app("export_stock_for", generated["export_stock_for"], "stale")
    print("Connected JDBC export, permission boundaries and immutable version gate verified")

if __name__ == "__main__":
    main()
