"""Check exclusive explicit initialization and read-only inspection using the Server EXE."""

import argparse
import hashlib
import json
from pathlib import Path
import subprocess
import tempfile


def require(condition, message):
    if not condition:
        raise RuntimeError(message)


def digest(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--server", required=True, type=Path)
    parser.add_argument("--scratch", required=True, type=Path)
    args = parser.parse_args()
    args.scratch.mkdir(parents=True, exist_ok=True)
    with tempfile.TemporaryDirectory(prefix="ownership-", dir=args.scratch.resolve()) as folder:
        root = Path(folder)
        for name in ("data", "log", "tmp"):
            (root / name).mkdir()
        # These local commands never initialize a TLS context.
        (root / "tls-reference").touch()
        config = root / "server.json"
        config.write_text(json.dumps(dict(data_dir="data", log_dir="log", temp_dir="tmp",
                                          tls_certificate="tls-reference", tls_private_key="tls-reference")),
                          encoding="utf-8")

        def run(mode):
            return subprocess.run([args.server, "--config", config, mode], capture_output=True, text=True, timeout=10)

        user_file = root / "data" / "keep.bin"
        user_file.write_bytes(b"unrelated fixture bytes")
        old_hash = digest(user_file)
        refused = run("--initialize-storage")
        data = root / "data" / "store.sdp"
        require(refused.returncode == 1 and not data.exists() and digest(user_file) == old_hash,
                "initialization changed a nonempty data directory")
        user_file.unlink()

        init = run("--initialize-storage")
        require(init.returncode == 0 and data.stat().st_size == 4096, "explicit initialization failed")
        original = digest(data)
        require(run("--initialize-storage").returncode == 1 and digest(data) == original,
                "existing storage was overwritten")
        inspected = run("--check-storage")
        require(inspected.returncode == 0 and inspected.stdout.startswith("Storage OK: 0 rows")
                and digest(data) == original, "integrity check did not read the original storage file")

    print("explicit initialization, existing file protection and read-only integrity command passed")


if __name__ == "__main__":
    main()
