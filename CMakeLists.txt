cmake_minimum_required(VERSION 3.28)
project(MusicSampleOrganizer VERSION 0.1.0 LANGUAGES C CXX)

# Toolchain contract: clang with libc++ (MSYS2 CLANG64 on Windows), C++23, no compiler extensions.
set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_C_STANDARD 11)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

if(NOT CMAKE_CXX_COMPILER_ID MATCHES "Clang")
  message(FATAL_ERROR "Music Sample Organizer is built with clang + libc++; got ${CMAKE_CXX_COMPILER_ID}")
endif()

option(MSO_BUILD_GUI "Build the Qt 6 desktop application when Qt 6 is available" ON)
option(MSO_WITH_FFMPEG "Decode AAC/Opus/ALAC/WMA through FFmpeg when its libraries are available" ON)
option(MSO_BUILD_TESTS "Build the test executable and register it with CTest" ON)
option(MSO_WARNINGS_AS_ERRORS "Treat compiler warnings as errors in project code" OFF)

include(cmake/ProjectOptions.cmake)
add_subdirectory(deps)

# ---------------------------------------------------------------- core modules
# Each module is one static library: sources in src/<module>/, headers in include/mso/<module>/.
# Dependencies point downward only:
# domain <- catalog <- search; domain <- media <- {audio, analysis}; files -> media;
# app -> everything below; cli/gui/worker are leaves.
function(mso_module name dir)
  file(GLOB _src CONFIGURE_DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/src/${dir}/*.cpp)
  add_library(${name} STATIC ${_src})
  target_include_directories(${name} PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include)
  mso_apply_options(${name})
endfunction()

mso_module(mso_domain domain)
target_link_libraries(mso_domain PUBLIC mso::stduuid mso::json mso::picosha2 PRIVATE mso::utf8proc)

mso_module(mso_catalog catalog)
target_link_libraries(mso_catalog PUBLIC mso_domain mso::sqlite3)

mso_module(mso_search search)
target_link_libraries(mso_search PUBLIC mso_catalog)

mso_module(mso_media media)
target_link_libraries(mso_media PUBLIC mso_domain PRIVATE mso::miniaudio mso::utf8proc)
if(MSO_WITH_FFMPEG AND TARGET mso::ffmpeg)
  message(STATUS "MSO: media module links FFmpeg")
  target_link_libraries(mso_media PRIVATE mso::ffmpeg)
  target_compile_definitions(mso_media PRIVATE MSO_HAVE_FFMPEG=1)
endif()

mso_module(mso_files files)
target_link_libraries(mso_files PUBLIC mso_catalog mso_media PRIVATE mso::thread_pool mso::readerwriterqueue)

mso_module(mso_audio audio)
target_link_libraries(mso_audio PUBLIC mso_media PRIVATE mso::miniaudio mso::readerwriterqueue mso::signalsmith_stretch)

mso_module(mso_analysis analysis)
target_link_libraries(mso_analysis PUBLIC mso_media mso::json PRIVATE mso::pocketfft)

mso_module(mso_app app)
target_link_libraries(mso_app PUBLIC mso_catalog mso_search mso_files mso_analysis mso_audio PRIVATE mso::csv_parser)

# ---------------------------------------------------------------- executables
add_executable(mso-worker src/worker/main.cpp)
target_link_libraries(mso-worker PRIVATE mso_analysis mso::json mso::picosha2)
mso_apply_options(mso-worker)

file(GLOB _cli_src CONFIGURE_DEPENDS src/cli/*.cpp)
add_executable(mso ${_cli_src})
target_link_libraries(mso PRIVATE mso_app mso::cli11)
mso_apply_options(mso)
add_dependencies(mso mso-worker)

if(MSO_BUILD_GUI)
  find_package(Qt6 6.5 QUIET COMPONENTS Widgets Concurrent)
  if(Qt6_FOUND)
    set(CMAKE_AUTOMOC ON)
    # Everything except main.cpp goes into mso_ui so the UI tests can link the models and panels.
    file(GLOB _ui_src CONFIGURE_DEPENDS src/ui/*.cpp include/mso/ui/*.hpp)
    list(FILTER _ui_src EXCLUDE REGEX "/src/ui/main\\.cpp$")
    add_library(mso_ui STATIC ${_ui_src})
    target_link_libraries(mso_ui PUBLIC mso_app Qt6::Widgets Qt6::Concurrent)
    mso_apply_options(mso_ui)

    qt_add_executable(MusicSampleOrganizer WIN32 src/ui/main.cpp)
    target_link_libraries(MusicSampleOrganizer PRIVATE mso_ui)
    target_compile_definitions(MusicSampleOrganizer PRIVATE MSO_VERSION_STRING="${PROJECT_VERSION}")
    mso_apply_options(MusicSampleOrganizer)
    add_dependencies(MusicSampleOrganizer mso-worker)

    if(MSO_BUILD_TESTS)
      # UI model tests run headless on the "offscreen" platform plugin. tests/CMakeLists.txt also
      # globs ui_tests.cpp into the Qt-free mso_tests, where MSO_UI_TESTS is undefined and it is empty.
      add_executable(mso_ui_tests tests/ui_tests.cpp tests/test_main.cpp)
      target_link_libraries(mso_ui_tests PRIVATE mso_ui mso::doctest)
      target_compile_definitions(mso_ui_tests PRIVATE MSO_UI_TESTS=1)
      mso_apply_options(mso_ui_tests)
      enable_testing()
      add_test(NAME ui COMMAND mso_ui_tests --test-suite=ui --no-intro)
      set_tests_properties(ui PROPERTIES ENVIRONMENT "QT_QPA_PLATFORM=offscreen")
    endif()
    message(STATUS "MSO: building Qt ${Qt6_VERSION} desktop application")
  else()
    message(STATUS "MSO: Qt 6 not found; desktop application skipped (CLI and core still build)")
  endif()
endif()

if(MSO_BUILD_TESTS)
  enable_testing()
  add_subdirectory(tests)
endif()
