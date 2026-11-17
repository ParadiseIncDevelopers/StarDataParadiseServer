"""Run the Backup, jobs, PL/SQL and model acceptance against isolated Server instances."""

from __future__ import annotations

import argparse
from pathlib import Path

from . import backup_acceptance, compute_jobs_acceptance, models_acceptance, plsql_acceptance, shell_js
from .fixture import BASEDIR, BIN, SHELL, WORKER, Fixture


CASES = {
    "backup": backup_acceptance.verify,
    "jobs": compute_jobs_acceptance.verify,
    "plsql": plsql_acceptance.verify,
    "models": models_acceptance.verify,
    "shell-js": shell_js.verify,
}
DEFAULT_CASES = ("backup", "jobs", "plsql", "models")


def artifact(parser, path, label, *, directory=False):
    try:
        path = path.resolve(strict=True)
    except OSError:
        parser.error(f"{label} path does not exist: {path}")
    if directory and not path.is_dir() or not directory and not path.is_file():
        parser.error(f"{label} has an unexpected filesystem type: {path}")
    return path


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--case", choices=CASES, action="append",
                        help="run a selected case, repeat to select several (default excludes shell-js)")
    parser.add_argument("--mysqld", type=Path, default=BIN / "mysqld.exe")
    parser.add_argument("--mysql", type=Path, default=BIN / "mysql.exe")
    parser.add_argument("--basedir", type=Path, default=BASEDIR)
    parser.add_argument("--shell", type=Path, default=SHELL)
    parser.add_argument("--worker", type=Path, default=WORKER)
    args = parser.parse_args()
    cases = tuple(dict.fromkeys(args.case or DEFAULT_CASES))
    fixture = Fixture(
        artifact(parser, args.mysqld, "Server executable"),
        artifact(parser, args.mysql, "classic client"),
        artifact(parser, args.basedir, "Server basedir", directory=True),
        artifact(parser, args.shell, "Shell executable"),
        artifact(parser, args.worker, "model worker") if "models" in cases else None,
    )
    for name in cases:
        print(f"Running {name} acceptance", flush=True)
        CASES[name](fixture)


if __name__ == "__main__":
    main()
