#include "test_support.hpp"

#include <doctest.h>

#include <algorithm>
#include <bit>
#include <cctype>
#include <cmath>
#include <cstdlib>
#include <fstream>
#include <iterator>
#include <numbers>
#include <system_error>
#include <thread>

#ifndef MSO_TEST_WORKER_EXECUTABLE
#error "tests/CMakeLists.txt defines MSO_TEST_WORKER_EXECUTABLE as $<TARGET_FILE:mso-worker>"
#endif

namespace mso::test {

// ---------------------------------------------------------------- files and directories

ScratchDirectory::ScratchDirectory(std::string_view label)
    : path_(std::filesystem::temp_directory_path() /
            path_from_utf8("mso-" + std::string(label) + "-" + uuids::to_string(new_uuid()))) {
    std::error_code error;
    std::filesystem::create_directories(path_, error);
    REQUIRE_MESSAGE(!error, "cannot create scratch directory: ", error.message());
}

ScratchDirectory::~ScratchDirectory() {
    std::error_code ignored;  // a leftover temp directory must not fail (or terminate) a test
    std::filesystem::remove_all(path_, ignored);
}

std::filesystem::path ScratchDirectory::operator/(std::string_view relative) const {
    return path_ / path_from_utf8(relative);
}

namespace {

/// Creates the parent directory of `file` (if any); fails the current test on error.
void create_parent_directories(const std::filesystem::path& file) {
    if (!file.has_parent_path()) return;
    std::error_code error;
    std::filesystem::create_directories(file.parent_path(), error);
    REQUIRE_MESSAGE(!error, "cannot create ", path_to_utf8(file.parent_path()), ": ", error.message());
}

}  // namespace

void write_file(const std::filesystem::path& file, std::span<const std::uint8_t> bytes) {
    write_file(file, std::string_view(reinterpret_cast<const char*>(bytes.data()), bytes.size()));
}

void write_file(const std::filesystem::path& file, std::string_view text) {
    create_parent_directories(file);
    std::ofstream stream(file, std::ios::binary | std::ios::trunc);
    REQUIRE_MESSAGE(stream.is_open(), "cannot open ", path_to_utf8(file), " for writing");
    stream.write(text.data(), static_cast<std::streamsize>(text.size()));
    stream.close();
    REQUIRE_MESSAGE(stream.good(), "cannot write ", path_to_utf8(file));
}

std::string read_file(const std::filesystem::path& file) {
    std::ifstream stream(file, std::ios::binary);
    REQUIRE_MESSAGE(stream.is_open(), "cannot open ", path_to_utf8(file), " for reading");
    return {std::istreambuf_iterator<char>(stream), std::istreambuf_iterator<char>()};
}

// ---------------------------------------------------------------- WAV fixtures

namespace {

/// Bits per stored sample of an encoding.
std::uint32_t bits_per_sample(WavEncoding encoding) {
    switch (encoding) {
        case WavEncoding::Int16:
            return 16;
        case WavEncoding::Int24:
            return 24;
        case WavEncoding::Float32:
            return 32;
    }
    return 16;
}

/// Little-endian byte appender for building RIFF images.
class LittleEndianBuffer {
public:
    /// Appends the low `byte_count` bytes of `value`, least significant first.
    void put(std::uint64_t value, int byte_count) {
        for (int byte = 0; byte < byte_count; ++byte)
            bytes_.push_back(static_cast<std::uint8_t>(value >> (8 * byte)));
    }
    /// Appends a four-character chunk identifier.
    void put_tag(std::string_view tag) { bytes_.insert(bytes_.end(), tag.begin(), tag.end()); }
    /// Reserves room for `byte_count` more bytes.
    void reserve_more(std::size_t byte_count) { bytes_.reserve(bytes_.size() + byte_count); }
    /// Takes the finished image.
    std::vector<std::uint8_t> take() { return std::move(bytes_); }

private:
    std::vector<std::uint8_t> bytes_;
};

/// Writes the 44-byte canonical header for `sample_count` interleaved samples of `format`.
LittleEndianBuffer wav_header(std::size_t sample_count, const WavFormat& format) {
    REQUIRE(format.channels > 0);
    REQUIRE(format.sample_rate > 0);
    const std::uint32_t bits = bits_per_sample(format.encoding);
    const std::uint64_t bytes_per_sample = bits / 8U;
    const std::uint64_t data_size = sample_count * bytes_per_sample;
    REQUIRE(data_size <= 0xFFFFFFFFULL - 36U);
    const auto channels = static_cast<std::uint64_t>(format.channels);
    const auto sample_rate = static_cast<std::uint64_t>(format.sample_rate);

    LittleEndianBuffer buffer;
    buffer.reserve_more(44 + data_size);
    buffer.put_tag("RIFF");
    buffer.put(36 + data_size, 4);
    buffer.put_tag("WAVE");
    buffer.put_tag("fmt ");
    buffer.put(16, 4);
    buffer.put(format.encoding == WavEncoding::Float32 ? 3U : 1U, 2);
    buffer.put(channels, 2);
    buffer.put(sample_rate, 4);
    buffer.put(sample_rate * channels * bytes_per_sample, 4);
    buffer.put(channels * bytes_per_sample, 2);
    buffer.put(bits, 2);
    buffer.put_tag("data");
    buffer.put(data_size, 4);
    return buffer;
}

}  // namespace

std::vector<std::uint8_t> wav_bytes(std::span<const std::int16_t> interleaved, const WavFormat& format) {
    LittleEndianBuffer buffer = wav_header(interleaved.size(), format);
    for (const std::int16_t sample : interleaved) {
        switch (format.encoding) {
            case WavEncoding::Int16:
                buffer.put(static_cast<std::uint16_t>(sample), 2);
                break;
            case WavEncoding::Int24:
                buffer.put(static_cast<std::uint32_t>(static_cast<std::int32_t>(sample) * 256), 3);
                break;
            case WavEncoding::Float32:
                buffer.put(std::bit_cast<std::uint32_t>(static_cast<float>(sample) / 32768.0F), 4);
                break;
        }
    }
    return buffer.take();
}

std::vector<std::uint8_t> wav_bytes(std::span<const float> interleaved, const WavFormat& format) {
    LittleEndianBuffer buffer = wav_header(interleaved.size(), format);
    for (const float sample : interleaved) {
        const double clamped = std::clamp(static_cast<double>(sample), -1.0, 1.0);
        switch (format.encoding) {
            case WavEncoding::Int16:
                buffer.put(static_cast<std::uint16_t>(static_cast<std::int16_t>(std::lround(clamped * 32767.0))), 2);
                break;
            case WavEncoding::Int24:
                buffer.put(static_cast<std::uint32_t>(static_cast<std::int32_t>(std::lround(clamped * 8388607.0))), 3);
                break;
            case WavEncoding::Float32:
                buffer.put(std::bit_cast<std::uint32_t>(sample), 4);
                break;
        }
    }
    return buffer.take();
}

void write_wav(const std::filesystem::path& file, std::span<const std::int16_t> interleaved, const WavFormat& format) {
    write_file(file, wav_bytes(interleaved, format));
}

void write_wav(const std::filesystem::path& file, std::span<const float> interleaved, const WavFormat& format) {
    write_file(file, wav_bytes(interleaved, format));
}

std::vector<float> sine_samples(double seconds, double frequency, int sample_rate, double amplitude, int channels) {
    REQUIRE(sample_rate > 0);
    REQUIRE(channels > 0);
    const auto frames = static_cast<std::size_t>(std::max(0.0, seconds) * sample_rate);
    std::vector<float> samples;
    samples.reserve(frames * static_cast<std::size_t>(channels));
    for (std::size_t frame = 0; frame < frames; ++frame) {
        const double phase = 2.0 * std::numbers::pi * frequency * static_cast<double>(frame) / sample_rate;
        const auto value = static_cast<float>(amplitude * std::sin(phase));
        samples.insert(samples.end(), static_cast<std::size_t>(channels), value);
    }
    return samples;
}

std::vector<std::int16_t> to_int16(std::span<const float> samples) {
    std::vector<std::int16_t> converted(samples.size());
    std::ranges::transform(samples, converted.begin(), [](float sample) {
        return static_cast<std::int16_t>(std::lround(std::clamp(static_cast<double>(sample), -1.0, 1.0) * 32767.0));
    });
    return converted;
}

void write_sine_wav(const std::filesystem::path& file, double seconds, double frequency, int sample_rate) {
    write_wav(file, std::span<const float>(sine_samples(seconds, frequency, sample_rate)),
              WavFormat{.channels = 1, .sample_rate = sample_rate, .encoding = WavEncoding::Int16});
}

// ---------------------------------------------------------------- timing and environment

bool wait_until(const std::function<bool()>& condition, std::chrono::milliseconds timeout,
                std::chrono::milliseconds poll_interval) {
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    while (!condition()) {
        if (std::chrono::steady_clock::now() >= deadline) return condition();
        std::this_thread::sleep_for(poll_interval);
    }
    return true;
}

std::filesystem::path worker_executable() {
    return path_from_utf8(MSO_TEST_WORKER_EXECUTABLE);
}

bool environment_flag(std::string_view name) {
    const char* value = std::getenv(std::string(name).c_str());
    if (value == nullptr) return false;
    std::string folded(value);
    std::ranges::transform(folded, folded.begin(),
                           [](unsigned char character) { return static_cast<char>(std::tolower(character)); });
    return !(folded.empty() || folded == "0" || folded == "false" || folded == "off" || folded == "no");
}

bool ffmpeg_required() {
    return environment_flag("MSO_REQUIRE_FFMPEG");
}

// ---------------------------------------------------------------- catalog fixtures

catalog::ScannedFile synthetic_file(std::string relative_path, std::int64_t size, std::int64_t duration_us,
                                    std::string format) {
    catalog::ScannedFile file;
    file.relative_path = std::move(relative_path);
    file.byte_size = size;
    file.mtime_ns = 1'700'000'000'000'000'000;
    catalog::ObservedMetadata metadata;
    metadata.format = std::move(format);
    metadata.codec = "pcm_s16";
    metadata.sample_rate = 44100;
    metadata.channels = 2;
    metadata.bit_depth = 16;
    metadata.duration_us = duration_us;
    metadata.duration_exact = true;
    file.metadata = metadata;
    return file;
}

PopulatedLibrary make_library(const std::filesystem::path& directory, const std::vector<catalog::ScannedFile>& files,
                              std::string_view name) {
    PopulatedLibrary populated;
    auto library = catalog::Library::create(directory, name);
    REQUIRE(library.has_value());
    populated.library = std::move(*library);
    auto root = populated.library->add_root("Samples", catalog::RootMode::Referenced);
    REQUIRE(root.has_value());
    populated.root_id = root->id;
    auto session = populated.library->begin_scan(root->id);
    REQUIRE(session.has_value());
    for (std::size_t offset = 0; offset < files.size(); offset += 500) {
        const std::span<const catalog::ScannedFile> batch(files.data() + offset,
                                                          std::min<std::size_t>(500, files.size() - offset));
        auto committed =
            populated.library->commit_scan_batch(*session, batch, {}, [](std::string_view) { return false; });
        REQUIRE(committed.has_value());
    }
    REQUIRE(populated.library->finish_scan(*session, false).has_value());
    return populated;
}

std::int64_t sample_id_named(catalog::Library& library, std::string_view display_name) {
    auto reader = library.acquire_reader();
    REQUIRE(reader.has_value());
    auto statement = reader->connection().prepare("SELECT id FROM samples WHERE display_name=?1");
    REQUIRE(statement.has_value());
    statement->bind(1, display_name);
    auto has_row = statement->next_row();
    REQUIRE(has_row.has_value());
    REQUIRE(*has_row);
    return statement->column_int64(0);
}

}  // namespace mso::test
