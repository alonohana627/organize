# Building Music Sample Organizer

The project builds with **clang and libc++ in C++23**, configured by **CMake** (3.28+; the Windows
installer needs 3.29+) with **Ninja**.
Everything the core needs is vendored in `deps/` and built as part of the project. Two large libraries
come from the system: **Qt 6.5+** (desktop UI) and **FFmpeg** (AAC, Opus, ALAC, WMA...). Both are
optional for a local build (the configure step says what is missing); `MSO_REQUIRE_GUI` and
`MSO_REQUIRE_FFMPEG` make them mandatory, as CI does. **Python 3** is needed for
`deps/update_deps.py` (dependency fetching and verification); the build itself does not use it.

## Windows (MSYS2 CLANG64) — verified

1. Install [MSYS2](https://www.msys2.org/) and open the **CLANG64** shell (or put `C:\msys64\clang64\bin` first on `PATH`).
2. Install the toolchain and system libraries:

   ```sh
   pacman -S --needed mingw-w64-clang-x86_64-clang mingw-w64-clang-x86_64-cmake mingw-w64-clang-x86_64-ninja \
                      mingw-w64-clang-x86_64-pkgconf mingw-w64-clang-x86_64-python \
                      mingw-w64-clang-x86_64-qt6-base mingw-w64-clang-x86_64-ffmpeg
   ```

3. Configure, build, test:

   ```sh
   cmake --preset debug          # or: release (warnings are errors)
   cmake --build --preset debug
   ctest --preset debug          # every CTest entry (see "Tests" below)
   ```

Outputs in `build/<preset>/`: `MusicSampleOrganizer.exe` (desktop app), `mso.exe` (CLI), `mso-worker.exe`
(analysis child process; must stay next to the other two), `mso_ui_tests.exe` (Qt UI tests) and
`tests/mso_tests.exe` (all other tests).

### Running and installing

From the build tree the executables need the MSYS2 runtime DLLs (Qt, FFmpeg, libc++) on `PATH`:

```powershell
$env:PATH = "C:\msys64\clang64\bin;$env:PATH"
& ".\build\release\MusicSampleOrganizer.exe"    # from the repository folder
```

```sh
PATH=/c/msys64/clang64/bin:$PATH build/release/MusicSampleOrganizer.exe
```

For a folder that starts by double-click, install it:

```sh
cmake --install build/release --prefix "C:/Programs/MusicSampleOrganizer"
```

`<prefix>\bin` then holds the three executables with every DLL they load (Qt through `windeployqt`;
libc++, FFmpeg and their dependencies resolved from the toolchain), and `<prefix>\share\qt6\plugins`
the Qt plugins that `qt.conf` points to. Network-related Qt plugins are not deployed (the application
is offline). The `ci` leg starts the installed app with only `C:\Windows\System32` on `PATH`; that
step passed in run 39233493989.

### Windows installer (.msi)

To install on another PC, build an installer from the same install rules (`cmake/Packaging.cmake`).
It needs the [WiX Toolset 3.x](https://github.com/wixtoolset/wix3/releases) and CMake 3.29 or newer
(older versions skip packaging with a warning, because they cannot make the installer per machine).
Unpack `wix314-binaries.zip` anywhere and put that folder on `PATH`, or point `WIX` at it: CPack
(3.29+) looks for `candle.exe` and `light.exe` in `$WIX` and in `$WIX/bin`, so both the flat zip and
the Chocolatey package `wixtoolset` 3.14.1 (preinstalled on the `ci` runner; its programs are in `bin`)
work (checked against the CPackWIX module of CMake 4.4). Build it from the MSYS2 CLANG64 shell: the
install step asks MSYS2's `pacman` which package each bundled DLL comes from.

```sh
export PATH="/c/Users/<you>/AppData/Local/wix314/bin:$PATH"  # folder with candle.exe and light.exe
cmake --preset release && cmake --build --preset release
cpack --config build/release/CPackConfig.cmake -B build/release/package
```

This produces `build/release/package/MusicSampleOrganizer-<version>-windows-x64.msi` with the app,
`mso`, `mso-worker`, Qt, FFmpeg, libc++ and every DLL they load. The installer is **per machine**
(`CPACK_WIX_INSTALL_SCOPE perMachine`; it asks for administrator rights): it installs into
`Program Files\Music Sample Organizer`, adds Start menu and desktop shortcuts for all users and one
uninstall entry, shows the GPL as its license page, and replaces an older version when a newer one is
installed. It ships the licenses with the program: `share\doc\MusicSampleOrganizer\` holds `LICENSE`,
`THIRD_PARTY_NOTICES.txt` (every bundled library with its license text, and where to get the source
of Qt and FFmpeg) and the README; `share\licenses\<package>\` holds the license files of each MSYS2
package a bundled DLL comes from, listed in `share\licenses\PACKAGES.txt`. To check its contents
without installing, run `msiexec /a <file>.msi /qn TARGETDIR=<folder>`. Verified so far: the package
builds and its WiX sources say `InstallScope="perMachine"`; installing on a clean PC is not verified
yet (`IMPLEMENTATION_STATUS.md`).

Switches: `--workspace <dir>` (a separate workspace instead of `%LOCALAPPDATA%\MusicSampleOrganizer`),
`--exit-when-ready` (start-up smoke test) and `--screenshot <file.png>` (save the main window after
start-up, then quit). With `QT_QPA_PLATFORM=offscreen` and `MSO_AUDIO_BACKEND=null` the app runs
headless. On first start the desktop app creates the library "My Samples" under
`<workspace>\Libraries`; adding a sample folder scans and analyzes it automatically.

## Linux — prepared, not yet verified

The code keeps platform specifics behind small interfaces (`#ifdef _WIN32` with POSIX branches:
file identity, worker process spawning and limits, audio backends). The owner's focus is Windows; the
Linux CI job is parked, and its last run (39229983572) compiled the core, the CLI and the worker but
failed three test entries (see `docs/DEVELOPING.md`). Setup on Ubuntu 24.04 (the parked CI job uses the
same steps without the Qt build, configuring with `-DMSO_BUILD_GUI=OFF -DMSO_REQUIRE_GUI=OFF`):

```sh
# clang, libc++ and libc++abi from the apt.llvm.org repository (key fingerprint checked; no piped script)
bash .github/scripts/install-llvm.sh 23
sudo apt-get install -y cmake ninja-build pkg-config python3 ffmpeg \
     libavformat-dev libavcodec-dev libavutil-dev libswresample-dev libpulse-dev libasound2-dev \
     libfontconfig1-dev libfreetype-dev libxkbcommon-dev
# Qt 6.11 (qtbase) built with clang + libc++ into ~/qt-libcxx (about 30 minutes, once)
bash .github/scripts/build-qt-libcxx.sh ~/qt-libcxx
cmake --preset debug -DCMAKE_C_COMPILER=clang-23 -DCMAKE_CXX_COMPILER=clang++-23 -DCMAKE_PREFIX_PATH=~/qt-libcxx
cmake --build --preset debug
ctest --preset debug
```

**Why Qt is built from source.** Distribution packages and the Qt online installer ship Qt built
against libstdc++. libc++ puts its types in the inline namespace `std::__1`, so a mismatch is a link
error, never silent corruption: most of Qt's API uses Qt types and links fine, but `QtConcurrent::run`
(used by the UI) instantiates `QUnhandledException(std::exception_ptr)` and
`QFutureInterfaceBase::reportException(const std::exception_ptr&)`, which libstdc++-built Qt exports
with libstdc++'s `std::exception_ptr`. The desktop app therefore needs a Qt built with libc++.
Without one, configure with `-DMSO_BUILD_GUI=OFF`: the CLI, the worker and all non-UI tests build
against the system libraries. The script builds only the offscreen platform plugin unless the X11
development packages are installed (`libxcb*-dev`, `libx11-xcb-dev`, `libxkbcommon-x11-dev`); pass
extra Qt CMake options after the prefix, for example `-DFEATURE_xcb=ON`.

Audio output uses PulseAudio (including PipeWire's pulse server) with an ALSA fallback.

## Presets

| Preset | Build type | Extras |
| --- | --- | --- |
| `debug` | Debug | libc++ debug hardening (`_LIBCPP_HARDENING_MODE_DEBUG`) |
| `release` | RelWithDebInfo | `-Werror`; optimized with symbols |
| `sanitize` | Debug | AddressSanitizer + UndefinedBehaviorSanitizer, `-Werror` |
| `fuzz` | RelWithDebInfo | libFuzzer targets only (`tests/fuzz/`), ASan + UBSan, libc++ extensive hardening, no tests or GUI |
| `ci`, `ci-debug`, `ci-sanitize` | as `release` / `debug` (+ `-Werror`) / `sanitize` | `MSO_REQUIRE_GUI` and `MSO_REQUIRE_FFMPEG` |

Each has a matching build preset, and each except `fuzz` a test preset; build output goes to
`build/<preset>/`. The sanitizer presets work on Windows (MSYS2 CLANG64 ships the ASan and UBSan
runtimes; there is no ThreadSanitizer for Windows). CTest prepends the toolchain's `bin` folder to
`PATH` and, in sanitizer builds, sets `ASAN_SYMBOLIZER_PATH` to the `llvm-symbolizer` next to clang, so
`ctest` works from any shell and sanitizer reports show file and line.

## CMake options

| Option | Default | Effect |
| --- | --- | --- |
| `MSO_BUILD_GUI` | ON | Build the Qt desktop app when Qt 6.5+ is found |
| `MSO_REQUIRE_GUI` | OFF (ON in `ci*`) | Fail the configuration when the desktop app cannot be built |
| `MSO_WITH_FFMPEG` | ON | Link FFmpeg decoders when found through pkg-config |
| `MSO_REQUIRE_FFMPEG` | OFF (ON in `ci*`) | Fail without FFmpeg, and set `MSO_REQUIRE_FFMPEG=1` for the tests so FFmpeg-dependent tests fail instead of skipping |
| `MSO_BUILD_TESTS` | ON | Build the test executables and register them with CTest |
| `MSO_WARNINGS_AS_ERRORS` | OFF (ON in `release`, `sanitize`, `ci*`) | `-Werror` for project code |
| `MSO_SANITIZERS` | empty | `address`, `undefined` or `address;undefined` for the whole build |
| `MSO_TEST_TIMEOUT` | 300 | CTest timeout per entry in seconds (at least 900 with sanitizers) |
| `MSO_BUILD_FUZZERS` | OFF (ON in `fuzz`) | Instrument the build for libFuzzer and build `tests/fuzz/`; needs `address` in `MSO_SANITIZERS` |

## Tests

CTest entries are generated from the sources: one per `TEST_SUITE("<name>")` (labels `core` or
`gui` plus the suite name), one catch-all per test executable (`unlisted-suites`,
`ui-unlisted-suites`) that runs every case outside those suites, and `include-graph` (label `lint`),
which checks that each module includes only the modules it declares in `CMakeLists.txt`. A suite added
after the last configure still runs in its catch-all; a named entry that matches no test case fails.
The number of entries is therefore the number of suites plus three; `ctest -N` lists them.

```sh
ctest --preset debug -L core          # all Qt-free tests
ctest --preset debug -R '^catalog$'   # one area
build/debug/tests/mso_tests.exe --test-suite=catalog
QT_QPA_PLATFORM=offscreen MSO_AUDIO_BACKEND=null build/debug/mso_ui_tests.exe
```

## Fuzzing

**Deferred by the owner to priority P10 (2026-10-19):** the CI fuzz job is off, and the targets are
not extended for now. They still build and run by hand as described here.

`tests/fuzz/` holds one libFuzzer target per piece of code that reads damaged or unusual input: the
media probe once per container family (`fuzz_probe_riff`, `_flac`, `_mpeg_id3`, `_ogg`, `_mp4`, `_asf`,
`_ape`, through `media::probe_memory`), `fuzz_stb_vorbis` (the vendored Vorbis decoder: open, decode,
seek), `fuzz_worker_protocol` (frames and messages on the worker's pipe), `fuzz_query_parser` (query
text and the in-memory matcher) and `fuzz_cursor` (paging cursors of the SQL search and the in-memory
index). Each has a small seed corpus in `tests/fuzz/corpus/<name>/`, including regression inputs of
past findings.

```sh
cmake --preset fuzz && cmake --build --preset fuzz
python tools/run_fuzzers.py                          # every target 60 s, 4 at a time
python tools/run_fuzzers.py --seconds 600 stb_vorbis # one target, longer
build/fuzz/tests/fuzz/fuzz_stb_vorbis.exe build/fuzz/fuzz-artifacts/stb_vorbis/crash-...   # replay a finding
```

A crash, sanitizer report, hang (25 s per input), or memory use above 2 GB fails the run and saves the
input under `build/fuzz/fuzz-artifacts/<name>/`. LeakSanitizer does not exist on Windows, so a leak
there is found only when it pushes a target past the memory limit (as the stb_vorbis codebook leak
was). On Windows, clang's driver does not accept
`-fsanitize=fuzzer` for the MinGW target, so `cmake/ProjectOptions.cmake` passes the equivalent
coverage flags and links MSYS2's `libclang_rt.fuzzer-x86_64.a` directly.

## Dependencies

See [`deps/README.md`](../deps/README.md) for every vendored library, its version, license and purpose.
`python deps/update_deps.py --check` verifies the vendored files offline (pinned commits and digests,
post-patch content hashes, no stray files); `python deps/update_deps.py <name>` re-fetches one. Local
changes to a dependency live as patches under `patches/<name>/`. The configure step generates
`mso/generated/dependencies.hpp` (target `mso::generated`: every shipped dependency with its license
text, for the About box and `mso --version`) and `THIRD_PARTY_NOTICES.txt` (installed next to
`LICENSE`) from `deps/manifest.json`.
