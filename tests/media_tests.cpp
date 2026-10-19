// Media module: bounded probes, streaming decoders, the parsing helpers, the extension filter and
// filename hints. Fixtures are generated at run time (byte-exact builders here, or the ffmpeg
// executable for encoded formats); no audio file is ever committed. FFmpeg-dependent cases are
// skipped through doctest when ffmpeg is not on PATH, and fail instead under MSO_REQUIRE_FFMPEG=1.
#include <doctest.h>

#include <algorithm>
#include <bit>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <optional>
#include <random>
#include <string>
#include <string_view>
#include <vector>

#include "mso/media/detail/bounded_reader.hpp"
#include "mso/media/detail/byte_cursor.hpp"
#include "mso/media/detail/decoder_backends.hpp"
#include "mso/media/detail/file_handle.hpp"
#include "mso/media/detail/probe_formats.hpp"
#include "mso/media/detail/tag_mapping.hpp"
#include "mso/media/detail/text_decoding.hpp"
#include "mso/media/media.hpp"
#include "mso/media/native_file.hpp"
#include "test_support.hpp"

#ifdef _WIN32
#include <windows.h>
// psapi.h needs windows.h first.
#include <psapi.h>
#else
#include <unistd.h>
#include <fstream>
#endif

namespace {

namespace fs = std::filesystem;
using mso::ErrorCode;
using mso::KeyMode;
using mso::MusicalKey;
using mso::test::ScratchDirectory;
using namespace mso::media;
using Bytes = std::vector<std::uint8_t>;
using namespace std::string_view_literals;

// ---------------------------------------------------------------- byte builders

/// Appends integers in either byte order and raw text to a byte vector.
class ByteWriter {
public:
    ByteWriter& u8(std::uint64_t value) { return little(value, 1); }
    ByteWriter& u16le(std::uint64_t value) { return little(value, 2); }
    ByteWriter& u32le(std::uint64_t value) { return little(value, 4); }
    ByteWriter& u64le(std::uint64_t value) { return little(value, 8); }
    ByteWriter& u16be(std::uint64_t value) { return big(value, 2); }
    ByteWriter& u24be(std::uint64_t value) { return big(value, 3); }
    ByteWriter& u32be(std::uint64_t value) { return big(value, 4); }
    ByteWriter& u64be(std::uint64_t value) { return big(value, 8); }
    ByteWriter& text(std::string_view value) {
        bytes_.insert(bytes_.end(), value.begin(), value.end());
        return *this;
    }
    ByteWriter& bytes(const Bytes& value) {
        bytes_.insert(bytes_.end(), value.begin(), value.end());
        return *this;
    }
    ByteWriter& zeros(std::size_t count) {
        bytes_.insert(bytes_.end(), count, 0);
        return *this;
    }
    ByteWriter& fill(std::size_t count, std::uint8_t value) {
        bytes_.insert(bytes_.end(), count, value);
        return *this;
    }
    const Bytes& result() const { return bytes_; }
    std::size_t size() const { return bytes_.size(); }

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
    Bytes bytes_;
};

/// Writes bytes to a file (creating parent directories).
void write_bytes(const fs::path& file, const Bytes& bytes) {
    mso::test::write_file(file, std::span<const std::uint8_t>(bytes));
}

/// Concatenates byte vectors.
Bytes concat(std::initializer_list<Bytes> parts) {
    Bytes joined;
    for (const Bytes& part : parts)
        joined.insert(joined.end(), part.begin(), part.end());
    return joined;
}

/// Latin-1/ASCII text as bytes.
Bytes latin1(std::string_view text) {
    return {text.begin(), text.end()};
}

// ---------------------------------------------------------------- RIFF / WAV fixtures

/// One RIFF chunk; odd payloads are padded when serialized.
struct RiffChunk {
    std::string id;
    Bytes payload;
    std::optional<std::uint32_t> declared_size;  // override for hostile or placeholder sizes
};

Bytes riff_file(std::string_view form, const std::vector<RiffChunk>& chunks) {
    ByteWriter body;
    for (const RiffChunk& chunk : chunks) {
        body.text(chunk.id).u32le(chunk.declared_size.value_or(static_cast<std::uint32_t>(chunk.payload.size())));
        body.bytes(chunk.payload);
        if (chunk.payload.size() % 2) body.u8(0);
    }
    ByteWriter file;
    file.text(form).u32le(form == "RIFF" ? body.size() + 4 : 0xFFFFFFFF).text("WAVE").bytes(body.result());
    return file.result();
}

Bytes format_chunk(std::uint16_t tag, std::uint16_t channels, std::uint32_t rate, std::uint16_t bits) {
    const auto block = static_cast<std::uint16_t>(channels * bits / 8);
    ByteWriter writer;
    writer.u16le(tag).u16le(channels).u32le(rate).u32le(std::uint64_t{rate} * block).u16le(block).u16le(bits);
    return writer.result();
}

Bytes extensible_format_chunk(std::uint16_t sub_format, std::uint16_t channels, std::uint32_t rate, std::uint16_t bits,
                              std::uint16_t valid_bits) {
    ByteWriter writer;
    writer.bytes(format_chunk(0xFFFE, channels, rate, bits)).u16le(22).u16le(valid_bits).u32le(channels == 1 ? 4 : 3);
    writer.u16le(sub_format).u16le(0).u32le(0x00100000).u32le(0xAA000080).u32le(0x719B3800);  // KSDATAFORMAT GUID tail
    return writer.result();
}

/// Deterministic int16 test signal: distinct per frame and channel.
std::int16_t test_sample(std::size_t frame, std::size_t channel) {
    return static_cast<std::int16_t>(static_cast<int>((frame * 37 + channel * 1000) % 65536) - 32768);
}

Bytes pcm16_payload(std::size_t frames, std::size_t channels, bool big_endian = false) {
    ByteWriter writer;
    for (std::size_t frame = 0; frame < frames; ++frame) {
        for (std::size_t channel = 0; channel < channels; ++channel) {
            const auto sample = static_cast<std::uint16_t>(test_sample(frame, channel));
            big_endian ? writer.u16be(sample) : writer.u16le(sample);
        }
    }
    return writer.result();
}

Bytes info_list(const std::vector<std::pair<std::string, std::string>>& items) {
    ByteWriter writer;
    writer.text("INFO");
    for (const auto& [id, value] : items) {
        writer.text(id).u32le(value.size() + 1).text(value).u8(0);
        if ((value.size() + 1) % 2) writer.u8(0);
    }
    return writer.result();
}

Bytes acid_chunk(float tempo, bool one_shot) {
    ByteWriter writer;
    writer.u32le(one_shot ? 0x01 : 0x00).u16le(60).u16le(0).u32le(0).u32le(8).u16le(4).u16le(4);
    writer.u32le(std::bit_cast<std::uint32_t>(tempo));
    return writer.result();
}

Bytes sampler_chunk(std::uint32_t declared_loops, const std::vector<std::pair<std::uint32_t, std::uint32_t>>& loops) {
    ByteWriter writer;
    writer.zeros(28).u32le(declared_loops).u32le(0);
    for (const auto& [start, end_inclusive] : loops)
        writer.u32le(0).u32le(0).u32le(start).u32le(end_inclusive).u32le(0).u32le(0);
    return writer.result();
}

/// A small valid 16-bit stereo WAV.
Bytes small_wav(std::size_t frames = 64) {
    return riff_file("RIFF", {{"fmt ", format_chunk(1, 2, 44100, 16)}, {"data", pcm16_payload(frames, 2)}});
}

// ---------------------------------------------------------------- MP3 and ID3 fixtures

constexpr std::uint32_t kMp3Header = 0xFFFB9000;  // MPEG-1 layer III, 128 kbit/s, 44.1 kHz, stereo
constexpr std::size_t kMp3FrameBytes = 417;       // 144 * 128000 / 44100

Bytes mp3_frame() {
    ByteWriter writer;
    writer.u32be(kMp3Header).zeros(kMp3FrameBytes - 4);
    return writer.result();
}

Bytes mp3_frames(int count) {
    ByteWriter writer;
    for (int index = 0; index < count; ++index)
        writer.bytes(mp3_frame());
    return writer.result();
}

Bytes info_frame(std::uint32_t frame_count, std::uint32_t delay, std::uint32_t padding) {
    ByteWriter writer;
    writer.u32be(kMp3Header).zeros(32).text("Info").u32be(0x1).u32be(frame_count);
    writer.text("LAME3.100").u8(0).u8(0).zeros(8).u8(0).u8(0).u24be((delay << 12) | padding);
    Bytes frame = writer.result();
    frame.resize(kMp3FrameBytes, 0);
    return frame;
}

Bytes vbri_frame(std::uint32_t frame_count) {
    ByteWriter writer;
    writer.u32be(kMp3Header).zeros(32).text("VBRI").u16be(1).u16be(0).u16be(75).u32be(100'000).u32be(frame_count);
    Bytes frame = writer.result();
    frame.resize(kMp3FrameBytes, 0);
    return frame;
}

Bytes syncsafe(std::uint32_t value) {
    return {static_cast<std::uint8_t>((value >> 21) & 0x7F), static_cast<std::uint8_t>((value >> 14) & 0x7F),
            static_cast<std::uint8_t>((value >> 7) & 0x7F), static_cast<std::uint8_t>(value & 0x7F)};
}

/// An ID3v2.3 frame (plain 32-bit size) carrying an encoding byte and text.
Bytes id3_text_frame(std::string_view id, std::uint8_t encoding, const Bytes& text) {
    ByteWriter writer;
    writer.text(id).u32be(text.size() + 1).u16be(0).u8(encoding).bytes(text);
    return writer.result();
}

/// An ID3v2.4 frame (syncsafe size).
Bytes id3v24_text_frame(std::string_view id, std::uint8_t encoding, const Bytes& text) {
    ByteWriter writer;
    writer.text(id).bytes(syncsafe(static_cast<std::uint32_t>(text.size() + 1))).u16be(0).u8(encoding).bytes(text);
    return writer.result();
}

Bytes id3v2_tag(int major_version, std::uint8_t flags, const Bytes& body) {
    ByteWriter writer;
    writer.text("ID3").u8(static_cast<std::uint64_t>(major_version)).u8(0).u8(flags);
    writer.bytes(syncsafe(static_cast<std::uint32_t>(body.size()))).bytes(body);
    return writer.result();
}

/// ID3v2.3 tag with Latin-1, UTF-16 and genre-reference frames plus padding.
Bytes sample_id3v23_tag() {
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

/// A 128-byte ID3v1 tag; a non-zero track number makes it ID3v1.1 (28-byte comment).
Bytes id3v1_tag(std::string_view title, std::string_view comment, std::uint8_t track, std::uint8_t genre) {
    ByteWriter writer;
    const auto field = [&writer](std::string_view value, std::size_t width) {
        writer.text(value.substr(0, width)).zeros(width - std::min(width, value.size()));
    };
    writer.text("TAG");
    field(title, 30);
    field("Artist", 30);
    field("Album", 30);
    field("1999", 4);
    if (track != 0) {
        field(comment, 28);
        writer.u8(0).u8(track);
    } else {
        field(comment, 30);
    }
    writer.u8(genre);
    return writer.result();
}

/// One APEv2 item: value, item flags (bits 1-2: 0 text, 1 binary), key.
struct ApeItem {
    std::string key;
    Bytes value;
    std::uint32_t flags = 0;
};

/// An APEv2 tag with header and footer.
Bytes ape_tag(const std::vector<ApeItem>& items) {
    ByteWriter body;
    for (const ApeItem& item : items)
        body.u32le(item.value.size()).u32le(item.flags).text(item.key).u8(0).bytes(item.value);
    const std::uint64_t tag_size = body.size() + 32;
    const auto header_or_footer = [&](bool is_header) {
        ByteWriter part;
        part.text("APETAGEX").u32le(2000).u32le(tag_size).u32le(items.size());
        part.u32le(0x80000000u | (is_header ? 0x20000000u : 0u)).zeros(8);
        return part.result();
    };
    return concat({header_or_footer(true), body.result(), header_or_footer(false)});
}

// ---------------------------------------------------------------- Ogg fixtures

/// One Ogg page with a correct checksum.
Bytes ogg_page(std::uint8_t header_type, std::uint64_t granule, std::uint32_t serial, std::uint32_t sequence,
               const Bytes& lacing, const Bytes& body) {
    ByteWriter writer;
    writer.text("OggS").u8(0).u8(header_type).u64le(granule).u32le(serial).u32le(sequence).u32le(0);
    writer.u8(lacing.size()).bytes(lacing).bytes(body);
    Bytes page = writer.result();
    const std::uint32_t checksum = detail::ogg_page_checksum(page);
    for (int index = 0; index < 4; ++index)
        page[22 + static_cast<std::size_t>(index)] = static_cast<std::uint8_t>(checksum >> (8 * index));
    return page;
}

/// Lays packets out as an Ogg stream: each packet starts a page, large packets continue over
/// pages of 255 segments, and the last page carries `final_granule` and the end-of-stream flag.
Bytes ogg_stream(const std::vector<Bytes>& packets, std::uint32_t serial, std::uint64_t final_granule) {
    ByteWriter stream;
    std::uint32_t sequence = 0;
    for (std::size_t packet_index = 0; packet_index < packets.size(); ++packet_index) {
        const Bytes& packet = packets[packet_index];
        const bool last_packet = packet_index + 1 == packets.size();
        Bytes lacing;
        for (std::size_t remaining = packet.size();; remaining -= 255) {
            lacing.push_back(static_cast<std::uint8_t>(std::min<std::size_t>(remaining, 255)));
            if (remaining < 255) break;
        }
        std::size_t lacing_offset = 0;
        std::size_t body_offset = 0;
        bool continued = false;
        while (lacing_offset < lacing.size()) {
            const std::size_t count = std::min<std::size_t>(255, lacing.size() - lacing_offset);
            const Bytes page_lacing(lacing.begin() + static_cast<std::ptrdiff_t>(lacing_offset),
                                    lacing.begin() + static_cast<std::ptrdiff_t>(lacing_offset + count));
            std::size_t body_size = 0;
            for (const std::uint8_t value : page_lacing)
                body_size += value;
            const Bytes body(packet.begin() + static_cast<std::ptrdiff_t>(body_offset),
                             packet.begin() + static_cast<std::ptrdiff_t>(body_offset + body_size));
            const bool packet_ends = lacing_offset + count == lacing.size();
            std::uint8_t header_type = continued ? 0x01 : 0x00;
            if (sequence == 0) header_type |= 0x02;
            if (last_packet && packet_ends) header_type |= 0x04;
            const std::uint64_t granule = !packet_ends ? ~0ull : (last_packet ? final_granule : 0);
            stream.bytes(ogg_page(header_type, granule, serial, sequence++, page_lacing, body));
            lacing_offset += count;
            body_offset += body_size;
            continued = true;
        }
    }
    return stream.result();
}

/// A Vorbis comment block (no packet prefix).
Bytes vorbis_comment_block(const std::vector<std::string>& entries) {
    ByteWriter writer;
    writer.u32le(4).text("test").u32le(entries.size());
    for (const std::string& entry : entries)
        writer.u32le(entry.size()).text(entry);
    return writer.result();
}

Bytes vorbis_identification(std::uint8_t channels, std::uint32_t rate) {
    ByteWriter writer;
    writer.u8(1).text("vorbis").u32le(0).u8(channels).u32le(rate).u32le(0).u32le(128000).u32le(0).u8(0xB8).u8(1);
    return writer.result();
}

// ---------------------------------------------------------------- ISO media (MP4) fixtures

Bytes box(std::string_view type, const Bytes& payload) {
    ByteWriter writer;
    writer.u32be(payload.size() + 8).text(type).bytes(payload);
    return writer.result();
}

/// A box with the 64-bit "largesize" header.
Bytes large_box(std::string_view type, const Bytes& payload) {
    ByteWriter writer;
    writer.u32be(1).text(type).u64be(payload.size() + 16).bytes(payload);
    return writer.result();
}

Bytes full_box(std::string_view type, std::uint8_t version, const Bytes& payload) {
    ByteWriter writer;
    writer.u8(version).zeros(3).bytes(payload);
    return box(type, writer.result());
}

Bytes handler_box(std::string_view handler) {
    ByteWriter writer;
    writer.u32be(0).text(handler).zeros(12).u8(0);
    return full_box("hdlr", 0, writer.result());
}

Bytes media_header_v1(std::uint32_t timescale, std::uint64_t duration) {
    ByteWriter writer;
    writer.u64be(0).u64be(0).u32be(timescale).u64be(duration).u16be(0x55C4).u16be(0);
    return full_box("mdhd", 1, writer.result());
}

/// The fixed fields of an audio sample entry (version 0, 1 or 2 layout prefix).
ByteWriter sample_entry_prefix(std::uint16_t version, std::uint16_t channels, std::uint16_t sample_size,
                               std::uint32_t rate_16_16) {
    ByteWriter writer;
    writer.zeros(6).u16be(1).u16be(version).u16be(0).u32be(0).u16be(channels).u16be(sample_size);
    writer.u16be(0).u16be(0).u32be(rate_16_16);
    return writer;
}

Bytes alac_entry(std::uint8_t bit_depth, std::uint8_t channels, std::uint32_t rate) {
    ByteWriter cookie;
    cookie.u32be(0).u32be(4096).u8(0).u8(bit_depth).u8(40).u8(10).u8(14).u8(channels).u16be(255).u32be(0);
    cookie.u32be(0).u32be(rate);
    ByteWriter entry = sample_entry_prefix(0, channels, bit_depth, rate << 16);
    entry.bytes(box("alac", cookie.result()));
    return box("alac", entry.result());
}

Bytes ilst_item(std::string_view type, std::uint32_t data_type, const Bytes& value) {
    ByteWriter data;
    data.u32be(data_type).u32be(0).bytes(value);
    return box(type, box("data", data.result()));
}

/// A complete MP4 file: ftyp, a moov with one track built from `sample_entry`, and iTunes tags.
Bytes mp4_file(std::string_view brand, const Bytes& sample_entry, std::uint32_t timescale, std::uint64_t duration,
               const Bytes& item_list, bool large_moov = false) {
    ByteWriter stsd;
    stsd.u32be(1).bytes(sample_entry);
    const Bytes sample_table = box("stbl", full_box("stsd", 0, stsd.result()));
    const Bytes media =
        box("mdia", concat({media_header_v1(timescale, duration), handler_box("soun"), box("minf", sample_table)}));
    const Bytes track = box("trak", concat({full_box("tkhd", 0, Bytes(80, 0)), media}));
    ByteWriter meta;
    meta.u32be(0).bytes(handler_box("mdir")).bytes(box("ilst", item_list));
    const Bytes user_data = box("udta", box("meta", meta.result()));
    const Bytes movie_payload = concat({full_box("mvhd", 0, Bytes(96, 0)), track, user_data});
    ByteWriter file_type;
    file_type.text(brand).u32be(0).text("isom");
    return concat({box("ftyp", file_type.result()),
                   large_moov ? large_box("moov", movie_payload) : box("moov", movie_payload),
                   box("mdat", Bytes(256, 0))});
}

// ---------------------------------------------------------------- ASF fixtures

Bytes guid_bytes(std::initializer_list<int> values) {
    Bytes bytes;
    for (const int value : values)
        bytes.push_back(static_cast<std::uint8_t>(value));
    return bytes;
}

const Bytes kAsfHeaderGuid =
    guid_bytes({0x30, 0x26, 0xB2, 0x75, 0x8E, 0x66, 0xCF, 0x11, 0xA6, 0xD9, 0x00, 0xAA, 0x00, 0x62, 0xCE, 0x6C});
const Bytes kAsfFilePropertiesGuid =
    guid_bytes({0xA1, 0xDC, 0xAB, 0x8C, 0x47, 0xA9, 0xCF, 0x11, 0x8E, 0xE4, 0x00, 0xC0, 0x0C, 0x20, 0x53, 0x65});
const Bytes kAsfStreamPropertiesGuid =
    guid_bytes({0x91, 0x07, 0xDC, 0xB7, 0xB7, 0xA9, 0xCF, 0x11, 0x8E, 0xE6, 0x00, 0xC0, 0x0C, 0x20, 0x53, 0x65});
const Bytes kAsfAudioMediaGuid =
    guid_bytes({0x40, 0x9E, 0x69, 0xF8, 0x4D, 0x5B, 0xCF, 0x11, 0xA8, 0xFD, 0x00, 0x80, 0x5F, 0x5C, 0x44, 0x2B});
const Bytes kAsfContentDescriptionGuid =
    guid_bytes({0x33, 0x26, 0xB2, 0x75, 0x8E, 0x66, 0xCF, 0x11, 0xA6, 0xD9, 0x00, 0xAA, 0x00, 0x62, 0xCE, 0x6C});
const Bytes kAsfExtendedContentGuid =
    guid_bytes({0x40, 0xA4, 0xD0, 0xD2, 0x07, 0xE3, 0xD2, 0x11, 0x97, 0xF0, 0x00, 0xA0, 0xC9, 0x5E, 0xA8, 0x50});
const Bytes kAsfContentEncryptionGuid =
    guid_bytes({0xFB, 0xB3, 0x11, 0x22, 0x23, 0xBD, 0xD2, 0x11, 0xB4, 0xB7, 0x00, 0xA0, 0xC9, 0x55, 0xFC, 0x6E});

Bytes utf16le(std::string_view ascii, bool terminated = true) {
    ByteWriter writer;
    for (const char character : ascii)
        writer.u16le(static_cast<std::uint8_t>(character));
    if (terminated) writer.u16le(0);
    return writer.result();
}

Bytes asf_object(const Bytes& guid, const Bytes& payload) {
    ByteWriter writer;
    writer.bytes(guid).u64le(payload.size() + 24).bytes(payload);
    return writer.result();
}

Bytes asf_file(const std::vector<Bytes>& objects) {
    ByteWriter body;
    for (const Bytes& object : objects)
        body.bytes(object);
    ByteWriter file;
    file.bytes(kAsfHeaderGuid).u64le(body.size() + 30).u32le(objects.size()).u8(1).u8(2).bytes(body.result());
    file.zeros(64);  // stand-in for the data object
    return file.result();
}

Bytes asf_stream_properties(std::uint16_t format_tag) {
    ByteWriter format;
    format.u16le(format_tag).u16le(2).u32le(44100).u32le(16000).u16le(2048).u16le(16).u16le(0);
    ByteWriter payload;
    payload.bytes(kAsfAudioMediaGuid).zeros(16).u64le(0).u32le(format.size()).u32le(0).u16le(1).u32le(0);
    payload.bytes(format.result());
    return asf_object(kAsfStreamPropertiesGuid, payload.result());
}

// ---------------------------------------------------------------- decoding helpers

/// Reads every remaining frame from a decoder, checking it never returns more than asked.
std::vector<float> read_all(Decoder& decoder) {
    std::vector<float> samples;
    constexpr std::size_t kBlockFrames = 4096;
    std::vector<float> block(kBlockFrames * static_cast<std::size_t>(decoder.channels()));
    while (true) {
        auto count = decoder.read(std::span<float>(block));
        REQUIRE(count.has_value());
        REQUIRE(*count <= kBlockFrames);
        if (*count == 0) break;
        samples.insert(
            samples.end(), block.begin(),
            block.begin() + static_cast<std::ptrdiff_t>(*count * static_cast<std::size_t>(decoder.channels())));
    }
    return samples;
}

/// Seeking at or past the end succeeds on every backend and leaves the decoder at its end.
void check_seek_past_end(Decoder& decoder) {
    const std::int64_t beyond = decoder.total_frames().value_or(1'000'000) + 1000;
    REQUIRE(decoder.seek(beyond).has_value());
    if (decoder.frames_exact()) CHECK(decoder.position() == *decoder.total_frames());
    std::vector<float> block(64 * static_cast<std::size_t>(decoder.channels()));
    const auto count = decoder.read(std::span<float>(block));
    REQUIRE(count.has_value());
    CHECK(*count == 0);
    REQUIRE(decoder.seek(0).has_value());
    CHECK(decoder.position() == 0);
}

/// Bytes and last write time of a file, for the invariant-11 checks.
struct FileSnapshot {
    std::string bytes;
    fs::file_time_type last_write;
    bool operator==(const FileSnapshot&) const = default;
};

FileSnapshot snapshot(const fs::path& file) {
    return {mso::test::read_file(file), fs::last_write_time(file)};
}

// ---------------------------------------------------------------- FFmpeg fixtures

/// True when an executable named `name` (with .exe on Windows) is in a PATH directory. Starts no
/// process, so it is cheap enough to evaluate while tests register.
bool executable_on_path(std::string_view name) {
    const char* path_variable = std::getenv("PATH");
    if (path_variable == nullptr) return false;
#ifdef _WIN32
    constexpr char kSeparator = ';';
    const std::string file_name = std::string(name) + ".exe";
#else
    constexpr char kSeparator = ':';
    const std::string file_name(name);
#endif
    const std::string_view directories(path_variable);
    std::size_t start = 0;
    while (start <= directories.size()) {
        const std::size_t end = std::min(directories.find(kSeparator, start), directories.size());
        if (end > start) {
            std::error_code error;
            const fs::path candidate = fs::path(std::string(directories.substr(start, end - start))) / file_name;
            if (fs::is_regular_file(candidate, error)) return true;
        }
        start = end + 1;
    }
    return false;
}

/// FFmpeg-dependent tests are skipped (through doctest) only when ffmpeg is missing and not required.
bool skip_without_ffmpeg() {
    static const bool skip = !mso::test::ffmpeg_required() && !executable_on_path("ffmpeg");
    return skip;
}

bool has_capability(std::string_view codec) {
    const auto codecs = decoder_capabilities();
    return std::ranges::find(codecs, codec) != codecs.end();
}

/// Tests that need the FFmpeg decoding backend too.
bool skip_without_ffmpeg_backend() {
    static const bool skip = !mso::test::ffmpeg_required() && (skip_without_ffmpeg() || !has_capability("aac"));
    return skip;
}

/// Renders `seconds` of a 440 Hz sine with `encoder_arguments`; false if ffmpeg failed.
bool render_with_ffmpeg(const fs::path& output, std::string_view encoder_arguments, int sample_rate = 44100,
                        int channels = 2, double seconds = 1.0) {
    const std::string arguments =
        "ffmpeg -hide_banner -loglevel error -y -f lavfi -i \"sine=frequency=440:sample_rate=" +
        std::to_string(sample_rate) + ":duration=" + std::to_string(seconds) + "\" -ac " + std::to_string(channels) +
        " " + std::string(encoder_arguments) + " \"";
#ifdef _WIN32
    // _wsystem keeps a non-ASCII output path intact (std::system reads UTF-8 in the ANSI code page).
    const std::wstring command = mso::path_from_utf8(arguments).native() + output.native() + L"\"";
    return _wsystem(command.c_str()) == 0 && fs::exists(output);
#else
    const std::string command = arguments + mso::path_to_utf8(output) + "\"";
    return std::system(command.c_str()) == 0 && fs::exists(output);
#endif
}

}  // namespace

namespace {

/// Packs values least-significant bit first, the bit order of Vorbis header fields.
class BitWriter {
public:
    /// Appends the low `count` bits of `value`.
    BitWriter& bits(std::uint32_t value, int count) {
        for (int bit = 0; bit < count; ++bit) {
            if (used_ == 0) bytes_.push_back(0);
            if ((value >> bit) & 1u) bytes_.back() = static_cast<std::uint8_t>(bytes_.back() | (1u << used_));
            used_ = (used_ + 1) % 8;
        }
        return *this;
    }
    const Bytes& result() const { return bytes_; }

private:
    Bytes bytes_;
    int used_ = 0;  ///< bits used in the last byte (0: start a new byte)
};

/// Memory the process has committed for itself (Windows private bytes, Linux data segment), or
/// nullopt where it cannot be measured.
std::optional<std::uint64_t> committed_bytes() {
#ifdef _WIN32
    PROCESS_MEMORY_COUNTERS_EX counters{};
    if (!K32GetProcessMemoryInfo(GetCurrentProcess(), reinterpret_cast<PROCESS_MEMORY_COUNTERS*>(&counters),
                                 sizeof(counters)))
        return std::nullopt;
    return counters.PrivateUsage;
#else
    std::ifstream statm("/proc/self/statm");
    std::uint64_t size = 0, resident = 0, shared = 0, text = 0, library = 0, data = 0;
    if (!(statm >> size >> resident >> shared >> text >> library >> data)) return std::nullopt;
    return data * static_cast<std::uint64_t>(::sysconf(_SC_PAGESIZE));
#endif
}

/// The leak check needs a memory measurement; elsewhere it is reported as skipped.
bool memory_not_measurable() {
    static const bool unmeasurable = !committed_bytes().has_value();
    return unmeasurable;
}

}  // namespace

TEST_SUITE("media") {
    // ---------------------------------------------------------------- helpers

    TEST_CASE("ByteCursor reads both byte orders and fails sticky on short input") {
        const Bytes bytes = {0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07, 0x08, 0x09};
        detail::ByteCursor cursor(bytes);
        CHECK(cursor.u16_be() == 0x0102);
        CHECK(cursor.u16_le() == 0x0403);
        CHECK(cursor.u24_be() == 0x050607);
        CHECK(cursor.remaining() == 2);
        CHECK(cursor.u32_le() == 0);  // only two bytes left
        CHECK_FALSE(cursor.ok());
        CHECK(cursor.u8() == 0);  // stays failed
        CHECK(cursor.position() == 7);

        CHECK(detail::checked_add(~0ull, 1) == std::nullopt);
        CHECK(detail::checked_add(1, 2) == 3u);
        CHECK(detail::checked_multiply(1ull << 32, 1ull << 32) == std::nullopt);
        CHECK(detail::decode_syncsafe(0x00000201) == 257);
        CHECK(detail::decode_syncsafe(0x7F7F7F7F) == 0x0FFFFFFF);
        CHECK(detail::units_to_microseconds(44100, 44100) == 1'000'000);
        CHECK(detail::units_to_microseconds(1, 0) == std::nullopt);
        CHECK(detail::units_to_microseconds(~0ull, 1) == std::nullopt);
    }

    TEST_CASE("Text decoding handles UTF-16 byte orders, surrogates, terminators and Latin-1 fallback") {
        using detail::TextEncoding;
        // "A😀" in UTF-16BE, then a NUL that ends the text.
        const Bytes big_endian = {0x00, 'A', 0xD8, 0x3D, 0xDE, 0x00, 0x00, 0x00, 0x00, 'B'};
        CHECK(detail::decode_text(big_endian, TextEncoding::Utf16BigEndian) == "A\xF0\x9F\x98\x80");
        const Bytes little_with_bom = {0xFF, 0xFE, 'h', 0, 'i', 0};
        CHECK(detail::decode_text(little_with_bom, TextEncoding::Utf16WithBom) == "hi");
        const Bytes big_with_bom = {0xFE, 0xFF, 0, 'h', 0, 'i'};
        CHECK(detail::decode_text(big_with_bom, TextEncoding::Utf16WithBom) == "hi");
        const Bytes lone_surrogate = {0xDC, 0x00, 0x00, 'x'};
        CHECK(detail::decode_text(lone_surrogate, TextEncoding::Utf16BigEndian) == "\xEF\xBF\xBDx");
        const Bytes latin = {'c', 'a', 'f', 0xE9};
        CHECK(detail::decode_text(latin, TextEncoding::Utf8) == "caf\xC3\xA9");  // not UTF-8: read as Latin-1
        const Bytes utf8 = {'c', 'a', 'f', 0xC3, 0xA9, 0, 'z'};
        CHECK(detail::decode_legacy_text(utf8) == "caf\xC3\xA9");
        CHECK(detail::find_terminator(Bytes{'a', 0, 0, 'b'}, TextEncoding::Utf16LittleEndian) == 4);
        CHECK(detail::find_terminator(Bytes{'a', 0, 'b', 0, 0, 0}, TextEncoding::Utf16LittleEndian) == 4);
        CHECK(detail::code_unit_size(TextEncoding::Utf16BigEndian) == 2);
    }

    TEST_CASE("Tag mapping parses tempos, classic, Camelot and Open Key keys, genres and keywords") {
        CHECK(detail::parse_tag_bpm("128") == 128'000);
        CHECK(detail::parse_tag_bpm(" 127,5 ") == 127'500);
        CHECK(detail::parse_tag_bpm("126 BPM") == 126'000);
        CHECK(detail::parse_tag_bpm("fast") == std::nullopt);
        CHECK(detail::parse_tag_bpm("1000") == std::nullopt);
        CHECK(detail::parse_tag_key("F#m") == MusicalKey{6, KeyMode::Minor});
        CHECK(detail::parse_tag_key("Bbmin") == MusicalKey{10, KeyMode::Minor});
        CHECK(detail::parse_tag_key("8A") == MusicalKey{9, KeyMode::Minor});
        CHECK(detail::parse_tag_key("8B") == MusicalKey{0, KeyMode::Major});
        CHECK(detail::parse_tag_key("1d") == MusicalKey{0, KeyMode::Major});
        CHECK(detail::parse_tag_key("1m") == MusicalKey{9, KeyMode::Minor});
        CHECK(detail::parse_tag_key("2d") == MusicalKey{7, KeyMode::Major});
        CHECK(detail::parse_tag_key("12m") == MusicalKey{2, KeyMode::Minor});
        CHECK(detail::parse_tag_key("8X") == std::nullopt);
        CHECK(detail::id3v1_genre_name(0) == "Blues");
        CHECK(detail::id3v1_genre_name(79) == "Hard Rock");
        CHECK(detail::id3v1_genre_name(80) == "Folk");
        CHECK(detail::id3v1_genre_name(127) == "Drum & Bass");
        CHECK(detail::id3v1_genre_name(189) == "Dubstep");
        CHECK(detail::id3v1_genre_name(191) == "Psybient");
        CHECK(detail::id3v1_genre_name(192) == std::nullopt);
        CHECK(detail::id3v1_genre_name(-1) == std::nullopt);

        EmbeddedTags tags;
        detail::store_tag(tags, detail::TagField::Title, "  First  ");
        detail::store_tag(tags, detail::TagField::Title, "Second");
        CHECK(tags.title == "First");
        detail::store_tag(tags, detail::TagField::Keywords, "drums; loop, drums,,");
        CHECK(tags.keywords == std::vector<std::string>{"drums", "loop"});
        detail::store_tag(tags, detail::TagField::Comment, std::string(5000, 'x'));
        CHECK(tags.comment->size() == 1024);
        CHECK(detail::field_for_tag_name(" INITIALKEY ") == detail::TagField::Key);
        CHECK(detail::field_for_tag_name("encoder") == std::nullopt);
    }

    TEST_CASE("BoundedFileReader charges every byte and refuses reads past the budget or deadline") {
        ScratchDirectory directory("media");
        const fs::path file = directory.file("bytes.bin");
        write_bytes(file, Bytes(1000, 7));
        auto reader = detail::BoundedFileReader::open(file, ProbeLimits{100, std::chrono::milliseconds(5000)});
        REQUIRE(reader.has_value());
        CHECK(reader->file_size() == 1000);
        auto first = reader->read_exact(0, 60);
        REQUIRE(first.has_value());
        CHECK(reader->bytes_read() == 60);
        auto too_much = reader->read_exact(100, 50);
        REQUIRE_FALSE(too_much.has_value());
        CHECK(too_much.error().code == ErrorCode::ProbeLimit);
        auto shrunk = reader->read_within_budget(200, 500);
        REQUIRE(shrunk.has_value());
        CHECK(shrunk->size() == 40);
        CHECK(reader->remaining_budget() == 0);
        CHECK(reader->read_within_budget(300, 1).error().code == ErrorCode::ProbeLimit);
        CHECK(reader->read_available(1000, 10)->empty());  // at the end nothing is charged

        auto truncated_reader = detail::BoundedFileReader::open(file, {});
        REQUIRE(truncated_reader.has_value());
        CHECK(truncated_reader->read_exact(990, 20).error().code == ErrorCode::CorruptAudio);
        auto expired = detail::BoundedFileReader::open(file, ProbeLimits{1000, std::chrono::milliseconds(0)});
        REQUIRE(expired.has_value());
        CHECK(expired->read_exact(0, 1).error().code == ErrorCode::ProbeLimit);
    }

    TEST_CASE("FileHandle reads positionally, refuses directories, and hands its file to stdio") {
        ScratchDirectory directory("media");
        const fs::path file = directory.file("handle.bin");
        write_bytes(file, latin1("0123456789"));
        auto handle = detail::FileHandle::open_for_read(file);
        REQUIRE(handle.has_value());
        CHECK(handle->size() == 10u);
        std::uint8_t buffer[4] = {};
        CHECK(handle->read_at(8, buffer) == std::size_t{2});
        CHECK(buffer[0] == '8');
        CHECK(handle->read_at(20, buffer) == std::size_t{0});

        auto stream = handle->release_to_stdio();
        REQUIRE(stream.has_value());
        char text[11] = {};
        CHECK(std::fread(text, 1, 10, *stream) == 10);
        CHECK(std::string_view(text) == "0123456789");
        CHECK(std::fclose(*stream) == 0);

        CHECK(detail::FileHandle::open_for_read(directory.path()).error().code == ErrorCode::UnsupportedCodec);
        CHECK(detail::FileHandle::open_for_read(directory.file("missing.bin")).error().code == ErrorCode::FileMissing);
    }

    TEST_CASE("Files beyond the classic 260-character limit are probed and decoded") {
        ScratchDirectory directory("media");
        fs::path deep = directory.path();
        for (int level = 0; level < 6; ++level)
            deep /= std::string(48, static_cast<char>('a' + level));
        const fs::path file = deep / "long_path_sample.wav";
        REQUIRE(mso::path_to_utf8(file).size() > 300);
        std::error_code error;
        fs::create_directories(native_path(deep), error);
        REQUIRE_FALSE(error);
        {
            auto handle_path = native_path(file);
            const Bytes wav = small_wav(32);
            std::FILE* stream = nullptr;
#ifdef _WIN32
            stream = _wfopen(handle_path.c_str(), L"wb");
#else
            stream = std::fopen(handle_path.c_str(), "wb");
#endif
            REQUIRE(stream != nullptr);
            CHECK(std::fwrite(wav.data(), 1, wav.size(), stream) == wav.size());
            CHECK(std::fclose(stream) == 0);
        }
        auto probe = probe_file(file);
        REQUIRE(probe.has_value());
        CHECK(probe->frames == 32);
        auto decoder = open_decoder(file);
        REQUIRE(decoder.has_value());
        CHECK(read_all(**decoder).size() == 64);
        decoder->reset();
        fs::remove_all(native_path(directory.path() / std::string(48, 'a')), error);
    }

    // ---------------------------------------------------------------- WAV

    TEST_CASE("WAV int16 probe reads layout, INFO tags, acid tempo and smpl loops; decoder streams and seeks") {
        ScratchDirectory directory("media");
        const fs::path file = directory.file("loop.wav");
        constexpr std::size_t kFrames = 1000;
        write_bytes(file, riff_file("RIFF", {
                                                {"fmt ", format_chunk(1, 2, 44100, 16)},
                                                {"LIST", info_list({{"INAM", "Drum Loop"},
                                                                    {"IART", "Studio"},
                                                                    {"IGNR", "House"},
                                                                    {"ICMT", "dry"},
                                                                    {"IKEY", "drums; loop, 909"}})},
                                                {"acid", acid_chunk(126.5f, false)},
                                                {"data", pcm16_payload(kFrames, 2)},
                                                {"smpl", sampler_chunk(2, {{100, 899}, {50, 5000}})},
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
        REQUIRE(probe->loops.size() == 1);  // the second loop ends past the data
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
        check_seek_past_end(stream);
    }

    TEST_CASE("WAV 24-bit and extensible float32 report codec, bit depth and decode exactly") {
        ScratchDirectory directory("media");
        const fs::path pcm24 = directory.file("mono24.wav");
        ByteWriter samples24;
        for (int value : {0, 1, -1, 8388607, -8388608, 4194304})
            samples24.u8(static_cast<std::uint64_t>(value & 0xFF))
                .u8(static_cast<std::uint64_t>((value >> 8) & 0xFF))
                .u8(static_cast<std::uint64_t>((value >> 16) & 0xFF));
        write_bytes(pcm24, riff_file("RIFF", {{"fmt ", format_chunk(1, 1, 96000, 24)}, {"data", samples24.result()}}));

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
        write_bytes(float32, riff_file("RIFF", {{"fmt ", extensible_format_chunk(3, 2, 48000, 32, 32)},
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
        write_bytes(packed20,
                    riff_file("RIFF", {{"fmt ", extensible_format_chunk(1, 2, 44100, 24, 20)}, {"data", Bytes(60)}}));
        auto probe20 = probe_file(packed20);
        REQUIRE(probe20.has_value());
        CHECK(probe20->codec == "pcm_s24");
        CHECK(probe20->bit_depth == 20);
        CHECK(probe20->frames == 10);
    }

    TEST_CASE("RF64 takes its data size from the ds64 chunk") {
        ScratchDirectory directory("media");
        const fs::path file = directory.file("long.rf64");
        const auto data = pcm16_payload(300, 2);
        ByteWriter ds64;
        ds64.u64le(0).u64le(data.size()).u64le(300).u32le(0);
        write_bytes(file, riff_file("RF64", {{"ds64", ds64.result()},
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

    TEST_CASE("Wave64 chunks are found by GUID, including the list chunk with INFO tags") {
        ScratchDirectory directory("media");
        const Bytes suffix = guid_bytes({0xF3, 0xAC, 0xD3, 0x11, 0x8C, 0xD1, 0x00, 0xC0, 0x4F, 0x8E, 0xDB, 0x8A});
        const auto chunk = [](const Bytes& guid, const Bytes& payload) {
            ByteWriter writer;
            writer.bytes(guid).u64le(payload.size() + 24).bytes(payload);
            while (writer.size() % 8 != 0)
                writer.u8(0);
            return writer.result();
        };
        const Bytes format_guid = concat({latin1("fmt "), suffix});
        const Bytes data_guid = concat({latin1("data"), suffix});
        const Bytes list_guid =
            guid_bytes({'l', 'i', 's', 't', 0x2F, 0x91, 0xCF, 0x11, 0xA5, 0xD6, 0x28, 0xDB, 0x04, 0xC1, 0x00, 0x00});
        const Bytes body =
            concat({chunk(format_guid, format_chunk(1, 2, 44100, 16)), chunk(list_guid, info_list({{"INAM", "Wide"}})),
                    chunk(data_guid, pcm16_payload(200, 2))});
        ByteWriter file;
        file.bytes(guid_bytes(
            {0x72, 0x69, 0x66, 0x66, 0x2E, 0x91, 0xCF, 0x11, 0xA5, 0xD6, 0x28, 0xDB, 0x04, 0xC1, 0x00, 0x00}));
        file.u64le(body.size() + 40).bytes(concat({latin1("wave"), suffix})).bytes(body);
        const fs::path path = directory.file("wide.w64");
        write_bytes(path, file.result());

        auto probe = probe_file(path);
        REQUIRE(probe.has_value());
        CHECK(probe->format == "w64");
        CHECK(probe->codec == "pcm_s16");
        CHECK(probe->frames == 200);
        CHECK(probe->tags.title == "Wide");
        auto decoder = open_decoder(path);
        REQUIRE(decoder.has_value());
        CHECK(read_all(**decoder).size() == 400);
    }

    TEST_CASE("Malformed WAV headers fail with CORRUPT_AUDIO and hostile sizes stay bounded") {
        ScratchDirectory directory("media");
        const auto probe_bytes = [&](std::string_view name, const Bytes& bytes) {
            const fs::path file = directory.file(name);
            write_bytes(file, bytes);
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
        CHECK(huge_list->bytes_read < 128 * 1024);
    }

    TEST_CASE(
        "Probe budget: metadata after a large data chunk costs only header reads; tiny budgets yield partial records") {
        ScratchDirectory directory("media");
        const fs::path file = directory.file("big.wav");
        constexpr std::size_t kFrames = 3 * 1024 * 1024 / 2;  // 6 MiB of stereo int16: larger than the 4 MiB budget
        write_bytes(file, riff_file("RIFF", {
                                                {"fmt ", format_chunk(1, 2, 44100, 16)},
                                                {"data", Bytes(kFrames * 4)},
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
        ScratchDirectory directory("media");
        constexpr std::uint32_t kFrames = 1000;
        const auto build_aiff = [&](bool compressed_form, std::string_view compression) {
            ByteWriter common;
            common.u16be(2).u32be(kFrames).u16be(16).u16be(0x400E).u32be(0xAC440000).u32be(0);  // 44100 Hz
            if (compressed_form) common.text(compression).u8(0).u8(0);
            ByteWriter markers;
            markers.u16be(2).u16be(1).u32be(100).u8(1).text("a").u16be(2).u32be(900).u8(0).u8(0);
            ByteWriter instrument;
            instrument.u8(60).u8(0).u8(0).u8(127).u8(1).u8(127).u16be(0).u16be(1).u16be(1).u16be(2);
            instrument.u16be(0).u16be(0).u16be(0);
            ByteWriter body;
            const auto chunk = [&body](std::string_view id, const Bytes& payload) {
                body.text(id).u32be(payload.size()).bytes(payload);
                if (payload.size() % 2) body.u8(0);
            };
            chunk("COMM", common.result());
            chunk("NAME", latin1("Pad"));
            chunk("AUTH", latin1("Bob"));
            ByteWriter sound;
            sound.u32be(0).u32be(0).bytes(pcm16_payload(kFrames, 2, !compressed_form || compression != "sowt"));
            chunk("SSND", sound.result());
            chunk("MARK", markers.result());
            chunk("INST", instrument.result());
            ByteWriter file;
            file.text("FORM").u32be(body.size() + 4).text(compressed_form ? "AIFC" : "AIFF").bytes(body.result());
            return file.result();
        };

        for (const bool compressed_form : {false, true}) {
            CAPTURE(compressed_form);
            const fs::path file = directory.file(compressed_form ? "pad.aifc" : "pad.aiff");
            write_bytes(file, build_aiff(compressed_form, "sowt"));
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

        // QuickTime in24/in32 are named as PCM with the width the type implies.
        const fs::path in24 = directory.file("wide.aifc");
        write_bytes(in24, build_aiff(true, "in24"));
        auto probe24 = probe_file(in24);
        REQUIRE(probe24.has_value());
        CHECK(probe24->codec == "pcm_s24");
        CHECK(probe24->bit_depth == 24);
    }

    TEST_CASE("Hostile AIFF, FLAC, CAF and MP4 sizes are bounded and fail cleanly") {
        ScratchDirectory directory("media");
        const auto probe_bytes = [&](std::string_view name, const Bytes& bytes) {
            const fs::path file = directory.file(name);
            write_bytes(file, bytes);
            return probe_file(file);
        };
        ByteWriter aiff;
        aiff.text("FORM").u32be(0xFFFFFFFF).text("AIFF").text("NAME").u32be(0xFFFFFFF0).text("Title");
        auto huge_name = probe_bytes("huge.aiff", aiff.result());
        REQUIRE_FALSE(huge_name.has_value());  // no COMM chunk: corrupt, without a 4 GiB read
        CHECK(huge_name.error().code == ErrorCode::CorruptAudio);

        ByteWriter flac;
        flac.text("fLaC").u8(0x04).u24be(0xFFFFFF).zeros(16);  // a comment block claiming 16 MiB, no STREAMINFO
        auto bad_flac = probe_bytes("bad.flac", flac.result());
        REQUIRE_FALSE(bad_flac.has_value());
        CHECK(bad_flac.error().code == ErrorCode::CorruptAudio);

        ByteWriter caf;
        caf.text("caff").u16be(1).u16be(0).text("info").u64be(0x7FFFFFFFFFFFFFFFull).u32be(1000).text("title");
        auto bad_caf = probe_bytes("bad.caf", caf.result());
        REQUIRE_FALSE(bad_caf.has_value());
        CHECK(bad_caf.error().code == ErrorCode::CorruptAudio);

        ByteWriter mp4;
        mp4.u32be(16).text("ftyp").text("M4A ").u32be(0).u32be(4).text("moov");  // size smaller than its header
        auto bad_mp4 = probe_bytes("bad.m4a", mp4.result());
        REQUIRE_FALSE(bad_mp4.has_value());
        CHECK(bad_mp4.error().code == ErrorCode::CorruptAudio);

        ByteWriter ogg;
        ogg.text("OggS").u8(0).u8(2).u64le(0).u32le(1).u32le(0).u32le(0).u8(255).fill(255,
                                                                                      255);  // segments past the end
        auto bad_ogg = probe_bytes("bad.ogg", ogg.result());
        REQUIRE_FALSE(bad_ogg.has_value());
        CHECK(bad_ogg.error().code == ErrorCode::CorruptAudio);
    }

    TEST_CASE("Probing random data behind every known signature never over-reads or crashes") {
        ScratchDirectory directory("media");
        std::mt19937 random(20261019);
        const std::vector<Bytes> signatures = {
            latin1("RIFF\x10\x00\x00\x00WAVE"sv),
            latin1("FORM\x00\x00\x10\x00"
                   "AIFC"sv),
            latin1("fLaC"sv),
            latin1("OggS"sv),
            latin1("\x00\x00\x00\x18"
                   "ftypM4A "sv),
            latin1("caff\x00\x01\x00\x00"sv),
            latin1("wvpk"sv),
            kAsfHeaderGuid,
            latin1("ID3\x04\x00\x00\x00\x00\x02\x00"sv),
            Bytes{0xFF, 0xFB, 0x90, 0x00},
            Bytes{0xFF, 0xF1, 0x50, 0x80},
        };
        const fs::path file = directory.file("fuzz.bin");
        constexpr ProbeLimits kLimits{64 * 1024, std::chrono::milliseconds(2000)};
        for (int iteration = 0; iteration < 400; ++iteration) {
            Bytes bytes = signatures[static_cast<std::size_t>(iteration) % signatures.size()];
            const std::size_t length = std::uniform_int_distribution<std::size_t>(0, 4096)(random);
            for (std::size_t index = 0; index < length; ++index)
                bytes.push_back(static_cast<std::uint8_t>(random()));
            write_bytes(file, bytes);
            auto probe = probe_file(file, kLimits);
            if (probe) CHECK(probe->bytes_read <= kLimits.max_bytes);
        }
    }

    // ---------------------------------------------------------------- MP3, ADTS and tag formats

    TEST_CASE("MP3 with ID3v2.3 tags and a LAME Info frame has exact gapless frames") {
        ScratchDirectory directory("media");
        const fs::path file = directory.file("tagged.mp3");
        write_bytes(file, concat({sample_id3v23_tag(), info_frame(10, 576, 1000), mp3_frames(10)}));

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

    TEST_CASE("MP3 without a VBR header gets an estimated duration; seeking past its estimated end is not an error") {
        ScratchDirectory directory("media");
        const fs::path file = directory.file("cbr.mp3");
        // v2.4 tag, unsynchronised: the title byte 0xFF is stored as 0xFF 0x00.
        ByteWriter frame;
        frame.text("TIT2").bytes(syncsafe(4)).u16be(0).u8(0).u8(0xFF).u8(0x00).text("a");
        write_bytes(file, concat({id3v2_tag(4, 0x80, frame.result()), mp3_frames(20)}));

        auto probe = probe_file(file);
        REQUIRE(probe.has_value());
        CHECK(probe->tags.title ==
              "\xC3\xBF"
              "a");
        CHECK_FALSE(probe->frames.has_value());
        CHECK_FALSE(probe->duration_exact);
        CHECK(probe->duration_us == 20 * kMp3FrameBytes * 8000 / 128);

        auto decoder = open_decoder(file);
        REQUIRE(decoder.has_value());
        CHECK_FALSE((*decoder)->frames_exact());
        check_seek_past_end(**decoder);
    }

    TEST_CASE("A VBRI header gives an exact frame count") {
        ScratchDirectory directory("media");
        const fs::path file = directory.file("vbri.mp3");
        write_bytes(file, concat({vbri_frame(50), mp3_frames(5)}));
        auto probe = probe_file(file);
        REQUIRE(probe.has_value());
        CHECK(probe->frames == 50 * 1152);
        CHECK(probe->duration_exact);
    }

    TEST_CASE("ADTS AAC streams are identified from the frame header") {
        ScratchDirectory directory("media");
        ByteWriter frames;
        for (int index = 0; index < 4; ++index) {
            constexpr std::uint32_t kFrameLength = 7 + 32;
            frames.u8(0xFF).u8(0xF1).u8(0x50).u8(0x80 | (kFrameLength >> 11)).u8((kFrameLength >> 3) & 0xFF);
            frames.u8(((kFrameLength & 7) << 5) | 0x1F).u8(0xFC).zeros(32);
        }
        const fs::path file = directory.file("raw.aac");
        write_bytes(file, frames.result());
        auto probe = probe_file(file);
        REQUIRE(probe.has_value());
        CHECK(probe->format == "aac");
        CHECK(probe->codec == "aac");
        CHECK(probe->sample_rate == 44100);
        CHECK(probe->channels == 2);
        CHECK_FALSE(probe->duration_exact);
    }

    TEST_CASE("ID3v2.2, ID3v2.4 extended headers and footers, UTF-16BE and plain v2.4 sizes are read") {
        ScratchDirectory directory("media");
        // ID3v2.2: three-character ids and 24-bit sizes.
        ByteWriter v22;
        v22.text("TT2").u24be(6).u8(0).text("Short");
        v22.text("TBP").u24be(4).u8(0).text("140");
        const fs::path old_tag = directory.file("v22.mp3");
        write_bytes(old_tag, concat({id3v2_tag(2, 0, v22.result()), mp3_frames(5)}));
        auto old_probe = probe_file(old_tag);
        REQUIRE(old_probe.has_value());
        CHECK(old_probe->tags.title == "Short");
        CHECK(old_probe->tags.bpm_milli == 140'000);

        // ID3v2.4 with an extended header (6 bytes) and a footer; UTF-8 and UTF-16BE frames.
        ByteWriter body;
        body.bytes(syncsafe(6)).u8(1).u8(0);  // extended header: size, flag byte count, flags
        body.bytes(id3v24_text_frame("TIT2", 3, latin1("Caf\xC3\xA9")));
        body.bytes(id3v24_text_frame("TPE1", 2, {0x00, 'Z', 0x00, 'o', 0xD8, 0x3D, 0xDE, 0x00}));
        ByteWriter footer_tag;
        footer_tag.text("ID3").u8(4).u8(0).u8(0x40 | 0x10).bytes(syncsafe(static_cast<std::uint32_t>(body.size())));
        footer_tag.bytes(body.result());
        footer_tag.text("3DI").u8(4).u8(0).u8(0x40 | 0x10).bytes(syncsafe(static_cast<std::uint32_t>(body.size())));
        const fs::path new_tag = directory.file("v24.mp3");
        write_bytes(new_tag, concat({footer_tag.result(), mp3_frames(5)}));
        auto new_probe = probe_file(new_tag);
        REQUIRE(new_probe.has_value());
        CHECK(new_probe->format == "mp3");
        CHECK(new_probe->tags.title == "Caf\xC3\xA9");
        CHECK(new_probe->tags.artist == "Zo\xF0\x9F\x98\x80");

        // Old iTunes wrote v2.4 frame sizes as plain integers: 300 is 0x0000012C, which also reads
        // as syncsafe 172. Only the plain reading lands on the next frame.
        const std::string long_title(299, 'x');
        ByteWriter itunes;
        itunes.text("TIT2").u32be(300).u16be(0).u8(0).text(long_title);
        itunes.bytes(id3v24_text_frame("TPE1", 0, latin1("Plain")));
        const fs::path plain_sizes = directory.file("itunes.mp3");
        write_bytes(plain_sizes, concat({id3v2_tag(4, 0, itunes.result()), mp3_frames(5)}));
        auto plain_probe = probe_file(plain_sizes);
        REQUIRE(plain_probe.has_value());
        CHECK(plain_probe->tags.title == long_title);
        CHECK(plain_probe->tags.artist == "Plain");
    }

    TEST_CASE("ID3v1 and ID3v1.1 comments, and APEv2 tags that win over ID3v1") {
        ScratchDirectory directory("media");
        const std::string thirty(30, 'c');
        const fs::path v10 = directory.file("v10.mp3");
        write_bytes(v10, concat({mp3_frames(5), id3v1_tag("Old Title", thirty, 0, 17)}));
        auto v10_probe = probe_file(v10);
        REQUIRE(v10_probe.has_value());
        CHECK(v10_probe->tags.title == "Old Title");
        CHECK(v10_probe->tags.comment == thirty);  // ID3v1.0: all 30 bytes
        CHECK(v10_probe->tags.genre == "Rock");

        const fs::path v11 = directory.file("v11.mp3");
        write_bytes(v11, concat({mp3_frames(5), id3v1_tag("Old Title", thirty, 7, 17)}));
        auto v11_probe = probe_file(v11);
        REQUIRE(v11_probe.has_value());
        CHECK(v11_probe->tags.comment == std::string(28, 'c'));  // ID3v1.1: the track number takes two bytes

        const fs::path ape = directory.file("ape.mp3");
        const Bytes tags = ape_tag({{"Title", latin1("Ape Title")},
                                    {"BPM", latin1("98")},
                                    {"Comment", Bytes(100'000, 0x55), 1u << 1},  // binary: skipped unread
                                    {"Cover Art (Front)", Bytes(200'000, 0xAA), 1u << 1}});
        write_bytes(ape, concat({mp3_frames(10), tags, id3v1_tag("V1 Title", "v1 comment", 0, 17)}));
        auto ape_probe = probe_file(ape);
        REQUIRE(ape_probe.has_value());
        CHECK(ape_probe->tags.title == "Ape Title");
        CHECK(ape_probe->tags.bpm_milli == 98'000);
        CHECK(ape_probe->tags.comment == "v1 comment");
        CHECK(ape_probe->tags.genre == "Rock");
        CHECK(ape_probe->duration_us == 10 * kMp3FrameBytes * 8000 / 128);  // tags are not audio
        CHECK(ape_probe->bytes_read < 100'000);
    }

    TEST_CASE("Cover art in an ID3 tag is skipped unread; a starved probe still keeps the tags it read") {
        ScratchDirectory directory("media");
        const fs::path file = directory.file("art.mp3");
        ByteWriter frames;
        frames.bytes(id3_text_frame("TIT2", 0, latin1("Big Art")));
        frames.text("APIC").u32be(2'000'000).u16be(0).zeros(2'000'000);
        frames.bytes(id3_text_frame("TPE1", 0, latin1("After Art")));
        write_bytes(file, concat({id3v2_tag(3, 0, frames.result()), mp3_frames(5)}));

        auto probe = probe_file(file);
        REQUIRE(probe.has_value());
        CHECK_FALSE(probe->partial);
        CHECK(probe->tags.title == "Big Art");
        CHECK(probe->tags.artist == "After Art");
        CHECK(probe->format == "mp3");
        CHECK(probe->bytes_read < 128 * 1024);

        // Signature (64) + tag header (10) + TIT2 (18) + APIC header (10) + TPE1 header (10) = 112 bytes:
        // the budget runs out on the TPE1 text, after the title was stored.
        auto starved = probe_file(file, ProbeLimits{120, std::chrono::milliseconds(2000)});
        REQUIRE(starved.has_value());
        CHECK(starved->partial);
        CHECK(starved->tags.title == "Big Art");
        CHECK_FALSE(starved->tags.artist.has_value());
        CHECK(starved->bytes_read <= 120);
    }

    // ---------------------------------------------------------------- Ogg

    TEST_CASE("Ogg FLAC and Ogg Speex are identified with their layout, tags and length") {
        ScratchDirectory directory("media");
        ByteWriter streaminfo;
        streaminfo.u16be(4096).u16be(4096).u24be(0).u24be(0);
        // rate 48000 (20 bits), channels-1 = 1 (3 bits), bits-1 = 23 (5 bits), total 0 (36 bits)
        streaminfo.u64be((std::uint64_t{48000} << 44) | (std::uint64_t{1} << 41) | (std::uint64_t{23} << 36)).zeros(16);
        ByteWriter flac_header;
        flac_header.u8(0x7F).text("FLAC").u8(1).u8(0).u16be(1).text("fLaC").u8(0x00).u24be(34).bytes(
            streaminfo.result());
        ByteWriter flac_comments;
        const Bytes comment_block = vorbis_comment_block({"TITLE=Ogg Flac", "BPM=100"});
        flac_comments.u8(0x84).u24be(comment_block.size()).bytes(comment_block);
        const fs::path flac = directory.file("stream.oga");
        write_bytes(flac, ogg_stream({flac_header.result(), flac_comments.result(), Bytes(20, 0)}, 7, 96000));
        auto flac_probe = probe_file(flac);
        REQUIRE(flac_probe.has_value());
        CHECK(flac_probe->format == "ogg");
        CHECK(flac_probe->codec == "flac");
        CHECK(flac_probe->sample_rate == 48000);
        CHECK(flac_probe->channels == 2);
        CHECK(flac_probe->bit_depth == 24);
        CHECK(flac_probe->frames == 96000);
        CHECK(flac_probe->tags.title == "Ogg Flac");
        CHECK(flac_probe->tags.bpm_milli == 100'000);

        ByteWriter speex;
        speex.text("Speex   ").text("1.2").zeros(17).u32le(1).u32le(80).u32le(16000).u32le(1).u32le(4).u32le(1);
        speex.zeros(80 - speex.size());
        const fs::path speex_file = directory.file("voice.spx");
        write_bytes(speex_file,
                    ogg_stream({speex.result(), vorbis_comment_block({"TITLE=Talk"}), Bytes(10, 0)}, 3, 16000));
        auto speex_probe = probe_file(speex_file);
        REQUIRE(speex_probe.has_value());
        CHECK(speex_probe->codec == "speex");
        CHECK(speex_probe->sample_rate == 16000);
        CHECK(speex_probe->channels == 1);
        CHECK(speex_probe->tags.title == "Talk");
    }

    TEST_CASE("Ogg Vorbis cover art in the comment packet is skipped unread") {
        ScratchDirectory directory("media");
        const std::string picture = "METADATA_BLOCK_PICTURE=" + std::string(300'000, 'Q');
        ByteWriter comments;
        comments.u8(3).text("vorbis").bytes(vorbis_comment_block({picture, "TITLE=Behind Art", "KEY=Dm"})).u8(1);
        const fs::path file = directory.file("art.ogg");
        write_bytes(file, ogg_stream({vorbis_identification(2, 44100), comments.result(), Bytes(30, 0)}, 11, 44100));
        auto probe = probe_file(file);
        REQUIRE(probe.has_value());
        CHECK(probe->codec == "vorbis");
        CHECK(probe->frames == 44100);
        CHECK(probe->tags.title == "Behind Art");
        CHECK(probe->tags.key == MusicalKey{2, KeyMode::Minor});
        CHECK(probe->bytes_read < 128 * 1024);
    }

    TEST_CASE("The Ogg tail scan verifies page checksums, so an OggS pattern in packet data is ignored") {
        ScratchDirectory directory("media");
        // The last packet holds bytes that look like a page header with a different granule.
        ByteWriter decoy;
        decoy.text("OggS").u8(0).u8(4).u64le(999'999).u32le(11).u32le(9).u32le(0x12345678).u8(1).u8(10).zeros(10);
        ByteWriter comments;
        comments.u8(3).text("vorbis").bytes(vorbis_comment_block({})).u8(1);
        const fs::path file = directory.file("decoy.ogg");
        write_bytes(file, ogg_stream({vorbis_identification(1, 48000), comments.result(), decoy.result()}, 11, 4800));
        auto probe = probe_file(file);
        REQUIRE(probe.has_value());
        CHECK(probe->frames == 4800);
        CHECK(probe->duration_exact);
    }

    // ---------------------------------------------------------------- MP4

    TEST_CASE("MP4: large boxes, ALAC, ilst tags and multi-megabyte cover art read within a small budget") {
        ScratchDirectory directory("media");
        const Bytes items =
            concat({ilst_item("\xA9nam", 1, latin1("Alac Loop")), ilst_item("covr", 13, Bytes(3'000'000, 0x42)),
                    ilst_item("tmpo", 21, Bytes{0x00, 0x80}), ilst_item("gnre", 0, Bytes{0x00, 0x12}),
                    ilst_item("\xA9"
                              "cmt",
                              2, Bytes{0x00, 'h', 0x00, 'i'})});
        const fs::path file = directory.file("loop.m4a");
        write_bytes(file, mp4_file("M4A ", alac_entry(16, 2, 44100), 44100, 88200, items, /*large_moov=*/true));
        auto probe = probe_file(file);
        REQUIRE(probe.has_value());
        CHECK_FALSE(probe->partial);
        CHECK(probe->format == "m4a");
        CHECK(probe->codec == "alac");
        CHECK(probe->sample_rate == 44100);
        CHECK(probe->channels == 2);
        CHECK(probe->bit_depth == 16);
        CHECK(probe->frames == 88200);
        CHECK(probe->duration_exact);
        CHECK(probe->tags.title == "Alac Loop");
        CHECK(probe->tags.bpm_milli == 128'000);
        CHECK(probe->tags.genre == "Rock");
        CHECK(probe->tags.comment == "hi");
        CHECK(probe->bytes_read < 16 * 1024);
    }

    TEST_CASE("MP4 QuickTime v1/v2 sample entries, srat rates, explicit HE-AAC and DRM") {
        ScratchDirectory directory("media");
        // Version 2 lpcm entry: float64 rate, 32-bit float flags.
        ByteWriter version2 = sample_entry_prefix(2, 3, 16, 0x00010000);
        version2.u32be(72).u64be(std::bit_cast<std::uint64_t>(96000.0)).u32be(2).u32be(0x7F000000).u32be(32);
        version2.u32be(0x1).u32be(8).u32be(1);
        const fs::path v2 = directory.file("v2.mov");
        write_bytes(v2, mp4_file("qt  ", box("lpcm", version2.result()), 96000, 9600, {}));
        auto v2_probe = probe_file(v2);
        REQUIRE(v2_probe.has_value());
        CHECK(v2_probe->codec == "pcm_f32");
        CHECK(v2_probe->sample_rate == 96000);
        CHECK(v2_probe->channels == 2);
        CHECK(v2_probe->frames == 9600);

        // Version 1 twos entry (16 extra bytes).
        ByteWriter version1 = sample_entry_prefix(1, 1, 16, 22050u << 16);
        version1.u32be(1).u32be(2).u32be(2).u32be(2);
        const fs::path v1 = directory.file("v1.mov");
        write_bytes(v1, mp4_file("qt  ", box("twos", version1.result()), 22050, 2205, {}));
        auto v1_probe = probe_file(v1);
        REQUIRE(v1_probe.has_value());
        CHECK(v1_probe->codec == "pcm_s16");
        CHECK(v1_probe->sample_rate == 22050);

        // FLAC at 192 kHz: the 16.16 field cannot hold the rate, "srat" can.
        ByteWriter high_rate = sample_entry_prefix(0, 2, 24, 0);
        ByteWriter srat;
        srat.u32be(0).u32be(192000);
        high_rate.bytes(box("srat", srat.result()));
        const fs::path flac = directory.file("high.mp4");
        write_bytes(flac, mp4_file("isom", box("fLaC", high_rate.result()), 192000, 1920, {}));
        auto flac_probe = probe_file(flac);
        REQUIRE(flac_probe.has_value());
        CHECK(flac_probe->codec == "flac");
        CHECK(flac_probe->sample_rate == 192000);

        // HE-AAC with explicit SBR signalling: core 22050 Hz, output 44100 Hz.
        ByteWriter descriptor;
        descriptor.u32be(0).u8(0x03).u8(23).u16be(1).u8(0).u8(0x04).u8(18).u8(0x40).u8(0x15).u24be(0).u32be(0);
        descriptor.u32be(0).u8(0x05).u8(3).u8(0x2B).u8(0x92).u8(0x08);
        ByteWriter he_aac = sample_entry_prefix(0, 2, 16, 22050u << 16);
        he_aac.bytes(box("esds", descriptor.result()));
        const fs::path aac = directory.file("he.m4a");
        write_bytes(aac, mp4_file("M4A ", box("mp4a", he_aac.result()), 44100, 44100, {}));
        auto aac_probe = probe_file(aac);
        REQUIRE(aac_probe.has_value());
        CHECK(aac_probe->codec == "aac");
        CHECK(aac_probe->sample_rate == 44100);
        CHECK_FALSE(aac_probe->duration_exact);

        const fs::path protected_file = directory.file("drm.m4p");
        write_bytes(protected_file, mp4_file("M4P ", box("enca", sample_entry_prefix(0, 2, 16, 44100u << 16).result()),
                                             44100, 44100, {}));
        auto drm = probe_file(protected_file);
        REQUIRE_FALSE(drm.has_value());
        CHECK(drm.error().code == ErrorCode::UnsupportedCodec);
    }

    // ---------------------------------------------------------------- ASF, CAF, WavPack

    TEST_CASE("ASF: unknown codecs keep their format tag, DWORD tempos are read, pictures skipped, DRM refused") {
        ScratchDirectory directory("media");
        ByteWriter properties;
        properties.zeros(16).u64le(0).u64le(0).u64le(0).u64le(20'000'000).u64le(0).u64le(1000).u32le(0).u32le(0);
        properties.u32le(0).u32le(0);
        ByteWriter description;
        description.u16le(utf16le("Wma Title").size()).u16le(0).u16le(0).u16le(0).u16le(0).bytes(utf16le("Wma Title"));
        ByteWriter extended;
        extended.u16le(2);
        extended.u16le(utf16le("WM/Picture").size()).bytes(utf16le("WM/Picture")).u16le(1).u16le(60000).zeros(60000);
        extended.u16le(utf16le("WM/BeatsPerMinute").size())
            .bytes(utf16le("WM/BeatsPerMinute"))
            .u16le(3)
            .u16le(4)
            .u32le(124);
        const fs::path file = directory.file("odd.wma");
        write_bytes(file,
                    asf_file({asf_object(kAsfFilePropertiesGuid, properties.result()), asf_stream_properties(0x1234),
                              asf_object(kAsfContentDescriptionGuid, description.result()),
                              asf_object(kAsfExtendedContentGuid, extended.result())}));
        auto probe = probe_file(file);
        REQUIRE(probe.has_value());
        CHECK(probe->format == "wma");
        CHECK(probe->codec == "wave_format_0x1234");
        CHECK(probe->sample_rate == 44100);
        CHECK(probe->duration_us == 1'000'000);  // 2 s play duration minus 1 s preroll
        CHECK(probe->tags.title == "Wma Title");
        CHECK(probe->tags.bpm_milli == 124'000);
        CHECK(probe->bytes_read < 16 * 1024);

        const fs::path drm_file = directory.file("drm.wma");
        write_bytes(drm_file,
                    asf_file({asf_stream_properties(0x0161), asf_object(kAsfContentEncryptionGuid, Bytes(16, 0))}));
        auto drm = probe_file(drm_file);
        REQUIRE_FALSE(drm.has_value());
        CHECK(drm.error().code == ErrorCode::UnsupportedCodec);
    }

    TEST_CASE("CAF linear PCM with an info chunk, and WavPack with a 40-bit length and an APE tag") {
        ScratchDirectory directory("media");
        ByteWriter caf;
        caf.text("caff").u16be(1).u16be(0);
        caf.text("desc").u64be(32).u64be(std::bit_cast<std::uint64_t>(48000.0)).text("lpcm").u32be(0).u32be(4);
        caf.u32be(1).u32be(2).u32be(16);
        const Bytes information = latin1("\x00\x00\x00\x01title\0Caf Title\0"sv);
        caf.text("info").u64be(information.size()).bytes(information);
        caf.text("data").u64be(~0ull).u32be(0).bytes(pcm16_payload(100, 2, true));
        const fs::path caf_file = directory.file("tone.caf");
        write_bytes(caf_file, caf.result());
        auto caf_probe = probe_file(caf_file);
        REQUIRE(caf_probe.has_value());
        CHECK(caf_probe->format == "caf");
        CHECK(caf_probe->codec == "pcm_s16");
        CHECK(caf_probe->sample_rate == 48000);
        CHECK(caf_probe->frames == 100);
        CHECK(caf_probe->tags.title == "Caf Title");

        constexpr std::uint32_t kFlags = 0x1 | 0x800 | 0x1000 | (9u << 23);  // 16-bit, initial+final block, 44.1 kHz
        ByteWriter wavpack;
        wavpack.text("wvpk").u32le(24).u16le(0x410).u8(0).u8(1).u32le(5).u32le(0).u32le(0).u32le(kFlags).u32le(0);
        const fs::path wavpack_file = directory.file("long.wv");
        write_bytes(wavpack_file, concat({wavpack.result(), ape_tag({{"Title", latin1("Pack")}})}));
        auto wavpack_probe = probe_file(wavpack_file);
        REQUIRE(wavpack_probe.has_value());
        CHECK(wavpack_probe->codec == "wavpack");
        CHECK(wavpack_probe->sample_rate == 44100);
        CHECK(wavpack_probe->channels == 2);
        CHECK(wavpack_probe->bit_depth == 16);
        CHECK(wavpack_probe->frames == static_cast<std::int64_t>(5 + (std::uint64_t{1} << 32) - 1));
        CHECK(wavpack_probe->tags.title == "Pack");
    }

    // ---------------------------------------------------------------- identification and errors

    TEST_CASE("The container is identified from its bytes, not its extension") {
        ScratchDirectory directory("media");
        const fs::path disguised = directory.file("actually_wav.mp3");
        write_bytes(disguised,
                    riff_file("RIFF", {{"fmt ", format_chunk(1, 1, 22050, 16)}, {"data", pcm16_payload(10, 1)}}));
        auto probe = probe_file(disguised);
        REQUIRE(probe.has_value());
        CHECK(probe->format == "wav");

        const fs::path text = directory.file("notes.wav");
        write_bytes(text, latin1("this is not audio at all, just some text in a file"));
        auto not_audio = probe_file(text);
        REQUIRE_FALSE(not_audio.has_value());
        CHECK(not_audio.error().code == ErrorCode::UnsupportedCodec);

        const fs::path midi = directory.file("song.mid");
        ByteWriter midi_bytes;
        midi_bytes.text("MThd").u32be(6).u16be(0).u16be(1).u16be(96);
        write_bytes(midi, midi_bytes.result());
        auto midi_probe = probe_file(midi);
        REQUIRE_FALSE(midi_probe.has_value());
        CHECK(midi_probe.error().code == ErrorCode::UnsupportedCodec);

        auto missing = probe_file(directory.file("missing.wav"));
        REQUIRE_FALSE(missing.has_value());
        CHECK(missing.error().code == ErrorCode::FileMissing);
        auto missing_decoder = open_decoder(directory.file("missing.flac"));
        REQUIRE_FALSE(missing_decoder.has_value());
        CHECK(missing_decoder.error().code == ErrorCode::FileMissing);
        auto folder = probe_file(directory.path());
        REQUIRE_FALSE(folder.has_value());
        CHECK(folder.error().code == ErrorCode::UnsupportedCodec);
    }

    TEST_CASE("Unicode file names are probed and decoded through wide-character paths") {
        ScratchDirectory directory("media");
        const fs::path file =
            directory.file("\xD7\x93\xD7\x92\xD7\x99\xD7\x9E\xD7\x94 \xE2\x99\xAF.wav");  // Hebrew + sharp
        write_bytes(file, riff_file("RIFF", {{"fmt ", format_chunk(1, 1, 44100, 16)}, {"data", pcm16_payload(64, 1)}}));
        auto probe = probe_file(file);
        REQUIRE(probe.has_value());
        CHECK(probe->frames == 64);
        auto decoder = open_decoder(file);
        REQUIRE(decoder.has_value());
        CHECK(read_all(**decoder).size() == 64);
    }

    TEST_CASE("A previewing decoder lets other programs rename and delete the file") {
        ScratchDirectory directory("media");
        const fs::path file = directory.file("preview.wav");
        write_bytes(file, small_wav(2000));
        auto decoder = open_decoder(file);
        REQUIRE(decoder.has_value());
        std::error_code error;
        fs::rename(file, directory.file("renamed.wav"), error);
        CHECK_FALSE(error);
        fs::remove(directory.file("renamed.wav"), error);
        CHECK_FALSE(error);
        CHECK(read_all(**decoder).size() == 4000);  // the open handle keeps reading
    }

    TEST_CASE("Probing and decoding leave the source bytes and modification time untouched (invariant 11)") {
        ScratchDirectory directory("media");
        const fs::path wav = directory.file("keep.wav");
        const fs::path mp3 = directory.file("keep.mp3");
        write_bytes(wav, small_wav(500));
        write_bytes(mp3, concat({sample_id3v23_tag(), info_frame(10, 576, 1000), mp3_frames(10)}));
        for (const fs::path& file : {wav, mp3}) {
            CAPTURE(mso::path_to_utf8(file.filename()));
            const FileSnapshot before = snapshot(file);
            REQUIRE(probe_file(file).has_value());
            auto decoder = open_decoder(file);
            REQUIRE(decoder.has_value());
            read_all(**decoder);
            REQUIRE((*decoder)->seek(10).has_value());
            decoder->reset();
            CHECK(snapshot(file) == before);
        }
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
              "Chord Cmaj7", "Note C3", "Kick 060", "Loop 99999999999"}) {
            CAPTURE(stem);
            const FilenameHints hints = filename_hints(stem);
            CHECK_FALSE(hints.bpm_milli.has_value());
            CHECK_FALSE(hints.key.has_value());
        }
    }

    // ---------------------------------------------------------------- FFmpeg-generated fixtures

    TEST_CASE("Encoded fixtures: FLAC, MP3, Ogg Vorbis probe exactly and decode the same frame count" *
              doctest::skip(skip_without_ffmpeg())) {
        REQUIRE_MESSAGE(executable_on_path("ffmpeg"), "MSO_REQUIRE_FFMPEG=1 but ffmpeg is not on PATH");
        ScratchDirectory directory("media");
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
            const FileSnapshot before = snapshot(file);
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
            check_seek_past_end(**decoder);
            decoder->reset();
            CHECK(snapshot(file) == before);  // invariant 11 for encoded formats too
        }
    }

    TEST_CASE("Every page of an FFmpeg-written Ogg file passes the page checksum" *
              doctest::skip(skip_without_ffmpeg())) {
        REQUIRE_MESSAGE(executable_on_path("ffmpeg"), "MSO_REQUIRE_FFMPEG=1 but ffmpeg is not on PATH");
        ScratchDirectory directory("media");
        const fs::path file = directory.file("pages.opus");
        REQUIRE(render_with_ffmpeg(file, "-c:a libopus"));
        const std::string bytes = mso::test::read_file(file);
        const Bytes data(bytes.begin(), bytes.end());
        std::size_t offset = 0;
        int pages = 0;
        while (offset + 27 <= data.size()) {
            REQUIRE(std::string_view(bytes).substr(offset, 4) == "OggS");
            const std::size_t segments = data[offset + 26];
            std::size_t size = 27 + segments;
            for (std::size_t index = 0; index < segments; ++index)
                size += data[offset + 27 + index];
            const auto page = std::span<const std::uint8_t>(data).subspan(offset, size);
            const std::uint32_t stored =
                static_cast<std::uint32_t>(page[22]) | (static_cast<std::uint32_t>(page[23]) << 8) |
                (static_cast<std::uint32_t>(page[24]) << 16) | (static_cast<std::uint32_t>(page[25]) << 24);
            CHECK(stored == detail::ogg_page_checksum(page));
            offset += size;
            ++pages;
        }
        CHECK(offset == data.size());
        CHECK(pages > 2);
    }

    TEST_CASE("The stb_vorbis backend reads through a shared handle and honours the decoder contract" *
              doctest::skip(skip_without_ffmpeg())) {
        REQUIRE_MESSAGE(executable_on_path("ffmpeg"), "MSO_REQUIRE_FFMPEG=1 but ffmpeg is not on PATH");
        ScratchDirectory directory("media");
        const fs::path file = directory.file("stb.ogg");
        REQUIRE(render_with_ffmpeg(file, "-c:a libvorbis"));
        auto probe = probe_file(file);
        REQUIRE(probe.has_value());
        auto decoder = detail::open_vorbis_decoder(file, *probe);
        REQUIRE(decoder.has_value());
        std::error_code error;
        fs::rename(file, directory.file("stb_renamed.ogg"), error);
        CHECK_FALSE(error);  // FILE_SHARE_DELETE on Windows
        CHECK((*decoder)->total_frames() == 44100);
        CHECK(read_all(**decoder).size() == 44100 * 2);
        check_seek_past_end(**decoder);
    }

    TEST_CASE(
        "Encoded fixtures: Opus, AAC, ALAC, WMA, WavPack and CAF probe natively; FFmpeg decodes them when built in" *
        doctest::skip(skip_without_ffmpeg())) {
        REQUIRE_MESSAGE(executable_on_path("ffmpeg"), "MSO_REQUIRE_FFMPEG=1 but ffmpeg is not on PATH");
        ScratchDirectory directory("media");
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
            {"sine.wv", "-c:a wavpack -sample_fmt s16p -metadata title=Sine", 44100, "wv", "wavpack", 44100, 16},
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
            if (fixture.format != "caf") CHECK(probe->tags.title == "Sine");

            auto decoder = open_decoder(file);
            if (!has_capability("aac")) {
                // Built without FFmpeg: compressed formats outside miniaudio are refused clearly.
                if (fixture.codec != "pcm_s16") {
                    REQUIRE_FALSE(decoder.has_value());
                    CHECK(decoder.error().code == ErrorCode::UnsupportedCodec);
                    CHECK(decoder.error().safe_detail.find("not built into this version") != std::string::npos);
                }
                continue;
            }
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
            check_seek_past_end(**decoder);
        }
    }

    TEST_CASE("FFmpeg decoding keeps the opening layout when a stream changes channels or rate part-way" *
              doctest::skip(skip_without_ffmpeg_backend())) {
        REQUIRE_MESSAGE(executable_on_path("ffmpeg"), "MSO_REQUIRE_FFMPEG=1 but ffmpeg is not on PATH");
        REQUIRE_MESSAGE(has_capability("aac"), "MSO_REQUIRE_FFMPEG=1 but the FFmpeg backend is not built in");
        ScratchDirectory directory("media");
        const fs::path mono = directory.file("mono.aac");
        const fs::path stereo = directory.file("stereo.aac");
        const fs::path stereo48 = directory.file("stereo48.aac");
        REQUIRE(render_with_ffmpeg(mono, "-c:a aac -b:a 96k", 44100, 1));
        REQUIRE(render_with_ffmpeg(stereo, "-c:a aac -b:a 128k", 44100, 2));
        REQUIRE(render_with_ffmpeg(stereo48, "-c:a aac -b:a 128k", 48000, 2));
        const auto join = [&](const fs::path& first, const fs::path& second, std::string_view name) {
            const fs::path joined = directory.file(name);
            mso::test::write_file(joined, mso::test::read_file(first) + mso::test::read_file(second));
            return joined;
        };
        struct Case {
            fs::path file;
            int channels;
            int sample_rate;
        };
        const Case cases[] = {
            {join(mono, stereo, "mono_then_stereo.aac"), 1, 44100},
            {join(stereo, mono, "stereo_then_mono.aac"), 2, 44100},
            {join(stereo, stereo48, "rate_change.aac"), 2, 44100},
        };
        for (const Case& test_case : cases) {
            CAPTURE(mso::path_to_utf8(test_case.file.filename()));
            auto decoder = open_decoder(test_case.file);
            REQUIRE_MESSAGE(decoder.has_value(), (decoder ? std::string() : decoder.error().message()));
            CHECK((*decoder)->channels() == test_case.channels);
            CHECK((*decoder)->sample_rate() == test_case.sample_rate);
            const auto samples = read_all(**decoder);  // read_all checks no read exceeds its request
            const auto frames = static_cast<std::int64_t>(samples.size()) / test_case.channels;
            CHECK(std::abs(frames - 2 * test_case.sample_rate) <= 8192);  // two seconds, at the opening rate
            CHECK((*decoder)->channels() == test_case.channels);
        }
    }

    TEST_CASE("Containers without a native parser fall back to FFmpeg probing when it is built in" *
              doctest::skip(skip_without_ffmpeg_backend())) {
        REQUIRE_MESSAGE(executable_on_path("ffmpeg"), "MSO_REQUIRE_FFMPEG=1 but ffmpeg is not on PATH");
        REQUIRE_MESSAGE(has_capability("aac"), "MSO_REQUIRE_FFMPEG=1 but the FFmpeg backend is not built in");
        ScratchDirectory directory("media");
        const fs::path file = directory.file("sine.mka");
        REQUIRE(render_with_ffmpeg(file, "-c:a flac -metadata TITLE=Matroska -metadata BPM=90"));
        auto probe = probe_file(file);
        REQUIRE(probe.has_value());
        CHECK(probe->format == "matroska");
        CHECK(probe->codec == "flac");
        CHECK(probe->sample_rate == 44100);
        CHECK(probe->channels == 2);
        CHECK_FALSE(probe->duration_exact);
        REQUIRE(probe->duration_us.has_value());  // filled by stream analysis
        CHECK(std::abs(*probe->duration_us - 1'000'000) < 50'000);
        CHECK(probe->tags.title == "Matroska");
        CHECK(probe->tags.bpm_milli == 90'000);
        auto decoder = open_decoder(file);
        REQUIRE(decoder.has_value());
        CHECK(read_all(**decoder).size() == 44100 * 2);

        auto starved = probe_file(file, ProbeLimits{2048, std::chrono::milliseconds(2000)});
        REQUIRE(starved.has_value());
        CHECK(starved->partial);  // a read the budget refused is never reported as a complete probe
        CHECK(starved->bytes_read <= 2048);
    }

    // ---------------------------------------------------------------- second review round

    TEST_CASE("A sparse Vorbis codebook that fails part-way leaks nothing, however often it is opened" *
              doctest::skip(memory_not_measurable())) {
        // patches/miniaudio/0001: a sparse codebook's code lengths live in temporary memory that
        // vorbis_deinit never frees; failing on the first entry used to leak `entries` bytes (4 MiB
        // here) on every open, so previewing such a file again and again grew the app without bound.
        constexpr std::uint32_t kEntries = 1u << 22;  // the most libvorbis allows with one dimension
        BitWriter setup;
        setup.bits(0, 8);                                 // one codebook
        setup.bits(0x42, 8).bits(0x43, 8).bits(0x56, 8);  // "BCV" sync pattern
        setup.bits(1, 16).bits(kEntries, 24);             // dimensions, entries
        setup.bits(0, 1).bits(1, 1);                      // not ordered, sparse
        setup.bits(1, 1).bits(31, 5);                     // entry 0: length 32, invalid
        ByteWriter setup_packet;
        setup_packet.u8(5).text("vorbis").bytes(setup.result()).zeros(8);
        ByteWriter comment_packet;
        comment_packet.u8(3).text("vorbis").bytes(vorbis_comment_block({})).u8(1);
        ScratchDirectory directory("media");
        const fs::path file = directory.file("leaky_codebook.ogg");
        write_bytes(file, ogg_stream({vorbis_identification(2, 44100), comment_packet.result(), setup_packet.result()},
                                     9, 4410));

        constexpr int kOpens = 200;  // a leak would be 800 MiB
        const auto open_once = [&file] {
            auto decoder = detail::open_vorbis_decoder(file, ProbeResult{});
            REQUIRE_FALSE(decoder.has_value());
            CHECK(decoder.error().code == ErrorCode::CorruptAudio);
        };
        open_once();  // first use: allocator and C runtime setup are not part of the measurement
        const std::uint64_t before = committed_bytes().value_or(0);
        for (int open = 0; open < kOpens; ++open)
            open_once();
        const std::uint64_t after = committed_bytes().value_or(0);
        const std::uint64_t growth = after > before ? after - before : 0;
        CAPTURE(growth);
        // Generous: allocator caches (and AddressSanitizer's quarantine of freed blocks) may keep
        // freed memory committed, but never the 800 MiB a leak would cost.
        CHECK(growth < std::uint64_t{kOpens} * kEntries / 2);
    }

    TEST_CASE("A Vorbis comment header with an overflowing count or length fails cleanly in stb_vorbis") {
        // patches/miniaudio/0001: setup_malloc() used to truncate sizeof(char*) * count to int, so
        // 0x20000001 comments got an 8-byte list (heap overflow) and 0x10000000 a NULL list that
        // vorbis_deinit() then walked. Each crafted header must now be refused without a crash
        // (the sanitize preset turns any overrun or undefined shift into a failure).
        const auto comment_packet = [](std::uint32_t vendor_length, std::uint32_t count, const Bytes& rest) {
            ByteWriter packet;
            packet.u8(3).text("vorbis").u32le(vendor_length);
            if (vendor_length < 64) packet.fill(vendor_length, 'v');
            packet.u32le(count).bytes(rest).u8(1);
            return packet.result();
        };
        ByteWriter short_entry;
        short_entry.u32le(5000).text("TITLE=cut short");  // claims 5000 bytes; the packet ends first
        struct Hostile {
            std::string_view name;
            Bytes comments;
        };
        const Hostile cases[] = {
            {"list size wraps to 8 bytes", comment_packet(4, 0x20000001u, {})},
            {"list size turns negative", comment_packet(4, 0x10000000u, {})},
            {"count above INT_MAX", comment_packet(4, 0x80000001u, {})},
            {"vendor length above INT_MAX", comment_packet(0xFFFFFFF0u, 0, {})},
            {"entry longer than its packet", comment_packet(4, 1, short_entry.result())},
            // Fuzzer finds: lengths far beyond the file used to be allocated before any byte was read.
            {"1.8 GB entry in a 200-byte file", comment_packet(4, 1, Bytes{0x00, 0x00, 0x75, 0x6C})},
            {"16 million entries in a 200-byte file", comment_packet(4, 0x00FFFFFFu, {})},
            {"packet ends inside the comment count", Bytes{3, 'v', 'o', 'r', 'b', 'i', 's', 0, 0, 0, 0, 0x01}},
        };
        ScratchDirectory directory("media");
        for (const Hostile& hostile : cases) {
            CAPTURE(hostile.name);
            const fs::path file = directory.file("hostile.ogg");
            write_bytes(file,
                        ogg_stream({vorbis_identification(2, 44100), hostile.comments, Bytes(64, 0x05)}, 7, 4410));
            auto decoder = detail::open_vorbis_decoder(file, ProbeResult{});
            REQUIRE_FALSE(decoder.has_value());
            CHECK(decoder.error().code == ErrorCode::CorruptAudio);
            // Through the public entry points too (FFmpeg, when built in, gets the file next).
            const auto probe = probe_file(file);
            CHECK((probe.has_value() || probe.error().code != ErrorCode::Internal));
            if (auto opened = open_decoder(file)) {
                std::vector<float> buffer(4096);
                const auto read = (*opened)->read(buffer);
                CHECK((read.has_value() || read.error().code != ErrorCode::Internal));
            }
        }
    }

    TEST_CASE("probe_memory runs the same parsers over a buffer as probe_file over the file") {
        ScratchDirectory directory("media");
        const Bytes wav = small_wav(48);
        const fs::path file = directory.file("tone.wav");
        write_bytes(file, wav);
        auto from_file = probe_file(file);
        auto from_memory = probe_memory(wav, "tone.wav");
        REQUIRE(from_file.has_value());
        REQUIRE(from_memory.has_value());
        CHECK(from_memory->format == from_file->format);
        CHECK(from_memory->codec == from_file->codec);
        CHECK(from_memory->frames == from_file->frames);
        CHECK(from_memory->duration_us == from_file->duration_us);
        CHECK(from_memory->bytes_read == from_file->bytes_read);

        // The hint supplies the extension: junk before an MP3 frame is only scanned for an .mp3.
        const Bytes junk_then_mp3 = concat({Bytes(100, 0x11), mp3_frames(4)});
        auto as_mp3 = probe_memory(junk_then_mp3, "beat.mp3");
        REQUIRE(as_mp3.has_value());
        CHECK(as_mp3->format == "mp3");
        CHECK(probe_memory(junk_then_mp3, "beat.bin").error().code == ErrorCode::UnsupportedCodec);

        CHECK(probe_memory({}, "empty.wav").error().code == ErrorCode::CorruptAudio);
        auto starved = probe_memory(wav, "tone.wav", ProbeLimits{20, std::chrono::milliseconds(2000)});
        REQUIRE(starved.has_value());
        CHECK(starved->partial);
        CHECK(starved->bytes_read <= 20);
    }

    TEST_CASE("BoundedFileReader's read-ahead window returns the file's bytes and charges only what it hands out") {
        ScratchDirectory directory("media");
        Bytes content(3 * detail::BoundedFileReader::kReadAheadBytes + 123);
        for (std::size_t index = 0; index < content.size(); ++index)
            content[index] = static_cast<std::uint8_t>(index % 251);
        const fs::path file = directory.file("pattern.bin");
        write_bytes(file, content);
        auto reader = detail::BoundedFileReader::open(file, ProbeLimits{1'000'000, std::chrono::milliseconds(5000)});
        REQUIRE(reader.has_value());

        std::mt19937 random(1234);
        std::uint64_t charged = 0;
        for (int read = 0; read < 400; ++read) {
            // Mostly small header-sized reads, forwards and backwards, sometimes past the end.
            const std::uint64_t offset = random() % (content.size() + 16);
            const std::size_t length = read % 50 == 0 ? 20'000 : 1 + random() % 64;
            CAPTURE(offset);
            CAPTURE(length);
            auto bytes = reader->read_available(offset, length);
            REQUIRE(bytes.has_value());
            const std::size_t expected =
                offset < content.size() ? std::min<std::size_t>(length, content.size() - offset) : 0;
            REQUIRE(bytes->size() == expected);
            CHECK(std::equal(bytes->begin(), bytes->end(), content.begin() + static_cast<std::ptrdiff_t>(offset)));
            charged += expected;
        }
        CHECK(reader->bytes_read() == charged);

        std::vector<std::uint8_t> destination(100);
        auto copied = reader->read_into(content.size() - 40, destination);
        REQUIRE(copied.has_value());
        CHECK(*copied == 40);
        CHECK(std::equal(destination.begin(), destination.begin() + 40, content.end() - 40));

        // The window never fetches more than the budget could hand out, and a tight budget still works.
        auto tight = detail::BoundedFileReader::open(file, ProbeLimits{10, std::chrono::milliseconds(5000)});
        REQUIRE(tight.has_value());
        auto first = tight->read_exact(5, 10);
        REQUIRE(first.has_value());
        CHECK((*first)[0] == content[5]);
        CHECK(tight->read_exact(15, 1).error().code == ErrorCode::ProbeLimit);
    }

    TEST_CASE("native_path judges a relative path by the absolute path it resolves to") {
        ScratchDirectory directory("media");
        // A relative path well under the classic limit whose absolute form is past it.
        fs::path relative = "nested";
        while (mso::path_to_utf8(fs::absolute(directory.path() / relative)).size() < 300)
            relative /= "level-of-a-long-folder";
        REQUIRE(mso::path_to_utf8(relative).size() < 240);
        std::error_code error;
        fs::create_directories(native_path(directory.path() / relative), error);
        REQUIRE_FALSE(error);
        write_bytes(directory.file("short.wav"), small_wav(16));
        fs::rename(native_path(directory.file("short.wav")), native_path(directory.path() / relative / "tone.wav"));

        const fs::path previous = fs::current_path();
        fs::current_path(directory.path());
        struct RestoreCurrentPath {
            const fs::path& path;
            ~RestoreCurrentPath() {
                std::error_code ignored;
                fs::current_path(path, ignored);
            }
        } restore{previous};
        const fs::path file = relative / "tone.wav";
#ifdef _WIN32
        CHECK(native_path(file).native().starts_with(LR"(\\?\)"));
#else
        CHECK(native_path(file) == file);
#endif
        CHECK(native_path(fs::path("short.wav")) == fs::path("short.wav"));
        auto probe = probe_file(file);
        REQUIRE(probe.has_value());
        CHECK(probe->frames == 16);
        fs::remove_all(native_path(directory.path() / "nested"), error);
    }

    TEST_CASE("MP3 gapless trim never ends inside the decoder delay, so the count matches what dr_mp3 delivers") {
        ScratchDirectory directory("media");
        const fs::path file = directory.file("short_padding.mp3");
        // LAME states 100 frames of end padding, fewer than the 529-frame decoder delay.
        write_bytes(file, concat({info_frame(10, 576, 100), mp3_frames(10)}));
        auto probe = probe_file(file);
        REQUIRE(probe.has_value());
        CHECK(probe->frames == 10 * 1152 - 576 - 529);
        CHECK(probe->duration_exact);
        auto decoder = open_decoder(file);
        REQUIRE(decoder.has_value());
        CHECK((*decoder)->frames_exact());
        CHECK((*decoder)->total_frames() == probe->frames);
        CHECK(static_cast<std::int64_t>(read_all(**decoder).size()) == *probe->frames * 2);
    }

    TEST_CASE("WavPack: custom rates and multichannel layouts from metadata sub-blocks, ID3v1-only tags") {
        ScratchDirectory directory("media");
        // 16-bit, initial block only (the channels continue in later blocks), rate index 15 (custom).
        constexpr std::uint32_t kFlags = 0x1 | 0x800 | (15u << 23);
        ByteWriter sub_blocks;
        sub_blocks.u8(0x0D).u8(1).u8(6).u8(0x3F);                           // ID_CHANNEL_INFO: 6 channels, mask
        sub_blocks.u8(0x27 | 0x40).u8(2).u8(0xA8).u8(0x93).u8(0x00).u8(0);  // ID_SAMPLE_RATE 37800, odd size
        sub_blocks.u8(0x0A).u8(2).zeros(4);                                 // ID_WV_BITSTREAM: audio follows
        ByteWriter block;
        block.text("wvpk").u32le(24 + sub_blocks.size()).u16le(0x410).u8(0).u8(0).u32le(37800).u32le(0).u32le(37800);
        block.u32le(kFlags).u32le(0).bytes(sub_blocks.result());
        const fs::path file = directory.file("surround.wv");
        write_bytes(file, concat({block.result(), id3v1_tag("Surround", "", 0, 0)}));
        auto probe = probe_file(file);
        REQUIRE(probe.has_value());
        CHECK(probe->codec == "wavpack");
        CHECK(probe->sample_rate == 37800);
        CHECK(probe->channels == 6);
        CHECK(probe->frames == 37800);
        CHECK(probe->duration_us == 1'000'000);
        CHECK(probe->duration_exact);
        CHECK(probe->tags.title == "Surround");
    }

    TEST_CASE("ASF linear PCM is named in the pcm_sNN vocabulary") {
        ScratchDirectory directory("media");
        const fs::path file = directory.file("pcm.wma");
        write_bytes(file, asf_file({asf_stream_properties(0x0001)}));
        auto probe = probe_file(file);
        REQUIRE(probe.has_value());
        CHECK(probe->codec == "pcm_s16");
        CHECK(probe->bit_depth == 16);
    }

}  // TEST_SUITE("media")
