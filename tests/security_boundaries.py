"""Check bounded malformed SQL and real CLI output using private loopback fixtures."""

import argparse
import hashlib
import json
import os
from pathlib import Path
import secrets
import ssl
import subprocess
import tempfile

from service_smoke import certificates, execute, listening, require, save_config
from sql_rpc import TEXT, client, text, write


SQL_FILE_LIMIT = 64 * 1024
CONTROL_CODES = (7, 27, 31)


def store_snapshot(root):
    return {
        path.name: hashlib.sha256(path.read_bytes()).hexdigest()
        for path in root.iterdir()
        if path.is_file() and path.name != ".sdp.lock"
    }


def check_sql(server, config, root):
    before = store_snapshot(root / "data")
    source = root / "syntax.sql"
    valid = {
        "plain": b"SELECT 1",
        "typed-expression": b"SELECT TRUE AND NULL",
        "file-boundary": b"SELECT 1".ljust(SQL_FILE_LIMIT, b" "),
    }
    malformed = {
        "empty": b"",
        "incomplete-keyword": b"SELE",
        "incomplete-expression": b"SELECT 1 +",
        "incomplete-parenthesis": b"SELECT (1",
        "incomplete-quoted-value": b"SELECT 'a",
        "incomplete-predicate": b"SELECT 1 WHERE",
        "extra-closing-token": b"SELECT 1)",
        "outside-file-boundary": b"SELECT 1".ljust(SQL_FILE_LIMIT + 1, b" "),
    }
    for name, statement in valid.items():
        source.write_bytes(statement)
        result = execute(server, config, "--check-sql", source)
        require(result.returncode == 0 and json.loads(result.stdout)["compatible"],
                f"valid SQL checker case rejected: {name}")
    for name, statement in malformed.items():
        source.write_bytes(statement)
        result = execute(server, config, "--check-sql", source)
        require(result.returncode != 0 and not result.stdout and result.stderr,
                f"invalid SQL checker case lacked a bounded rejection: {name}")
    require(store_snapshot(root / "data") == before, "read-only parser checks changed persistent files")
    return {"valid": list(valid), "rejected": list(malformed), "data_unchanged": True}


def run_cli(args, root, options, stdin, secrets_to_scan, **kwargs):
    result = subprocess.run([args.cli, *options], input=stdin, cwd=root,
                            capture_output=True, timeout=25, creationflags=subprocess.CREATE_NO_WINDOW, **kwargs)
    emitted = result.stdout + result.stderr
    require(all(value.encode("ascii") not in emitted for value in secrets_to_scan),
            "CLI emitted a generated credential")
    return result


def check_terminal(args, root, port, password):
    profile = root / "profile.json"
    profile.write_text(json.dumps({
        "host": "localhost", "port": port, "user": "local-admin",
        "ca_file": "ca.pem", "timeout_ms": 12_000,
    }), encoding="utf-8")
    stdin = (password + "\n").encode("ascii")
    command = ["--profile", profile, "--password-stdin"]
    query = [*command, "--command", "SELECT note FROM boundary_rows WHERE id=1"]
    human = run_cli(args, root, query, stdin, [password], env={**os.environ, "NO_COLOR": "1"})
    require(human.returncode == 0 and not human.stderr, "human-mode query failed")
    require(all(bytes([code]) not in human.stdout for code in CONTROL_CODES),
            "human-mode output carried raw control data")
    require(all(f"\\x{code:02x}".encode("ascii") in human.stdout for code in CONTROL_CODES),
            "human-mode output did not represent the complete control-data value")

    colored = run_cli(args, root, [*command, "--status", "--color", "always"], stdin, [password])
    require(colored.returncode == 0 and b"\x1b[" in colored.stdout, "explicit color control did not activate")
    machine = run_cli(args, root, [*query, "--format", "json", "--color", "always"], stdin, [password])
    require(machine.returncode == 0 and not machine.stderr, "machine-mode query failed")
    require(all(bytes([code]) not in machine.stdout for code in CONTROL_CODES),
            "machine-mode output carried raw control bytes")
    payload = json.loads(machine.stdout)
    rows = [row for event in payload["events"] if event["type"] == "batch" for row in event["data"]["rows"]]
    require(rows == [[text(control_value())]], "machine-mode output changed typed control data")

    sample_secret = secrets.token_urlsafe(32)
    bad_profile = root / "profile-extra.json"
    bad_profile.write_text(json.dumps({"password": sample_secret}), encoding="utf-8")
    rejected = run_cli(args, root, ["--profile", bad_profile, "--format", "json", "--password-stdin"],
                       b"", [password, sample_secret])
    require(rejected.returncode == 2 and not rejected.stdout and rejected.stderr,
            "unsupported credential field was not rejected cleanly")
    return {"human_data_escaped": True, "explicit_color_available": True, "machine_typed_value_exact": True,
            "raw_machine_controls": False, "profile_credential_rejected": True, "generated_credentials_exposed": False}


def control_value():
    return "data-" + "-".join(chr(code) for code in CONTROL_CODES) + "-value"


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    for name in ("server", "openssl", "cli", "scratch", "report"):
        parser.add_argument("--" + name, type=Path, required=True)
    args = parser.parse_args()
    for name in ("server", "openssl", "cli"):
        setattr(args, name, getattr(args, name).resolve(strict=True))
    args.scratch.mkdir(parents=True, exist_ok=True)
    with tempfile.TemporaryDirectory(prefix="boundaries-", dir=args.scratch.resolve()) as workspace:
        root = Path(workspace)
        for folder in ("data", "log", "temp"):
            (root / folder).mkdir()
        certificates(args.openssl, root)
        config = save_config(root, timeout_ms=12_000, max_connections=8)
        require(execute(args.server, config, "--initialize-storage").returncode == 0,
                "could not initialize isolated data")
        parser_results = check_sql(args.server, config, root)
        password = secrets.token_urlsafe(32)
        bootstrap = execute(args.server, config, "--bootstrap-admin", "local-admin",
                            stdin=((password + "\n") * 2).encode("ascii"))
        require(bootstrap.returncode == 0 and password.encode("ascii") not in bootstrap.stdout + bootstrap.stderr,
                "bootstrap failed or exposed the generated credential")
        context = ssl.create_default_context(cafile=str(root / "ca.pem"))
        with listening(args.server, config) as (_, port):
            with client(port, context, password) as owner:
                write(owner, "CREATE TABLE boundary_rows (id BIGINT PRIMARY KEY, note TEXT)", 0)
                write(owner, "INSERT INTO boundary_rows VALUES (1,:note)", 1,
                      types={"note": TEXT}, values={"note": text(control_value())})
            terminal_results = check_terminal(args, root, port, password)
        logs = [item for item in (root / "log").iterdir() if item.is_file()]
        require(logs and all(password.encode("ascii") not in item.read_bytes() for item in logs),
                "isolated Server logs were missing or disclosed the generated credential")
    report = {"sql_parser": parser_results, "cli_terminal": terminal_results,
              "server_log_files_scanned": len(logs), "server_log_credentials_exposed": False}
    args.report.write_text(json.dumps(report, indent=2) + "\n", encoding="utf-8")
    print(f"SQL bounds {len(parser_results['valid'])}/{len(parser_results['rejected'])}; "
          "verified isolated CLI terminal rendering, typed JSON and credential redaction")


if __name__ == "__main__":
    main()