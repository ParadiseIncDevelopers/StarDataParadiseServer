"""Exercise History storage and terminate only dedicated fixture subprocesses."""

import argparse
from pathlib import Path
import subprocess
import tempfile


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--runner", required=True, type=Path)
    parser.add_argument("--scratch", required=True, type=Path)
    args = parser.parse_args()
    args.scratch.mkdir(parents=True, exist_ok=True)
    directory = Path(tempfile.mkdtemp(prefix="run-", dir=args.scratch))
    cases = (
        ("normal", "policy", 0),
        ("tamper", "forged-seal", 0),
        ("bad-ledger", "wrong-ledger", 0),
        ("seed", "interrupted-before", 0),
        ("kill-before", "interrupted-before", 73),
        ("verify-before", "interrupted-before", 0),
        ("seed", "interrupted-after", 0),
        ("kill-after", "interrupted-after", 73),
        ("verify-after", "interrupted-after", 0),
        ("delete-seed", "restore-before", 0),
        ("kill-restore-before", "restore-before", 73),
        ("verify-restore-before", "restore-before", 0),
        ("delete-seed", "restore-after", 0),
        ("kill-restore-after", "restore-after", 73),
        ("verify-restore-after", "restore-after", 0),
    )
    for mode, name, expected in cases:
        process = subprocess.run(
            [args.runner, mode, directory / f"{name}.sdp"],
            capture_output=True,
            text=True,
            timeout=20,
        )
        if process.returncode != expected:
            raise AssertionError(f"{mode}: exit {process.returncode}, expected {expected}: "
                                 f"{process.stdout} {process.stderr}")
        print(f"{mode}: exit {expected}")


if __name__ == "__main__":
    main()
