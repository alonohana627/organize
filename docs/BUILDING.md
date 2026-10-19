# Building Music Sample Organizer

The project builds with **clang and libc++ in C++23**, configured by **CMake** (3.28+) with **Ninja**.
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
& "C:\Users\user\organize\build\release\MusicSampleOrganizer.exe"
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
is offline). CI starts the installed app with only `C:\Windows\System32` on `PATH` to prove this.

### Windows installer (.msi)

To install on another PC, build an installer from the same install rules (`cmake/Packaging.cmake`).
It needs the [WiX Toolset 3.x](https://github.com/wixtoolset/wix3/releases): unpack
`wix314-binaries.zip` anywhere and point `WIX` at that folder, or put it on `PATH`.

```sh
export WIX="C:\Users\<you>\AppData\Local\wix314\bin"     # folder with candle.exe and light.exe
cmake --preset release && cmake --build --preset release
cpack --config build/release/CPackConfig.cmake -B build/release/package
```

This produces `build/release/package/MusicSampleOrganizer-<version>-windows-x64.msi` (about 140 MB:
the app, `mso`, `mso-worker`, Qt, FFmpeg and libc++). It installs per machine into
`Program Files\Music Sample Organizer`, adds Start menu and desktop shortcuts, shows the GPL as its
license page, and replaces an older version when a newer one is installed. To check its contents
without installing, run `msiexec /a <file>.msi /qn TARGETDIR=<folder>`.

Switches: `--workspace <dir>` (a separate workspace instead of `%LOCALAPPDATA%\MusicSampleOrganizer`),
`--exit-when-ready` (start-up smoke test) and `--screenshot <file.png>` (save the main window after
start-up, then quit). With `QT_QPA_PLATFORM=offscreen` and `MSO_AUDIO_BACKEND=null` the app runs
headless. On first start the desktop app creates the library "My Samples" under
`<workspace>\Libraries`; adding a sample folder scans and analyzes it automatically.

## Linux — prepared, not yet verified

The code keeps platform specifics behind small interfaces (`#ifdef _WIN32` with POSIX branches:
file identity, worker process spawning and limits, audio backends). Setup on Ubuntu 24.04 (CI runs
the same steps except the Qt build: its Linux jobs configure with `-DMSO_BUILD_GUI=OFF -DMSO_REQUIRE_GUI=OFF`
and test the core, the CLI and the worker; the desktop app is tested on Windows):

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
| `ci`, `ci-sanitize` | as `release` / `sanitize` | `MSO_REQUIRE_GUI` and `MSO_REQUIRE_FFMPEG` |

Each has a matching build and test preset; build output goes to `build/<preset>/`. The sanitizer
presets also work on Windows (MSYS2 ships the ASan/UBSan runtimes).

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

## Tests

CTest entries are generated from the sources: one per `TEST_SUITE("<name>")` (labels `core` or
`gui` plus the suite name), one catch-all per test executable (`unlisted-suites`,
`ui-unlisted-suites`) that runs every case outside those suites, and `include-graph` (label `lint`),
which checks that each module includes only the modules it declares in `CMakeLists.txt`. A suite added
after the last configure still runs in its catch-all; a named entry that matches no test case fails.

```sh
ctest --preset debug -L core          # all Qt-free tests
ctest --preset debug -R '^catalog$'   # one area
build/debug/tests/mso_tests.exe --test-suite=catalog
QT_QPA_PLATFORM=offscreen MSO_AUDIO_BACKEND=null build/debug/mso_ui_tests.exe
```

## Dependencies

See [`deps/README.md`](../deps/README.md) for every vendored library, its version, license and purpose.
`python deps/update_deps.py --check` verifies the vendored files offline (pinned commits and digests,
post-patch content hashes, no stray files); `python deps/update_deps.py <name>` re-fetches one. Local
changes to a dependency live as patches under `patches/<name>/`. The configure step generates
`mso/generated/dependencies.hpp` (target `mso::generated`: every dependency with its license text, for
the About box) from `deps/manifest.json`.
