#!/usr/bin/env python3
"""Fetch, update and verify the vendored third-party sources listed in deps/manifest.json.

Usage:
    python deps/update_deps.py                    # re-fetch every dependency at its pinned commit/digest
    python deps/update_deps.py sqlite cli11       # re-fetch only the named dependencies
    python deps/update_deps.py --check            # offline: verify deps/ against the manifest (CI runs this)
    python deps/update_deps.py --verify-upstream  # fetch into a temporary directory and compare; changes nothing

Pins: a git dependency is fetched by its exact "commit" (its "ref" tag must still point there); an
archive or single-file download must match its "sha256". Everything is fetched, verified and patched
in a staging directory first and only then copied into deps/, so a failed download or patch never
leaves a half-updated tree. Files that a dependency no longer lists are deleted.

After a fetch the script records "content_sha256" (the post-patch digest of every vendored file) in
the manifest. --check verifies those digests offline, so a hand edit of vendored code (owner rule 2)
or a patch that was not applied fails, and it reports files in a dependency directory that the
manifest does not produce.

To upgrade a dependency, change its "ref" and "commit" (or "url" and "sha256") and "version", run this
script for it, rebuild, run the tests, then review the diff.

This is a developer bootstrap tool. The shipped application never downloads anything, and nothing
fetched is executed.
"""
from __future__ import annotations

import argparse
import fnmatch
import hashlib
import json
import os
import re
import shutil
import subprocess
import sys
import tempfile
import urllib.error
import urllib.request
import zipfile
from pathlib import Path

DEPS_DIR = Path(__file__).resolve().parent
REPOSITORY_ROOT = DEPS_DIR.parent
MANIFEST = DEPS_DIR / "manifest.json"
NETWORK_TIMEOUT_SECONDS = 60
COMMIT_PATTERN = re.compile(r"^[0-9a-f]{40}$")
SHA256_PATTERN = re.compile(r"^[0-9a-f]{64}$")
# Project files directly in deps/ that the manifest does not produce. miniaudio_impl.c is the project's
# single translation unit compiling the vendored miniaudio and stb_vorbis implementations (see
# deps/README.md); .clang-format keeps formatters away from vendored code.
TOOLING_FILES = {"CMakeLists.txt", "README.md", "manifest.json", "update_deps.py", "miniaudio_impl.c", ".clang-format"}


class DependencyError(Exception):
    """A dependency could not be fetched, verified or patched."""


def load_manifest() -> dict:
    with MANIFEST.open(encoding="utf-8") as handle:
        return json.load(handle)


def save_manifest(manifest: dict) -> None:
    """Rewrites the manifest atomically (temporary file, then rename)."""
    temporary = MANIFEST.with_name(MANIFEST.name + ".tmp")
    temporary.write_text(json.dumps(manifest, indent=2, ensure_ascii=False) + "\n", encoding="utf-8", newline="\n")
    os.replace(temporary, MANIFEST)


def sha256_of(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as handle:
        for block in iter(lambda: handle.read(1 << 20), b""):
            digest.update(block)
    return digest.hexdigest()


def destinations(dependency: dict) -> list[str]:
    """Every deps/-relative path the dependency produces."""
    produced = list(dependency.get("files", {}).values())
    produced += [item["destination"] for item in dependency.get("downloads", [])]
    return produced


def download(url: str) -> bytes:
    with urllib.request.urlopen(url, timeout=NETWORK_TIMEOUT_SECONDS) as response:
        return response.read()


def run_git(*arguments: str, cwd: Path | None = None) -> str:
    # core.autocrlf=false: vendored files are the upstream bytes, whatever the developer's git config.
    completed = subprocess.run(["git", "-c", "advice.detachedHead=false", "-c", "core.autocrlf=false", *arguments],
                               cwd=cwd, check=False,
                               capture_output=True, text=True, timeout=600)
    if completed.returncode != 0:
        raise DependencyError(f"git {' '.join(arguments)} failed: {completed.stderr.strip()}")
    return completed.stdout


# ---------------------------------------------------------------- fetching into a staging directory

def verify_ref_points_to_commit(dependency: dict) -> None:
    """A pinned tag must still name the pinned commit (a moved tag is reported, never followed). A branch
    ref (e.g. pocketfft's "cpp") may move on; the commit pin alone decides what is fetched."""
    ref, commit = dependency["ref"], dependency["commit"]
    listing = run_git("ls-remote", dependency["url"], f"refs/tags/{ref}", f"refs/tags/{ref}^{{}}", f"refs/heads/{ref}")
    references = {}
    for line in listing.splitlines():
        object_name, _, reference = line.partition("\t")
        references[reference] = object_name
    tag_commit = references.get(f"refs/tags/{ref}^{{}}") or references.get(f"refs/tags/{ref}")
    if tag_commit is not None and tag_commit != commit:
        raise DependencyError(f"{dependency['name']}: tag {ref} now points to {tag_commit}, manifest pins {commit}")
    if tag_commit is None and f"refs/heads/{ref}" not in references:
        raise DependencyError(f"{dependency['name']}: ref {ref} not found at {dependency['url']}")


def fetch_git(dependency: dict, workdir: Path) -> Path:
    """Fetches exactly the pinned commit (shallow) and returns the checkout directory."""
    verify_ref_points_to_commit(dependency)
    checkout = workdir / "checkout"
    checkout.mkdir()
    run_git("init", "--quiet", cwd=checkout)
    run_git("fetch", "--quiet", "--depth", "1", dependency["url"], dependency["commit"], cwd=checkout)
    run_git("checkout", "--quiet", "FETCH_HEAD", cwd=checkout)
    actual = run_git("rev-parse", "HEAD", cwd=checkout).strip()
    if actual != dependency["commit"]:
        raise DependencyError(f"{dependency['name']}: fetched {actual}, manifest pins {dependency['commit']}")
    return checkout


def fetch_archive(dependency: dict, workdir: Path) -> Path:
    """Downloads a zip archive, verifies its SHA-256 and extracts it (zipfile drops '..' components)."""
    payload = download(dependency["url"])
    digest = hashlib.sha256(payload).hexdigest()
    if digest != dependency["sha256"]:
        raise DependencyError(f"{dependency['name']}: archive sha256 {digest}, manifest pins {dependency['sha256']}")
    archive = workdir / "archive.zip"
    archive.write_bytes(payload)
    extracted = workdir / "extracted"
    with zipfile.ZipFile(archive) as bundle:
        bundle.extractall(extracted)
    return extracted


def stage_files(dependency: dict, source_root: Path, stage: Path) -> None:
    """Copies each manifest-listed file (glob patterns allowed on the source side) into the stage."""
    for pattern, destination in dependency.get("files", {}).items():
        matches = [path for path in source_root.rglob("*") if path.is_file()
                   and fnmatch.fnmatch(path.relative_to(source_root).as_posix(), pattern)]
        if len(matches) != 1:
            raise DependencyError(f"{dependency['name']}: pattern {pattern!r} matched {len(matches)} files")
        target = stage / "deps" / destination
        target.parent.mkdir(parents=True, exist_ok=True)
        shutil.copyfile(matches[0], target)


def stage_downloads(dependency: dict, stage: Path) -> None:
    """Downloads single files into the stage, verifying each pinned SHA-256."""
    for item in dependency.get("downloads", []):
        payload = download(item["url"])
        digest = hashlib.sha256(payload).hexdigest()
        if digest != item["sha256"]:
            raise DependencyError(f"{dependency['name']}: {item['url']} has sha256 {digest}, manifest pins {item['sha256']}")
        target = stage / "deps" / item["destination"]
        target.parent.mkdir(parents=True, exist_ok=True)
        target.write_bytes(payload)


def normalize_newlines(dependency: dict, stage: Path) -> None:
    """With "normalize_newlines": true, converts CRLF to LF in the staged files before patching (used
    where upstream ships CRLF files but the vendored copy and its patches are LF)."""
    if not dependency.get("normalize_newlines", False):
        return
    for destination in destinations(dependency):
        path = stage / "deps" / destination
        path.write_bytes(path.read_bytes().replace(b"\r\n", b"\n"))


def apply_patches(dependency: dict, stage: Path) -> None:
    """Applies the dependency's patches (paths relative to the repository root) to the staged copy.
    Vendored sources are never edited by hand: every local change is a patch under patches/<name>/."""
    for patch in dependency.get("patches", []):
        completed = subprocess.run(["git", "-c", "core.autocrlf=false", "apply", "--whitespace=nowarn",
                                    str(REPOSITORY_ROOT / patch)],
                                   cwd=stage, check=False, capture_output=True, text=True, timeout=120)
        if completed.returncode != 0:
            raise DependencyError(f"{dependency['name']}: patch {patch} does not apply: {completed.stderr.strip()}")


def stage_dependency(dependency: dict, workdir: Path) -> Path:
    """Fetches, verifies and patches one dependency into `workdir/stage/deps/...`; returns the stage."""
    stage = workdir / "stage"
    (stage / "deps").mkdir(parents=True)
    kind = dependency["kind"]
    if kind == "git":
        stage_files(dependency, fetch_git(dependency, workdir), stage)
    elif kind == "archive":
        stage_files(dependency, fetch_archive(dependency, workdir), stage)
    elif kind != "download":
        raise DependencyError(f"{dependency['name']}: unknown kind {kind!r}")
    stage_downloads(dependency, stage)
    normalize_newlines(dependency, stage)
    apply_patches(dependency, stage)
    return stage


# ---------------------------------------------------------------- update and upstream comparison

def remove_empty_parents(path: Path) -> None:
    for parent in path.parents:
        if parent == DEPS_DIR or not parent.is_relative_to(DEPS_DIR):
            return
        try:
            parent.rmdir()
        except OSError:
            return


def update(dependency: dict) -> None:
    """Stages the dependency, copies it into deps/, deletes files it no longer lists, records digests."""
    print(f"{dependency['name']} {dependency['version']}")
    produced = destinations(dependency)
    with tempfile.TemporaryDirectory(prefix="mso-deps-") as temporary:
        stage = stage_dependency(dependency, Path(temporary))
        digests = {destination: sha256_of(stage / "deps" / destination) for destination in produced}
        for destination in produced:
            target = DEPS_DIR / destination
            target.parent.mkdir(parents=True, exist_ok=True)
            shutil.copyfile(stage / "deps" / destination, target)
            print(f"  {destination}")
    for dropped in sorted(set(dependency.get("content_sha256", {})) - set(produced)):
        stale = DEPS_DIR / dropped
        if stale.is_file():
            stale.unlink()
            remove_empty_parents(stale)
            print(f"  removed {dropped} (no longer listed)")
    dependency["content_sha256"] = dict(sorted(digests.items()))


def verify_upstream(dependency: dict) -> list[str]:
    """Fetches into a temporary directory and lists the vendored files that differ from it."""
    differences = []
    with tempfile.TemporaryDirectory(prefix="mso-deps-") as temporary:
        stage = stage_dependency(dependency, Path(temporary))
        for destination in destinations(dependency):
            vendored, upstream = DEPS_DIR / destination, stage / "deps" / destination
            if not vendored.is_file():
                differences.append(f"{dependency['name']}: {destination} is missing")
                continue
            vendored_bytes, upstream_bytes = vendored.read_bytes(), upstream.read_bytes()
            if vendored_bytes == upstream_bytes:
                continue
            same_text = vendored_bytes.replace(b"\r\n", b"\n") == upstream_bytes.replace(b"\r\n", b"\n")
            reason = "line endings only" if same_text else "content"
            differences.append(f"{dependency['name']}: {destination} differs from the pinned upstream ({reason})")
    return differences


# ---------------------------------------------------------------- offline check

def check_entry(dependency: dict) -> list[str]:
    """Problems in one manifest entry (missing fields or pins)."""
    name = dependency.get("name", "<unnamed>")
    problems = [f"{name}: missing '{field}'" for field in ("name", "purpose", "version", "license", "homepage", "kind")
                if not dependency.get(field)]
    kind = dependency.get("kind")
    if kind == "git" and not COMMIT_PATTERN.match(dependency.get("commit", "")):
        problems.append(f"{name}: git dependency without a 40-hex 'commit' pin")
    if kind == "archive" and not SHA256_PATTERN.match(dependency.get("sha256", "")):
        problems.append(f"{name}: archive without a 'sha256' pin")
    for item in dependency.get("downloads", []):
        if not SHA256_PATTERN.match(item.get("sha256", "")):
            problems.append(f"{name}: download {item.get('url')} without a 'sha256' pin")
    license_file = dependency.get("license_file")
    if license_file is not None and license_file not in destinations(dependency):
        problems.append(f"{name}: license_file {license_file} is not one of its vendored files")
    for patch in dependency.get("patches", []):
        if not (REPOSITORY_ROOT / patch).is_file():
            problems.append(f"{name}: patch {patch} is missing")
    return problems


def check(manifest: dict) -> int:
    """Offline verification of deps/ against the manifest; returns the process exit code."""
    problems: list[str] = []
    owned_directories: set[str] = set()
    produced_files: set[str] = set()
    for dependency in manifest["dependencies"]:
        problems += check_entry(dependency)
        name = dependency.get("name", "<unnamed>")
        produced = destinations(dependency)
        produced_files.update(produced)
        owned_directories.update(destination.split("/", 1)[0] for destination in produced)
        recorded = dependency.get("content_sha256", {})
        if set(recorded) != set(produced):
            problems.append(f"{name}: content_sha256 does not list exactly its files; run update_deps.py {name}")
        for destination in produced:
            path = DEPS_DIR / destination
            if not path.is_file():
                problems.append(f"{name}: {destination} is missing")
            elif destination in recorded and sha256_of(path) != recorded[destination]:
                problems.append(f"{name}: {destination} differs from its recorded post-patch digest "
                                "(hand edit or unapplied patch)")
    for entry in sorted(DEPS_DIR.iterdir()):
        if entry.is_dir() and entry.name not in owned_directories and entry.name != "__pycache__":
            problems.append(f"deps/{entry.name}/ is not produced by any manifest entry")
        elif entry.is_file() and entry.name not in TOOLING_FILES:
            problems.append(f"deps/{entry.name} is neither produced by the manifest nor a known project file")
    for directory in sorted(owned_directories):
        for path in sorted((DEPS_DIR / directory).rglob("*")):
            relative = path.relative_to(DEPS_DIR).as_posix()
            if path.is_file() and relative not in produced_files:
                problems.append(f"deps/{relative} is not produced by the manifest (stray or hand-added file)")
    for problem in problems:
        print(problem)
    print(f"{len(produced_files)} vendored files match the manifest" if not problems
          else f"{len(problems)} problem(s) found")
    return 1 if problems else 0


# ---------------------------------------------------------------- entry point

def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("names", nargs="*", help="dependencies to fetch or compare (default: all)")
    mode = parser.add_mutually_exclusive_group()
    mode.add_argument("--check", action="store_true", help="offline verification of deps/ against the manifest")
    mode.add_argument("--verify-upstream", action="store_true",
                      help="fetch into a temporary directory and compare with deps/ (changes nothing)")
    arguments = parser.parse_args()

    manifest = load_manifest()
    if arguments.check:
        if arguments.names:
            parser.error("--check always verifies every dependency")
        return check(manifest)
    dependencies = manifest["dependencies"]
    known = {dependency["name"] for dependency in dependencies}
    unknown = set(arguments.names) - known
    if unknown:
        parser.error(f"unknown dependencies: {', '.join(sorted(unknown))}; known: {', '.join(sorted(known))}")
    selected = [dependency for dependency in dependencies if not arguments.names or dependency["name"] in arguments.names]
    problems = [problem for dependency in selected for problem in check_entry(dependency)]
    if problems:
        print("\n".join(problems), file=sys.stderr)
        return 1
    try:
        if arguments.verify_upstream:
            differences = [difference for dependency in selected for difference in verify_upstream(dependency)]
            print("\n".join(differences) if differences else f"{len(selected)} dependencies match their pinned upstream")
            return 1 if differences else 0
        for dependency in selected:
            update(dependency)
            save_manifest(manifest)  # after each dependency, so an interrupted run keeps the finished records
    except (DependencyError, OSError, urllib.error.URLError, zipfile.BadZipFile, subprocess.TimeoutExpired) as error:
        print(f"error: {error}", file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
