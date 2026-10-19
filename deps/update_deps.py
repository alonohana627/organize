#!/usr/bin/env python3
"""Fetch or update the vendored third-party sources listed in deps/manifest.json.

Usage:
    python deps/update_deps.py            # refresh every dependency at its pinned ref
    python deps/update_deps.py sqlite     # refresh only the named dependencies
    python deps/update_deps.py --check    # verify every vendored file exists; no network

To upgrade a dependency, edit its "ref"/"version" (and "url"/"sha256" for archives and downloads) in
manifest.json, run this script for it, rebuild, run the tests, then review the diff.

This is a developer bootstrap tool. The shipped application never downloads anything.
Only the files named in the manifest are copied; nothing fetched is executed.
"""
from __future__ import annotations

import argparse
import fnmatch
import hashlib
import json
import shutil
import subprocess
import sys
import tempfile
import urllib.request
import zipfile
from pathlib import Path

DEPS_DIR = Path(__file__).resolve().parent
REPOSITORY_ROOT = DEPS_DIR.parent
MANIFEST = DEPS_DIR / "manifest.json"


def load_manifest() -> list[dict]:
    with MANIFEST.open(encoding="utf-8") as handle:
        return json.load(handle)["dependencies"]


def fetch_git(dependency: dict, workdir: Path) -> Path:
    """Shallow-clones the pinned tag or commit and returns the checkout directory."""
    checkout = workdir / dependency["name"]
    subprocess.run(
        ["git", "-c", "advice.detachedHead=false", "clone", "--quiet", "--depth", "1",
         "--branch", dependency["ref"], dependency["url"], str(checkout)],
        check=True,
    )
    expected_commit = dependency.get("commit")
    if expected_commit:
        actual = subprocess.run(["git", "-C", str(checkout), "rev-parse", "HEAD"], check=True,
                                capture_output=True, text=True).stdout.strip()
        if actual != expected_commit:
            raise SystemExit(f"{dependency['name']}: {dependency['ref']} is at {actual}, manifest pins {expected_commit}")
    return checkout


def fetch_archive(dependency: dict, workdir: Path) -> Path:
    """Downloads a zip archive, verifies its SHA-256 and extracts it."""
    archive = workdir / "archive.zip"
    with urllib.request.urlopen(dependency["url"]) as response, archive.open("wb") as out:
        shutil.copyfileobj(response, out)
    digest = hashlib.sha256(archive.read_bytes()).hexdigest()
    if digest != dependency["sha256"]:
        raise SystemExit(f"{dependency['name']}: sha256 mismatch: expected {dependency['sha256']}, got {digest}")
    extracted = workdir / "extracted"
    with zipfile.ZipFile(archive) as bundle:
        bundle.extractall(extracted)
    return extracted


def fetch_downloads(dependency: dict) -> None:
    """Downloads single files straight to their destinations, verifying pinned SHA-256 digests."""
    for download in dependency["downloads"]:
        with urllib.request.urlopen(download["url"]) as response:
            payload = response.read()
        expected = download.get("sha256")
        if expected and hashlib.sha256(payload).hexdigest() != expected:
            raise SystemExit(f"{dependency['name']}: sha256 mismatch for {download['url']}")
        target = DEPS_DIR / download["destination"]
        target.parent.mkdir(parents=True, exist_ok=True)
        target.write_bytes(payload)
        print(f"  {download['destination']}")


def copy_files(dependency: dict, source_root: Path) -> None:
    """Copies each manifest-listed file (glob patterns allowed on the source side)."""
    for pattern, destination in dependency["files"].items():
        matches = [p for p in source_root.rglob("*") if p.is_file()
                   and fnmatch.fnmatch(p.relative_to(source_root).as_posix(), pattern)]
        if len(matches) != 1:
            raise SystemExit(f"{dependency['name']}: pattern {pattern!r} matched {len(matches)} files")
        target = DEPS_DIR / destination
        target.parent.mkdir(parents=True, exist_ok=True)
        shutil.copyfile(matches[0], target)
        print(f"  {destination}")


def apply_patches(dependency: dict) -> None:
    """Re-applies our local modifications. Vendored sources are never edited by hand: every change
    lives as a patch under patches/<name>/ (paths relative to the repository root) and is listed in
    the dependency's "patches" array, applied in order."""
    for patch in dependency.get("patches", []):
        subprocess.run(["git", "apply", "--whitespace=nowarn", str(REPOSITORY_ROOT / patch)],
                       cwd=REPOSITORY_ROOT, check=True)
        print(f"  patched with {patch}")


def update(dependency: dict) -> None:
    print(f"{dependency['name']} {dependency['version']}")
    if dependency["kind"] == "download":
        fetch_downloads(dependency)
        apply_patches(dependency)
        return
    with tempfile.TemporaryDirectory() as temporary:
        workdir = Path(temporary)
        if dependency["kind"] == "git":
            source_root = fetch_git(dependency, workdir)
        elif dependency["kind"] == "archive":
            source_root = fetch_archive(dependency, workdir)
        else:
            raise SystemExit(f"{dependency['name']}: unknown kind {dependency['kind']!r}")
        copy_files(dependency, source_root)
    apply_patches(dependency)


def check(dependencies: list[dict]) -> int:
    destinations = [(d["name"], dest) for d in dependencies for dest in d["files"].values()]
    destinations += [(d["name"], item["destination"]) for d in dependencies for item in d.get("downloads", [])]
    missing = [f"{name}: {dest}" for name, dest in destinations if not (DEPS_DIR / dest).is_file()]
    missing += [f"{d['name']}: patch {patch}" for d in dependencies for patch in d.get("patches", [])
                if not (REPOSITORY_ROOT / patch).is_file()]
    for entry in missing:
        print(f"missing {entry}")
    print("all vendored files present" if not missing else f"{len(missing)} file(s) missing")
    return 1 if missing else 0


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("names", nargs="*", help="dependencies to refresh (default: all)")
    parser.add_argument("--check", action="store_true", help="only verify that vendored files exist")
    arguments = parser.parse_args()

    dependencies = load_manifest()
    if arguments.check:
        return check(dependencies)
    known = {d["name"] for d in dependencies}
    unknown = set(arguments.names) - known
    if unknown:
        raise SystemExit(f"unknown dependencies: {', '.join(sorted(unknown))}; known: {', '.join(sorted(known))}")
    for dependency in dependencies:
        if not arguments.names or dependency["name"] in arguments.names:
            update(dependency)
    return 0


if __name__ == "__main__":
    sys.exit(main())
