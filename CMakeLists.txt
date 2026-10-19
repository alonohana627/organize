cmake_minimum_required(VERSION 3.28)
project(MusicSampleOrganizer VERSION 0.1.0 LANGUAGES C CXX)

# Toolchain contract: clang with libc++ (MSYS2 CLANG64 on Windows), C++23, no compiler extensions.
set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_C_STANDARD 11)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)  # compile_commands.json for clangd (Zed, VS Code, CLion)
# No C++20 modules are used. Without this, CMake adds per-file @*.modmap response files to every
# compile command, which clangd cannot read.
set(CMAKE_CXX_SCAN_FOR_MODULES OFF)

if(NOT CMAKE_CXX_COMPILER_ID MATCHES "Clang")
  message(FATAL_ERROR "Music Sample Organizer is built with clang + libc++; got ${CMAKE_CXX_COMPILER_ID}")
endif()

option(MSO_BUILD_GUI "Build the Qt 6 desktop application when Qt 6.5+ is available" ON)
option(MSO_REQUIRE_GUI "Fail the configuration when the desktop application cannot be built (CI)" OFF)
option(MSO_WITH_FFMPEG "Decode AAC/Opus/ALAC/WMA through FFmpeg when its libraries are available" ON)
option(MSO_REQUIRE_FFMPEG "Fail the configuration without FFmpeg, and fail (not skip) the FFmpeg-dependent tests (CI)" OFF)
option(MSO_BUILD_TESTS "Build the test executables and register them with CTest" ON)
option(MSO_WARNINGS_AS_ERRORS "Treat compiler warnings as errors in project code" OFF)

if(MSO_REQUIRE_GUI AND NOT MSO_BUILD_GUI)
  message(FATAL_ERROR "MSO_REQUIRE_GUI=ON contradicts MSO_BUILD_GUI=OFF")
endif()
if(MSO_REQUIRE_FFMPEG AND NOT MSO_WITH_FFMPEG)
  message(FATAL_ERROR "MSO_REQUIRE_FFMPEG=ON contradicts MSO_WITH_FFMPEG=OFF")
endif()

include(cmake/ProjectOptions.cmake)
include(cmake/Modules.cmake)
include(cmake/DependencyNotices.cmake)
include(cmake/ApplicationIcon.cmake)
if(MSO_BUILD_TESTS)
  enable_testing()
endif()

add_subdirectory(deps)
if(MSO_REQUIRE_FFMPEG AND NOT TARGET mso::ffmpeg)
  message(FATAL_ERROR "MSO_REQUIRE_FFMPEG=ON but FFmpeg (libavformat, libavcodec, libavutil, libswresample) "
                      "was not found through pkg-config")
endif()

# ---------------------------------------------------------------- core modules
# Dependencies point downward only (see cmake/Modules.cmake for the include-graph rule):
#   domain <- catalog <- search;  domain <- media <- {audio, analysis};  {catalog, media} <- files;
#   analysis <- worker (protocol and supervision of the mso-worker process);
#   {catalog, search, files, analysis, worker} <- app;  app, audio <- cli;  ui is the desktop app.
mso_module(domain PUBLIC mso::stduuid mso::json mso::picosha2 PRIVATE mso::utf8proc)
mso_module(catalog MODULES domain PUBLIC mso::sqlite3)
mso_module(search MODULES catalog)
mso_module(media MODULES domain PRIVATE mso::miniaudio mso::utf8proc)
if(TARGET mso::ffmpeg)
  message(STATUS "MSO: media module links FFmpeg")
  target_link_libraries(mso_media PRIVATE mso::ffmpeg)
  target_compile_definitions(mso_media PRIVATE MSO_HAVE_FFMPEG=1)
endif()
mso_module(files MODULES catalog media PRIVATE mso::thread_pool mso::readerwriterqueue)
mso_module(audio MODULES media PRIVATE mso::miniaudio mso::readerwriterqueue mso::signalsmith_stretch)
mso_module(analysis MODULES media PUBLIC mso::json PRIVATE mso::pocketfft)
# Worker IPC protocol, process supervision and platform confinement: every src/worker/ source except
# main.cpp (the mso-worker executable). Headers in include/mso/worker/.
mso_module(worker MODULES analysis PUBLIC mso::json PRIVATE mso::picosha2 EXCLUDE main.cpp)
mso_module(app MODULES catalog search files analysis worker PRIVATE mso::csv_parser mso::thread_pool)
# Every CLI source except main.cpp, so the CLI tests can drive the commands in-process.
mso_module(cli MODULES app audio media PUBLIC mso::cli11 EXCLUDE main.cpp)

# ---------------------------------------------------------------- executables
# The analysis child process.
add_executable(mso-worker src/worker/main.cpp)
target_link_libraries(mso-worker PRIVATE mso_worker mso_analysis mso::json mso::picosha2 mso::cli11)
mso_apply_options(mso-worker)

add_executable(mso src/cli/main.cpp)
mso_add_executable_icon(mso)
target_link_libraries(mso PRIVATE mso_cli)
mso_apply_options(mso)
add_dependencies(mso mso-worker)  # the CLI starts mso-worker from its own directory

set(_mso_system_versions "")
if(TARGET mso::ffmpeg)
  list(APPEND _mso_system_versions FFmpeg "libavformat ${MSO_FFMPEG_libavformat_VERSION}")
endif()

# Declared even when the desktop app is not built, so the include-graph check still knows the ui/
# directories (and checks their includes) in GUI-less builds.
mso_declare_module(ui MODULES app audio)
if(MSO_BUILD_GUI)
  if(MSO_REQUIRE_GUI)
    find_package(Qt6 6.5 REQUIRED COMPONENTS Widgets Concurrent)
  else()
    find_package(Qt6 6.5 QUIET COMPONENTS Widgets Concurrent)
  endif()
  if(Qt6_FOUND)
    list(APPEND _mso_system_versions Qt "${Qt6_VERSION}")
    # Everything except main.cpp goes into mso_ui so the UI tests can link the models and panels.
    # AUTOMOC is enabled on the Qt targets only; the core libraries and mso_tests have no moc step.
    file(GLOB _mso_ui_sources CONFIGURE_DEPENDS src/ui/*.cpp include/mso/ui/*.hpp include/mso/ui/detail/*.hpp)
    list(FILTER _mso_ui_sources EXCLUDE REGEX "/src/ui/main\\.cpp$")
    add_library(mso_ui STATIC ${_mso_ui_sources})
    target_link_libraries(mso_ui PUBLIC mso_app mso_audio mso::generated Qt6::Widgets Qt6::Concurrent)
    set_target_properties(mso_ui PROPERTIES AUTOMOC ON)
    mso_apply_options(mso_ui)

    qt_add_executable(MusicSampleOrganizer WIN32 src/ui/main.cpp)
    mso_add_executable_icon(MusicSampleOrganizer)
    mso_add_window_icon_resources(MusicSampleOrganizer)
    target_link_libraries(MusicSampleOrganizer PRIVATE mso_ui)
    target_compile_definitions(MusicSampleOrganizer PRIVATE MSO_VERSION_STRING="${PROJECT_VERSION}")
    set_target_properties(MusicSampleOrganizer PROPERTIES AUTOMOC ON)
    mso_apply_options(MusicSampleOrganizer)
    add_dependencies(MusicSampleOrganizer mso-worker)
    message(STATUS "MSO: building Qt ${Qt6_VERSION} desktop application")
  else()
    message(STATUS "MSO: Qt 6.5+ not found; desktop application skipped (CLI and core still build). "
                   "Set MSO_REQUIRE_GUI=ON to make this an error.")
  endif()
endif()

# mso/generated/dependencies.hpp (About box notices) from deps/manifest.json; target mso::generated.
mso_generate_dependency_notices(SYSTEM_VERSIONS ${_mso_system_versions})
target_link_libraries(mso_cli PRIVATE mso::generated)

if(MSO_BUILD_TESTS)
  add_subdirectory(tests)
endif()
mso_finalize_module_graph()

include(cmake/Install.cmake)
include(cmake/Packaging.cmake)
