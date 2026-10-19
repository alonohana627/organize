# Skills for coding agents

Task guides for LLM agents working on this repository. Each folder holds one `SKILL.md` with a
`name`/`description` header (so tools that load skills can pick it), the files to touch, the steps,
and how to verify. Read `AGENTS.md` and the project rules below first.

| Skill | Use it when |
| --- | --- |
| [build-and-test](build-and-test/SKILL.md) | Building, running tests, reading failures, Windows vs Linux |
| [cli-end-to-end-check](cli-end-to-end-check/SKILL.md) | Proving a change works through the real `mso` CLI on generated audio |
| [add-dependency](add-dependency/SKILL.md) | Adding or upgrading a third-party library, or patching one |
| [add-schema-migration](add-schema-migration/SKILL.md) | Any change to `catalog.sqlite` tables or indexes |
| [add-search-field](add-search-field/SKILL.md) | A new `field:value` in the query language or a new sort |
| [add-audio-format](add-audio-format/SKILL.md) | Probing or decoding another container/codec |
| [add-analysis-stage](add-analysis-stage/SKILL.md) | A new explicit analysis result (e.g. loudness LUFS) |

## Project rules every task must follow

- **Use libraries, don't hand-roll.** UUIDs are `uuids::uuid` (stduuid, `mso::new_uuid()`), JSON is
  `nlohmann::json` (never aliased), SHA-256 is picosha2, Unicode is utf8proc, FFT is
  pocketfft, CLI parsing is CLI11, tests use doctest. A missing capability means a new vendored dependency
  (see add-dependency), not new utility code. The project is GPL-3.0, so any GPL-compatible library is fine.
- **Clear names, no abbreviations** (`PreparedStatement`, not `Stmt`), and a `///` doc comment on every
  meaningful class and non-trivial function.
- **Layout:** every header lives in `include/mso/<module>/` (internal ones in
  `include/mso/<module>/detail/`), every `.cpp` in `src/<module>/`. Modules: domain, catalog, search,
  media, files, audio, analysis, worker, app, cli, ui. A module includes only the modules it declares
  in `CMakeLists.txt` (`mso_module(<name> MODULES ...)`); the `include-graph` CTest entry checks it.
  Test helpers are the one exception: `tests/test_support.{hpp,cpp}`.
- **Errors** are `mso::Result<T>` (`std::expected<T, mso::Error>`) with the stable `ErrorCode` taxonomy;
  no exceptions cross module boundaries.
- **Portability:** Windows (MSYS2 CLANG64) is verified; Linux must stay buildable. Platform code goes
  behind `#ifdef _WIN32` with a POSIX branch, in `.cpp` files or `detail/` headers. A public header may
  only select a platform type or constant (both branches present, e.g. a native handle type or the
  worker executable name), never contain platform logic.
- **Tests:** every component has doctest cases in `tests/<area>_tests.cpp` inside
  `TEST_SUITE("<area>")` (CTest registers each suite automatically). Generate fixtures at runtime with
  the shared helpers in `tests/test_support.hpp`; never commit audio; poll with deadlines
  (`test::wait_until`), never sleep a fixed time.
- **Docs move with code:** update `docs/DEVELOPING.md`, `docs/BUILDING.md`, `deps/README.md`,
  `IMPLEMENTATION_STATUS.md` and the relevant skill in the same change. `README.md` is the user
  guide (written for producers, not developers): update it only for user-visible changes.
- **Git:** never commit or push without the owner's explicit OK.
