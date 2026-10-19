// ISO base media / QuickTime (MP4, M4A, MOV audio) metadata probe.
#include <algorithm>
#include <bit>
#include <cmath>
#include <limits>
#include <vector>

#include "mso/domain/core.hpp"
#include "mso/media/probe_formats.hpp"
#include "mso/media/tag_mapping.hpp"
#include "mso/media/text_decoding.hpp"

namespace mso::media::detail {

namespace {

constexpr std::uint64_t kMaxMovieBoxBytes = 16ull * 1024 * 1024;
constexpr std::uint32_t kMp4TextUtf16 = 2;          // well-known data type; 1 (UTF-8) and others decode as UTF-8
constexpr std::size_t kSampleEntryFixedBytes = 28;  // SampleEntry + AudioSampleEntry v0 fields
constexpr std::size_t kQuickTimeV1ExtraBytes = 16;
constexpr std::size_t kQuickTimeV2ExtraBytes = 36;

/// One box inside an in-memory buffer.
struct Box {
    std::string type;
    Bytes payload;
};

/// Splits a container payload into child boxes, stopping at the first malformed header.
std::vector<Box> child_boxes(Bytes container) {
    std::vector<Box> boxes;
    ByteCursor cursor(container);
    while (cursor.has(8) && boxes.size() < static_cast<std::size_t>(kMaxChunkCount)) {
        std::uint64_t size = cursor.u32_be();
        std::string type = cursor.fourcc();
        std::uint64_t header_size = 8;
        if (size == 1) {
            size = cursor.u64_be();
            header_size = 16;
        } else if (size == 0) {
            size = header_size + cursor.remaining();  // extends to the end of the parent
        }
        if (size < header_size) break;
        const Bytes payload = cursor.take(size - header_size);
        if (!cursor.ok()) break;
        boxes.push_back({std::move(type), payload});
    }
    return boxes;
}

const Box* find_box(const std::vector<Box>& boxes, std::string_view type) {
    const auto found = std::ranges::find(boxes, type, &Box::type);
    return found == boxes.end() ? nullptr : &*found;
}

/// Descends through nested containers ("mdia", "minf", ...) to the box at the end of `path`.
std::optional<Bytes> find_path(Bytes container, std::initializer_list<std::string_view> path) {
    Bytes current = container;
    for (const std::string_view type : path) {
        const auto children = child_boxes(current);
        const Box* box = find_box(children, type);
        if (box == nullptr) return std::nullopt;
        current = box->payload;
    }
    return current;
}

/// Payload of a "meta" box. ISO files give it a FullBox version/flags word; QuickTime does not.
Bytes meta_children(Bytes meta) {
    const bool quicktime_layout = has_magic(meta, 4, "hdlr");
    return quicktime_layout || meta.size() < 4 ? meta : meta.subspan(4);
}

std::uint64_t big_endian_integer(Bytes bytes) {
    std::uint64_t value = 0;
    for (const std::uint8_t byte : bytes.first(std::min<std::size_t>(bytes.size(), 8)))
        value = (value << 8) | byte;
    return value;
}

/// iTunes-style "ilst" items: each item box holds a "data" box (and "mean"/"name" for "----").
void parse_item_list(Bytes item_list, EmbeddedTags& tags) {
    for (const Box& item : child_boxes(item_list)) {
        const auto children = child_boxes(item.payload);
        const Box* data = find_box(children, "data");
        if (data == nullptr) continue;
        ByteCursor cursor(data->payload);
        const std::uint32_t data_type = cursor.u32_be() & 0x00FFFFFF;
        cursor.skip(4);  // locale
        const Bytes value = cursor.take_rest();
        if (!cursor.ok()) continue;
        const std::string text =
            decode_text(value, data_type == kMp4TextUtf16 ? TextEncoding::Utf16BigEndian : TextEncoding::Utf8);

        const std::string& type = item.type;
        if (type ==
            "\xA9"
            "nam")
            store_tag(tags, TagField::Title, text);
        else if (type ==
                 "\xA9"
                 "ART")
            store_tag(tags, TagField::Artist, text);
        else if (type ==
                 "\xA9"
                 "alb")
            store_tag(tags, TagField::Album, text);
        else if (type ==
                 "\xA9"
                 "gen")
            store_tag(tags, TagField::Genre, text);
        else if (type ==
                 "\xA9"
                 "cmt")
            store_tag(tags, TagField::Comment, text);
        else if (type == "keyw")
            store_tag(tags, TagField::Keywords, text);
        else if (type == "tmpo" && !tags.bpm_milli) {
            tags.bpm_milli = milli_bpm_from_value(static_cast<double>(big_endian_integer(value)));
        } else if (type == "gnre") {
            const auto genre = id3v1_genre_name(static_cast<int>(big_endian_integer(value)) - 1);
            if (genre) store_tag(tags, TagField::Genre, *genre);
        } else if (type == "----") {
            const Box* name = find_box(children, "name");
            if (name == nullptr || name->payload.size() < 4) continue;
            if (const auto field = field_for_tag_name(decode_legacy_text(name->payload.subspan(4)))) {
                store_tag(tags, *field, text);
            }
        }
    }
}

/// Codec family of a sample entry, which decides bit depth and duration exactness.
struct SampleEntryCodec {
    std::string codec;
    bool lossless = false;
};

std::optional<SampleEntryCodec> codec_for_sample_entry(std::string_view type) {
    if (type == "mp4a") return SampleEntryCodec{"aac", false};
    if (type == "alac") return SampleEntryCodec{"alac", true};
    if (type == "fLaC") return SampleEntryCodec{"flac", true};
    if (type == "Opus") return SampleEntryCodec{"opus", false};
    if (type == "ac-3") return SampleEntryCodec{"ac3", false};
    if (type == "ec-3") return SampleEntryCodec{"eac3", false};
    if (type == ".mp3") return SampleEntryCodec{"mp3", false};
    if (type == "ulaw") return SampleEntryCodec{"pcm_mulaw", false};
    if (type == "alaw") return SampleEntryCodec{"pcm_alaw", false};
    return std::nullopt;
}

bool is_linear_pcm_entry(std::string_view type) {
    return type == "lpcm" || type == "sowt" || type == "twos" || type == "in24" || type == "in32" || type == "fl32" ||
           type == "fl64" || type == "raw ";
}

bool is_encrypted_entry(std::string_view type) {
    return type == "enca" || type == "drms" || type == "drmi" || type == "encv";
}

/// Audio sample description: codec, channels, rate and (for ALAC and PCM) bit depth.
struct AudioDescription {
    SampleEntryCodec codec;
    std::int64_t channels = 0;
    std::int64_t sample_rate = 0;
    std::optional<std::int64_t> bit_depth;
    bool is_float = false;
};

/// ALAC magic cookie ("alac" box inside the sample entry): the authoritative bit depth and layout.
void apply_alac_cookie(Bytes cookie_box, AudioDescription& description) {
    ByteCursor cursor(cookie_box);
    cursor.skip(4 + 4 + 1);  // FullBox header, frame length, compatible version
    const std::uint8_t bit_depth = cursor.u8();
    cursor.skip(3);  // rice parameters
    const std::uint8_t channels = cursor.u8();
    cursor.skip(2 + 4 + 4);  // max run, max frame bytes, average bitrate
    const std::uint32_t sample_rate = cursor.u32_be();
    if (!cursor.ok()) return;
    description.bit_depth = bit_depth;
    description.channels = channels;
    description.sample_rate = sample_rate;
}

Result<AudioDescription> parse_sample_entry(const Box& entry) {
    if (is_encrypted_entry(entry.type)) {
        return fail(ErrorCode::UnsupportedCodec, "encrypted (DRM-protected) audio is not supported");
    }
    AudioDescription description;
    const bool is_pcm = is_linear_pcm_entry(entry.type);
    if (const auto known = codec_for_sample_entry(entry.type)) {
        description.codec = *known;
    } else if (!is_pcm) {
        description.codec = {to_lower_ascii(trim(entry.type)), false};
    }
    ByteCursor cursor(entry.payload);
    cursor.skip(8);  // reserved, data reference index
    const std::uint16_t version = cursor.u16_be();
    cursor.skip(2 + 4);  // revision, vendor
    description.channels = cursor.u16_be();
    std::int64_t sample_size = cursor.u16_be();
    cursor.skip(4);                                   // compression id, packet size
    description.sample_rate = cursor.u32_be() >> 16;  // 16.16 fixed point
    std::uint32_t pcm_flags = 0;
    std::size_t children_offset = kSampleEntryFixedBytes;
    if (version == 1) {
        children_offset += kQuickTimeV1ExtraBytes;
    } else if (version == 2) {
        cursor.skip(4);  // size of struct
        const double rate = std::bit_cast<double>(cursor.u64_be());
        description.channels = cursor.u32_be();
        cursor.skip(4);  // always 0x7F000000
        sample_size = cursor.u32_be();
        pcm_flags = cursor.u32_be();
        if (std::isfinite(rate) && rate >= 1.0 && rate <= static_cast<double>(kMaxSampleRate)) {
            description.sample_rate = std::llround(rate);
        }
        children_offset += kQuickTimeV2ExtraBytes;
    }
    if (!cursor.ok()) return fail(ErrorCode::CorruptAudio, "MP4 audio sample entry is too short");

    if (is_pcm) {
        description.is_float = entry.type == "fl32" || entry.type == "fl64" || (pcm_flags & 0x1);
        if (entry.type == "fl64") sample_size = 64;
        if (entry.type == "fl32") sample_size = 32;
        const auto codec = pcm_codec_name(sample_size, description.is_float, entry.type == "raw ");
        if (!codec) return fail(ErrorCode::CorruptAudio, "unsupported PCM sample size in MP4 sample entry");
        description.codec = {*codec, true};
        description.bit_depth = sample_size;
    }
    if (entry.payload.size() > children_offset) {
        const auto children = child_boxes(entry.payload.subspan(children_offset));
        if (const Box* cookie = find_box(children, "alac")) apply_alac_cookie(cookie->payload, description);
    }
    return description;
}

/// Media header: ticks per second and track duration in those ticks.
struct MediaHeader {
    std::uint32_t timescale = 0;
    std::optional<std::uint64_t> duration;
};

MediaHeader parse_media_header(Bytes payload) {
    ByteCursor cursor(payload);
    const std::uint8_t version = cursor.u8();
    cursor.skip(3);
    MediaHeader header;
    std::uint64_t duration = 0;
    bool unknown = false;
    if (version == 1) {
        cursor.skip(16);
        header.timescale = cursor.u32_be();
        duration = cursor.u64_be();
        unknown = duration == ~0ull;
    } else {
        cursor.skip(8);
        header.timescale = cursor.u32_be();
        duration = cursor.u32_be();
        unknown = duration == 0xFFFFFFFFu;
    }
    if (cursor.ok() && !unknown) header.duration = duration;
    return header;
}

/// Parses one "trak"; false when it is not a sound track.
Result<bool> parse_track(Bytes track, ProbeResult& result) {
    const auto handler = find_path(track, {"mdia", "hdlr"});
    if (!handler || !has_magic(*handler, 8, "soun")) return false;
    const auto media_header = find_path(track, {"mdia", "mdhd"});
    const auto sample_descriptions = find_path(track, {"mdia", "minf", "stbl", "stsd"});
    if (!media_header || !sample_descriptions || sample_descriptions->size() < 8) {
        return fail(ErrorCode::CorruptAudio, "MP4 sound track is missing its sample description");
    }
    const auto entries = child_boxes(sample_descriptions->subspan(8));  // FullBox header, entry count
    if (entries.empty()) return fail(ErrorCode::CorruptAudio, "MP4 sound track has no sample entry");
    auto description = parse_sample_entry(entries.front());
    if (!description) return std::unexpected(description.error());

    const MediaHeader header = parse_media_header(*media_header);
    if (description->sample_rate == 0) description->sample_rate = header.timescale;
    result.codec = description->codec.codec;
    if (auto layout = set_stream_layout(result, description->channels, description->sample_rate); !layout) {
        return std::unexpected(layout.error());
    }
    result.float_samples = description->is_float;
    if (description->codec.lossless) result.bit_depth = description->bit_depth;

    if (header.duration && header.timescale > 0) {
        const bool frame_exact = description->codec.lossless && header.timescale == description->sample_rate;
        if (frame_exact) {
            result.frames = static_cast<std::int64_t>(
                std::min<std::uint64_t>(*header.duration, std::numeric_limits<std::int64_t>::max()));
        } else {
            // Lossy tracks carry encoder priming that only the edit list or decoder trims.
            result.duration_us = units_to_microseconds(*header.duration, header.timescale);
            result.duration_exact = false;
        }
    }
    return true;
}

Result<void> parse_movie(Bytes movie, ProbeResult& result) {
    const auto children = child_boxes(movie);
    bool found_sound_track = false;
    for (const Box& box : children) {
        if (box.type != "trak") continue;
        auto parsed = parse_track(box.payload, result);
        if (!parsed) return std::unexpected(parsed.error());
        if (*parsed) {
            found_sound_track = true;
            break;
        }
    }
    for (const auto& meta : {find_path(movie, {"udta", "meta"}), find_path(movie, {"meta"})}) {
        if (!meta) continue;
        if (const auto item_list = find_path(meta_children(*meta), {"ilst"})) parse_item_list(*item_list, result.tags);
    }
    if (!found_sound_track) return fail(ErrorCode::UnsupportedCodec, "MP4 file has no audio track");
    return {};
}

}  // namespace

Result<void> probe_iso_media(BoundedFileReader& reader, ProbeResult& result) {
    result.format = "mp4";
    std::uint64_t offset = 0;
    for (int count = 0; count < kMaxChunkCount; ++count) {
        if (reader.file_size() - std::min(offset, reader.file_size()) < 8) break;
        auto header = reader.read_exact(offset, 8);
        if (!header) return std::unexpected(header.error());
        ByteCursor cursor(*header);
        std::uint64_t size = cursor.u32_be();
        const std::string type = cursor.fourcc();
        std::uint64_t header_size = 8;
        if (size == 1) {
            auto large_size = reader.read_exact(offset + 8, 8);
            if (!large_size) return std::unexpected(large_size.error());
            size = ByteCursor(*large_size).u64_be();
            header_size = 16;
        } else if (size == 0) {
            size = reader.file_size() - offset;
        }
        if (size < header_size) return fail(ErrorCode::CorruptAudio, "invalid MP4 box size");
        const std::uint64_t payload_size = size - header_size;

        if (type == "ftyp") {
            auto brand = reader.read_exact(offset + header_size, 4);
            if (!brand) return std::unexpected(brand.error());
            const std::string_view major_brand = as_text(*brand);
            if (major_brand == "M4A " || major_brand == "M4B " || major_brand == "M4P ") result.format = "m4a";
        } else if (type == "moov") {
            if (payload_size > kMaxMovieBoxBytes)
                return fail(ErrorCode::ProbeLimit, "MP4 movie box exceeds the probe limit");
            auto movie = reader.read_available(offset + header_size, static_cast<std::size_t>(payload_size));
            if (!movie) return std::unexpected(movie.error());
            return parse_movie(*movie, result);
        }
        const auto next = checked_add(offset, size);
        if (!next) break;
        offset = *next;
    }
    return fail(ErrorCode::CorruptAudio, "MP4 file has no moov box");
}

}  // namespace mso::media::detail
