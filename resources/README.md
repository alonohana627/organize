# Resources

Non-code files that are built into the executables or installed with them.

| Path | What it is |
| --- | --- |
| `icons/music_sample_organizer.svg` | The application icon; the source of every other icon file |
| `icons/png/music_sample_organizer_<size>.png` | 16–256 px renders, compiled into the desktop app's Qt resources (`:/icons/`) for the window icon |
| `icons/music_sample_organizer.ico` | Windows executable icon (16–256 px), embedded in `MusicSampleOrganizer.exe` and `mso.exe` |
| `windows/application_icon.rc.in` | Resource script template; CMake fills in the absolute `.ico` path |
| `linux/music-sample-organizer.desktop` | Desktop entry installed on Linux, together with the PNGs and SVG in the hicolor icon theme |

The PNG and ICO files are generated and committed, so building needs no SVG renderer. After editing
the SVG, regenerate them with `python tools/render_icon.py` (needs `rsvg-convert`: MSYS2
`mingw-w64-clang-x86_64-librsvg`, Debian/Ubuntu `librsvg2-bin`). The CMake wiring is in
`cmake/ApplicationIcon.cmake`.
