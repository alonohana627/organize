#pragma once
/// @file
/// Helpers shared by every test executable (mso_tests and mso_ui_tests link the mso_test_support
/// library): scratch directories, WAV fixture writers, deadline-based polling, the path of the
/// mso-worker built by this tree, the MSO_REQUIRE_FFMPEG switch, and catalog fixtures.
///
/// Use these instead of per-file copies; a helper that two test files need belongs here.
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <memory>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "mso/catalog/library.hpp"

namespace mso::test {

// ---------------------------------------------------------------- files and directories

/// A fresh, uniquely named directory under the system temp directory, removed with its contents on
/// destruction. Safe to use from tests running in parallel processes.
class ScratchDirectory {
public:
    /// Creates `<temp>/mso-<label>-<uuid>`; fails the current test when it cannot be created.
    explicit ScratchDirectory(std::string_view label = "test");
    /// Removes the directory tree (errors are ignored: a leftover temp directory must not fail a test).
    ~ScratchDirectory();
    ScratchDirectory(const ScratchDirectory&) = delete;
    ScratchDirectory& operator=(const ScratchDirectory&) = delete;

    /// The directory itself.
    const std::filesystem::path& path() const { return path_; }
    /// A path inside the directory; `relative` is UTF-8 and may contain `/` separators.
    std::filesystem::path operator/(std::string_view relative) const;
    /// Same as `*this / relative` (reads better for single file names).
    std::filesystem::path file(std::string_view relative) const { return *this / relative; }

private:
    std::filesystem::path path_;
};

/// Writes `bytes` to `file`, creating parent directories; fails the current test on error.
void write_file(const std::filesystem::path& file, std::span<const std::uint8_t> bytes);
/// Writes `text` (any bytes) to `file`, creating parent directories; fails the current test on error.
void write_file(const std::filesystem::path& file, std::string_view text);
/// The whole content of `file` as bytes in a string; fails the current test when it cannot be read.
std::string read_file(const std::filesystem::path& file);

// ---------------------------------------------------------------- WAV fixtures

/// Sample encoding of a generated WAV file.
enum class WavEncoding {
    Int16,    ///< 16-bit PCM (format tag 1)
    Int24,    ///< 24-bit packed PCM (format tag 1)
    Float32,  ///< 32-bit IEEE float (format tag 3)
};

/// Layout of a generated WAV file.
struct WavFormat {
    int channels = 1;                           ///< interleaved channel count
    int sample_rate = 44100;                    ///< frames per second
    WavEncoding encoding = WavEncoding::Int16;  ///< sample encoding
};

/// A canonical RIFF/WAVE image (44-byte header, `fmt ` then `data`) of interleaved 16-bit samples.
/// Int24 stores `sample * 256` and Float32 stores `sample / 32768`, so all three encodings carry exactly
/// the same audio.
std::vector<std::uint8_t> wav_bytes(std::span<const std::int16_t> interleaved, const WavFormat& format = {});
/// A canonical RIFF/WAVE image of interleaved float samples in [-1, 1]. Float32 stores them exactly;
/// integer encodings clamp and round to the nearest step.
std::vector<std::uint8_t> wav_bytes(std::span<const float> interleaved, const WavFormat& format = {});
/// Writes wav_bytes(interleaved, format) to `file`, creating parent directories.
void write_wav(const std::filesystem::path& file, std::span<const std::int16_t> interleaved,
               const WavFormat& format = {});
/// Writes wav_bytes(interleaved, format) to `file`, creating parent directories.
void write_wav(const std::filesystem::path& file, std::span<const float> interleaved, const WavFormat& format = {});

/// `seconds` of a sine at `frequency` and `amplitude`, as interleaved float samples with the same value
/// on every channel.
std::vector<float> sine_samples(double seconds, double frequency, int sample_rate, double amplitude = 0.5,
                                int channels = 1);
/// Converts float samples in [-1, 1] to 16-bit integers (clamped, rounded to nearest).
std::vector<std::int16_t> to_int16(std::span<const float> samples);
/// Writes a mono 16-bit PCM WAV of `seconds` of a half-scale sine tone.
void write_sine_wav(const std::filesystem::path& file, double seconds, double frequency = 440.0,
                    int sample_rate = 44100);

// ---------------------------------------------------------------- timing and environment

/// Polls `condition` every `poll_interval` until it holds or `timeout` elapses, and returns its final
/// value. Use it instead of sleeping for a fixed time: derive `timeout` from the configured interval
/// of the code under test and give it generous slack (sanitizer builds run several times slower).
bool wait_until(const std::function<bool()>& condition, std::chrono::milliseconds timeout,
                std::chrono::milliseconds poll_interval = std::chrono::milliseconds(5));

/// The mso-worker executable built by this tree (CMake passes `$<TARGET_FILE:mso-worker>`).
std::filesystem::path worker_executable();

/// True when the environment variable `name` is set to a value other than "", "0", "false", "off"
/// or "no" (case-insensitive).
bool environment_flag(std::string_view name);

/// True when FFmpeg-dependent tests must run rather than skip (MSO_REQUIRE_FFMPEG=1; CTest sets it
/// when the build was configured with -DMSO_REQUIRE_FFMPEG=ON, as CI is). A test that cannot run its
/// FFmpeg part must then FAIL; otherwise it may skip with a MESSAGE.
bool ffmpeg_required();

// ---------------------------------------------------------------- catalog fixtures

/// A synthetic scanned file with probe metadata, for catalog tests that need no real audio.
catalog::ScannedFile synthetic_file(std::string relative_path, std::int64_t size, std::int64_t duration_us,
                                    std::string format = "wav");

/// A library with one referenced root, as made by make_library().
struct PopulatedLibrary {
    std::unique_ptr<catalog::Library> library;  ///< the open library
    std::int64_t root_id = 0;                   ///< id of its single root
};

/// Creates a library in `directory` with one referenced root and commits `files` in one scan.
PopulatedLibrary make_library(const std::filesystem::path& directory, const std::vector<catalog::ScannedFile>& files,
                              std::string_view name = "Test");

/// Sample id by display name (fails the current test when absent).
std::int64_t sample_id_named(catalog::Library& library, std::string_view display_name);

}  // namespace mso::test
