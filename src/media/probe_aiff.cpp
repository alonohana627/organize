// AIFF / AIFF-C metadata probe.
#include <algorithm>
#include <map>

#include "mso/domain/core.hpp"
#include "mso/media/detail/id3.hpp"
#include "mso/media/detail/probe_formats.hpp"
#include "mso/media/detail/tag_mapping.hpp"
#include "mso/media/detail/text_decoding.hpp"

namespace mso::media::detail {

namespace {

constexpr std::size_t kFormHeaderSize = 12;
constexpr std::size_t kChunkHeaderSize = 8;
constexpr std::size_t kMaxCommonChunkBytes = 512;
constexpr std::size_t kMaxMarkerChunkBytes = 64 * 1024;
constexpr std::size_t kInstrumentChunkBytes = 20;
constexpr std::int64_t kImaFramesPerPacket = 64;

/// Converts an IEEE 754 80-bit extended float (AIFF sample rate) to the nearest integer;
/// nullopt for negative, zero, sub-1 or huge values.
std::optional<std::int64_t> extended_to_integer(Bytes ten_bytes) {
    ByteCursor cursor(ten_bytes);
    const std::uint16_t sign_and_exponent = cursor.u16_be();
    const std::uint64_t mantissa = cursor.u64_be();
    if (!cursor.ok() || (sign_and_exponent & 0x8000) || mantissa == 0) return std::nullopt;
    // value = mantissa * 2^(exponent - 16383 - 63), with an explicit integer bit in the mantissa.
    const int shift = 16383 + 63 - (sign_and_exponent & 0x7FFF);
    if (shift < 1 || shift > 63) return std::nullopt;
    const std::uint64_t rounded = (mantissa >> shift) + ((mantissa >> (shift - 1)) & 1);
    return static_cast<std::int64_t>(rounded);
}

/// The AIFF-C compression types this probe can name, and what they mean.
struct CompressionType {
    std::string_view fourcc;
    std::string_view codec;  ///< empty: linear PCM named from the sample size
    bool is_float = false;
    bool is_unsigned = false;
    std::int64_t fixed_bits = 0;  ///< sample width implied by the type; 0 when sampleSize decides
};

constexpr CompressionType kCompressionTypes[] = {
    {"NONE", ""},
    {"twos", ""},
    {"sowt", ""},
    {"raw ", "", false, true},
    {"fl32", "", true},
    {"FL32", "", true},
    {"fl64", "", true},
    {"FL64", "", true},
    {"alaw", "pcm_alaw"},
    {"ALAW", "pcm_alaw"},
    {"ulaw", "pcm_mulaw"},
    {"ULAW", "pcm_mulaw"},
    {"ima4", "adpcm_ima_qt"},
    // QuickTime fixed-width integer PCM; the bit width comes from the type, not sampleSize.
    {"in24", "", false, false, 24},
    {"in32", "", false, false, 32},
    {"23ni", "", false, false, 24},  // little-endian in24
    {"42ni", "", false, false, 32},  // little-endian in32
};

/// Parses the AIFF/AIFF-C chunk list into the probe result.
class AiffParser {
public:
    AiffParser(BoundedFileReader& reader, ProbeResult& result, bool is_compressed_form)
        : reader_(reader), result_(result), is_compressed_form_(is_compressed_form) {}

    /// Walks every chunk, then checks that the essential COMM and SSND chunks were present.
    Result<void> run() {
        const Result<void> walk = walk_chunks();
        resolve_loops();
        if (!walk) return walk;
        if (!result_.sample_rate) return fail(ErrorCode::CorruptAudio, "AIFF file has no COMM chunk");
        if (!has_sound_data_) return fail(ErrorCode::CorruptAudio, "AIFF file has no SSND chunk");
        return {};
    }

private:
    /// Visits chunks in file order with header-sized reads (at most kMaxChunkCount).
    Result<void> walk_chunks() {
        std::uint64_t offset = kFormHeaderSize;
        for (int count = 0; count < kMaxChunkCount; ++count) {
            if (reader_.file_size() - std::min(offset, reader_.file_size()) < kChunkHeaderSize) break;
            auto header = reader_.read_exact(offset, kChunkHeaderSize);
            if (!header) return std::unexpected(header.error());
            ByteCursor cursor(*header);
            const std::string id = cursor.fourcc();
            const std::uint64_t size = cursor.u32_be();
            const std::uint64_t payload_offset = offset + kChunkHeaderSize;
            if (auto visited = visit(id, payload_offset, size); !visited) return visited;
            offset = payload_offset + size + (size % 2);  // 32-bit sizes cannot overflow 64 bits
        }
        return {};
    }

    /// Notes the sound data, or loads and parses a metadata chunk this probe reads.
    Result<void> visit(std::string_view id, std::uint64_t payload_offset, std::uint64_t size) {
        if (id == "SSND") {
            has_sound_data_ = true;
            return {};
        }
        if (id == "ID3 " || id == "id3 ") {
            // Walked frame by frame in the file: embedded pictures are skipped unread.
            return read_id3v2(reader_, payload_offset, size, result_.tags);
        }
        std::uint64_t limit = 0;
        if (id == "COMM")
            limit = kMaxCommonChunkBytes;
        else if (id == "MARK")
            limit = kMaxMarkerChunkBytes;
        else if (id == "INST")
            limit = kInstrumentChunkBytes;
        else if (id == "NAME" || id == "AUTH" || id == "ANNO")
            limit = kMaxTextItemBytes;
        else
            return {};

        auto payload = reader_.read_available(payload_offset, static_cast<std::size_t>(std::min(size, limit)));
        if (!payload) return std::unexpected(payload.error());
        if (id == "COMM") return parse_common(*payload);
        if (id == "MARK")
            parse_markers(*payload);
        else if (id == "INST")
            parse_instrument(*payload);
        else if (id == "NAME")
            store_tag(result_.tags, TagField::Title, decode_legacy_text(*payload));
        else if (id == "AUTH")
            store_tag(result_.tags, TagField::Artist, decode_legacy_text(*payload));
        else
            store_tag(result_.tags, TagField::Comment, decode_legacy_text(*payload));
        return {};
    }

    /// COMM: channels, frame count, sample size, 80-bit rate and (AIFF-C) compression type.
    Result<void> parse_common(Bytes payload) {
        ByteCursor cursor(payload);
        const std::int64_t channels = cursor.u16_be();
        const std::uint32_t frame_count = cursor.u32_be();
        const std::int64_t sample_size = cursor.u16_be();
        const auto sample_rate = extended_to_integer(cursor.take(10));
        std::string compression = "NONE";
        if (is_compressed_form_) compression = cursor.fourcc();
        if (!cursor.ok()) return fail(ErrorCode::CorruptAudio, "AIFF COMM chunk is too short");
        if (!sample_rate) return fail(ErrorCode::CorruptAudio, "invalid AIFF sample rate");
        if (auto layout = set_stream_layout(result_, channels, *sample_rate); !layout) return layout;

        const auto known = std::ranges::find(kCompressionTypes, compression, &CompressionType::fourcc);
        if (known == std::ranges::end(kCompressionTypes)) {
            result_.codec = to_lower_ascii(trim(compression));  // e.g. "sdx2", "mac3": recognised, not decodable
            return {};
        }
        if (!known->codec.empty()) {
            result_.codec = std::string(known->codec);
            const std::int64_t frames_per_packet = compression == "ima4" ? kImaFramesPerPacket : 1;
            result_.frames = static_cast<std::int64_t>(frame_count) * frames_per_packet;
            return {};
        }
        // Float compression types fix the sample width; some writers leave sampleSize at 0.
        const bool is_double = compression == "fl64" || compression == "FL64";
        std::int64_t bits = known->is_float ? (is_double ? 64 : 32) : sample_size;
        if (known->fixed_bits > 0) bits = known->fixed_bits;
        const auto codec = pcm_codec_name(bits, known->is_float, known->is_unsigned);
        if (!codec) return fail(ErrorCode::CorruptAudio, "unsupported AIFF sample size");
        result_.codec = *codec;
        result_.float_samples = known->is_float;
        result_.bit_depth = bits;
        result_.frames = frame_count;
        return {};
    }

    /// MARK: marker id -> frame position. Names are Pascal strings padded to an even length.
    void parse_markers(Bytes payload) {
        ByteCursor cursor(payload);
        const std::uint16_t marker_count = cursor.u16_be();
        for (std::uint16_t index = 0; index < marker_count && cursor.has(7); ++index) {
            const std::uint16_t id = cursor.u16_be();
            const std::uint32_t position = cursor.u32_be();
            const std::uint8_t name_length = cursor.u8();
            cursor.skip(name_length + ((name_length + 1) % 2));
            if (cursor.ok()) marker_positions_[id] = position;
        }
    }

    /// INST: sustain and release loops as (play mode, begin marker, end marker) triples.
    void parse_instrument(Bytes payload) {
        ByteCursor cursor(payload);
        cursor.skip(8);  // base note, detune, note and velocity range, gain
        for (int loop = 0; loop < 2 && cursor.has(6); ++loop) {
            const std::uint16_t play_mode = cursor.u16_be();
            const std::uint16_t begin_marker = cursor.u16_be();
            const std::uint16_t end_marker = cursor.u16_be();
            if (play_mode != 0) instrument_loops_.emplace_back(begin_marker, end_marker);
        }
    }

    /// Loops reference markers, which may appear in either order relative to INST.
    void resolve_loops() {
        for (const auto& [begin_marker, end_marker] : instrument_loops_) {
            const auto begin = marker_positions_.find(begin_marker);
            const auto end = marker_positions_.find(end_marker);
            if (begin == marker_positions_.end() || end == marker_positions_.end()) continue;
            result_.loops.push_back({begin->second, end->second});
        }
    }

    BoundedFileReader& reader_;
    ProbeResult& result_;
    bool is_compressed_form_;
    bool has_sound_data_ = false;
    std::map<std::uint16_t, std::uint32_t> marker_positions_;
    std::vector<std::pair<std::uint16_t, std::uint16_t>> instrument_loops_;
};

}  // namespace

Result<void> probe_aiff(BoundedFileReader& reader, Bytes signature, ProbeResult& result) {
    const bool is_compressed_form = has_magic(signature, 8, "AIFC");
    result.format = is_compressed_form ? "aifc" : "aiff";
    return AiffParser(reader, result, is_compressed_form).run();
}

}  // namespace mso::media::detail
