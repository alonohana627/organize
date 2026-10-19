// libFuzzer target: the bounded metadata probe (media::probe_memory) on damaged or unusual file bytes.
//
// One source, several targets: CMake compiles it once per container family with
// MSO_FUZZ_FILE_NAME_HINT set to a file name ("sample.mp3", "sample.m4a", ...). The probe identifies
// the container from the bytes and uses the name only as a hint, so every target can reach every
// parser; the hint and the seed corpus (tests/fuzz/corpus/<target>/) steer each target towards its
// family: RIFF/AIFF/CAF chunks, FLAC and Vorbis comments, MPEG frames with ID3v1/ID3v2, Ogg pages
// with Vorbis comments and OpusTags, MP4 atoms, ASF objects, and APEv2 tags (WavPack, MP3).
//
// The probe must never crash, read out of bounds or leak whatever the bytes say; AddressSanitizer
// and UndefinedBehaviorSanitizer turn any such defect into a fuzzer crash. Errors are expected
// results here, not findings.
#include <cstddef>
#include <cstdint>
#include <span>

#include "mso/media/media.hpp"

#ifndef MSO_FUZZ_FILE_NAME_HINT
#error "Set MSO_FUZZ_FILE_NAME_HINT to the file name the probe sees (tests/fuzz/CMakeLists.txt)"
#endif

/// libFuzzer entry point: probes `size` bytes at `data` as the content of a file named
/// MSO_FUZZ_FILE_NAME_HINT, with the production byte and time limits.
extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t* data, std::size_t size) {
    const std::span<const std::uint8_t> bytes(data, size);
    const auto probed = mso::media::probe_memory(bytes, MSO_FUZZ_FILE_NAME_HINT);
    // A result or an error are both fine; reading the result keeps the optimizer from dropping the
    // work and checks the documented bound on consumed bytes.
    if (probed && probed->bytes_read > mso::media::ProbeLimits{}.max_bytes) __builtin_trap();
    return 0;
}
