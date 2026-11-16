import argparse
from pathlib import Path


def replace_once(source, signature, updated):
    if source.count(signature) != 1:
        raise ValueError(f"Expected one adapter anchor: {signature!r}")
    return source.replace(signature, updated, 1)


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("language", choices=("dotnet", "java"))
    parser.add_argument("source", type=Path)
    parser.add_argument("output", type=Path)
    args = parser.parse_args()
    overlay = Path(__file__).parent / (args.language + "-observations.txt")
    observations = overlay.read_text(encoding="utf-8")
    source = args.source.read_text(encoding="utf-8")
    if args.language == "dotnet":
        anchor = 'Console.WriteLine("SDP_MANAGED_OK");'
        source = replace_once(source, anchor, observations + "\n" + anchor)
    else:
        invocation = "                extensions(admin);"
        source = replace_once(source, invocation, invocation + "\n                observeStage47(admin, owner);")
        method = "    private static void extensions(SdpConnection connection) throws Exception {"
        source = replace_once(source, method, observations + "\n" + method)
    args.output.write_text(source, encoding="utf-8")


if __name__ == "__main__":
    main()