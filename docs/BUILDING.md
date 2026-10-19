# Building Music Sample Organizer

The project builds with **clang and libc++ in C++23**, configured by **CMake** (3.28+) with **Ninja**.
Everything the core needs is vendored in `deps/` and built as part of the project. Two large libraries
come from the system package manager: **Qt 6** (desktop UI) and **FFmpeg** (AAC, Opus, ALAC, WMA...).
Both are optional; without them the CLI and core still build and the configure step says what is missing.

## Windows (MSYS2 CLANG64) — verified

1. Install [MSYS2](https://www.msys2.org/) and open the **CLANG64** shell (or put `C:\msys64\clang64\bin` first on `PATH`).
2. Install the toolchain and system libraries:

   ```sh
   pacman -S --needed mingw-w64-clang-x86_64-clang mingw-w64-clang-x86_64-cmake mingw-w64-clang-x86_64-ninja \
                      mingw-w64-clang-x86_64-pkgconf mingw-w64-clang-x86_64-qt6-base mingw-w64-clang-x86_64-ffmpeg
   ```

3. Configure, build, test:

   ```sh
   cmake --preset debug          # or: release (warnings are errors)
   cmake --build --preset debug
   ctest --preset debug          # or: build/debug/tests/mso_tests.exe
   ```

Outputs in `build/<preset>/`: `MusicSampleOrganizer.exe` (desktop app), `mso.exe` (CLI), `mso-worker.exe`
(analysis child process; must stay next to the other two), `tests/mso_tests.exe` and `mso_ui_tests.exe`.

### Running

The executables need the MSYS2 runtime DLLs (Qt, FFmpeg, libc++) on `PATH`, so a double-click from
Explorer does not work yet:

```powershell
$env:PATH = "C:\msys64\clang64\bin;$env:PATH"
& "C:\Users\user\organize\build\release\MusicSampleOrganizer.exe"
```

```sh
PATH=/c/msys64/clang64/bin:$PATH build/release/MusicSampleOrganizer.exe
```

Switches: `--workspace <dir>` (a separate workspace instead of `%LOCALAPPDATA%\MusicSampleOrganizer`),
`--exit-when-ready` (start-up smoke test) and `--screenshot <file.png>` (save the main window after
start-up, then quit). With `QT_QPA_PLATFORM=offscreen` and `MSO_AUDIO_BACKEND=null` the app runs
headless. On first start the desktop app creates the library "My Samples" under
`<workspace>\Libraries`; adding a sample folder scans and analyzes it automatically.

## Linux — prepared, not yet verified

The code keeps platform specifics behind small interfaces (`#ifdef _WIN32` with POSIX branches:
file identity, worker process spawning and limits, audio backends). Expected setup on Ubuntu 24.04:

```sh
wget -qO- https://apt.llvm.org/llvm.sh | sudo bash -s -- 20
sudo apt-get install -y libc++-20-dev libc++abi-20-dev cmake ninja-build pkg-config \
     qt6-base-dev libavformat-dev libavcodec-dev libavutil-dev libswresample-dev libpulse-dev libasound2-dev
cmake --preset debug -DCMAKE_C_COMPILER=clang-20 -DCMAKE_CXX_COMPILER=clang++-20
cmake --build --preset debug
QT_QPA_PLATFORM=offscreen ctest --preset debug
```

Audio output uses PulseAudio (including PipeWire's pulse server) with an ALSA fallback. The same steps
run in `.github/workflows/ci.yml`.

## CMake options

| Option | Default | Effect |
| --- | --- | --- |
| `MSO_BUILD_GUI` | ON | Build the Qt desktop app when Qt 6 is found |
| `MSO_WITH_FFMPEG` | ON | Link FFmpeg decoders when found through pkg-config |
| `MSO_BUILD_TESTS` | ON | Build `mso_tests` and register one CTest entry per area |
| `MSO_WARNINGS_AS_ERRORS` | OFF (ON in `release`) | `-Werror` for project code |

## Dependencies

See [`deps/README.md`](../deps/README.md) for every vendored library, its version, license and purpose,
and `python deps/update_deps.py` to fetch or upgrade them. Local changes to a dependency live as
patches under `patches/<name>/`.
