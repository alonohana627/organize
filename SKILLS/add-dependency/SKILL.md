---
name: add-dependency
description: Add, upgrade or patch a vendored third-party library under deps/ (manifest.json, update_deps.py, deps/CMakeLists.txt, README table, patches/<name>/).
---

# Add or upgrade a dependency

The owner's rule: prefer an established library over writing utility code. The project is GPL-3.0, so
the library must be GPL-compatible (MIT, BSD, zlib, Apache-2.0, LGPL, GPL, public domain...).

## Add a library

1. Pick a pinned release tag (or a branch plus an exact `commit`).
2. Add an entry to `deps/manifest.json`:
   - `kind: "git"` with `url`, `ref` (and `commit` for branches) and a `files` map
     `"source/path/in/repo": "<dir>/dest/path"` (glob patterns allowed on the left);
   - `kind: "archive"` with `url`, `sha256`, `files`;
   - `kind: "download"` with a `downloads` list of `{url, sha256, destination}` for single files.
3. Fetch it: `python deps/update_deps.py <name>`; check with `python deps/update_deps.py --check`.
4. Expose a target in `deps/CMakeLists.txt` named `mso::<name>` (INTERFACE for header-only, STATIC with
   `-w` for compiled sources; include directories as `SYSTEM`).
5. Link it from the module that uses it in the top-level `CMakeLists.txt` (PRIVATE unless its types
   appear in a public header).
6. Add a row to the table in `deps/README.md` (directory, version, license, what it is used for).
7. Make sure it is actually used: `grep -rn "<header>" src include` must find a user.

## Upgrade

Edit `ref`/`version` (and `url`/`sha256`/`commit`) in the manifest, run the script for that name,
rebuild, run the full test suite, review the diff.

## Patch a library

Never edit vendored files by hand. Create the change, export it with `git diff -- deps/<dir> >
patches/<name>/0001-short-description.patch` (paths relative to the repository root), restore the
pristine file, and list the patch in the entry's `"patches"` array. `update_deps.py` applies patches in
order after every fetch.

## System libraries

Qt 6 and FFmpeg are too large to vendor; they are found with `find_package(Qt6)` and pkg-config in
`deps/CMakeLists.txt`. New system libraries need a row in `deps/README.md` and install lines for MSYS2
and apt in `docs/BUILDING.md` and `.github/workflows/ci.yml`.
