# libFuzzer targets, one per entry point for damaged or unusual input (crash-proofing) (built with -DMSO_BUILD_FUZZERS=ON, which the
# `fuzz` preset sets together with AddressSanitizer and UndefinedBehaviorSanitizer; see
# cmake/ProjectOptions.cmake for how the instrumentation is applied).
#
#   build/<preset>/tests/fuzz/fuzz_<name>.exe     one executable per target
#   tests/fuzz/corpus/<name>/                     its seed corpus (small, checked in, never written to)
#
# Run them all for a fixed time with tools/run_fuzzers.py (CI runs 60 s each), or one by hand:
#   build/fuzz/tests/fuzz/fuzz_query_parser.exe -max_total_time=60 <scratch corpus dir> tests/fuzz/corpus/query_parser
# libFuzzer writes new inputs into the FIRST corpus directory, so give it a scratch directory first and
# the checked-in seeds second.

set(MSO_FUZZ_CORPUS_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}/corpus")

# Adds the fuzz target fuzz_<name> from `SOURCE`, linked with `LIBRARIES` and the libFuzzer runtime.
function(mso_add_fuzzer name)
  cmake_parse_arguments(PARSE_ARGV 1 ARG "" "SOURCE" "LIBRARIES;DEFINITIONS")
  if(NOT EXISTS "${MSO_FUZZ_CORPUS_DIRECTORY}/${name}")
    message(FATAL_ERROR "Fuzz target ${name} has no seed corpus: add tests/fuzz/corpus/${name}/")
  endif()
  add_executable(fuzz_${name} ${ARG_SOURCE})
  target_link_libraries(fuzz_${name} PRIVATE ${ARG_LIBRARIES} ${MSO_FUZZER_LINK_LIBRARIES})
  target_compile_definitions(fuzz_${name} PRIVATE ${ARG_DEFINITIONS})
  mso_apply_options(fuzz_${name})
  set_property(GLOBAL APPEND PROPERTY MSO_FUZZ_TARGETS ${name})
endfunction()

# The media probe through media::probe_memory, once per container family (the file-name hint and the
# seeds steer each target; the probe still identifies the container from the bytes).
foreach(family_and_hint IN ITEMS "probe_riff=sample.wav" "probe_flac=sample.flac" "probe_mpeg_id3=sample.mp3"
                                 "probe_ogg=sample.ogg" "probe_mp4=sample.m4a" "probe_asf=sample.wma"
                                 "probe_ape=sample.wv")
  string(REPLACE "=" ";" family_and_hint "${family_and_hint}")
  list(GET family_and_hint 0 family)
  list(GET family_and_hint 1 hint)
  mso_add_fuzzer(${family} SOURCE fuzz_media_probe.cpp LIBRARIES mso_media
                 DEFINITIONS "MSO_FUZZ_FILE_NAME_HINT=\"${hint}\"")
endforeach()

mso_add_fuzzer(stb_vorbis SOURCE fuzz_stb_vorbis.cpp LIBRARIES mso::miniaudio)
mso_add_fuzzer(worker_protocol SOURCE fuzz_worker_protocol.cpp LIBRARIES mso_worker)
mso_add_fuzzer(query_parser SOURCE fuzz_query_parser.cpp LIBRARIES mso_search)
mso_add_fuzzer(cursor SOURCE fuzz_cursor.cpp LIBRARIES mso_app)

# `cmake --build <dir> --target fuzzers` builds every target.
get_property(_mso_fuzz_targets GLOBAL PROPERTY MSO_FUZZ_TARGETS)
list(TRANSFORM _mso_fuzz_targets PREPEND "fuzz_")
add_custom_target(fuzzers)
add_dependencies(fuzzers ${_mso_fuzz_targets})
