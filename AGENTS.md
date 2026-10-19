# Engineering agent rules

Read `docs/DEVELOPING.md`, `IMPLEMENTATION_STATUS.md` and the relevant guide in `SKILLS/` before coding. The
specification in `docs/01`–`09` and `contracts/schema.sql` remains the requirements baseline; where
the owner has changed a requirement, `IMPLEMENTATION_STATUS.md` lists the deviation and it wins.

## Owner's rules (binding)

1. **Use libraries, never hand-roll utilities.** UUIDs are `uuids::uuid` (stduuid; `mso::new_uuid()`),
   JSON is `nlohmann::json` (no aliases), SHA-256 is `picosha2`, Unicode is utf8proc, FFT is pocketfft,
   CSV is csv-parser, CLI parsing is CLI11, queues are moodycamel, time-stretch is signalsmith-stretch,
   tests are doctest. A missing capability means a new vendored dependency (`SKILLS/add-dependency`).
   The project is GPL-3.0; any GPL-compatible library is acceptable.
2. **Vendored code is never edited by hand.** Changes are patches in `patches/<name>/`, listed in
   `deps/manifest.json` and re-applied by `deps/update_deps.py`.
3. **Clear names, no abbreviations**, and a `///` doc comment on every meaningful class and
   non-trivial function. Aim for code a reviewer rates 10/10.
4. **Layout:** all headers in `include/mso/<module>/` (internal ones in `include/mso/<module>/detail/`),
   all sources in `src/<module>/` (`.cpp` only). A module includes only the modules it declares in
   `CMakeLists.txt` (`mso_module(... MODULES ...)`); the `include-graph` CTest entry enforces it.
   Exception: test-only helpers live next to the tests (`tests/test_support.{hpp,cpp}`).
5. **Every component has doctest tests** (`tests/<area>_tests.cpp`, `TEST_SUITE("<area>")`).
6. **Docs move with code:** update `docs/DEVELOPING.md`, `docs/BUILDING.md`, `deps/README.md`,
   `IMPLEMENTATION_STATUS.md` and the affected `SKILLS/` guide in the same change. `README.md` is
   for the people who use the app (producers, sound designers): change it when something they see
   changes, in plain words with no implementation detail.
7. **Windows first, Linux buildable:** Windows (MSYS2 CLANG64, clang + libc++) is the owner's focus
   and is verified; Linux must stay buildable, but Linux-only work waits (`CODE_REVIEW.md` §8).
   Platform code goes behind `#ifdef _WIN32` with a POSIX branch.
8. **Git:** never commit or push without the owner's explicit OK for that commit.

## Product invariants (from the specification, as amended)

9. The application is completely offline: no accounts, telemetry, updater, downloads, cloud
   inference or network decoder protocols. Developer dependency fetching is separate.
10. Import is bounded metadata work. Analysis (hashing, waveforms, BPM, key) runs only as durable
    analysis runs in `mso-worker`. The desktop app queues them automatically after imports (owner's
    decision); the CLI only on request. Interactive preview may decode the selected file.
11. Preserve source files: never delete, move, rewrite embedded metadata or silently merge records.
    Availability is not deletion.
12. The audio callback does no allocation, locking, I/O, SQLite, logging, Qt calls or decoding.
13. Datasets stay disk-backed and UI models paged; every queue, cache and pool is bounded.
14. One serialized writer per library; metadata, history, revision and outbox commit atomically.
    Never claim atomicity across libraries or across SQLite and the filesystem.
15. Schema changes are new numbered migrations; released migrations are immutable (checksummed).
16. Never relax tests or acceptance criteria to make something pass. Report real command output in
    `IMPLEMENTATION_STATUS.md`, never invented results; a claim cites the run that showed it or says
    "not verified".
17. **Stability, bug catching and performance come first** (owner, 2026-10-19). The app runs on the
    user's own workstation. Basic robustness is required: no crash, hang or memory corruption from a
    bad, damaged or unusual file, tag, query or catalog, and nothing read from a file or a library is
    ever run as code. Sanitizers, fuzzing and hardened debug builds exist to catch bugs and crashes.
    Security-only hardening is out of scope: secured named pipes, sandboxing, threat-model findings.
