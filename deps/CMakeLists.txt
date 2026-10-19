# Vendored third-party dependencies, pinned in deps/manifest.json and fetched by deps/update_deps.py.
# They are built here with their own (quiet) flags so project warnings stay meaningful.

# ---- SQLite amalgamation. The options only affect how sqlite3.c is compiled (none changes sqlite3.h),
# so they are PRIVATE. The catalog checks FTS5 at startup (sqlite3_compileoption_used); the rest are
# SQLite's recommended options (https://sqlite.org/compile.html#recommended_compile_time_options)
# plus the safety settings the catalog relies on.
add_library(mso_sqlite3 STATIC sqlite/sqlite3.c)
target_include_directories(mso_sqlite3 SYSTEM PUBLIC sqlite)
target_compile_definitions(mso_sqlite3 PRIVATE
  SQLITE_ENABLE_FTS5=1
  SQLITE_THREADSAFE=2              # multi-thread: connections are thread-confined
  SQLITE_DEFAULT_FOREIGN_KEYS=1
  SQLITE_DQS=0                     # no double-quoted string literals
  SQLITE_DEFAULT_MEMSTATUS=0
  SQLITE_OMIT_LOAD_EXTENSION=1     # catalogs never load code
  SQLITE_OMIT_DEPRECATED=1         # no legacy APIs (sqlite3_trace, sqlite3_profile, ...)
  SQLITE_OMIT_SHARED_CACHE=1       # every connection has its own cache; shared cache is never used
  SQLITE_LIKE_DOESNT_MATCH_BLOBS=1 # LIKE (codec filters) only ever compares text
  SQLITE_DEFAULT_WAL_SYNCHRONOUS=2
  SQLITE_MAX_EXPR_DEPTH=1000)
target_compile_options(mso_sqlite3 PRIVATE -w)
add_library(mso::sqlite3 ALIAS mso_sqlite3)

# ---- stduuid: header-only RFC 4122 UUIDs.
add_library(mso_stduuid INTERFACE)
target_include_directories(mso_stduuid SYSTEM INTERFACE stduuid/include)
target_compile_definitions(mso_stduuid INTERFACE UUID_SYSTEM_GENERATOR=0)
add_library(mso::stduuid ALIAS mso_stduuid)

# ---- nlohmann/json: header-only JSON.
add_library(mso_nlohmann_json INTERFACE)
target_include_directories(mso_nlohmann_json SYSTEM INTERFACE nlohmann_json/include)
target_compile_definitions(mso_nlohmann_json INTERFACE JSON_USE_IMPLICIT_CONVERSIONS=0)
add_library(mso::json ALIAS mso_nlohmann_json)

# ---- utf8proc: Unicode normalization (NFKC) and case folding.
add_library(mso_utf8proc STATIC utf8proc/utf8proc.c)
target_include_directories(mso_utf8proc SYSTEM PUBLIC utf8proc)
target_compile_definitions(mso_utf8proc PUBLIC UTF8PROC_STATIC)
target_compile_options(mso_utf8proc PRIVATE -w)
add_library(mso::utf8proc ALIAS mso_utf8proc)

# ---- pocketfft: header-only FFT (C++ branch).
add_library(mso_pocketfft INTERFACE)
target_include_directories(mso_pocketfft SYSTEM INTERFACE pocketfft)
target_compile_definitions(mso_pocketfft INTERFACE POCKETFFT_NO_MULTITHREADING)
add_library(mso::pocketfft ALIAS mso_pocketfft)

# ---- BS::thread_pool: header-only bounded thread pool.
add_library(mso_thread_pool INTERFACE)
target_include_directories(mso_thread_pool SYSTEM INTERFACE thread_pool/include)
add_library(mso::thread_pool ALIAS mso_thread_pool)

# ---- moodycamel readerwriterqueue: lock-free bounded SPSC queues.
add_library(mso_readerwriterqueue INTERFACE)
target_include_directories(mso_readerwriterqueue SYSTEM INTERFACE readerwriterqueue/include)
add_library(mso::readerwriterqueue ALIAS mso_readerwriterqueue)

# ---- CLI11: header-only command-line parser (mso tool).
add_library(mso_cli11 INTERFACE)
target_include_directories(mso_cli11 SYSTEM INTERFACE cli11/include)
add_library(mso::cli11 ALIAS mso_cli11)

# ---- signalsmith-stretch (+ its signalsmith-linear dependency): header-only time-stretch and
# pitch-shift for independent tempo/pitch preview.
add_library(mso_signalsmith_stretch INTERFACE)
target_include_directories(mso_signalsmith_stretch SYSTEM INTERFACE signalsmith_stretch/include signalsmith_linear/include)
add_library(mso::signalsmith_stretch ALIAS mso_signalsmith_stretch)

# ---- csv-parser: RFC 4180 CSV writer (patched for CRLF records; see patches/csv_parser/).
add_library(mso_csv_parser INTERFACE)
target_include_directories(mso_csv_parser SYSTEM INTERFACE csv_parser/include)
add_library(mso::csv_parser ALIAS mso_csv_parser)

# ---- doctest: test framework (tests/ only).
add_library(mso_doctest INTERFACE)
target_include_directories(mso_doctest SYSTEM INTERFACE doctest)
add_library(mso::doctest ALIAS mso_doctest)

# ---- PicoSHA2: header-only SHA-256.
add_library(mso_picosha2 INTERFACE)
target_include_directories(mso_picosha2 SYSTEM INTERFACE picosha2)
add_library(mso::picosha2 ALIAS mso_picosha2)

# ---- miniaudio: WASAPI output plus built-in WAV/FLAC/MP3 decoders; Vorbis through stb_vorbis.
# deps/miniaudio_impl.c is the project's one glue translation unit (not vendored): it compiles the
# miniaudio and stb_vorbis implementations in the order stb_vorbis requires.
add_library(mso_miniaudio STATIC miniaudio_impl.c)
target_include_directories(mso_miniaudio SYSTEM PUBLIC miniaudio miniaudio/extras)
target_compile_definitions(mso_miniaudio PUBLIC
  MA_NO_ENCODING MA_NO_GENERATION MA_NO_NODE_GRAPH MA_NO_ENGINE MA_NO_RESOURCE_MANAGER
  MA_ENABLE_ONLY_SPECIFIC_BACKENDS MA_ENABLE_NULL)
target_compile_options(mso_miniaudio PRIVATE -w)
if(WIN32)
  # WASAPI shared mode on Windows.
  target_compile_definitions(mso_miniaudio PUBLIC MA_ENABLE_WASAPI)
  target_link_libraries(mso_miniaudio PUBLIC ole32 avrt winmm)
else()
  # PulseAudio (incl. PipeWire's pulse server) with ALSA fallback; both are loaded at runtime.
  target_compile_definitions(mso_miniaudio PUBLIC MA_ENABLE_PULSEAUDIO MA_ENABLE_ALSA)
  find_package(Threads REQUIRED)
  target_link_libraries(mso_miniaudio PUBLIC Threads::Threads ${CMAKE_DL_LIBS} m)
endif()
add_library(mso::miniaudio ALIAS mso_miniaudio)

# ---- FFmpeg (optional, system-provided): libavformat/libavcodec/libswresample for AAC, Opus, ALAC, WMA.
# FFmpeg is too large to vendor; it is discovered through pkg-config (MSYS2: mingw-w64-clang-x86_64-ffmpeg).
if(MSO_WITH_FFMPEG)
  find_package(PkgConfig QUIET)
  if(PKG_CONFIG_FOUND)
    # GLOBAL: the imported target (and its alias) must be visible to the top-level project.
    pkg_check_modules(MSO_FFMPEG QUIET IMPORTED_TARGET GLOBAL libavformat libavcodec libavutil libswresample)
    if(MSO_FFMPEG_FOUND)
      add_library(mso::ffmpeg ALIAS PkgConfig::MSO_FFMPEG)
      message(STATUS "MSO: FFmpeg ${MSO_FFMPEG_libavformat_VERSION} decoders enabled")
    endif()
  endif()
  if(NOT MSO_FFMPEG_FOUND)
    message(STATUS "MSO: FFmpeg not found; AAC/Opus/ALAC decoding unavailable (WAV/AIFF/FLAC/MP3/Vorbis still work)")
  endif()
endif()
