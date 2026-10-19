---
name: build-and-test
description: Build Music Sample Organizer with clang + libc++ via CMake presets and run its doctest suites on Windows (MSYS2 CLANG64) or Linux; read failures, run one area at a time, use the shared test helpers.
---

# Build and test

## Windows (verified environment)

Toolchain: MSYS2 CLANG64 (clang 23, libc++, CMake, Ninja, Python, Qt 6, FFmpeg). In Git Bash put it first on PATH:

```sh
export PATH=/c/msys64/clang64/bin:$PATH
cmake --preset debug                       # configure into build/debug (libc++ debug hardening)
cmake --build --preset debug               # everything: app, CLI, worker, tests
ctest --preset debug                       # every CTest entry
ctest --preset debug -R '^catalog$'        # one area
build/debug/tests/mso_tests.exe --test-suite=catalog
QT_QPA_PLATFORM=offscreen MSO_AUDIO_BACKEND=null build/debug/mso_ui_tests.exe   # Qt UI suite
```

Presets: `release` = RelWithDebInfo with `-Werror` (use it before handing work over); `sanitize` =
Debug with AddressSanitizer + UndefinedBehaviorSanitizer and `-Werror` (works on Windows and Linux);
`ci` / `ci-sanitize` additionally require Qt and FFmpeg. All options are in `docs/BUILDING.md`.

If the Qt UI is mid-edit and breaks the build, build the rest with
`cmake --preset debug -DMSO_BUILD_GUI=OFF`.

## Linux

See `docs/BUILDING.md`: LLVM 23 from apt.llvm.org (`.github/scripts/install-llvm.sh 23`), FFmpeg `-dev`
packages, and Qt built with libc++ (`.github/scripts/build-qt-libcxx.sh ~/qt-libcxx`; distribution Qt
is built with libstdc++ and cannot link). Pass
`-DCMAKE_C_COMPILER=clang-23 -DCMAKE_CXX_COMPILER=clang++-23 -DCMAKE_PREFIX_PATH=~/qt-libcxx` to the preset.

## Test layout

- `mso_tests` (`build/<preset>/tests/`): every `tests/*.cpp` except `ui_*.cpp`, Qt-free.
- `mso_ui_tests` (`build/<preset>/`): `tests/ui_*.cpp`, offscreen Qt.
- Each file wraps its cases in `TEST_SUITE("<area>")`: `domain`, `catalog` (also `sqlite_tests.cpp`),
  `search`, `media`, `import`, `audio`, `analysis`, `worker`, `app`, `export`, `cli`, `infra`
  (the shared helpers and generated headers) and `ui`.
- CTest gets one entry per suite found at configure time (labels `core`/`gui` plus the suite name), a
  catch-all per executable (`unlisted-suites`, `ui-unlisted-suites`) for cases outside those suites,
  and `include-graph` (label `lint`), which fails when a module includes a module it does not declare
  in `CMakeLists.txt`. A named entry that runs zero cases fails. `ctest -L core`, `-L gui`, `-L lint`.
- Entries time out after `MSO_TEST_TIMEOUT` seconds (300; 900 under sanitizers).

## Shared helpers (`tests/test_support.hpp`, library `mso_test_support`)

Use these instead of per-file copies:

- `test::ScratchDirectory dir("label")`: unique temp directory, removed afterwards; `dir / "a/b.wav"`.
- `test::write_wav(path, samples, {.channels, .sample_rate, .encoding})` for `int16_t` or `float`
  samples (`WavEncoding::Int16/Int24/Float32`), `test::wav_bytes(...)` for in-memory images,
  `test::sine_samples(...)`, `test::to_int16(...)`, `test::write_sine_wav(path, seconds)`.
- `test::write_file`, `test::read_file`.
- `test::wait_until(condition, timeout)`: poll with a deadline; never sleep a fixed time and then check.
- `test::worker_executable()`: the `mso-worker` this tree built (`$<TARGET_FILE:mso-worker>`).
- `test::ffmpeg_required()`: true under `MSO_REQUIRE_FFMPEG=1` (set by CTest in CI builds); a test that
  needs ffmpeg must then fail instead of skipping (use `doctest::skip()`/MESSAGE only otherwise).
- Catalog fixtures: `synthetic_file`, `make_library`, `sample_id_named`.

## Running the app after a build

From the build tree, executables need `C:\msys64\clang64\bin` on `PATH`; an installed tree
(`cmake --install build/release --prefix <dir>`) does not. For a headless check of the desktop app:
`QT_QPA_PLATFORM=offscreen MSO_AUDIO_BACKEND=null build/release/MusicSampleOrganizer.exe
--workspace <tmp> --screenshot shot.png` (then look at the PNG); `--exit-when-ready` only checks start-up.
Always pass a temporary `--workspace` so the user's own libraries are never touched.

## Reading failures

- doctest prints `file:line: ERROR: CHECK(...) is NOT correct!` with the evaluated values.
- On a failed `Result<T>` check, print `result.error().message()` to see the stable code
  (e.g. `HISTORY_CONFLICT: ...`).
- libc++ hardening (Debug) aborts with `libc++ Hardening assertion ... failed`: a real out-of-bounds or
  invalid-iterator bug at that line, not a flaky test.
- Sanitizer builds abort on the first finding and print the stack (`UBSAN_OPTIONS`/`ASAN_OPTIONS` are
  set by CTest).
- Worker tests spawn `test::worker_executable()`; the `mso-worker` target is built with the tests.

## Rules

Build while iterating; run the suites for what you touched once the change is complete, then the full
suite before hand-off. Never leave test or worker processes running.
