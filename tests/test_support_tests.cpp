// Tests of the shared test helpers (tests/test_support.*) and of the generated dependency notices
// (mso/generated/dependencies.hpp, produced from deps/manifest.json at configure time).
#include <doctest.h>

#include <algorithm>
#include <bit>
#include <chrono>
#include <cmath>
#include <filesystem>
#include <numbers>
#include <string_view>
#include <vector>

#include "mso/generated/dependencies.hpp"
#include "test_support.hpp"

namespace {

using namespace std::chrono_literals;

/// Little-endian unsigned integer of `byte_count` bytes at `offset`.
std::uint64_t read_little_endian(const std::vector<std::uint8_t>& bytes, std::size_t offset, int byte_count) {
    std::uint64_t value = 0;
    for (int byte = byte_count - 1; byte >= 0; --byte)
        value = (value << 8) | bytes.at(offset + static_cast<std::size_t>(byte));
    return value;
}

/// The four bytes at `offset` as text.
std::string_view tag_at(const std::vector<std::uint8_t>& bytes, std::size_t offset) {
    return {reinterpret_cast<const char*>(bytes.data()) + offset, 4};
}

}  // namespace

TEST_SUITE("infra") {
    TEST_CASE("ScratchDirectory is unique, writable and removed on destruction") {
        std::filesystem::path first_path;
        {
            const mso::test::ScratchDirectory first("infra");
            const mso::test::ScratchDirectory second("infra");
            first_path = first.path();
            CHECK(first.path() != second.path());
            CHECK(std::filesystem::is_directory(first.path()));
            CHECK(first.path().filename().string().starts_with("mso-infra-"));

            const auto nested = first / "a/b/\xD7\xA9\xD7\x9C\xD7\x95\xD7\x9D.txt";  // Hebrew file name
            CHECK(nested == first.file("a/b/\xD7\xA9\xD7\x9C\xD7\x95\xD7\x9D.txt"));
            mso::test::write_file(nested, std::string_view("payload\r\n"));
            CHECK(mso::test::read_file(nested) == "payload\r\n");  // binary, no newline translation
        }
        CHECK_FALSE(std::filesystem::exists(first_path));
    }

    TEST_CASE("wav_bytes writes a canonical header and identical audio in every encoding") {
        const std::vector<std::int16_t> samples = {0, 16384, -16384, 32767, -32768, 1};
        for (const auto encoding :
             {mso::test::WavEncoding::Int16, mso::test::WavEncoding::Int24, mso::test::WavEncoding::Float32}) {
            CAPTURE(static_cast<int>(encoding));
            const mso::test::WavFormat format{.channels = 2, .sample_rate = 48000, .encoding = encoding};
            const auto bytes = mso::test::wav_bytes(std::span<const std::int16_t>(samples), format);
            const std::uint64_t bytes_per_sample = encoding == mso::test::WavEncoding::Int16   ? 2
                                                   : encoding == mso::test::WavEncoding::Int24 ? 3
                                                                                               : 4;
            REQUIRE(bytes.size() == 44 + samples.size() * bytes_per_sample);
            CHECK(tag_at(bytes, 0) == "RIFF");
            CHECK(read_little_endian(bytes, 4, 4) == bytes.size() - 8);
            CHECK(tag_at(bytes, 8) == "WAVE");
            CHECK(tag_at(bytes, 12) == "fmt ");
            CHECK(read_little_endian(bytes, 20, 2) == (encoding == mso::test::WavEncoding::Float32 ? 3U : 1U));
            CHECK(read_little_endian(bytes, 22, 2) == 2);
            CHECK(read_little_endian(bytes, 24, 4) == 48000);
            CHECK(read_little_endian(bytes, 28, 4) == 48000 * 2 * bytes_per_sample);
            CHECK(read_little_endian(bytes, 32, 2) == 2 * bytes_per_sample);
            CHECK(read_little_endian(bytes, 34, 2) == bytes_per_sample * 8);
            CHECK(tag_at(bytes, 36) == "data");
            CHECK(read_little_endian(bytes, 40, 4) == samples.size() * bytes_per_sample);

            // The second sample (16384, half scale) decodes to the same value in every encoding.
            const std::uint64_t raw =
                read_little_endian(bytes, 44 + bytes_per_sample, static_cast<int>(bytes_per_sample));
            switch (encoding) {
                case mso::test::WavEncoding::Int16:
                    CHECK(raw == 16384);
                    break;
                case mso::test::WavEncoding::Int24:
                    CHECK(raw == 16384U * 256U);
                    break;
                case mso::test::WavEncoding::Float32:
                    CHECK(std::bit_cast<float>(static_cast<std::uint32_t>(raw)) == 0.5F);
                    break;
            }
        }
    }

    TEST_CASE("float samples are clamped and rounded for integer encodings and exact for float") {
        const std::vector<float> samples = {0.5F, 2.0F, -2.0F};
        const auto int16 = mso::test::wav_bytes(std::span<const float>(samples));
        CHECK(read_little_endian(int16, 44, 2) == 16384);  // lround(0.5 * 32767)
        CHECK(read_little_endian(int16, 46, 2) == 32767);
        CHECK(read_little_endian(int16, 48, 2) == static_cast<std::uint16_t>(-32767));
        const auto float32 =
            mso::test::wav_bytes(std::span<const float>(samples), {.encoding = mso::test::WavEncoding::Float32});
        CHECK(std::bit_cast<float>(static_cast<std::uint32_t>(read_little_endian(float32, 52, 4))) == -2.0F);

        CHECK(mso::test::to_int16(std::span<const float>(samples)) == std::vector<std::int16_t>{16384, 32767, -32767});
    }

    TEST_CASE("sine_samples and write_sine_wav produce the requested length and level") {
        const auto stereo = mso::test::sine_samples(0.5, 1000.0, 8000, 0.25, 2);
        REQUIRE(stereo.size() == 8000);
        CHECK(stereo[0] == 0.0F);
        CHECK(stereo[2] == doctest::Approx(0.25 * std::sin(2.0 * std::numbers::pi / 8.0)));
        CHECK(stereo[2] == stereo[3]);  // same value on both channels
        CHECK(std::ranges::max(stereo) <= 0.25F);

        const mso::test::ScratchDirectory scratch("infra");
        const auto file = scratch / "tone.wav";
        mso::test::write_sine_wav(file, 0.1, 440.0, 44100);
        CHECK(std::filesystem::file_size(file) == 44 + 4410 * 2);
    }

    TEST_CASE("wait_until polls until the condition holds or the deadline passes") {
        int polls = 0;
        CHECK(mso::test::wait_until([&polls] { return ++polls == 3; }, 10s, 1ms));
        CHECK(polls == 3);  // stops polling as soon as the condition holds

        const auto started = std::chrono::steady_clock::now();
        CHECK_FALSE(mso::test::wait_until([] { return false; }, 30ms, 1ms));
        CHECK(std::chrono::steady_clock::now() - started >= 30ms);
    }

    TEST_CASE("worker_executable names the mso-worker built by this tree") {
        const auto worker = mso::test::worker_executable();
        CHECK(worker.is_absolute());
        CHECK(worker.stem() == "mso-worker");
        CHECK(std::filesystem::is_regular_file(worker));  // mso_tests depends on mso-worker
    }

    TEST_CASE("environment_flag treats unset, empty, 0, false, off and no as false") {
        CHECK_FALSE(mso::test::environment_flag("MSO_TEST_SUPPORT_SURELY_UNSET_VARIABLE"));
        // ffmpeg_required() mirrors MSO_REQUIRE_FFMPEG; CTest sets it in builds configured to require FFmpeg.
        CHECK(mso::test::ffmpeg_required() == mso::test::environment_flag("MSO_REQUIRE_FFMPEG"));
    }

    TEST_CASE("generated dependency notices cover the manifest with license texts") {
        using mso::generated::kDependencies;
        CHECK_FALSE(mso::generated::kProjectVersion.empty());
        const auto find = [](std::string_view name) {
            return std::ranges::find(kDependencies, name, &mso::generated::DependencyNotice::name);
        };
        for (const std::string_view name : {"sqlite", "nlohmann_json", "utf8proc", "stduuid", "picosha2", "miniaudio",
                                            "pocketfft", "signalsmith_linear", "signalsmith_stretch", "thread_pool",
                                            "readerwriterqueue", "csv_parser", "cli11", "doctest"}) {
            CAPTURE(name);
            const auto notice = find(name);
            REQUIRE(notice != std::end(kDependencies));
            CHECK_FALSE(notice->system_library);
            CHECK_FALSE(notice->version.empty());
            CHECK_FALSE(notice->license.empty());
            CHECK_FALSE(notice->purpose.empty());
            CHECK(notice->url.starts_with("https://"));
            // Every non-public-domain dependency ships its notice text (MIT/BSD require it).
            if (notice->license != "Public domain") CHECK(notice->license_text.size() > 100);
        }
        for (const auto& notice : kDependencies) {
            if (notice.system_library) {
                CHECK((notice.name == "Qt" || notice.name == "FFmpeg"));
                CHECK_FALSE(notice.license.empty());
            }
        }
    }
}  // TEST_SUITE("infra")
