---
name: build-and-test
description: Build Music Sample Organizer with clang + libc++ via CMake presets and run its doctest suites on Windows (MSYS2 CLANG64) or Linux; read failures and run one area at a time.
---

# Build and test

## Windows (verified environment)

Toolchain: MSYS2 CLANG64 (clang 23, libc++, CMake, Ninja, Qt 6, FFmpeg). In Git Bash put it first on PATH:

```sh
export PATH=/c/msys64/clang64/bin:$PATH
cmake --preset debug                       # configure into build/debug
cmake --build --preset debug               # everything: app, CLI, worker, tests
build/debug/tests/mso_tests.exe            # all core suites
QT_QPA_PLATFORM=offscreen MSO_AUDIO_BACKEND=null build/debug/mso_ui_tests.exe   # Qt UI suite
build/debug/tests/mso_tests.exe --test-suite=catalog   # one area
ctest --preset debug                       # one CTest entry per area
```

`release` preset = RelWithDebInfo with `-Werror`; use it before handing work over.

If the Qt UI is mid-edit and breaks configure, build the rest with
`cmake --preset debug -DMSO_BUILD_GUI=OFF`.

## Linux

See `docs/BUILDING.md` (LLVM 20 from apt.llvm.org, `libc++-20-dev`, `qt6-base-dev`, FFmpeg `-dev`
packages). Pass `-DCMAKE_C_COMPILER=clang-20 -DCMAKE_CXX_COMPILER=clang++-20` to the preset and run tests
with `QT_QPA_PLATFORM=offscreen`.

## Test areas

`domain`, `catalog`, `search`, `import`, `export` (also the application layer and automatic mode),
`media`, `audio`, `analysis` (includes the worker process tests) in `mso_tests`, and `ui` in the
separate Qt-linked `mso_ui_tests`. Each file `tests/<area>_tests.cpp` wraps its cases in `TEST_SUITE("<area>")`.
Shared fixtures are in `tests/test_support.{hpp,cpp}` (`ScratchDirectory`, `write_sine_wav`,
`synthetic_file`, `make_library`, `sample_id_named`).

## Running the app after a build

Executables need `C:\msys64\clang64\bin` on `PATH` (see `docs/BUILDING.md`). For a headless check of
the desktop app: `QT_QPA_PLATFORM=offscreen MSO_AUDIO_BACKEND=null build/release/MusicSampleOrganizer.exe
--workspace <tmp> --screenshot shot.png` (then look at the PNG); `--exit-when-ready` only checks start-up.
Always pass a temporary `--workspace` so the user's own libraries are never touched.

## Reading failures

- doctest prints `file:line: ERROR: CHECK(...) is NOT correct!` with the evaluated values.
- `REQUIRE_OK`-style checks on `Result<T>`: print `result.error().message()` to see the stable code
  (e.g. `HISTORY_CONFLICT: ...`).
- Worker tests spawn `mso-worker(.exe)` from the build directory; build the `mso-worker` target first.
- Media tests that need FFmpeg-encoded fixtures run `ffmpeg` from PATH and are skipped without it.

## Rules

Build while iterating; run the suites for what you touched once the change is complete, then the full
suite before hand-off. Never leave test or worker processes running.
