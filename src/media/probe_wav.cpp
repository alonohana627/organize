// RIFF/WAVE, RF64/BW64 and Wave64 metadata probe.
#include <algorithm>
#include <bit>
#include <format>

#include "mso/media/id3.hpp"
#include "mso/media/probe_formats.hpp"
#include "mso/media/tag_mapping.hpp"
#include "mso/media/text_decoding.hpp"

namespace mso::media::detail {

namespace {

constexpr std::uint16_t kFormatPcm = 0x0001;
constexpr std::uint16_t kFormatAdpcmMicrosoft = 0x0002;
constexpr std::uint16_t kFormatIeeeFloat = 0x0003;
constexpr std::uint16_t kFormatAlaw = 0x0006;
constexpr std::uint16_t kFormatMulaw = 0x0007;
constexpr std::uint16_t kFormatAdpcmIma = 0x0011;
constexpr std::uint16_t kFormatMpeg = 0x0050;
constexpr std::uint16_t kFormatMpegLayer3 = 0x0055;
constexpr std::uint16_t kFormatExtensible = 0xFFFE;

constexpr std::uint32_t kUnknownChunkSize = 0xFFFFFFFF;  // streaming writers and RF64 placeholders
constexpr std::uint32_t kAcidOneShotFlag = 0x01;
constexpr std::size_t kMaxFormatChunkBytes = 256;
constexpr std::size_t kMaxSamplerLoops = 64;
constexpr std::size_t kSamplerHeaderBytes = 36;
constexpr std::size_t kSamplerLoopBytes = 24;
constexpr std::size_t kRiffHeaderSize = 12;
constexpr std::size_t kWave64HeaderSize = 40;

/// Trailing 12 bytes shared by the Wave64 GUIDs of standard chunks ("fmt ", "data", "fact", ...).
constexpr std::uint8_t kWave64GuidSuffix[12] = {0xF3, 0xAC, 0xD3, 0x11, 0x8C, 0xD1, 0x00, 0xC0, 0x4F, 0x8E, 0xDB, 0x8A};

/// Fields of a "fmt " chunk (WAVEFORMATEX, with WAVEFORMATEXTENSIBLE resolved to its sub-format).
struct WaveFormat {
    std::uint16_t format_tag = 0;
    std::uint16_t channels = 0;
    std::uint32_t sample_rate = 0;
    std::uint16_t block_align = 0;
    std::uint16_t bits_per_sample = 0;
    std::uint16_t valid_bits = 0;  // extensible only; 0 when absent
};

Result<WaveFormat> parse_wave_format(Bytes payload) {
    ByteCursor cursor(payload);
    WaveFormat format;
    format.format_tag = cursor.u16_le();
    format.channels = cursor.u16_le();
    format.sample_rate = cursor.u32_le();
    cursor.skip(4);  // average bytes per second
    format.block_align = cursor.u16_le();
    if (!cursor.ok()) return fail(ErrorCode::CorruptAudio, "WAVE fmt chunk is too short");
    if (cursor.has(2)) format.bits_per_sample = cursor.u16_le();
    if (format.format_tag == kFormatExtensible && cursor.has(2 + 22)) {
        const std::uint16_t extension_size = cursor.u16_le();
        if (extension_size >= 22) {
            format.valid_bits = cursor.u16_le();
            cursor.skip(4);                       // channel mask
            format.format_tag = cursor.u16_le();  // first two bytes of the sub-format GUID
        }
    }
    return format;
}

/// Codec name for WAVE format tags that are not linear PCM or float.
std::string compressed_codec_name(std::uint16_t format_tag) {
    switch (format_tag) {
        case kFormatAlaw:
            return "pcm_alaw";
        case kFormatMulaw:
            return "pcm_mulaw";
        case kFormatAdpcmMicrosoft:
            return "adpcm_ms";
        case kFormatAdpcmIma:
            return "adpcm_ima";
        case kFormatMpeg:
            return "mp2";
        case kFormatMpegLayer3:
            return "mp3";
        default:
            return std::format("wave_format_0x{:04x}", format_tag);
    }
}

/// Records codec, layout and bit depth from the fmt chunk.
Result<void> apply_wave_format(WaveFormat& format, ProbeResult& result) {
    if (auto layout = set_stream_layout(result, format.channels, format.sample_rate); !layout) return layout;
    const bool is_float = format.format_tag == kFormatIeeeFloat;
    if (format.format_tag != kFormatPcm && !is_float) {
        result.codec = compressed_codec_name(format.format_tag);
        return {};
    }
    const auto codec = pcm_codec_name(format.bits_per_sample, is_float, format.bits_per_sample <= 8);
    if (!codec) return fail(ErrorCode::CorruptAudio, "unsupported PCM sample size in WAVE fmt chunk");
    result.codec = *codec;
    result.float_samples = is_float;
    const bool valid_bits_meaningful = format.valid_bits > 0 && format.valid_bits <= format.bits_per_sample;
    result.bit_depth = valid_bits_meaningful ? format.valid_bits : format.bits_per_sample;
    const auto minimum_block = static_cast<std::uint16_t>(format.channels * ((format.bits_per_sample + 7) / 8));
    if (format.block_align < minimum_block) format.block_align = minimum_block;
    return {};
}

/// RIFF LIST/INFO text items.
void parse_info_list(Bytes payload, EmbeddedTags& tags) {
    ByteCursor cursor(payload);
    if (cursor.fourcc() != "INFO") return;
    while (cursor.has(8)) {
        const std::string id = cursor.fourcc();
        const std::uint32_t size = cursor.u32_le();
        const Bytes value = cursor.take(std::min<std::uint64_t>(size, cursor.remaining()));
        if (size % 2 != 0) cursor.skip(1);
        const std::string text = decode_legacy_text(value);
        if (id == "INAM")
            store_tag(tags, TagField::Title, text);
        else if (id == "IART")
            store_tag(tags, TagField::Artist, text);
        else if (id == "IPRD")
            store_tag(tags, TagField::Album, text);
        else if (id == "IGNR")
            store_tag(tags, TagField::Genre, text);
        else if (id == "ICMT")
            store_tag(tags, TagField::Comment, text);
        else if (id == "IKEY")
            store_tag(tags, TagField::Keywords, text);
    }
}

/// Sony ACID loop metadata: the tempo of looped (not one-shot) material.
void parse_acid(Bytes payload, EmbeddedTags& tags) {
    ByteCursor cursor(payload);
    const std::uint32_t flags = cursor.u32_le();
    cursor.skip(2 + 2 + 4 + 4 + 2 + 2);  // root note, reserved, reserved, beats, meter
    const float tempo = std::bit_cast<float>(cursor.u32_le());
    if (!cursor.ok() || (flags & kAcidOneShotFlag) || tags.bpm_milli) return;
    tags.bpm_milli = milli_bpm_from_value(tempo);
}

/// "smpl" sampler loops. Their end offsets are inclusive; LoopPoint ends are exclusive.
void parse_sampler_loops(Bytes payload, std::vector<LoopPoint>& loops) {
    ByteCursor cursor(payload);
    cursor.skip(28);  // manufacturer, product, period, unity note, pitch fraction, SMPTE format/offset
    const std::uint32_t loop_count = cursor.u32_le();
    cursor.skip(4);  // sampler-specific data size
    for (std::uint32_t index = 0; index < loop_count && index < kMaxSamplerLoops && cursor.has(kSamplerLoopBytes);
         ++index) {
        cursor.skip(8);  // cue point id, loop type
        const std::uint32_t start = cursor.u32_le();
        const std::uint32_t end = cursor.u32_le();
        cursor.skip(8);  // fraction, play count
        loops.push_back({start, static_cast<std::int64_t>(end) + 1});
    }
}

/// A chunk located by the walker. Payload bounds are as declared; they may pass the end of a
/// truncated file.
struct WaveChunk {
    std::string id;
    std::uint64_t payload_offset = 0;
    std::uint64_t payload_size = 0;
};

/// RIFF-family chunk headers come in two layouts.
enum class ChunkLayout {
    Riff,    ///< 4-character id, 32-bit payload size, 2-byte alignment (RIFF, RF64, BW64)
    Wave64,  ///< 16-byte GUID id, 64-bit size including the 24-byte header, 8-byte alignment
};

/// Walks the chunks of one WAVE-family file and derives the probe result from them.
class WaveParser {
public:
    WaveParser(BoundedFileReader& reader, ProbeResult& result, ChunkLayout layout)
        : reader_(reader), result_(result), layout_(layout) {}

    Result<void> run() {
        const Result<void> walk = walk_chunks();
        derive_frame_count();
        if (!walk) return walk;
        if (!format_) return fail(ErrorCode::CorruptAudio, "WAVE file has no fmt chunk");
        if (!data_offset_) return fail(ErrorCode::CorruptAudio, "WAVE file has no data chunk");
        return {};
    }

private:
    std::size_t header_size() const noexcept { return layout_ == ChunkLayout::Riff ? 8 : 24; }

    Result<void> walk_chunks() {
        std::uint64_t offset = layout_ == ChunkLayout::Riff ? kRiffHeaderSize : kWave64HeaderSize;
        for (int count = 0; count < kMaxChunkCount; ++count) {
            if (reader_.file_size() - std::min(offset, reader_.file_size()) < header_size()) break;
            auto header = reader_.read_exact(offset, header_size());
            if (!header) return std::unexpected(header.error());
            const auto chunk = decode_chunk_header(*header, offset);
            if (!chunk) break;
            if (auto visited = visit(*chunk); !visited) return visited;
            const auto next = next_chunk_offset(*chunk);
            if (!next || *next <= offset) break;
            offset = *next;
        }
        return {};
    }

    std::optional<WaveChunk> decode_chunk_header(Bytes header, std::uint64_t offset) const {
        ByteCursor cursor(header);
        WaveChunk chunk;
        chunk.payload_offset = offset + header_size();
        if (layout_ == ChunkLayout::Riff) {
            chunk.id = cursor.fourcc();
            chunk.payload_size = cursor.u32_le();
            if (chunk.id == "data" && chunk.payload_size == kUnknownChunkSize && rf64_data_size_) {
                chunk.payload_size = *rf64_data_size_;
            }
            return chunk;
        }
        const Bytes guid = cursor.take(16);
        const std::uint64_t size_with_header = cursor.u64_le();
        if (size_with_header < header_size()) return std::nullopt;
        if (std::ranges::equal(guid.subspan(4), kWave64GuidSuffix)) chunk.id = std::string(as_text(guid.first(4)));
        chunk.payload_size = size_with_header - header_size();
        return chunk;
    }

    std::optional<std::uint64_t> next_chunk_offset(const WaveChunk& chunk) const {
        const std::uint64_t alignment = layout_ == ChunkLayout::Riff ? 2 : 8;
        const std::uint64_t padding = (alignment - chunk.payload_size % alignment) % alignment;
        const auto end = checked_add(chunk.payload_offset, chunk.payload_size);
        return end ? checked_add(*end, padding) : std::nullopt;
    }

    /// Loads at most `limit` bytes of a chunk payload (less if the file ends first).
    Result<std::vector<std::uint8_t>> load(const WaveChunk& chunk, std::uint64_t limit) {
        return reader_.read_available(chunk.payload_offset,
                                      static_cast<std::size_t>(std::min(chunk.payload_size, limit)));
    }

    Result<void> visit(const WaveChunk& chunk) {
        if (chunk.id == "data") {
            data_offset_ = chunk.payload_offset;
            data_size_ = chunk.payload_size;
            return {};
        }
        const auto handler = payload_handler(chunk.id);
        if (!handler) return {};
        auto payload = load(chunk, handler->byte_limit);
        if (!payload) return std::unexpected(payload.error());
        return (this->*(handler->parse))(*payload);
    }

    /// How to read one kind of metadata chunk.
    struct PayloadHandler {
        std::uint64_t byte_limit;
        Result<void> (WaveParser::*parse)(Bytes payload);
    };

    std::optional<PayloadHandler> payload_handler(std::string_view id) const {
        if (id == "fmt ") return PayloadHandler{kMaxFormatChunkBytes, &WaveParser::on_format};
        if (id == "fact") return PayloadHandler{4, &WaveParser::on_fact};
        if (id == "ds64") return PayloadHandler{24, &WaveParser::on_ds64};
        if (id == "LIST") return PayloadHandler{kMaxTagBytes, &WaveParser::on_list};
        if (id == "acid") return PayloadHandler{24, &WaveParser::on_acid};
        if (id == "smpl") {
            return PayloadHandler{kSamplerHeaderBytes + kMaxSamplerLoops * kSamplerLoopBytes, &WaveParser::on_sampler};
        }
        if (id == "id3 " || id == "ID3 ") return PayloadHandler{kMaxTagBytes, &WaveParser::on_id3};
        return std::nullopt;
    }

    Result<void> on_format(Bytes payload) {
        if (format_) return {};  // only the first fmt chunk describes the data
        auto format = parse_wave_format(payload);
        if (!format) return std::unexpected(format.error());
        if (auto applied = apply_wave_format(*format, result_); !applied) return applied;
        format_ = *format;
        return {};
    }

    Result<void> on_fact(Bytes payload) {
        ByteCursor cursor(payload);
        const std::uint32_t frames = cursor.u32_le();
        if (cursor.ok()) fact_frames_ = frames;
        return {};
    }

    Result<void> on_ds64(Bytes payload) {
        ByteCursor cursor(payload);
        cursor.skip(8);  // RIFF size
        const std::uint64_t data_size = cursor.u64_le();
        if (!cursor.ok()) return fail(ErrorCode::CorruptAudio, "RF64 ds64 chunk is too short");
        rf64_data_size_ = data_size;
        return {};
    }

    Result<void> on_list(Bytes payload) {
        parse_info_list(payload, result_.tags);
        return {};
    }

    Result<void> on_acid(Bytes payload) {
        parse_acid(payload, result_.tags);
        return {};
    }

    Result<void> on_sampler(Bytes payload) {
        parse_sampler_loops(payload, result_.loops);
        return {};
    }

    Result<void> on_id3(Bytes payload) {
        parse_id3v2(payload, result_.tags);
        return {};
    }

    /// Frames from the data size for block-aligned codecs, otherwise from the fact chunk. A data
    /// chunk running past the end of a truncated file counts only the bytes actually present.
    void derive_frame_count() {
        if (!format_ || !data_offset_) return;
        const std::uint64_t file_size = reader_.file_size();
        const std::uint64_t present = file_size > *data_offset_ ? file_size - *data_offset_ : 0;
        const bool size_unknown = layout_ == ChunkLayout::Riff && data_size_ == kUnknownChunkSize;
        const std::uint64_t data_bytes = size_unknown ? present : std::min(data_size_, present);
        const bool block_aligned = format_->format_tag == kFormatPcm || format_->format_tag == kFormatIeeeFloat ||
                                   format_->format_tag == kFormatAlaw || format_->format_tag == kFormatMulaw;
        if (block_aligned && format_->block_align > 0) {
            result_.frames = static_cast<std::int64_t>(data_bytes / format_->block_align);
        } else if (fact_frames_) {
            result_.frames = *fact_frames_;
        }
    }

    BoundedFileReader& reader_;
    ProbeResult& result_;
    ChunkLayout layout_;
    std::optional<WaveFormat> format_;
    std::optional<std::uint64_t> data_offset_;
    std::uint64_t data_size_ = 0;
    std::optional<std::uint32_t> fact_frames_;
    std::optional<std::uint64_t> rf64_data_size_;
};

}  // namespace

Result<void> probe_riff_wave(BoundedFileReader& reader, Bytes signature, ProbeResult& result) {
    result.format = has_magic(signature, 0, "RIFF") ? "wav" : "rf64";  // RF64 and BW64 share the ds64 layout
    return WaveParser(reader, result, ChunkLayout::Riff).run();
}

Result<void> probe_wave64(BoundedFileReader& reader, ProbeResult& result) {
    result.format = "w64";
    return WaveParser(reader, result, ChunkLayout::Wave64).run();
}

}  // namespace mso::media::detail
