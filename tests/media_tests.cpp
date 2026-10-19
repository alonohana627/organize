// Media module: bounded probes, streaming decoders, extension filter and filename hints.
// Fixtures are generated into a temporary directory; FFmpeg-generated fixtures are skipped when
// ffmpeg is not on PATH. No audio file is ever committed.
#include <doctest.h>

#include <algorithm>
#include <bit>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <random>
#include <string>
#include <string_view>
#include <vector>

#include "mso/media/media.hpp"

namespace {

namespace fs = std::filesystem;
using mso::ErrorCode;
using mso::KeyMode;
using mso::MusicalKey;
using namespace mso::media;

// ---------------------------------------------------------------- fixture helpers

/// Temporary directory removed with everything in it when the test ends.
class TempDirectory {
public:
    TempDirectory() {
        std::random_device random;
        path_ = fs::temp_directory_path() / ("mso_media_tests_" + std::to_string(random()));
        fs::create_directories(path_);
    }
    TempDirectory(const TempDirectory&) = delete;
    TempDirectory& operator=(const TempDirectory&) = delete;
    ~TempDirectory() {
        std::error_code ignored;
        fs::remove_all(path_, ignored);
    }
    fs::path file(std::string_view name) const { return path_ / fs::path(std::u8string(name.begin(), name.end())); }

private:
    fs::path path_;
};

/// Appends integers in either byte order and raw text to a byte vector.
class ByteWriter {
public:
    ByteWriter& u8(std::uint32_t value) {
        bytes_.push_back(static_cast<std::uint8_t>(value));
        return *this;
    }
    ByteWriter& u16le(std::uint32_t value) { return little(value, 2); }
    ByteWriter& u32le(std::uint64_t value) { return little(value, 4); }
    ByteWriter& u64le(std::uint64_t value) { return little(value, 8); }
    ByteWriter& u16be(std::uint32_t value) { return big(value, 2); }
    ByteWriter& u24be(std::uint32_t value) { return big(value, 3); }
    ByteWriter& u32be(std::uint64_t value) { return big(value, 4); }
    ByteWriter& text(std::string_view value) {
        bytes_.insert(bytes_.end(), value.begin(), value.end());
        return *this;
    }
    ByteWriter& bytes(const std::vector<std::uint8_t>& value) {
        bytes_.insert(bytes_.end(), value.begin(), value.end());
        return *this;
    }
    ByteWriter& zeros(std::size_t count) {
        bytes_.insert(bytes_.end(), count, 0);
        return *this;
    }
    const std::vector<std::uint8_t>& result() const { return bytes_; }

private:
    ByteWriter& little(std::uint64_t value, int width) {
        for (int index = 0; index < width; ++index)
            bytes_.push_back(static_cast<std::uint8_t>(value >> (8 * index)));
        return *this;
    }
    ByteWriter& big(std::uint64_t value, int width) {
        for (int index = width - 1; index >= 0; --index)
            bytes_.push_back(static_cast<std::uint8_t>(value >> (8 * index)));
        return *this;
    }
    std::vector<std::uint8_t> bytes_;
};

void write_file(const fs::path& path, const std::vector<std::uint8_t>& bytes) {
    std::ofstream stream(path, std::ios::binary);
    stream.write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
}

/// One RIFF chunk; odd payloads are padded when serialized.
struct RiffChunk {
    std::string id;
    std::vector<std::uint8_t> payload;
    std::optional<std::uint32_t> declared_size;  // override for hostile or placeholder sizes
};

std::vector<std::uint8_t> riff_file(std::string_view form, const std::vector<RiffChunk>& chunks) {
    ByteWriter body;
    for (const RiffChunk& chunk : chunks) {
        body.text(chunk.id).u32le(chunk.declared_size.value_or(static_cast<std::uint32_t>(chunk.payload.size())));
        body.bytes(chunk.payload);
        if (chunk.payload.size() % 2) body.u8(0);
    }
    ByteWriter file;
    file.text(form).u32le(form == "RIFF" ? body.result().size() + 4 : 0xFFFFFFFF).text("WAVE").bytes(body.result());
    return file.result();
}

std::vector<std::uint8_t> format_chunk(std::uint16_t tag, std::uint16_t channels, std::uint32_t rate,
                                       std::uint16_t bits) {
    const std::uint16_t block = static_cast<std::uint16_t>(channels * bits / 8);
    ByteWriter writer;
    writer.u16le(tag).u16le(channels).u32le(rate).u32le(rate * block).u16le(block).u16le(bits);
    return writer.result();
}

std::vector<std::uint8_t> extensible_format_chunk(std::uint16_t sub_format, std::uint16_t channels, std::uint32_t rate,
                                                  std::uint16_t bits, std::uint16_t valid_bits) {
    ByteWriter writer;
    writer.bytes(format_chunk(0xFFFE, channels, rate, bits)).u16le(22).u16le(valid_bits).u32le(channels == 1 ? 4 : 3);
    writer.u16le(sub_format).u16le(0).u32le(0x00100000).u32le(0xAA000080).u32le(0x719B3800);  // KSDATAFORMAT GUID tail
    return writer.result();
}

/// Deterministic int16 test signal: distinct per frame and channel.
std::int16_t test_sample(std::size_t frame, std::size_t channel) {
    return static_cast<std::int16_t>(static_cast<int>((frame * 37 + channel * 1000) % 65536) - 32768);
}

std::vector<std::uint8_t> pcm16_payload(std::size_t frames, std::size_t channels, bool big_endian = false) {
    ByteWriter writer;
    for (std::size_t frame = 0; frame < frames; ++frame) {
        for (std::size_t channel = 0; channel < channels; ++channel) {
            const auto sample = static_cast<std::uint16_t>(test_sample(frame, channel));
            big_endian ? writer.u16be(sample) : writer.u16le(sample);
        }
    }
    return writer.result();
}

std::vector<std::uint8_t> info_list(const std::vector<std::pair<std::string, std::string>>& items) {
    ByteWriter writer;
    writer.text("INFO");
    for (const auto& [id, value] : items) {
        writer.text(id).u32le(value.size() + 1).text(value).u8(0);
        if ((value.size() + 1) % 2) writer.u8(0);
    }
    return writer.result();
}

std::vector<std::uint8_t> acid_chunk(float tempo, bool one_shot) {
    ByteWriter writer;
    writer.u32le(one_shot ? 0x01 : 0x00).u16le(60).u16le(0).u32le(0).u32le(8).u16le(4).u16le(4);
    writer.u32le(std::bit_cast<std::uint32_t>(tempo));
    return writer.result();
}

std::vector<std::uint8_t> sampler_chunk(std::uint32_t declared_loops,
                                        const std::vector<std::pair<std::uint32_t, std::uint32_t>>& loops) {
    ByteWriter writer;
    writer.zeros(28).u32le(declared_loops).u32le(0);
    for (const auto& [start, end_inclusive] : loops)
        writer.u32le(0).u32le(0).u32le(start).u32le(end_inclusive).u32le(0).u32le(0);
    return writer.result();
}

/// Reads every remaining frame from a decoder.
std::vector<float> read_all(Decoder& decoder) {
    std::vector<float> samples;
    std::vector<float> block(4096 * static_cast<std::size_t>(decoder.channels()));
    while (true) {
        auto count = decoder.read(block.data(), 4096);
        REQUIRE(count.has_value());
        if (*count == 0) break;
        samples.insert(samples.end(), block.begin(),
                       block.begin() + static_cast<std::ptrdiff_t>(*count * decoder.channels()));
    }
    return samples;
}

// ---------------------------------------------------------------- MP3 fixtures

constexpr std::uint32_t kMp3Header = 0xFFFB9000;  // MPEG-1 layer III, 128 kbit/s, 44.1 kHz, stereo
constexpr std::size_t kMp3FrameBytes = 417;       // 144 * 128000 / 44100

std::vector<std::uint8_t> mp3_frame() {
    ByteWriter writer;
    writer.u32be(kMp3Header).zeros(kMp3FrameBytes - 4);
    return writer.result();
}

std::vector<std::uint8_t> info_frame(std::uint32_t frame_count, std::uint32_t delay, std::uint32_t padding) {
    ByteWriter writer;
    writer.u32be(kMp3Header).zeros(32).text("Info").u32be(0x1).u32be(frame_count);
    writer.text("LAME3.100").u8(0).u8(0).zeros(8).u8(0).u8(0).u24be((delay << 12) | padding);
    std::vector<std::uint8_t> frame = writer.result();
    frame.resize(kMp3FrameBytes, 0);
    return frame;
}

std::vector<std::uint8_t> id3_text_frame(std::string_view id, std::uint8_t encoding,
                                         const std::vector<std::uint8_t>& text) {
    ByteWriter writer;
    writer.text(id).u32be(text.size() + 1).u16be(0).u8(encoding).bytes(text);
    return writer.result();
}

std::vector<std::uint8_t> latin1(std::string_view text) {
    return {text.begin(), text.end()};
}

std::vector<std::uint8_t> syncsafe(std::uint32_t value) {
    return {static_cast<std::uint8_t>((value >> 21) & 0x7F), static_cast<std::uint8_t>((value >> 14) & 0x7F),
            static_cast<std::uint8_t>((value >> 7) & 0x7F), static_cast<std::uint8_t>(value & 0x7F)};
}

std::vector<std::uint8_t> id3v2_tag(int major_version, std::uint8_t flags, const std::vector<std::uint8_t>& frames) {
    ByteWriter writer;
    writer.text("ID3")
        .u8(static_cast<std::uint32_t>(major_version))
        .u8(0)
        .u8(flags)
        .bytes(syncsafe(static_cast<std::uint32_t>(frames.size())));
    writer.bytes(frames);
    return writer.result();
}

/// ID3v2.3 tag with Latin-1, UTF-16 and genre-reference frames plus padding.
std::vector<std::uint8_t> sample_id3v23_tag() {
    ByteWriter frames;
    frames.bytes(id3_text_frame("TIT2", 0, latin1("Kick Loop")));
    frames.bytes(id3_text_frame("TPE1", 1, {0xFF, 0xFE, 'A', 0, 'n', 0, 'n', 0}));
    frames.bytes(id3_text_frame("TBPM", 0, latin1("126")));
    frames.bytes(id3_text_frame("TKEY", 0, latin1("F#m")));
    frames.bytes(id3_text_frame("TCON", 0, latin1("(52)")));
    frames.text("COMM").u32be(1 + 3 + 1 + 5).u16be(0).u8(0).text("eng").u8(0).text("hello");
    frames.zeros(32);  // padding
    return id3v2_tag(3, 0, frames.result());
}

// ---------------------------------------------------------------- FFmpeg fixtures

bool ffmpeg_available() {
#ifdef _WIN32
    static const bool available = std::system("ffmpeg -hide_banner -version >NUL 2>&1") == 0;
#else
    static const bool available = std::system("ffmpeg -hide_banner -version >/dev/null 2>&1") == 0;
#endif
    return available;
}

/// Renders one second of a 440 Hz sine with `encoder_arguments`; false if ffmpeg failed.
bool render_with_ffmpeg(const fs::path& output, std::string_view encoder_arguments, int sample_rate = 44100) {
    const std::string command = "ffmpeg -hide_banner -loglevel error -y -f lavfi -i \"sine=frequency=440:sample_rate=" +
                                std::to_string(sample_rate) + ":duration=1\" -ac 2 " + std::string(encoder_arguments) +
                                " \"" + output.string() + "\"";
    return std::system(command.c_str()) == 0 && fs::exists(output);
}

bool has_capability(std::string_view codec) {
    const auto codecs = decoder_capabilities();
    return std::ranges::find(codecs, codec) != codecs.end();
}

}  // namespace

TEST_SUITE("media") {
    // ---------------------------------------------------------------- WAV

    TEST_CASE("WAV int16 probe reads layout, INFO tags, acid tempo and smpl loops; decoder streams and seeks") {
        TempDirectory directory;
        const fs::path file = directory.file("loop.wav");
        constexpr std::size_t kFrames = 1000;
        write_file(
            file, riff_file("RIFF",
                            {
                                {"fmt ", format_chunk(1, 2, 44100, 16)},
                                {"LIST", info_list({{"INAM", "Drum Loop"},
                                                    {"IART", "Studio"},
                                                    {"IGNR", "House"},
                                                    {"ICMT", "dry"},
                                                    {"IKEY", "drums; loop, 909"}})},
                                {"acid", acid_chunk(126.5f, false)},
                                {"data", pcm16_payload(kFrames, 2)},
                                {"smpl", sampler_chunk(2, {{100, 899}, {50, 5000}})},  // second loop ends past the data
                            }));

        auto probe = probe_file(file);
        REQUIRE(probe.has_value());
        CHECK(probe->format == "wav");
        CHECK(probe->codec == "pcm_s16");
        CHECK(probe->sample_rate == 44100);
        CHECK(probe->channels == 2);
        CHECK(probe->bit_depth == 16);
        CHECK_FALSE(probe->float_samples);
        CHECK(probe->frames == static_cast<std::int64_t>(kFrames));
        CHECK(probe->duration_exact);
        CHECK(probe->duration_us == kFrames * 1'000'000 / 44100);
        CHECK_FALSE(probe->partial);
        CHECK(probe->tags.title == "Drum Loop");
        CHECK(probe->tags.artist == "Studio");
        CHECK(probe->tags.genre == "House");
        CHECK(probe->tags.comment == "dry");
        CHECK(probe->tags.keywords == std::vector<std::string>{"drums", "loop", "909"});
        CHECK(probe->tags.bpm_milli == 126'500);
        REQUIRE(probe->loops.size() == 1);
        CHECK(probe->loops[0].start_frame == 100);
        CHECK(probe->loops[0].end_frame == 900);
        CHECK(probe->bytes_read < 1024);

        auto decoder = open_decoder(file);
        REQUIRE(decoder.has_value());
        Decoder& stream = **decoder;
        CHECK(stream.channels() == 2);
        CHECK(stream.sample_rate() == 44100);
        CHECK(stream.total_frames() == static_cast<std::int64_t>(kFrames));
        CHECK(stream.frames_exact());
        const auto samples = read_all(stream);
        REQUIRE(samples.size() == kFrames * 2);
        for (std::size_t frame : {std::size_t{0}, std::size_t{1}, std::size_t{500}, kFrames - 1}) {
            CHECK(samples[frame * 2] == doctest::Approx(test_sample(frame, 0) / 32768.0f));
            CHECK(samples[frame * 2 + 1] == doctest::Approx(test_sample(frame, 1) / 32768.0f));
        }
        CHECK(stream.position() == static_cast<std::int64_t>(kFrames));

        REQUIRE(stream.seek(750).has_value());
        CHECK(stream.position() == 750);
        float frame[2] = {};
        REQUIRE(stream.read(frame, 1) == std::size_t{1});
        CHECK(frame[0] == doctest::Approx(test_sample(750, 0) / 32768.0f));
        CHECK(stream.seek(-1).error().code == ErrorCode::InvalidArgument);
    }

    TEST_CASE("WAV 24-bit and extensible float32 report codec, bit depth and decode exactly") {
        TempDirectory directory;
        const fs::path pcm24 = directory.file("mono24.wav");
        ByteWriter samples24;
        for (int value : {0, 1, -1, 8388607, -8388608, 4194304})
            samples24.u8(value & 0xFF).u8((value >> 8) & 0xFF).u8((value >> 16) & 0xFF);
        write_file(pcm24, riff_file("RIFF", {{"fmt ", format_chunk(1, 1, 96000, 24)}, {"data", samples24.result()}}));

        auto probe24 = probe_file(pcm24);
        REQUIRE(probe24.has_value());
        CHECK(probe24->codec == "pcm_s24");
        CHECK(probe24->bit_depth == 24);
        CHECK(probe24->frames == 6);
        auto decoder24 = open_decoder(pcm24);
        REQUIRE(decoder24.has_value());
        const auto decoded24 = read_all(**decoder24);
        REQUIRE(decoded24.size() == 6);
        CHECK(decoded24[1] == doctest::Approx(1.0 / 8388608.0));
        CHECK(decoded24[4] == doctest::Approx(-1.0));
        CHECK(decoded24[5] == doctest::Approx(0.5));

        const fs::path float32 = directory.file("float.wav");
        ByteWriter floats;
        for (float value : {0.25f, -0.5f, 0.75f, -1.0f})
            floats.u32le(std::bit_cast<std::uint32_t>(value));
        write_file(float32, riff_file("RIFF", {{"fmt ", extensible_format_chunk(3, 2, 48000, 32, 32)},
                                               {"data", floats.result()}}));
        auto probe_float = probe_file(float32);
        REQUIRE(probe_float.has_value());
        CHECK(probe_float->codec == "pcm_f32");
        CHECK(probe_float->float_samples);
        CHECK(probe_float->bit_depth == 32);
        CHECK(probe_float->frames == 2);
        auto decoder_float = open_decoder(float32);
        REQUIRE(decoder_float.has_value());
        CHECK(read_all(**decoder_float) == std::vector<float>{0.25f, -0.5f, 0.75f, -1.0f});

        const fs::path packed20 = directory.file("valid_bits.wav");
        write_file(packed20, riff_file("RIFF", {{"fmt ", extensible_format_chunk(1, 2, 44100, 24, 20)},
                                                {"data", std::vector<std::uint8_t>(60)}}));
        auto probe20 = probe_file(packed20);
        REQUIRE(probe20.has_value());
        CHECK(probe20->codec == "pcm_s24");
        CHECK(probe20->bit_depth == 20);
        CHECK(probe20->frames == 10);
    }

    TEST_CASE("RF64 takes its data size from the ds64 chunk") {
        TempDirectory directory;
        const fs::path file = directory.file("long.rf64");
        const auto data = pcm16_payload(300, 2);
        ByteWriter ds64;
        ds64.u64le(0).u64le(data.size()).u64le(300).u32le(0);
        write_file(file, riff_file("RF64", {{"ds64", ds64.result()},
                                            {"fmt ", format_chunk(1, 2, 48000, 16)},
                                            {"data", data, 0xFFFFFFFF}}));

        auto probe = probe_file(file);
        REQUIRE(probe.has_value());
        CHECK(probe->format == "rf64");
        CHECK(probe->frames == 300);
        CHECK(probe->sample_rate == 48000);
        auto decoder = open_decoder(file);
        REQUIRE(decoder.has_value());
        CHECK(read_all(**decoder).size() == 600);
    }

    TEST_CASE("Malformed WAV headers fail with CORRUPT_AUDIO and hostile sizes stay bounded") {
        TempDirectory directory;
        const auto probe_bytes = [&](std::string_view name, const std::vector<std::uint8_t>& bytes) {
            const fs::path file = directory.file(name);
            write_file(file, bytes);
            return probe_file(file);
        };

        auto truncated = probe_bytes(
            "truncated.wav", {'R', 'I', 'F', 'F', 0x24, 0, 0, 0, 'W', 'A', 'V', 'E', 'f', 'm', 't', ' ', 16, 0});
        REQUIRE_FALSE(truncated.has_value());
        CHECK(truncated.error().code == ErrorCode::CorruptAudio);

        auto no_channels = probe_bytes("zero_channels.wav",
                                       riff_file("RIFF", {{"fmt ", format_chunk(1, 0, 44100, 16)}, {"data", {}}}));
        REQUIRE_FALSE(no_channels.has_value());
        CHECK(no_channels.error().code == ErrorCode::CorruptAudio);

        auto no_data = probe_bytes("no_data.wav", riff_file("RIFF", {{"fmt ", format_chunk(1, 2, 44100, 16)}}));
        REQUIRE_FALSE(no_data.has_value());
        CHECK(no_data.error().code == ErrorCode::CorruptAudio);

        auto empty = probe_bytes("empty.wav", {});
        REQUIRE_FALSE(empty.has_value());
        CHECK(empty.error().code == ErrorCode::CorruptAudio);

        // A metadata chunk claiming ~4 GiB, a sampler chunk claiming 4 billion loops, and a data
        // chunk running past the end of the file: parsed without over-reads or huge allocations.
        auto hostile =
            probe_bytes("hostile.wav", riff_file("RIFF", {
                                                             {"fmt ", format_chunk(1, 1, 44100, 16)},
                                                             {"smpl", sampler_chunk(0xFFFFFFFF, {{10, 5}, {0, 3}})},
                                                             {"data", pcm16_payload(100, 1), 1'000'000},
                                                         }));
        REQUIRE(hostile.has_value());
        CHECK(hostile->frames == 100);  // only the bytes actually present
        REQUIRE(hostile->loops.size() == 1);
        CHECK(hostile->loops[0].end_frame == 4);

        auto huge_list =
            probe_bytes("huge_list.wav", riff_file("RIFF", {
                                                               {"fmt ", format_chunk(1, 1, 44100, 16)},
                                                               {"data", pcm16_payload(10, 1)},
                                                               {"LIST", info_list({{"INAM", "x"}}), 0xFFFFFFF0},
                                                           }));
        REQUIRE(huge_list.has_value());
        CHECK(huge_list->tags.title == "x");
        CHECK(huge_list->bytes_read < 64 * 1024);
    }

    TEST_CASE(
        "Probe budget: metadata after a large data chunk costs only header reads; tiny budgets yield partial records") {
        TempDirectory directory;
        const fs::path file = directory.file("big.wav");
        constexpr std::size_t kFrames = 3 * 1024 * 1024 / 2;  // 6 MiB of stereo int16: larger than the 4 MiB budget
        write_file(file, riff_file("RIFF", {
                                               {"fmt ", format_chunk(1, 2, 44100, 16)},
                                               {"data", std::vector<std::uint8_t>(kFrames * 4)},
                                               {"LIST", info_list({{"INAM", "After Data"}})},
                                           }));

        auto probe = probe_file(file);
        REQUIRE(probe.has_value());
        CHECK_FALSE(probe->partial);
        CHECK(probe->frames == static_cast<std::int64_t>(kFrames));
        CHECK(probe->tags.title == "After Data");
        CHECK(probe->bytes_read < 4096);

        auto starved = probe_file(file, ProbeLimits{40, std::chrono::milliseconds(2000)});
        REQUIRE(starved.has_value());
        CHECK(starved->partial);
        CHECK(starved->format == "wav");
        CHECK(starved->bytes_read <= 40);
        CHECK_FALSE(starved->tags.title.has_value());

        auto out_of_time = probe_file(file, ProbeLimits{4 * 1024 * 1024, std::chrono::milliseconds(0)});
        REQUIRE(out_of_time.has_value());
        CHECK(out_of_time->partial);
        CHECK(out_of_time->bytes_read == 0);
    }

    // ---------------------------------------------------------------- AIFF

    TEST_CASE(
        "AIFF probe decodes the 80-bit rate, NAME/AUTH and MARK+INST loops; miniaudio decodes AIFF and AIFC sowt") {
        TempDirectory directory;
        constexpr std::uint32_t kFrames = 1000;
        const auto build_aiff = [&](bool compressed_form) {
            ByteWriter common;
            common.u16be(2).u32be(kFrames).u16be(16).u16be(0x400E).u32be(0xAC440000).u32be(0);  // 44100 Hz
            if (compressed_form) common.text("sowt").u8(0).u8(0);
            ByteWriter markers;
            markers.u16be(2).u16be(1).u32be(100).u8(1).text("a").u16be(2).u32be(900).u8(0).u8(0);
            ByteWriter instrument;
            instrument.u8(60)
                .u8(0)
                .u8(0)
                .u8(127)
                .u8(1)
                .u8(127)
                .u16be(0)
                .u16be(1)
                .u16be(1)
                .u16be(2)
                .u16be(0)
                .u16be(0)
                .u16be(0);
            ByteWriter body;
            const auto chunk = [&body](std::string_view id, const std::vector<std::uint8_t>& payload) {
                body.text(id).u32be(payload.size()).bytes(payload);
                if (payload.size() % 2) body.u8(0);
            };
            chunk("COMM", common.result());
            chunk("NAME", latin1("Pad"));
            chunk("AUTH", latin1("Bob"));
            ByteWriter sound;
            sound.u32be(0).u32be(0).bytes(pcm16_payload(kFrames, 2, !compressed_form));
            chunk("SSND", sound.result());
            chunk("MARK", markers.result());
            chunk("INST", instrument.result());
            ByteWriter file;
            file.text("FORM")
                .u32be(body.result().size() + 4)
                .text(compressed_form ? "AIFC" : "AIFF")
                .bytes(body.result());
            return file.result();
        };

        for (const bool compressed_form : {false, true}) {
            CAPTURE(compressed_form);
            const fs::path file = directory.file(compressed_form ? "pad.aifc" : "pad.aiff");
            write_file(file, build_aiff(compressed_form));
            auto probe = probe_file(file);
            REQUIRE(probe.has_value());
            CHECK(probe->format == (compressed_form ? "aifc" : "aiff"));
            CHECK(probe->codec == "pcm_s16");
            CHECK(probe->sample_rate == 44100);
            CHECK(probe->channels == 2);
            CHECK(probe->bit_depth == 16);
            CHECK(probe->frames == kFrames);
            CHECK(probe->tags.title == "Pad");
            CHECK(probe->tags.artist == "Bob");
            REQUIRE(probe->loops.size() == 1);
            CHECK(probe->loops[0].start_frame == 100);
            CHECK(probe->loops[0].end_frame == 900);

            auto decoder = open_decoder(file);
            REQUIRE(decoder.has_value());
            const auto samples = read_all(**decoder);
            REQUIRE(samples.size() == kFrames * 2);
            CHECK(samples[2 * 321 + 1] == doctest::Approx(test_sample(321, 1) / 32768.0f));
        }
    }

    // ---------------------------------------------------------------- MP3 and ID3

    TEST_CASE("MP3 with ID3v2.3 tags and a LAME Info frame has exact gapless frames") {
        TempDirectory directory;
        const fs::path file = directory.file("tagged.mp3");
        ByteWriter bytes;
        bytes.bytes(sample_id3v23_tag()).bytes(info_frame(10, 576, 1000));
        for (int index = 0; index < 10; ++index)
            bytes.bytes(mp3_frame());
        write_file(file, bytes.result());

        auto probe = probe_file(file);
        REQUIRE(probe.has_value());
        CHECK(probe->format == "mp3");
        CHECK(probe->codec == "mp3");
        CHECK(probe->sample_rate == 44100);
        CHECK(probe->channels == 2);
        CHECK_FALSE(probe->bit_depth.has_value());
        CHECK(probe->frames == 10 * 1152 - 576 - 1000);
        CHECK(probe->duration_exact);
        CHECK(probe->tags.title == "Kick Loop");
        CHECK(probe->tags.artist == "Ann");
        CHECK(probe->tags.bpm_milli == 126'000);
        CHECK(probe->tags.key == MusicalKey{6, KeyMode::Minor});
        CHECK(probe->tags.genre == "Electronic");
        CHECK(probe->tags.comment == "hello");
    }

    TEST_CASE("MP3 without a VBR header gets an estimated, inexact duration; ID3v2.4 unsynchronisation is undone") {
        TempDirectory directory;
        const fs::path file = directory.file("cbr.mp3");
        // v2.4 tag, unsynchronised: the title byte 0xFF is stored as 0xFF 0x00.
        ByteWriter frame;
        frame.text("TIT2").bytes(syncsafe(4)).u16be(0).u8(0).u8(0xFF).u8(0x00).text("a");
        ByteWriter bytes;
        bytes.bytes(id3v2_tag(4, 0x80, frame.result()));
        for (int index = 0; index < 20; ++index)
            bytes.bytes(mp3_frame());
        write_file(file, bytes.result());

        auto probe = probe_file(file);
        REQUIRE(probe.has_value());
        CHECK(probe->tags.title ==
              "\xC3\xBF"
              "a");
        CHECK_FALSE(probe->frames.has_value());
        CHECK_FALSE(probe->duration_exact);
        CHECK(probe->duration_us == 20 * kMp3FrameBytes * 8000 / 128);
    }

    TEST_CASE("An ID3 tag larger than the probe budget yields a partial record with the tags read so far") {
        TempDirectory directory;
        const fs::path file = directory.file("art.mp3");
        ByteWriter frames;
        frames.bytes(id3_text_frame("TIT2", 0, latin1("Big Art")));
        frames.text("APIC").u32be(200'000).u16be(0).zeros(200'000);
        ByteWriter bytes;
        bytes.bytes(id3v2_tag(3, 0, frames.result()));
        for (int index = 0; index < 5; ++index)
            bytes.bytes(mp3_frame());
        write_file(file, bytes.result());

        auto probe = probe_file(file, ProbeLimits{64 * 1024, std::chrono::milliseconds(2000)});
        REQUIRE(probe.has_value());
        CHECK(probe->partial);
        CHECK(probe->tags.title == "Big Art");
        CHECK(probe->bytes_read <= 64 * 1024);
    }

    // ---------------------------------------------------------------- identification and errors

    TEST_CASE("The container is identified from its bytes, not its extension") {
        TempDirectory directory;
        const fs::path disguised = directory.file("actually_wav.mp3");
        write_file(disguised,
                   riff_file("RIFF", {{"fmt ", format_chunk(1, 1, 22050, 16)}, {"data", pcm16_payload(10, 1)}}));
        auto probe = probe_file(disguised);
        REQUIRE(probe.has_value());
        CHECK(probe->format == "wav");

        const fs::path text = directory.file("notes.wav");
        write_file(text, latin1("this is not audio at all, just some text in a file"));
        auto not_audio = probe_file(text);
        REQUIRE_FALSE(not_audio.has_value());
        CHECK(not_audio.error().code == ErrorCode::UnsupportedCodec);

        const fs::path midi = directory.file("song.mid");
        ByteWriter midi_bytes;
        midi_bytes.text("MThd").u32be(6).u16be(0).u16be(1).u16be(96);
        write_file(midi, midi_bytes.result());
        auto midi_probe = probe_file(midi);
        REQUIRE_FALSE(midi_probe.has_value());
        CHECK(midi_probe.error().code == ErrorCode::UnsupportedCodec);

        auto missing = probe_file(directory.file("missing.wav"));
        REQUIRE_FALSE(missing.has_value());
        CHECK(missing.error().code == ErrorCode::FileMissing);
        auto missing_decoder = open_decoder(directory.file("missing.flac"));
        REQUIRE_FALSE(missing_decoder.has_value());
        CHECK(missing_decoder.error().code == ErrorCode::FileMissing);
    }

    TEST_CASE("Unicode file names are probed and decoded through wide-character paths") {
        TempDirectory directory;
        const fs::path file =
            directory.file("\xD7\x93\xD7\x92\xD7\x99\xD7\x9E\xD7\x94 \xE2\x99\xAF.wav");  // Hebrew + sharp sign
        write_file(file, riff_file("RIFF", {{"fmt ", format_chunk(1, 1, 44100, 16)}, {"data", pcm16_payload(64, 1)}}));
        auto probe = probe_file(file);
        REQUIRE(probe.has_value());
        CHECK(probe->frames == 64);
        auto decoder = open_decoder(file);
        REQUIRE(decoder.has_value());
        CHECK(read_all(**decoder).size() == 64);
    }

    TEST_CASE("is_candidate_extension accepts audio suffixes and rejects MIDI") {
        for (std::string_view extension : {"wav", "wave", "rf64", "bwf", "aif", "aiff", "aifc", "flac", "mp3", "ogg",
                                           "oga", "opus", "m4a", "mp4", "aac", "caf", "wv", "wma", "alac"}) {
            CAPTURE(extension);
            CHECK(is_candidate_extension(extension));
        }
        for (std::string_view extension : {"mid", "midi", "txt", "png", "", "WAV"}) {
            CAPTURE(extension);
            CHECK_FALSE(is_candidate_extension(extension));
        }
    }

    TEST_CASE("decoder_capabilities lists the built-in codecs") {
        for (std::string_view codec : {"pcm_s16", "pcm_s24", "pcm_f32", "flac", "mp3", "vorbis"}) {
            CAPTURE(codec);
            CHECK(has_capability(codec));
        }
    }

    // ---------------------------------------------------------------- filename hints

    TEST_CASE("filename_hints finds marked tempos and keys") {
        struct Expectation {
            std::string_view stem;
            std::optional<std::int64_t> bpm_milli;
            std::optional<MusicalKey> key;
        };
        const Expectation cases[] = {
            {"Bass_128bpm_F#m", 128'000, MusicalKey{6, KeyMode::Minor}},
            {"Lead 128 BPM Amin", 128'000, MusicalKey{9, KeyMode::Minor}},
            {"pad-BPM_90-C major", 90'000, MusicalKey{0, KeyMode::Major}},
            {"Keys_127.5bpm_Bb", 127'500, MusicalKey{10, KeyMode::Unknown}},
            {"Drum_Loop_120_Ebm", 120'000, MusicalKey{3, KeyMode::Minor}},
            {"Synth \xE2\x99\xAF stab 140 G\xE2\x99\xAFm", 140'000, MusicalKey{8, KeyMode::Minor}},
            {"Vox Key C 100bpm", 100'000, MusicalKey{0, KeyMode::Unknown}},
        };
        for (const auto& expected : cases) {
            CAPTURE(expected.stem);
            const FilenameHints hints = filename_hints(expected.stem);
            CHECK(hints.bpm_milli == expected.bpm_milli);
            CHECK(hints.key == expected.key);
        }
    }

    TEST_CASE("filename_hints stays silent on ambiguous or misleading tokens") {
        for (std::string_view stem :
             {"TR-808 Kick", "909 Hat", "01 Intro", "Kick 07", "Snare A", "A Day In The Life", "I Am Here",
              "Sine 100 Hz", "Take 120", "Juno 106 Pad", "Admin Sounds", "Dmitri FX", "Loop 120 140", "Bass Am Cm",
              "Chord Cmaj7", "Note C3", "Kick 060"}) {
            CAPTURE(stem);
            const FilenameHints hints = filename_hints(stem);
            CHECK_FALSE(hints.bpm_milli.has_value());
            CHECK_FALSE(hints.key.has_value());
        }
    }

    // ---------------------------------------------------------------- FFmpeg-generated fixtures

    TEST_CASE("Encoded fixtures: FLAC, MP3, Ogg Vorbis probe exactly and decode the same frame count") {
        if (!ffmpeg_available()) {
            MESSAGE("ffmpeg not on PATH: skipping encoded fixtures");
            return;
        }
        TempDirectory directory;
        struct Fixture {
            std::string_view name;
            std::string_view arguments;
            std::string_view format;
            std::string_view codec;
            std::optional<std::int64_t> bit_depth;
        };
        const Fixture fixtures[] = {
            {"sine.flac", "-c:a flac -sample_fmt s16 -metadata TITLE=Sine -metadata BPM=128 -metadata INITIALKEY=8A",
             "flac", "flac", 16},
            {"sine.mp3",
             "-c:a libmp3lame -b:a 128k -id3v2_version 3 -metadata title=Sine -metadata TBPM=128 -metadata TKEY=Am",
             "mp3", "mp3", std::nullopt},
            {"sine.ogg", "-c:a libvorbis -metadata TITLE=Sine -metadata BPM=128 -metadata KEY=Am", "ogg", "vorbis",
             std::nullopt},
        };
        for (const Fixture& fixture : fixtures) {
            CAPTURE(fixture.name);
            const fs::path file = directory.file(fixture.name);
            REQUIRE(render_with_ffmpeg(file, fixture.arguments));
            auto probe = probe_file(file);
            REQUIRE(probe.has_value());
            CHECK(probe->format == fixture.format);
            CHECK(probe->codec == fixture.codec);
            CHECK(probe->sample_rate == 44100);
            CHECK(probe->channels == 2);
            CHECK(probe->bit_depth == fixture.bit_depth);
            CHECK(probe->frames == 44100);
            CHECK(probe->duration_exact);
            CHECK(probe->tags.title == "Sine");
            CHECK(probe->tags.bpm_milli == 128'000);
            CHECK(probe->tags.key == MusicalKey{9, KeyMode::Minor});

            auto decoder = open_decoder(file);
            REQUIRE(decoder.has_value());
            CHECK((*decoder)->total_frames() == 44100);
            CHECK((*decoder)->frames_exact());
            const auto samples = read_all(**decoder);
            CHECK(samples.size() == 44100 * 2);
            const float peak = std::ranges::max(samples, {}, [](float sample) { return std::fabs(sample); });
            CHECK(peak > 0.05f);
            REQUIRE((*decoder)->seek(22050).has_value());
            CHECK((*decoder)->position() == 22050);
            CHECK(read_all(**decoder).size() == 22050 * 2);
        }
    }

    TEST_CASE(
        "Encoded fixtures: Opus, AAC, ALAC, WMA, WavPack and CAF probe natively; FFmpeg decodes them when built in") {
        if (!ffmpeg_available()) {
            MESSAGE("ffmpeg not on PATH: skipping encoded fixtures");
            return;
        }
        TempDirectory directory;
        struct Fixture {
            std::string_view name;
            std::string_view arguments;
            int sample_rate;
            std::string_view format;
            std::string_view codec;
            std::optional<std::int64_t> exact_frames;
            std::optional<std::int64_t> bit_depth;
        };
        const Fixture fixtures[] = {
            {"sine.opus", "-c:a libopus -metadata TITLE=Sine", 48000, "opus", "opus", 48000, std::nullopt},
            {"sine.m4a", "-c:a aac -b:a 128k -metadata title=Sine", 44100, "m4a", "aac", std::nullopt, std::nullopt},
            {"alac.m4a", "-c:a alac -sample_fmt s16p -metadata title=Sine", 44100, "m4a", "alac", 44100, 16},
            {"sine.wma", "-c:a wmav2 -b:a 128k -metadata title=Sine", 44100, "wma", "wma", std::nullopt, std::nullopt},
            {"sine.wv", "-c:a wavpack -sample_fmt s16p", 44100, "wv", "wavpack", 44100, 16},
            {"sine.caf", "-c:a pcm_s16be", 44100, "caf", "pcm_s16", 44100, 16},
        };
        for (const Fixture& fixture : fixtures) {
            CAPTURE(fixture.name);
            const fs::path file = directory.file(fixture.name);
            REQUIRE(render_with_ffmpeg(file, fixture.arguments, fixture.sample_rate));
            auto probe = probe_file(file);
            REQUIRE(probe.has_value());
            CHECK(probe->format == fixture.format);
            CHECK(probe->codec == fixture.codec);
            CHECK(probe->sample_rate == fixture.sample_rate);
            CHECK(probe->channels == 2);
            CHECK(probe->bit_depth == fixture.bit_depth);
            CHECK(probe->frames == fixture.exact_frames);
            CHECK(probe->duration_exact == fixture.exact_frames.has_value());
            REQUIRE(probe->duration_us.has_value());
            CHECK(std::abs(*probe->duration_us - 1'000'000) < 100'000);
            if (fixture.format != "wv" && fixture.format != "caf") CHECK(probe->tags.title == "Sine");

            auto decoder = open_decoder(file);
            const bool decodable = has_capability(fixture.codec) || fixture.codec == "pcm_s16";
            if (!has_capability("aac")) {
                // Built without FFmpeg: compressed formats outside miniaudio are refused clearly.
                if (fixture.codec != "pcm_s16") {
                    REQUIRE_FALSE(decoder.has_value());
                    CHECK(decoder.error().code == ErrorCode::UnsupportedCodec);
                    CHECK(decoder.error().safe_detail.find("not built into this version") != std::string::npos);
                }
                continue;
            }
            REQUIRE(decodable);
            REQUIRE(decoder.has_value());
            CHECK((*decoder)->sample_rate() == fixture.sample_rate);
            CHECK((*decoder)->channels() == 2);
            const auto frames = static_cast<std::int64_t>(read_all(**decoder).size() / 2);
            if (fixture.exact_frames) {
                CHECK(frames == *fixture.exact_frames);
            } else {
                CHECK(std::abs(frames - fixture.sample_rate) <= 2048);
            }
            // Lossy codecs may decode a little more than one second (priming, final-frame padding),
            // so compare against what this decoder produced in full.
            REQUIRE((*decoder)->seek(fixture.sample_rate / 2).has_value());
            CHECK((*decoder)->position() == fixture.sample_rate / 2);
            const auto remaining = static_cast<std::int64_t>(read_all(**decoder).size() / 2);
            CHECK(std::abs(remaining - (frames - fixture.sample_rate / 2)) <= (fixture.exact_frames ? 0 : 64));
        }
    }

    TEST_CASE("Containers without a native parser fall back to FFmpeg probing when it is built in") {
        if (!ffmpeg_available() || !has_capability("aac")) {
            MESSAGE("ffmpeg executable or FFmpeg backend unavailable: skipping fallback probe");
            return;
        }
        TempDirectory directory;
        const fs::path file = directory.file("sine.mka");
        REQUIRE(render_with_ffmpeg(file, "-c:a flac"));
        auto probe = probe_file(file);
        REQUIRE(probe.has_value());
        CHECK(probe->format == "matroska");
        CHECK(probe->codec == "flac");
        CHECK(probe->sample_rate == 44100);
        CHECK(probe->channels == 2);
        CHECK_FALSE(probe->duration_exact);
        auto decoder = open_decoder(file);
        REQUIRE(decoder.has_value());
        CHECK(read_all(**decoder).size() == 44100 * 2);
    }

}  // TEST_SUITE("media")
