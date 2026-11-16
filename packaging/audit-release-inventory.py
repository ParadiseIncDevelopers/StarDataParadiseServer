"""Inventory the audited Windows release artifacts and report credential matches without values."""

import argparse
from collections import Counter
from datetime import datetime, timezone
import hashlib
import json
from pathlib import Path, PurePosixPath, PureWindowsPath
import re
import stat
import subprocess
import tarfile
from xml.etree import ElementTree
from zipfile import ZipFile


ARCHIVES = (".zip", ".jar", ".nupkg", ".whl", ".tar.gz", ".tgz")
TEXT_SUFFIXES = {
    ".bat", ".c", ".cfg", ".cmake", ".cmd", ".cpp", ".cs", ".gradle", ".h", ".hpp", ".ini",
    ".java", ".js", ".json", ".md", ".mjs", ".ps1", ".psm1", ".py", ".sql", ".toml", ".txt",
    ".wxs", ".xml", ".yaml", ".yml",
}
TEXT_NAMES = {"license", "notice", ".env", ".gitignore", ".gitattributes"}
MAX_TEXT = 2 * 1024 * 1024
RULES = {
    "private-key-pem": re.compile(r"^-----BEGIN (?:[A-Z0-9]+ )?PRIVATE KEY-----$"),
    "github-token": re.compile(r"\b(?:github_pat_[A-Za-z0-9_]{50,}|gh[pousr]_[A-Za-z0-9]{30,})\b"),
    "cloud-key-id": re.compile(r"\b(?:AKIA|ASIA)[A-Z0-9]{16}\b"),
    "literal-nuget-api-key": re.compile(
        r'''(?i)\b(?:nuget_api_key|nugetapikey)\b["']?\s*(?:=|:)\s*["'][a-z0-9]{42,}["']'''
    ),
}
CORE_NOTICES = {
    "Asio-1.38.2.txt": "Boost Software License",
    "nlohmann-json-3.12.0.txt": "MIT License",
    "OpenSSL-3.5.9.txt": "Apache License",
}


def check(condition, label):
    if not condition:
        raise ValueError(label)


def digest(path):
    with path.open("rb") as stream:
        return hashlib.file_digest(stream, "sha256").hexdigest()


def read_json(path):
    return json.loads(path.read_text(encoding="utf-8-sig"))


def strict_path(root, relative):
    path = (root / relative).resolve(strict=True)
    check(path.is_relative_to(root) and path.is_file(), "artifact path leaves the permitted root")
    return path


def archive_member(name):
    if "\\" in name:
        raise ValueError("archive member uses an unexpected separator")
    path = PurePosixPath(name)
    check(not PureWindowsPath(name).drive and not path.is_absolute() and ".." not in path.parts,
          "archive member leaves its root")
    return path


def text_member(name):
    return (name.suffix.lower() in TEXT_SUFFIXES or name.name.lower() in TEXT_NAMES or
            name.name in {"METADATA", "PKG-INFO", "MANIFEST.MF"} or name.suffix.lower() in {".nuspec", ".pom"})


class Scan:
    def __init__(self):
        self.count = Counter()
        self.findings = []

    def text(self, scope, name, data):
        self.count[scope] += 1
        encoding = "utf-16" if data.startswith((b"\xff\xfe", b"\xfe\xff")) else "utf-8-sig"
        for number, line in enumerate(data.decode(encoding, errors="replace").splitlines(), 1):
            for category, pattern in RULES.items():
                if pattern.search(line):
                    self.findings.append({"scope": scope, "file": name, "line": number, "rule": category})

    def repo(self, repo):
        tracked = subprocess.run(["git", "-C", repo, "ls-files", "-z"], check=True, timeout=30,
                                 stdout=subprocess.PIPE).stdout.decode("utf-8").split("\0")
        for name in filter(None, tracked):
            path = repo / name
            if (path.is_file() and not path.is_symlink() and text_member(path) and
                    path.stat().st_size <= MAX_TEXT):
                self.text("tracked-source", f"{repo.name}/{name}", path.read_bytes())

    def package(self, path, label):
        if path.name.endswith((".tar.gz", ".tgz")):
            with tarfile.open(path, "r:gz") as archive:
                for item in archive:
                    name = archive_member(item.name)
                    check(item.isdir() or item.isfile(), "package contains a nonregular member")
                    if item.isfile() and text_member(name) and item.size <= MAX_TEXT:
                        with archive.extractfile(item) as stream:
                            self.text("package-text", label + ":" + item.name, stream.read())
            return
        with ZipFile(path) as archive:
            for item in archive.infolist():
                name = archive_member(item.filename)
                check(not stat.S_ISLNK(item.external_attr >> 16), "package contains a symbolic link")
                if not item.is_dir() and text_member(name) and item.file_size <= MAX_TEXT:
                    self.text("package-text", label + ":" + item.filename, archive.read(item))


class Document:
    def __init__(self, manifest_sha):
        self.packages = []
        self.relationships = []
        self.manifest_sha = manifest_sha

    def package(self, name, version="", license_="NOASSERTION", sha=None, note=None):
        ref = f"SPDXRef-Package-{len(self.packages) + 1}"
        item = {"name": name, "SPDXID": ref, "downloadLocation": "NOASSERTION", "filesAnalyzed": False,
                "licenseConcluded": "NOASSERTION", "licenseDeclared": license_, "copyrightText": "NOASSERTION"}
        if version:
            item["versionInfo"] = version
        if sha:
            item["checksums"] = [{"algorithm": "SHA256", "checksumValue": sha}]
        if note:
            item["comment"] = note
        self.packages.append(item)
        return ref

    def relate(self, parent, relationship, child):
        self.relationships.append({"spdxElementId": parent, "relationshipType": relationship,
                                   "relatedSpdxElement": child})

    def data(self):
        return {"spdxVersion": "SPDX-2.3", "dataLicense": "CC0-1.0", "SPDXID": "SPDXRef-DOCUMENT",
                "name": "StarDataParadise Windows x64 security audit",
                "documentNamespace": "https://spdx.org/spdxdocs/StarDataParadise-stage48-" + self.manifest_sha,
                "creationInfo": {"created": datetime.now(timezone.utc).strftime("%Y-%m-%dT%H:%M:%SZ"),
                                 "creators": ["Tool: StarDataParadise release inventory"]},
                "packages": self.packages, "relationships": self.relationships}


def manifest_artifacts(node):
    if isinstance(node, dict):
        if isinstance(node.get("file"), str) and isinstance(node.get("sha256"), str):
            yield node
        for value in node.values():
            yield from manifest_artifacts(value)
    elif isinstance(node, list):
        for value in node:
            yield from manifest_artifacts(value)


def audit_core(payload, doc, products, scan):
    dependency_specs = [
        ("OpenSSL", "3.5.9", "Apache-2.0", "Statically linked TLS/cryptography"),
        ("Asio", "1.38.2", "BSL-1.0", "C++ network transport headers"),
        ("nlohmann/json", "3.12.0", "MIT", "C++ JSON implementation"),
        ("Microsoft Visual C++ Redistributable", "14.51.36231", "NOASSERTION", "Application-local x64 runtime"),
    ]
    dependency_refs = [doc.package(name, version, license_, note=note)
                       for name, version, license_, note in dependency_specs]
    qt_file = strict_path(payload, "workbench/Core/docs/licenses/qtbase-6.11.3.spdx.json")
    qt_doc = read_json(qt_file)
    check(qt_doc["spdxVersion"] == "SPDX-2.3" and len(qt_doc["packages"]) > 0, "invalid Qt vendor SPDX")
    qt_ref = doc.package("Qt Base", "6.11.3", sha=digest(qt_file),
                        note="SHA-256 refers to the supplied Qt vendor SPDX document. Its package records carry upstream license data.")
    doc.relate(products["workbench"], "DEPENDS_ON", qt_ref)
    wix = doc.package("WiX Toolset", "5.0.2", "MS-RL", note="Installer build tool, not the runtime.")
    win_owners = {}
    notice_hashes = {}
    for product, parent in products.items():
        root = (payload / product / "Core").resolve(strict=True)
        for name, marker in CORE_NOTICES.items():
            notice = strict_path(root, "docs/licenses/" + name)
            data = notice.read_bytes()
            check(marker.encode("ascii") in data, f"missing license declaration: {product}/{name}")
            item_hash = digest(notice)
            check(notice_hashes.setdefault(name, item_hash) == item_hash,
                  "core license copies differ across product payloads")
        for dependency in dependency_refs:
            doc.relate(parent, "DEPENDS_ON", dependency)
        doc.relate(wix, "BUILD_DEPENDENCY_OF", parent)
        runtime = sorted(file.name for file in root.iterdir() if re.match(r"(?:msvcp|vcruntime)\d.*\.dll$", file.name, re.I))
        check(bool(runtime), "product has no application-local C++ runtime")
        win_owners[product] = runtime
        for file in root.rglob("*"):
            if file.is_file() and text_member(file) and file.stat().st_size <= MAX_TEXT:
                scan.text("product-payload-text", file.relative_to(payload).as_posix(), file.read_bytes())
    return {"core_license_hashes": notice_hashes, "qt_vendor_spdx_packages": len(qt_doc["packages"]),
            "qt_vendor_spdx_sha256": digest(qt_file), "application_local_runtime": win_owners}


def nuget_metadata(path):
    with ZipFile(path) as archive:
        metadata = [item for item in archive.namelist() if item.endswith(".nuspec")]
        check(len(metadata) == 1, "invalid NuGet manifest count")
        root = ElementTree.fromstring(archive.read(metadata[0]))
    for element in root.iter():
        element.tag = element.tag.partition("}")[2] if "}" in element.tag else element.tag
    node = root.find("metadata")
    license_node = node.find("license")
    license_ = (license_node.text if license_node is not None and
                license_node.get("type") == "expression" else "NOASSERTION")
    return node.findtext("id"), node.findtext("version"), license_ or "NOASSERTION"


def save(path, data):
    path.write_text(json.dumps(data, ensure_ascii=False, indent=2) + "\n", encoding="utf-8")


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    for name in ("workspace", "release-manifest", "components", "payload", "framework-feed", "output", "report"):
        parser.add_argument("--" + name, type=Path, required=True)
    parser.add_argument("--repo", action="append", type=Path, required=True)
    args = parser.parse_args()
    workspace = args.workspace.resolve(strict=True)
    payload = args.payload.resolve(strict=True)
    release = read_json(args.release_manifest)
    components = read_json(args.components)
    scan = Scan()
    doc = Document(digest(args.release_manifest))
    artifacts = {}
    owners = {}

    def artifact(path, scope, expected=None):
        name = path.relative_to(workspace).as_posix()
        if name not in artifacts:
            sha = digest(path)
            check(expected is None or sha == expected, "release checksum mismatch")
            artifacts[name] = {"sha256": sha, "bytes": path.stat().st_size, "scope": scope}
            suffix = path.name.lower()
            if suffix.endswith(ARCHIVES):
                scan.package(path, name)
        elif expected:
            check(artifacts[name]["sha256"] == expected, "repeated manifest checksum mismatch")
        return artifacts[name]["sha256"]

    for item in manifest_artifacts({"products": release["products"], "sdks": release["sdks"]}):
        artifact(strict_path(workspace, item["file"]), "release-manifest", item["sha256"])
    for item in components["components"]:
        candidate = strict_path(args.components.parent.resolve(), item["artifact"])
        check(candidate.stat().st_size == item["downloadBytes"], "component archive size mismatch")
        artifact(candidate, "component-manifest", item["sha256"])

    for product, item in release["products"].items():
        parent = doc.package(f"StarDataParadise {product} MSI", item["version"], sha=item["msi"]["sha256"])
        doc.relate("SPDXRef-DOCUMENT", "DESCRIBES", parent)
        owners[product] = parent
    license_evidence = audit_core(payload, doc, owners, scan)

    sdk_root = (payload / "workbench" / "Sdk" / "sdk").resolve(strict=True)
    sdk_refs = {}
    for file in sorted(sdk_root.iterdir()):
        if not file.is_file() or not file.name.lower().endswith(ARCHIVES):
            continue
        sha = artifact(file, "workbench-sdk-feature")
        if file.suffix.lower() == ".nupkg":
            product, version, license_ = nuget_metadata(file)
        else:
            product, version, license_ = file.name, "", "NOASSERTION"
        child = doc.package(product, version, license_, sha)
        doc.relate(owners["workbench"], "CONTAINS", child)
        sdk_refs[file.name] = child
    check(len(sdk_refs) >= 6, "expected installed Native/.NET/JDBC/Python/Node SDK archives")

    framework = next(child for name, child in sdk_refs.items() if name.startswith("StarDataParadiseNetFramework."))
    managed_license_map = {}
    for file in sorted(args.framework_feed.resolve(strict=True).rglob("*.nupkg")):
        product, version, license_ = nuget_metadata(file)
        check(product not in managed_license_map, "duplicate Framework dependency")
        child = doc.package(product, version, license_, artifact(file, "external-framework-feed"))
        doc.relate(framework, "DEPENDS_ON", child)
        managed_license_map[product] = {"version": version, "license": license_}
    check("System.Text.Json" in managed_license_map, "missing Framework runtime dependency metadata")

    jdbc = next(child for name, child in sdk_refs.items() if name.startswith("stardataparadise-jdbc-"))
    doc.relate(jdbc, "DEPENDS_ON", doc.package("Gson", "2.13.2", "Apache-2.0",
                                            note="External Maven runtime dependency declared by the JDBC POM."))
    for repo in args.repo:
        scan.repo(repo.resolve(strict=True))
    summary = {"manifest_sha256": digest(args.release_manifest), "artifacts": artifacts,
               "license_evidence": license_evidence, "framework_dependency_licenses": managed_license_map,
               "spdx_packages": len(doc.packages), "scan_scope": dict(scan.count),
               "credential_findings": scan.findings, "value_redaction": "findings contain only scope, file, line and rule"}
    save(args.output, doc.data())
    save(args.report, summary)
    print(f"{len(artifacts)} artifacts, {len(doc.packages)} SPDX packages, "
          f"{sum(scan.count.values())} scanned texts, {len(scan.findings)} credential findings")
    if scan.findings:
        raise SystemExit(1)


if __name__ == "__main__":
    main()