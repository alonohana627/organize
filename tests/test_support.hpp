#pragma once
/// @file
/// Helpers shared by the catalog, search, import, export and application tests.
#include <cstdint>
#include <filesystem>
#include <memory>
#include <string>
#include <vector>

#include "mso/catalog/library.hpp"

namespace mso::test {

/// A fresh directory under the system temp directory, removed with its contents on destruction.
class ScratchDirectory {
public:
    ScratchDirectory();
    ~ScratchDirectory();
    ScratchDirectory(const ScratchDirectory&) = delete;
    ScratchDirectory& operator=(const ScratchDirectory&) = delete;
    const std::filesystem::path& path() const { return path_; }
    std::filesystem::path operator/(std::string_view relative) const { return path_ / path_from_utf8(relative); }

private:
    std::filesystem::path path_;
};

/// Writes a mono 16-bit PCM WAV of `seconds` of a sine tone.
void write_sine_wav(const std::filesystem::path& file, double seconds, double frequency = 440.0,
                    int sample_rate = 44100);

/// A synthetic scanned file with probe metadata, for catalog tests that need no real audio.
catalog::ScannedFile synthetic_file(std::string relative_path, std::int64_t size, std::int64_t duration_us,
                                    std::string format = "wav");

/// Creates a library with one referenced root and commits `files` in one scan.
struct PopulatedLibrary {
    std::unique_ptr<catalog::Library> library;
    std::int64_t root_id = 0;
};
PopulatedLibrary make_library(const std::filesystem::path& directory, const std::vector<catalog::ScannedFile>& files,
                              std::string_view name = "Test");

/// Sample id by display name (fails the current test when absent).
std::int64_t sample_id_named(catalog::Library& library, std::string_view display_name);

}  // namespace mso::test
