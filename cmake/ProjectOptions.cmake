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

# ---------------------------------------------------------------- standard library hardening
# Debug builds run libc++ in its "debug" hardening mode: bounds, iterator and comparator checks abort
# with a message instead of corrupting memory. The mode does not change the ABI, so it may differ from
# the (prebuilt) Qt and FFmpeg binaries. Release configurations keep the vendor default.
add_compile_definitions(
  "$<$<AND:$<COMPILE_LANGUAGE:CXX>,$<CONFIG:Debug>>:_LIBCPP_HARDENING_MODE=_LIBCPP_HARDENING_MODE_DEBUG>")

# ---------------------------------------------------------------- warnings
# Every flag was checked against the whole tree (moc output included) before it was added, so -Werror
# stays green. Deliberately not enabled yet:
#   -Wsign-conversion, -Wimplicit-int-float-conversion: a few implicit conversions remain in
#     src/media/decoder_vorbis.cpp, src/media/probe_ogg.cpp and src/ui/waveform_view.cpp;
#   -Wdouble-promotion: the float DSP code in src/analysis promotes to double in about 20 places;
#   -Wmissing-prototypes: nlohmann's ADL to_json/from_json hooks in src/analysis/worker_protocol.cpp;
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
