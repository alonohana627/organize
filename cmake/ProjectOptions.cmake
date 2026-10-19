# Compiler options shared by every first-party target (modules, executables, tests). Third-party code
# under deps/ is built with its own quiet flags and consumed through SYSTEM include directories.

# ---------------------------------------------------------------- sanitizers
set(MSO_SANITIZERS "" CACHE STRING
  "Sanitizers for the whole build, as a list of address and/or undefined (e.g. \"address;undefined\"); empty = none")
set_property(CACHE MSO_SANITIZERS PROPERTY STRINGS "" "address" "undefined" "address;undefined")
foreach(_mso_sanitizer IN LISTS MSO_SANITIZERS)
  if(NOT _mso_sanitizer MATCHES "^(address|undefined)$")
    message(FATAL_ERROR "MSO_SANITIZERS: unsupported sanitizer '${_mso_sanitizer}' (use address and/or undefined)")
  endif()
endforeach()
if(MSO_SANITIZERS)
  # Applied to every target, vendored C code included, so instrumented and uninstrumented code never
  # share a container or an allocation. UBSan findings abort, so a test run cannot pass over them.
  list(JOIN MSO_SANITIZERS "," _mso_sanitizer_list)
  add_compile_options(-fsanitize=${_mso_sanitizer_list} -fno-omit-frame-pointer -fno-sanitize-recover=all)
  add_link_options(-fsanitize=${_mso_sanitizer_list})
  message(STATUS "MSO: sanitizers enabled: ${_mso_sanitizer_list}")
endif()

# ---------------------------------------------------------------- fuzzing
# MSO_BUILD_FUZZERS instruments the whole build (project and vendored code, so the fuzzer sees
# coverage inside stb_vorbis or SQLite too) for libFuzzer and builds the targets in tests/fuzz.
# libFuzzer only finds memory errors together with AddressSanitizer, so that sanitizer is required.
if(MSO_BUILD_FUZZERS)
  if(NOT "address" IN_LIST MSO_SANITIZERS)
    message(FATAL_ERROR "MSO_BUILD_FUZZERS=ON needs MSO_SANITIZERS to include address (use the fuzz preset)")
  endif()
  if(WIN32)
    # clang's driver rejects -fsanitize=fuzzer for the MinGW target, although MSYS2 CLANG64 ships the
    # runtime (lib/clang/<version>/lib/windows/libclang_rt.fuzzer-x86_64.a). The same instrumentation
    # that -fsanitize=fuzzer-no-link selects is therefore spelled out, and the runtime, which provides
    # main(), is linked into each fuzz target by path.
    add_compile_options(-fsanitize-coverage=inline-8bit-counters,indirect-calls,trace-cmp,pc-table)
    execute_process(COMMAND "${CMAKE_CXX_COMPILER}" -print-resource-dir
                    OUTPUT_VARIABLE _mso_clang_resource_directory OUTPUT_STRIP_TRAILING_WHITESPACE)
    find_file(MSO_FUZZER_RUNTIME NAMES libclang_rt.fuzzer-x86_64.a
              PATHS "${_mso_clang_resource_directory}/lib/windows" NO_DEFAULT_PATH)
    if(NOT MSO_FUZZER_RUNTIME)
      message(FATAL_ERROR "MSO_BUILD_FUZZERS: libclang_rt.fuzzer-x86_64.a not found under "
                          "${_mso_clang_resource_directory}/lib/windows (MSYS2: mingw-w64-clang-x86_64-compiler-rt)")
    endif()
    set(MSO_FUZZER_LINK_LIBRARIES "${MSO_FUZZER_RUNTIME}")
  else()
    add_compile_options(-fsanitize=fuzzer-no-link)
    set(MSO_FUZZER_LINK_LIBRARIES -fsanitize=fuzzer)
  endif()
  message(STATUS "MSO: libFuzzer targets enabled (tests/fuzz)")
endif()

# ---------------------------------------------------------------- standard library hardening
# Debug builds run libc++ in its "debug" hardening mode: bounds, iterator and comparator checks abort
# with a message instead of corrupting memory. The mode does not change the ABI, so it may differ from
# the (prebuilt) Qt and FFmpeg binaries. Release configurations keep the vendor default.
add_compile_definitions(
  "$<$<AND:$<COMPILE_LANGUAGE:CXX>,$<CONFIG:Debug>>:_LIBCPP_HARDENING_MODE=_LIBCPP_HARDENING_MODE_DEBUG>")
# Fuzz builds are optimized (fuzzing needs the speed) but keep libc++'s "extensive" checks (bounds,
# iterators, preconditions), so an out-of-range container access is a fuzzer finding, not a silent
# read that AddressSanitizer may miss inside an allocation.
if(MSO_BUILD_FUZZERS)
  add_compile_definitions(
    "$<$<AND:$<COMPILE_LANGUAGE:CXX>,$<NOT:$<CONFIG:Debug>>>:_LIBCPP_HARDENING_MODE=_LIBCPP_HARDENING_MODE_EXTENSIVE>")
endif()

# ---------------------------------------------------------------- warnings
# Every flag was checked against the whole tree (moc output included) before it was added, so -Werror
# stays green. Deliberately not enabled yet:
#   -Wsign-conversion, -Wimplicit-int-float-conversion: a few implicit conversions remain in
#     src/media/decoder_vorbis.cpp, src/media/probe_ogg.cpp and src/ui/waveform_view.cpp;
#   -Wdouble-promotion: the float DSP code in src/analysis promotes to double in about 20 places;
#   -Wmissing-prototypes: nlohmann's ADL to_json/from_json hooks in src/worker/worker_protocol.cpp;
#   -Wshadow-all: Qt lambdas in src/ui reuse outer names on purpose.
set(MSO_WARNING_FLAGS
  -Wall -Wextra -Wpedantic
  -Wshadow -Wnon-virtual-dtor -Woverloaded-virtual -Wsuggest-override -Winconsistent-missing-destructor-override
  -Wold-style-cast -Wcast-qual -Wcast-function-type -Wzero-as-null-pointer-constant
  -Wconversion -Wno-sign-conversion -Wno-implicit-int-float-conversion
  -Wextra-semi -Wnewline-eof -Wundef -Wcomma -Wformat=2 -Wnull-dereference
  -Wmove -Wloop-analysis -Wdeprecated -Wdocumentation
  -Wimplicit-fallthrough -Wno-missing-field-initializers)

# Applies the project warnings and platform definitions to one first-party target.
function(mso_apply_options target)
  target_compile_options(${target} PRIVATE ${MSO_WARNING_FLAGS})
  if(MSO_WARNINGS_AS_ERRORS)
    target_compile_options(${target} PRIVATE -Werror)
  endif()
  if(WIN32)
    # Unicode Win32 APIs only, no min/max macros, Windows 10+ API surface (Windows 11 is the target).
    target_compile_definitions(${target} PRIVATE UNICODE _UNICODE NOMINMAX WIN32_LEAN_AND_MEAN _WIN32_WINNT=0x0A00)
  endif()
endfunction()
