"""Build the v1 contract archive independently of paths, timestamps and line endings."""

import argparse
import hashlib
from io import BytesIO
from pathlib import Path
import zipfile


def archive(source: Path) -> bytes:
    buffer = BytesIO()
    with zipfile.ZipFile(buffer, "w", compression=zipfile.ZIP_STORED) as output:
        for path in sorted(source.rglob("*")):
            if path.is_file():
                entry = zipfile.ZipInfo(path.relative_to(source).as_posix())
                entry.create_system = 3
                entry.external_attr = 0o100644 << 16
                output.writestr(entry, path.read_text(encoding="utf-8").encode("utf-8"))
    return buffer.getvalue()


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("output", type=Path)
    parser.add_argument("--check", action="store_true")
    args = parser.parse_args()
    source = Path(__file__).resolve().parent.parent / "v1"
    content = archive(source)
    digest = hashlib.sha256(content).hexdigest()

    if args.check:
        if content != archive(source) or args.output.read_bytes() != content:
            raise SystemExit("contract archive is not reproducible")
    else:
        args.output.parent.mkdir(parents=True, exist_ok=True)
        args.output.write_bytes(content)
        args.output.with_suffix(".zip.sha256").write_text(
            f"{digest}  {args.output.name}\n", encoding="ascii", newline="\n"
        )
    print(f"{digest}  {args.output.name}")


if __name__ == "__main__":
    main()
