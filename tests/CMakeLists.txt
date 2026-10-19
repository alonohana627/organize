# Test executables and their CTest entries.
#
#   mso_tests     (build/<preset>/tests/)  every tests/*.cpp except the Qt UI files; Qt-free
#   mso_ui_tests  (build/<preset>/)        tests/ui_*.cpp, linked with the Qt UI; runs offscreen
#
# CTest entries are generated from the sources: one entry per TEST_SUITE("<name>") found at configure
# time (labels: the suite name plus core/gui), plus one catch-all entry per executable that runs every
# test case outside those suites. A suite added after the last configure therefore still runs (in the
# catch-all) until the next configure gives it its own entry. A named entry that matches no test case
# fails, so a stale or misspelled suite cannot pass silently.

set(MSO_TEST_TIMEOUT 300 CACHE STRING "CTest timeout in seconds for each test-suite entry")
if(MSO_SANITIZERS AND MSO_TEST_TIMEOUT LESS 900)
  set(_mso_test_timeout 900)  # instrumented builds run several times slower
else()
  set(_mso_test_timeout ${MSO_TEST_TIMEOUT})
endif()

# Environment shared by every test process (the analysis worker child inherits it).
set(_mso_test_environment "")
if(MSO_REQUIRE_FFMPEG)
  # FFmpeg-dependent media tests fail instead of skipping when ffmpeg or the FFmpeg backend is missing.
  list(APPEND _mso_test_environment "MSO_REQUIRE_FFMPEG=1")
endif()
if(MSO_SANITIZERS)
  list(APPEND _mso_test_environment
       "UBSAN_OPTIONS=print_stacktrace=1:halt_on_error=1"
       "ASAN_OPTIONS=abort_on_error=0:detect_stack_use_after_return=1:strict_string_checks=1")
endif()

# ---------------------------------------------------------------- shared helpers
# tests/test_support.{hpp,cpp}: scratch directories, WAV writers, polling waits, catalog fixtures and
# the worker executable path ($<TARGET_FILE:mso-worker>, so no test has to guess where it is).
add_library(mso_test_support STATIC test_support.cpp)
target_include_directories(mso_test_support PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}")
target_link_libraries(mso_test_support PUBLIC mso_catalog mso::doctest)
target_compile_definitions(mso_test_support PUBLIC MSO_TEST_WORKER_EXECUTABLE="$<TARGET_FILE:mso-worker>")
mso_apply_options(mso_test_support)

# Registers one CTest entry per TEST_SUITE found in `sources`, plus the catch-all entry `catch_all`.
function(mso_register_test_suites executable catch_all group)
  cmake_parse_arguments(PARSE_ARGV 3 ARG "" "" "SOURCES;ENVIRONMENT")
  set(suites "")
  foreach(source IN LISTS ARG_SOURCES)
    # Declarations only: the line starts with TEST_SUITE (a "}  // TEST_SUITE(...)" comment does not count).
    file(STRINGS "${source}" declarations REGEX "^[ \t]*TEST_SUITE(_BEGIN)?[ \t]*\\([ \t]*\"[^\"]+\"")
    foreach(declaration IN LISTS declarations)
      string(REGEX MATCHALL "TEST_SUITE(_BEGIN)?[ \t]*\\([ \t]*\"[^\"]+\"" matches "${declaration}")
      foreach(match IN LISTS matches)
        string(REGEX REPLACE "^.*\"([^\"]+)\"$" "\\1" suite "${match}")
        list(APPEND suites "${suite}")
      endforeach()
    endforeach()
  endforeach()
  list(REMOVE_DUPLICATES suites)
  list(SORT suites)

  foreach(suite IN LISTS suites)
    if(TEST "${suite}")
      message(FATAL_ERROR "TEST_SUITE(\"${suite}\") is used by two test executables; suite names must be unique")
    endif()
    add_test(NAME "${suite}" COMMAND ${executable} "--test-suite=${suite}" --no-intro)
    # doctest exits 0 when a filter matches nothing; an empty named entry is a configuration error.
    set_tests_properties("${suite}" PROPERTIES TIMEOUT ${_mso_test_timeout} ENVIRONMENT "${ARG_ENVIRONMENT}"
                         LABELS "${group};${suite}" FAIL_REGULAR_EXPRESSION "test cases: +0 \\|")
  endforeach()
  list(JOIN suites "," excluded)
  if(excluded STREQUAL "")
    add_test(NAME "${catch_all}" COMMAND ${executable} --no-intro)
  else()
    add_test(NAME "${catch_all}" COMMAND ${executable} "--test-suite-exclude=${excluded}" --no-intro)
  endif()
  set_tests_properties("${catch_all}" PROPERTIES TIMEOUT ${_mso_test_timeout} ENVIRONMENT "${ARG_ENVIRONMENT}"
                       LABELS "${group};catch-all")
endfunction()

file(GLOB _mso_test_sources CONFIGURE_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/*.cpp")
list(REMOVE_ITEM _mso_test_sources "${CMAKE_CURRENT_SOURCE_DIR}/test_main.cpp"
                                   "${CMAKE_CURRENT_SOURCE_DIR}/test_support.cpp")
set(_mso_ui_test_sources ${_mso_test_sources})
list(FILTER _mso_ui_test_sources INCLUDE REGEX "/ui_[^/]*\\.cpp$")
list(FILTER _mso_test_sources EXCLUDE REGEX "/ui_[^/]*\\.cpp$")

# ---------------------------------------------------------------- core tests (no Qt)
add_executable(mso_tests test_main.cpp ${_mso_test_sources})
target_link_libraries(mso_tests PRIVATE mso_app mso_audio mso_cli mso_test_support mso::generated mso::doctest)
mso_apply_options(mso_tests)
add_dependencies(mso_tests mso-worker)  # worker tests spawn it
mso_register_test_suites(mso_tests unlisted-suites core
                         SOURCES ${_mso_test_sources} ENVIRONMENT ${_mso_test_environment})

# ---------------------------------------------------------------- UI tests (Qt, offscreen)
if(TARGET mso_ui)
  add_executable(mso_ui_tests test_main.cpp ${_mso_ui_test_sources})
  target_link_libraries(mso_ui_tests PRIVATE mso_ui mso_test_support mso::doctest)
  # Next to the application and mso-worker (as before), so the Application under test finds the worker.
  set_target_properties(mso_ui_tests PROPERTIES AUTOMOC ON RUNTIME_OUTPUT_DIRECTORY "${PROJECT_BINARY_DIR}")
  mso_apply_options(mso_ui_tests)
  add_dependencies(mso_ui_tests mso-worker)
  mso_register_test_suites(mso_ui_tests ui-unlisted-suites gui
                           SOURCES ${_mso_ui_test_sources}
                           ENVIRONMENT ${_mso_test_environment} "QT_QPA_PLATFORM=offscreen" "MSO_AUDIO_BACKEND=null")
elseif(_mso_ui_test_sources)
  message(STATUS "MSO: UI tests skipped (no Qt UI in this configuration)")
endif()
