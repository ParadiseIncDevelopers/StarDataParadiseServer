"""Exercise the read-only catalog and SQL compatibility commands of the Server."""
import argparse
import hashlib
import json
from pathlib import Path
import subprocess
import tempfile


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--server", required=True, type=Path)
    parser.add_argument("--scratch", required=True, type=Path)
    args = parser.parse_args()
    args.scratch.mkdir(parents=True, exist_ok=True)
    with tempfile.TemporaryDirectory(dir=args.scratch, prefix="checker-") as folder:
        root = Path(folder)
        data = root / "data"
        for name in ("data", "log", "tmp"):
            (root / name).mkdir()
        (root / "tls-reference").touch()
        config = root / "server.json"
        config.write_text(json.dumps(dict(data_dir="data", log_dir="log", temp_dir="tmp",
                                          tls_certificate="tls-reference", tls_private_key="tls-reference")),
                          encoding="utf-8")

        def run(*args_for_server, check=True):
            return subprocess.run([args.server, "--config", config, *args_for_server], capture_output=True,
                                  encoding="utf-8", check=check, timeout=10)

        def snapshot():
            return {file.name: hashlib.sha256(file.read_bytes()).hexdigest()
                    for file in data.iterdir() if file.name != ".sdp.lock"}

        run("--initialize-storage")
        original = snapshot()
        metadata = json.loads(run("--catalog-info").stdout)
        assert ["main", "public"] in [item["qualified_name"] for item in metadata["objects"]], metadata
        source = root / "check.sql"
        source.write_text("CREATE TABLE checked (id BIGINT NOT NULL)", encoding="utf-8")
        assert json.loads(run("--check-sql", source).stdout) == {"compatible": True, "kind": "ddl"}
        source.write_text("SELECT TRUE AND NULL, 'İstanbul'", encoding="utf-8")
        typed = json.loads(run("--check-sql", source).stdout)
        assert typed["kind"] == "select" and len(typed["columns"]) == 2, typed
        source.write_text("SELECT 1\nWHERE )", encoding="utf-8")
        error = run("--check-sql", source, check=False)
        assert error.returncode != 0 and "SQL 2:" in error.stderr, error.stderr
        assert snapshot() == original, "Read-only checks modified data or WAL on disk"


if __name__ == "__main__":
    main()
