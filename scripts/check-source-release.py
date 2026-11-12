#!/usr/bin/env python3
"""Audit the distributable source tree without contacting any service."""
from pathlib import Path
import argparse
import csv
import hashlib
import io
import json
import os
import re
import sys

VERSION = "1.1.0"
TITLE = "辅助挂号软件 v" + VERSION
MANIFEST = "SOURCE_MANIFEST.json"
EXCLUDED_DIRECTORIES = {
    ".git", ".vs", ".vscode", ".idea", ".wireit", "bin", "build",
    "node_modules", "records", "output", "verification", "logs", "__pycache__",
    "debug", "release", "captures", "session", "sessions", "cookies",
}
EXCLUDED_SUFFIXES = {
    ".exe", ".dll", ".obj", ".pdb", ".log", ".lib", ".o", ".exp",
    ".ilk", ".res", ".pyc", ".zip", ".har", ".pcap", ".pcapng",
}

def excluded(relative):
    parts = relative.parts
    if any(part.lower() in EXCLUDED_DIRECTORIES or part.lower().startswith(("build-", "build_")) for part in parts[:-1]):
        return True
    name = parts[-1].lower()
    return (relative.suffix.lower() in EXCLUDED_SUFFIXES or name.startswith("context_")
            or name in {"上下文摘要.md", "thumbs.db", "desktop.ini", ".ds_store", ".qmake.stash"}
            or name.startswith("makefile") or name.startswith("moc_")
            or name == ".env" or (name.startswith(".env.") and name != ".env.example"))

def is_link(path):
    return path.is_symlink() or bool(getattr(path.stat(follow_symlinks=False), "st_file_attributes", 0) & 1024)

def inventory(root, hash_strategy="raw_bytes_sha256"):
    result = []
    for directory, dirs, names in os.walk(root, followlinks=False):
        directory = Path(directory)
        retained = []
        for name in dirs:
            relative = (directory / name).relative_to(root)
            if excluded(relative / "placeholder"):
                continue
            if is_link(directory / name):
                raise ValueError("linked directory: " + relative.as_posix())
            retained.append(name)
        dirs[:] = sorted(retained)
        for name in sorted(names):
            path = directory / name
            relative = path.relative_to(root)
            if excluded(relative) or relative.as_posix() == MANIFEST:
                continue
            if is_link(path):
                raise ValueError("linked file: " + relative.as_posix())
            data = path.read_bytes()
            if hash_strategy == "git_text_lf_sha256" and b"\0" not in data and path.suffix.lower() not in {".png", ".jpg", ".ico"}:
                data = data.replace(b"\r\n", b"\n")
            result.append({"path": relative.as_posix(), "bytes": len(data), "sha256": hashlib.sha256(data).hexdigest()})
    return sorted(result, key=lambda item: item["path"])

def require(condition, label, checks):
    checks.append({"check": label, "pass": bool(condition)})

def audit(root):
    checks = []
    manifest = json.loads((root / MANIFEST).read_text(encoding="utf-8-sig"))
    files = inventory(root, manifest.get("hash_strategy", "raw_bytes_sha256"))
    package = json.loads((root / "package.json").read_text(encoding="utf-8-sig"))
    lock = json.loads((root / "package-lock.json").read_text(encoding="utf-8-sig"))
    main = (root / "src/main.cpp").read_text(encoding="utf-8-sig")
    cmake = (root / "CMakeLists.txt").read_text(encoding="utf-8-sig")
    require(package.get("version") == lock.get("version") == lock["packages"][""].get("version") == VERSION, "version metadata", checks)
    require(package.get("license") == lock["packages"][""].get("license") == "MIT" and "MIT License" in (root / "LICENSE").read_text(encoding="utf-8-sig"), "MIT license retained", checks)
    require("VERSION " + VERSION in cmake, "CMake version", checks)
    require('setWindowTitle(QString::fromUtf8("' + TITLE + '"))' in main, "window title", checks)
    require('new QLabel(QString::fromUtf8("免责协议"))' in main and 'setObjectName("disclaimerAgreement")' in main, "first-line agreement", checks)
    require("人工确认后提交一次预约" in main and "验证码须由本人输入" in main, "manual confirmation and captcha disclosure", checks)
    require(not re.search(r"scheduled|定时", main, re.IGNORECASE), "no scheduled booking UI", checks)
    require(not any(value in main for value in ["打开官方页面查看协议", "打开官方页面", "setOpenExternalLinks", "QDesktopServices"]), "removed external agreement link absent", checks)
    for name in ["README.md", "CHANGELOG.md", "docs/免责协议.md"]:
        text = (root / name).read_text(encoding="utf-8-sig")
        require(VERSION in text, name + " version", checks)
    require(not any("context_summary" in item["path"].lower() or "上下文摘要" in item["path"] for item in files), "internal context excluded", checks)
    csv_data = (root / "data/hospitals.csv").read_bytes()
    hospitals = csv.DictReader(io.StringIO(csv_data.decode("utf-8-sig")))
    require("source_file" not in (hospitals.fieldnames or []), "local CSV provenance removed", checks)
    snapshot = json.loads((root / "data/hospital_departments_snapshot.json").read_text(encoding="utf-8-sig"))
    require(snapshot.get("source_hospital_csv_sha256") == hashlib.sha256(csv_data).hexdigest(), "snapshot CSV digest", checks)
    backend = (root / "src/native_backend.cpp").read_text(encoding="utf-8-sig")
    for symbol, name in [("FAST_HASH", "query_runtime.cjs"), ("LEGACY_QUERY_HASH", "legacy_readonly_runtime.cjs"), ("BOOKING_HASH", "booking_route_runtime.cjs")]:
        digest = hashlib.sha256((root / "runtime" / name).read_bytes()).hexdigest()
        require(symbol + '("' + digest + '")' in backend, name + " pinned digest", checks)
    credential_patterns = [
        re.compile(r"gh[pousr]_[A-Za-z0-9]{20,}|github_pat_[A-Za-z0-9_]{25,}"),
        re.compile(r"-----BEGIN (?:RSA |EC |OPENSSH )?PRIVATE KEY-----"),
        re.compile(r"(?i)(?:authorization|access_token|refresh_token|password)\s*[=:]\s*[\"'][A-Za-z0-9+/=_-]{20,}[\"']"),
        re.compile(r"[A-Za-z]:[\\/](?:Users[\\/]|抢号软件开发[\\/]|Anaconda|Program Files|Visual Studio)"),
    ]
    findings = []
    for entry in files:
        path = root / entry["path"]
        try:
            text = path.read_text(encoding="utf-8-sig")
        except UnicodeDecodeError:
            continue
        for index, pattern in enumerate(credential_patterns):
            if pattern.search(text):
                findings.append({"path": entry["path"], "rule": index + 1})
    require(not findings, "credential and local-path signatures", checks)
    manifest = json.loads((root / MANIFEST).read_text(encoding="utf-8-sig"))
    require(manifest.get("version") == VERSION and manifest.get("hash_strategy") in {"raw_bytes_sha256", "git_text_lf_sha256"}, "manifest metadata", checks)
    require(manifest.get("file_count") == len(files) and manifest.get("files") == files, "all manifest bytes and hashes", checks)
    return {"status": "PASS" if all(item["pass"] for item in checks) else "FAIL", "version": VERSION, "file_count": len(files), "network_requests": 0, "checks": checks, "findings": findings}

def main_entry():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--root", type=Path, default=Path(__file__).resolve().parents[1])
    args = parser.parse_args()
    try:
        result = audit(args.root.resolve())
    except (ValueError, OSError, KeyError) as error:
        result = {"status": "FAIL", "error": str(error), "network_requests": 0}
    print(json.dumps(result, ensure_ascii=False, indent=2))
    return 0 if result["status"] == "PASS" else 1

if __name__ == "__main__":
    sys.exit(main_entry())
