---
name: add-dependency
description: Add, upgrade or patch a vendored third-party library under deps/ (manifest.json, update_deps.py, deps/CMakeLists.txt, README table, patches/<name>/).
---

# Add or upgrade a dependency

The owner's rule: prefer an established library over writing utility code. The project is GPL-3.0, so
the library must be GPL-compatible (MIT, BSD, zlib, Apache-2.0, LGPL, GPL, public domain...).

## Add a library

1. Pick a pinned release tag and resolve its commit: `git ls-remote <url> refs/tags/<tag> 'refs/tags/<tag>^{}'`
   (for an annotated tag use the `^{}` line). A branch is allowed only with an exact `commit`.
2. Add an entry to `deps/manifest.json` with `name`, `purpose`, `version`, `license` (SPDX), `homepage`,
   `license_file` (the vendored license, shown in the About box) and:
   - `kind: "git"` with `url`, `ref`, `commit` (required) and a `files` map
     `"source/path/in/repo": "<dir>/dest/path"` (glob patterns allowed on the left);
   - `kind: "archive"` with `url`, `sha256`, `files`;
   - `kind: "download"` with a `downloads` list of `{url, sha256, destination}` (sha256 required).
   Do not write `content_sha256`: the script records it.
3. Fetch it: `python deps/update_deps.py <name>` (stages, verifies, patches, then copies and records the
   post-patch digests); check with `python deps/update_deps.py --check`.
4. Expose a target in `deps/CMakeLists.txt` named `mso::<name>` (INTERFACE for header-only, STATIC with
   `-w` for compiled sources; include directories as `SYSTEM`). Exception kept for brevity:
   nlohmann_json is `mso::json`.
5. Link it from the module that uses it in the top-level `CMakeLists.txt` (`mso_module(... PRIVATE ...)`
   unless its types appear in a public header, then `PUBLIC`).
6. Add a row to the table in `deps/README.md` (directory, version, license, what it is used for). The
   About-box notices (`mso/generated/dependencies.hpp`) are generated from the manifest at configure
   time; nothing else to update.
7. Make sure it is actually used: `grep -rn "<header>" src include` must find a user.

## Upgrade

Edit `ref`, `commit` and `version` (or `url`/`sha256`) in the manifest, run the script for that name,
rebuild, run the full test suite, review the diff. `python deps/update_deps.py --verify-upstream`
compares the vendored tree with the pinned upstream without changing anything.

## Patch a library

Never edit vendored files by hand. Create the change, export it with `git diff -- deps/<dir> >
patches/<name>/0001-short-description.patch` (paths relative to the repository root), restore the
pristine file, list the patch in the entry's `"patches"` array, and run `update_deps.py <name>`: it
applies the patches in order in its staging directory and records the patched digests, so `--check`
fails if the patch is ever lost or the file is edited by hand.

## System libraries

Qt 6 and FFmpeg are too large to vendor. Qt is found with `find_package(Qt6 6.5 ...)` in the top-level
`CMakeLists.txt`; FFmpeg with pkg-config in `deps/CMakeLists.txt`. Both are listed under
`system_dependencies` in the manifest (license and purpose for the About box). New system libraries
need an entry there, a row in `deps/README.md`, and install lines for MSYS2 and apt in
`docs/BUILDING.md` and `.github/workflows/ci.yml`.
