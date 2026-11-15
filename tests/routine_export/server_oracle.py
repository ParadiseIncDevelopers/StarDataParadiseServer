#!/usr/bin/env python3
"""Record the frozen Server oracle for Stage 35 vectors. An exporter is not needed."""

import argparse
import json
from pathlib import Path
import secrets
import ssl
import sys
import tempfile

sys.dont_write_bytecode = True
for directory in Path(__file__).resolve().parents:
    tests = directory if (directory / "service_smoke.py").is_file() else directory / "tests"
    if (tests / "service_smoke.py").is_file():
        sys.path.insert(0, str(tests))
        break

from service_smoke import certificates, execute, listening, require, save_config
from sql_rpc import client, write


def collect(connection, result, vector):
    expected = vector["expected"]
    if "columns" in expected:
        require(result["columns"] == expected["columns"], f'{vector["id"]}: wrong columns')
    rows = []
    for index in range(64):
        batch = connection.call("fetch", result_id=result["result_id"], batch_rows=4)["batch"]
        require(batch["result_set"] == 0 and batch["batch_index"] == index,
                f'{vector["id"]}: unexpected result order')
        if index == 0 and "set_name" in expected:
            require(batch["set_name"] == expected["set_name"], f'{vector["id"]}: wrong set name')
        rows.extend(batch["rows"])
        if batch["end_of_results"]:
            return {"rows": rows}
    raise AssertionError(f'{vector["id"]}: result limit exceeded')


def observe(connection, vector):
    with connection.prepared(vector["sql"], vector["parameter_types"]) as statement:
        reply = connection.raw("execute", statement_id=statement, parameters=vector["parameters"])
        if reply["ok"]:
            return collect(connection, reply["body"], vector)
        error = reply["error"]
        return {key: error[key] for key in ("code", "line", "column", "pl_exception") if key in error}


def main():
    cli = argparse.ArgumentParser(description=__doc__)
    for field in ("server", "openssl", "scratch", "observed"):
        cli.add_argument(f"--{field}", required=True, type=Path)
    options = cli.parse_args()
    server = str(options.server.resolve(strict=True))
    ssl_tool = str(options.openssl.resolve(strict=True))
    scratch = options.scratch.resolve()
    observed_file = options.observed.resolve()
    require(observed_file.is_relative_to(scratch), "observations must stay in scratch")
    scratch.mkdir(parents=True, exist_ok=True)
    fixture = json.loads(Path(__file__).with_name("vectors.json").read_text(encoding="utf-8"))
    output = []
    with tempfile.TemporaryDirectory(prefix="export-baseline-", dir=scratch) as directory:
        root = Path(directory)
        for child in ("data", "log", "temp"):
            (root / child).mkdir()
        certificates(ssl_tool, root)
        config = save_config(root, timeout_ms=15_000, max_connections=3)
        require(execute(server, config, "--initialize-storage").returncode == 0, "fixture storage failed")
        secret = secrets.token_urlsafe(32)
        credentials = (f"{secret}\n" * 2).encode("ascii")
        require(execute(server, config, "--bootstrap-admin", "local-admin", stdin=credentials).returncode == 0,
                "fixture administrator failed")
        tls = ssl.create_default_context(cafile=str(root / "ca.pem"))
        with listening(server, config) as (_, port), client(port, tls, secret) as connection:
            for definition in fixture["fixtures"]:
                try:
                    write(connection, definition["sql"], definition["affected_rows"])
                except RuntimeError as problem:
                    raise RuntimeError(f'{definition["name"]} setup rejected: {problem}') from problem
            for vector in fixture["vectors"]:
                observed = observe(connection, vector)
                expected = vector["expected"]
                for key in ("rows", "pl_exception"):
                    if key in expected:
                        require(observed.get(key) == expected[key], f'{vector["id"]}: oracle mismatch {observed}')
                if "pl_exception" in expected:
                    require(observed.get("line", 0) > 0 and observed.get("column", 0) > 0,
                            f'{vector["id"]}: source coordinates absent')
                output.append({"id": vector["id"], "observed": observed})
    observed_file.write_text(json.dumps({"format": 1, "vectors": output}, indent=2) + "\n", encoding="utf-8")
    print(f"Stage 35 Server-side oracle: {len(output)} expected vectors matched")


if __name__ == "__main__":
    main()
