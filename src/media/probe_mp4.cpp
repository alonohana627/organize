// ISO base media / QuickTime (MP4, M4A, MOV audio) metadata probe.
//
// The movie box is walked in the file with header-sized reads: only the boxes the probe needs
// (handler, media header, sample description, the wanted ilst items) are loaded, so a movie box
// holding large cover art or a long sample table costs a few hundred bytes of budget.
#include <algorithm>
#include <array>
#include <bit>
#include <cmath>
#include <limits>
#include <vector>

#include "mso/domain/core.hpp"
#include "mso/media/detail/probe_formats.hpp"
#include "mso/media/detail/tag_mapping.hpp"
#include "mso/media/detail/text_decoding.hpp"

namespace mso::media::detail {

namespace {

constexpr std::size_t kSampleEntryFixedBytes = 28;  // SampleEntry + AudioSampleEntry v0 fields
constexpr std::size_t kQuickTimeV1ExtraBytes = 16;
constexpr std::size_t kQuickTimeV2ExtraBytes = 36;
/// Sample descriptions are a few hundred bytes; anything larger is cut here.
constexpr std::size_t kMaxSampleDescriptionBytes = 64 * 1024;
constexpr std::size_t kHandlerBytes = 12;      // FullBox header, pre-defined, handler type
constexpr std::size_t kMediaHeaderBytes = 36;  // enough for version 0 and version 1
/// ilst data types (QuickTime "well-known types").
constexpr std::uint32_t kDataTypeImplicit = 0;
constexpr std::uint32_t kDataTypeUtf8 = 1;
constexpr std::uint32_t kDataTypeUtf16 = 2;
constexpr std::uint32_t kDataTypeSignedInteger = 21;
constexpr std::uint32_t kDataTypeUnsignedInteger = 22;
/// MPEG-4 audio object types that announce spectral band replication explicitly.
constexpr std::uint32_t kAudioObjectSbr = 5;
constexpr std::uint32_t kAudioObjectParametricStereo = 29;
constexpr std::array<std::int64_t, 13> kMpeg4SampleRates = {96000, 88200, 64000, 48000, 44100, 32000, 24000,
                                                            22050, 16000, 12000, 11025, 8000,  7350};

/// A box located in the file. The payload is clamped to its parent.
struct FileBox {
    std::string type;
    std::uint64_t payload_offset = 0;
    std::uint64_t payload_size = 0;
};

/// Lists the child boxes of the byte range [offset, offset + size) by reading only their headers.
/// Stops at the first malformed header; at most kMaxChunkCount boxes.
Result<std::vector<FileBox>> list_boxes(BoundedFileReader& reader, std::uint64_t offset, std::uint64_t size) {
    std::vector<FileBox> boxes;
    const std::uint64_t end = std::min(checked_add(offset, size).value_or(~0ull), reader.file_size());
    std::uint64_t position = offset;
    while (end > position && end - position >= 8 && boxes.size() < static_cast<std::size_t>(kMaxChunkCount)) {
        auto header = reader.read_exact(position, 8);
        if (!header) return std::unexpected(header.error());
        ByteCursor cursor(*header);
        std::uint64_t box_size = cursor.u32_be();
        std::string type = cursor.fourcc();
        std::uint64_t header_size = 8;
        if (box_size == 1) {
            if (end - position < 16) break;
            auto large_size = reader.read_exact(position + 8, 8);
            if (!large_size) return std::unexpected(large_size.error());
            box_size = ByteCursor(*large_size).u64_be();
            header_size = 16;
        } else if (box_size == 0) {
            box_size = end - position;  // extends to the end of the parent
        }
        if (box_size < header_size) break;
        const std::uint64_t payload_offset = position + header_size;
        const std::uint64_t payload_size = std::min(box_size - header_size, end - payload_offset);
        boxes.push_back({std::move(type), payload_offset, payload_size});
        const auto next = checked_add(position, box_size);
        if (!next || *next <= position) break;
        position = *next;
    }
    return boxes;
}

/// The first box of `type`, or nullptr.
const FileBox* find_box(const std::vector<FileBox>& boxes, std::string_view type) {
    const auto found = std::ranges::find(boxes, type, &FileBox::type);
    return found == boxes.end() ? nullptr : &*found;
}

/// Descends from `parent` through nested containers ("mdia", "minf", ...) to the box at the end of
/// `path`; nullopt when any level is missing.
Result<std::optional<FileBox>> find_path(BoundedFileReader& reader, const FileBox& parent,
                                         std::initializer_list<std::string_view> path) {
    FileBox current = parent;
    for (const std::string_view type : path) {
        auto children = list_boxes(reader, current.payload_offset, current.payload_size);
        if (!children) return std::unexpected(children.error());
        const FileBox* box = find_box(*children, type);
        if (box == nullptr) return std::optional<FileBox>{};
        current = *box;
    }
    return std::optional<FileBox>{current};
}

/// Loads at most `limit` bytes of a box payload.
Result<std::vector<std::uint8_t>> load(BoundedFileReader& reader, const FileBox& box, std::size_t limit) {
    return reader.read_available(box.payload_offset,
                                 static_cast<std::size_t>(std::min<std::uint64_t>(box.payload_size, limit)));
}

/// One box inside an in-memory buffer (sample entries and their children).
struct MemoryBox {
    std::string type;
    Bytes payload;
};

/// Splits an in-memory container payload into child boxes, stopping at the first malformed header.
std::vector<MemoryBox> child_boxes(Bytes container) {
    std::vector<MemoryBox> boxes;
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

/// The first in-memory box of `type`, or nullptr.
const MemoryBox* find_memory_box(const std::vector<MemoryBox>& boxes, std::string_view type) {
    const auto found = std::ranges::find(boxes, type, &MemoryBox::type);
    return found == boxes.end() ? nullptr : &*found;
}

/// Unsigned big-endian integer of up to 8 bytes.
std::uint64_t big_endian_integer(Bytes bytes) {
    std::uint64_t value = 0;
    for (const std::uint8_t byte : bytes.first(std::min<std::size_t>(bytes.size(), 8)))
        value = (value << 8) | byte;
    return value;
}

/// The iTunes item types the probe stores, and where they go.
enum class ItemKind { Text, Tempo, GenreNumber, Freeform };

/// What an ilst item type is used for; nullopt for items that are never read (cover art "covr",
/// track numbers, flags, ...).
std::optional<std::pair<ItemKind, TagField>> item_kind(std::string_view type) {
    static constexpr std::string_view kTitle = "\xA9nam";
    static constexpr std::string_view kArtist =
        "\xA9"
        "ART";
    static constexpr std::string_view kAlbum =
        "\xA9"
        "alb";
    static constexpr std::string_view kGenre = "\xA9gen";
    static constexpr std::string_view kComment =
        "\xA9"
        "cmt";
    if (type == kTitle) return std::pair{ItemKind::Text, TagField::Title};
    if (type == kArtist) return std::pair{ItemKind::Text, TagField::Artist};
    if (type == kAlbum) return std::pair{ItemKind::Text, TagField::Album};
    if (type == kGenre) return std::pair{ItemKind::Text, TagField::Genre};
    if (type == kComment) return std::pair{ItemKind::Text, TagField::Comment};
    if (type == "keyw") return std::pair{ItemKind::Text, TagField::Keywords};
    if (type == "tmpo") return std::pair{ItemKind::Tempo, TagField::Bpm};
    if (type == "gnre") return std::pair{ItemKind::GenreNumber, TagField::Genre};
    if (type == "----") return std::pair{ItemKind::Freeform, TagField::Keywords};
    return std::nullopt;
}

/// Reads one wanted ilst item: its "data" box (type, locale, value) and, for "----", its name.
Result<void> read_item(BoundedFileReader& reader, const FileBox& item, ItemKind kind, TagField field,
                       EmbeddedTags& tags) {
    auto children = list_boxes(reader, item.payload_offset, item.payload_size);
    if (!children) return std::unexpected(children.error());
    const FileBox* data = find_box(*children, "data");
    if (data == nullptr || data->payload_size < 8 || data->payload_size > kMaxTextItemBytes) return {};
    auto payload = load(reader, *data, kMaxTextItemBytes);
    if (!payload) return std::unexpected(payload.error());
    ByteCursor cursor(*payload);
    const std::uint32_t data_type = cursor.u32_be() & 0x00FFFFFF;
    cursor.skip(4);  // locale
    const Bytes value = cursor.take_rest();
    if (!cursor.ok()) return {};
    const bool is_text = data_type == kDataTypeUtf8 || data_type == kDataTypeUtf16;
    const bool is_integer =
        data_type == kDataTypeImplicit || data_type == kDataTypeSignedInteger || data_type == kDataTypeUnsignedInteger;
    const auto text = [&] {
        return decode_text(value, data_type == kDataTypeUtf16 ? TextEncoding::Utf16BigEndian : TextEncoding::Utf8);
    };
    switch (kind) {
        case ItemKind::Text:
            if (is_text) store_tag(tags, field, text());
            break;
        case ItemKind::Tempo:
            if (is_integer && !tags.bpm_milli && !value.empty()) {
                tags.bpm_milli = milli_bpm_from_value(static_cast<double>(big_endian_integer(value)));
            }
            break;
        case ItemKind::GenreNumber:
            if (is_integer && !value.empty()) {
                // ID3v1 genre number plus one; anything beyond the table is ignored.
                const std::uint64_t number = big_endian_integer(value);
                if (number >= 1 && number <= 256) {
                    if (const auto genre = id3v1_genre_name(static_cast<int>(number) - 1)) {
                        store_tag(tags, TagField::Genre, *genre);
                    }
                }
            }
            break;
        case ItemKind::Freeform: {
            const FileBox* name = find_box(*children, "name");
            if (!is_text || name == nullptr || name->payload_size < 4) break;
            auto name_bytes = load(reader, *name, 4 + 256);
            if (!name_bytes) return std::unexpected(name_bytes.error());
            if (name_bytes->size() < 4) break;
            if (const auto freeform_field = field_for_tag_name(decode_legacy_text(Bytes(*name_bytes).subspan(4)))) {
                store_tag(tags, *freeform_field, text());
            }
            break;
        }
    }
    return {};
}

/// iTunes-style "ilst" items. Only the wanted item types are opened; cover art is never read.
Result<void> read_item_list(BoundedFileReader& reader, const FileBox& item_list, EmbeddedTags& tags) {
    auto items = list_boxes(reader, item_list.payload_offset, item_list.payload_size);
    if (!items) return std::unexpected(items.error());
    for (const FileBox& item : *items) {
        const auto kind = item_kind(item.type);
        if (!kind) continue;
        if (auto read = read_item(reader, item, kind->first, kind->second, tags); !read) return read;
    }
    return {};
}

/// The "ilst" inside a "meta" box. ISO files give "meta" a FullBox version/flags word; QuickTime
/// files do not (their first child, "hdlr", starts right away).
Result<void> read_meta(BoundedFileReader& reader, const FileBox& meta, EmbeddedTags& tags) {
    auto head = reader.read_available(meta.payload_offset,
                                      static_cast<std::size_t>(std::min<std::uint64_t>(meta.payload_size, 8)));
    if (!head) return std::unexpected(head.error());
    const bool quicktime_layout = has_magic(*head, 4, "hdlr");
    const std::uint64_t skip = quicktime_layout || meta.payload_size < 4 ? 0 : 4;
    auto children = list_boxes(reader, meta.payload_offset + skip, meta.payload_size - skip);
    if (!children) return std::unexpected(children.error());
    if (const FileBox* item_list = find_box(*children, "ilst")) return read_item_list(reader, *item_list, tags);
    return {};
}

/// Codec family of a sample entry, which decides bit depth and duration exactness.
struct SampleEntryCodec {
    std::string codec;
    bool lossless = false;
};

/// Codec for well-known sample entry types; nullopt for PCM variants and unknown types.
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

/// QuickTime and ISO linear PCM sample entry types.
bool is_linear_pcm_entry(std::string_view type) {
    return type == "lpcm" || type == "sowt" || type == "twos" || type == "in24" || type == "in32" || type == "fl32" ||
           type == "fl64" || type == "raw ";
}

/// Sample entry types of protected (encrypted) streams.
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

/// Reads MSB-first bit fields from a byte span (MPEG-4 AudioSpecificConfig).
class BitCursor {
public:
    explicit BitCursor(Bytes bytes) noexcept : bytes_(bytes) {}
    /// The next `count` (at most 32) bits; nullopt when the span ends first.
    std::optional<std::uint32_t> bits(int count) {
        std::uint32_t value = 0;
        for (int index = 0; index < count; ++index) {
            if (bit_position_ / 8 >= bytes_.size()) return std::nullopt;
            const std::uint8_t byte = bytes_[bit_position_ / 8];
            value = (value << 1) | ((byte >> (7 - bit_position_ % 8)) & 1u);
            ++bit_position_;
        }
        return value;
    }

private:
    Bytes bytes_;
    std::size_t bit_position_ = 0;
};

/// Sampling frequency of an AudioSpecificConfig index (15 = explicit 24-bit value follows).
std::optional<std::int64_t> mpeg4_sample_rate(BitCursor& cursor) {
    const auto index = cursor.bits(4);
    if (!index) return std::nullopt;
    if (*index == 15) {
        const auto explicit_rate = cursor.bits(24);
        if (!explicit_rate) return std::nullopt;
        return static_cast<std::int64_t>(*explicit_rate);
    }
    if (*index >= kMpeg4SampleRates.size()) return std::nullopt;
    return kMpeg4SampleRates[*index];
}

/// Length of an MPEG-4 descriptor (1-4 bytes, 7 bits each, high bit = more bytes follow).
std::optional<std::uint32_t> descriptor_length(ByteCursor& cursor) {
    std::uint32_t length = 0;
    for (int count = 0; count < 4; ++count) {
        const std::uint8_t byte = cursor.u8();
        if (!cursor.ok()) return std::nullopt;
        length = (length << 7) | (byte & 0x7F);
        if ((byte & 0x80) == 0) return length;
    }
    return length;
}

/// Output sample rate announced by an "esds" box: the SBR extension rate when the AudioSpecificConfig
/// signals HE-AAC explicitly (object type 5 or 29). nullopt when it does not (implicit SBR
/// signalling is only visible to the decoder, so such files report the core rate).
std::optional<std::int64_t> explicit_sbr_sample_rate(Bytes esds) {
    ByteCursor cursor(esds);
    cursor.skip(4);  // FullBox version and flags
    // ES_Descriptor (tag 3) -> DecoderConfigDescriptor (tag 4) -> DecoderSpecificInfo (tag 5).
    if (cursor.u8() != 0x03 || !descriptor_length(cursor)) return std::nullopt;
    cursor.skip(2);  // ES_ID
    const std::uint8_t es_flags = cursor.u8();
    if (es_flags & 0x80) cursor.skip(2);            // dependsOn_ES_ID
    if (es_flags & 0x40) cursor.skip(cursor.u8());  // URL
    if (es_flags & 0x20) cursor.skip(2);            // OCR_ES_Id
    if (cursor.u8() != 0x04 || !descriptor_length(cursor)) return std::nullopt;
    cursor.skip(1 + 1 + 3 + 4 + 4);  // object type, stream type, buffer size, max and average bitrate
    if (cursor.u8() != 0x05) return std::nullopt;
    const auto config_length = descriptor_length(cursor);
    if (!config_length) return std::nullopt;
    const Bytes config = cursor.take(*config_length);
    if (!cursor.ok()) return std::nullopt;

    BitCursor bits(config);
    auto object_type = bits.bits(5);
    if (object_type == 31u) {
        const auto extended = bits.bits(6);
        object_type = extended ? std::optional<std::uint32_t>(32 + *extended) : std::nullopt;
    }
    if (!object_type || !mpeg4_sample_rate(bits) || !bits.bits(4)) return std::nullopt;  // core rate, channels
    if (*object_type != kAudioObjectSbr && *object_type != kAudioObjectParametricStereo) return std::nullopt;
    return mpeg4_sample_rate(bits);
}

/// Decodes the first sample entry of a sound track's "stsd" box.
Result<AudioDescription> parse_sample_entry(const MemoryBox& entry) {
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
    description.sample_rate = cursor.u32_be() >> 16;  // 16.16 fixed point: at most 65535
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
        if (entry.type == "in24") sample_size = 24;
        if (entry.type == "in32") sample_size = 32;
        const auto codec = pcm_codec_name(sample_size, description.is_float, entry.type == "raw ");
        if (!codec) return fail(ErrorCode::CorruptAudio, "unsupported PCM sample size in MP4 sample entry");
        description.codec = {*codec, true};
        description.bit_depth = sample_size;
    }
    if (entry.payload.size() > children_offset) {
        const auto children = child_boxes(entry.payload.subspan(children_offset));
        if (const MemoryBox* cookie = find_memory_box(children, "alac"))
            apply_alac_cookie(cookie->payload, description);
        // ISO "srat": the real rate when it does not fit the 16.16 field (above 65535 Hz).
        if (const MemoryBox* rate = find_memory_box(children, "srat")) {
            ByteCursor rate_cursor(rate->payload);
            rate_cursor.skip(4);  // FullBox version and flags
            const std::uint32_t sampling_rate = rate_cursor.u32_be();
            if (rate_cursor.ok() && sampling_rate > 0) description.sample_rate = sampling_rate;
        }
        if (const MemoryBox* esds = find_memory_box(children, "esds"); esds != nullptr && entry.type == "mp4a") {
            if (const auto sbr_rate = explicit_sbr_sample_rate(esds->payload)) description.sample_rate = *sbr_rate;
        }
    }
    return description;
}

/// Media header: ticks per second and track duration in those ticks.
struct MediaHeader {
    std::uint32_t timescale = 0;
    std::optional<std::uint64_t> duration;
};

/// Decodes an "mdhd" payload (version 0 or 1).
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
Result<bool> parse_track(BoundedFileReader& reader, const FileBox& track, ProbeResult& result) {
    auto media = find_path(reader, track, {"mdia"});
    if (!media) return std::unexpected(media.error());
    if (!*media) return false;
    auto media_children = list_boxes(reader, (*media)->payload_offset, (*media)->payload_size);
    if (!media_children) return std::unexpected(media_children.error());
    const FileBox* handler = find_box(*media_children, "hdlr");
    if (handler == nullptr) return false;
    auto handler_bytes = load(reader, *handler, kHandlerBytes);
    if (!handler_bytes) return std::unexpected(handler_bytes.error());
    if (!has_magic(*handler_bytes, 8, "soun")) return false;

    const FileBox* media_header_box = find_box(*media_children, "mdhd");
    const FileBox* media_information = find_box(*media_children, "minf");
    if (media_header_box == nullptr || media_information == nullptr) {
        return fail(ErrorCode::CorruptAudio, "MP4 sound track is missing its media header");
    }
    auto sample_descriptions = find_path(reader, *media_information, {"stbl", "stsd"});
    if (!sample_descriptions) return std::unexpected(sample_descriptions.error());
    if (!*sample_descriptions || (*sample_descriptions)->payload_size < 8) {
        return fail(ErrorCode::CorruptAudio, "MP4 sound track is missing its sample description");
    }
    auto description_bytes = load(reader, **sample_descriptions, kMaxSampleDescriptionBytes);
    if (!description_bytes) return std::unexpected(description_bytes.error());
    if (description_bytes->size() < 8) return fail(ErrorCode::CorruptAudio, "MP4 sample description is truncated");
    const auto entries = child_boxes(Bytes(*description_bytes).subspan(8));  // FullBox header, entry count
    if (entries.empty()) return fail(ErrorCode::CorruptAudio, "MP4 sound track has no sample entry");
    auto description = parse_sample_entry(entries.front());
    if (!description) return std::unexpected(description.error());

    auto media_header_bytes = load(reader, *media_header_box, kMediaHeaderBytes);
    if (!media_header_bytes) return std::unexpected(media_header_bytes.error());
    const MediaHeader header = parse_media_header(*media_header_bytes);
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

/// The first sound track of the movie, then its iTunes tags (udta/meta and moov/meta).
Result<void> parse_movie(BoundedFileReader& reader, const FileBox& movie, ProbeResult& result) {
    auto children = list_boxes(reader, movie.payload_offset, movie.payload_size);
    if (!children) return std::unexpected(children.error());
    bool found_sound_track = false;
    for (const FileBox& box : *children) {
        if (box.type != "trak") continue;
        auto parsed = parse_track(reader, box, result);
        if (!parsed) return std::unexpected(parsed.error());
        if (*parsed) {
            found_sound_track = true;
            break;
        }
    }
    if (!found_sound_track) return fail(ErrorCode::UnsupportedCodec, "MP4 file has no audio track");
    if (const FileBox* user_data = find_box(*children, "udta")) {
        auto meta = find_path(reader, *user_data, {"meta"});
        if (!meta) return std::unexpected(meta.error());
        if (*meta) {
            if (auto read = read_meta(reader, **meta, result.tags); !read) return read;
        }
    }
    if (const FileBox* meta = find_box(*children, "meta")) return read_meta(reader, *meta, result.tags);
    return {};
}

}  // namespace

Result<void> probe_iso_media(BoundedFileReader& reader, ProbeResult& result) {
    result.format = "mp4";
    auto top_level = list_boxes(reader, 0, reader.file_size());
    if (!top_level) return std::unexpected(top_level.error());
    if (const FileBox* file_type = find_box(*top_level, "ftyp"); file_type != nullptr && file_type->payload_size >= 4) {
        auto brand = reader.read_exact(file_type->payload_offset, 4);
        if (!brand) return std::unexpected(brand.error());
        const std::string_view major_brand = as_text(*brand);
        if (major_brand == "M4A " || major_brand == "M4B " || major_brand == "M4P ") result.format = "m4a";
    }
    const FileBox* movie = find_box(*top_level, "moov");
    if (movie == nullptr) return fail(ErrorCode::CorruptAudio, "MP4 file has no moov box");
    return parse_movie(reader, *movie, result);
}

}  // namespace mso::media::detail
