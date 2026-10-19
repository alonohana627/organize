#pragma once
// Media boundary: bounded metadata probes (import) and streaming float decoders (preview/analysis).
// No SQLite, Qt or catalog dependencies. Every read is bounded and every size is checked 64-bit.
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "mso/domain/core.hpp"
#include "mso/domain/error.hpp"

namespace mso::media {

struct ProbeLimits {
    std::uint64_t max_bytes = 4ull * 1024 * 1024;  // total reads, including footer seeks
    std::chrono::milliseconds max_time{2000};      // wall time
};

struct EmbeddedTags {
    std::optional<std::string> title, artist, album, genre, comment;
    std::optional<std::int64_t> bpm_milli;  // from TBPM / BPM / acid chunk
    std::optional<MusicalKey> key;          // from KEY / INITIALKEY / TKEY
    std::vector<std::string> keywords;      // other short textual tags worth indexing
};

struct LoopPoint {
    std::int64_t start_frame = 0;
    std::int64_t end_frame = 0;  // exclusive
};

struct ProbeResult {
    std::string
        format;         // actual container: wav, rf64, w64, aiff, aifc, flac, mp3, ogg, opus, m4a, caf, wv, aac, wma...
    std::string codec;  // pcm_s16, pcm_s24, pcm_s32, pcm_f32, pcm_f64, pcm_u8, flac, mp3, vorbis, opus, aac, alac...
    std::optional<std::int64_t> sample_rate;
    std::optional<std::int64_t> channels;
    std::optional<std::int64_t> bit_depth;  // only when meaningful (PCM / lossless); empty for lossy codecs
    bool float_samples = false;
    std::optional<std::int64_t> frames;  // exact source frames when known
    std::optional<std::int64_t> duration_us;
    bool duration_exact = false;  // never true for an estimate (VBR, bitrate-derived)
    bool partial = false;         // probe limit hit: some fields unknown (PROBE_LIMIT evidence)
    EmbeddedTags tags;
    std::vector<LoopPoint> loops;  // container loop markers (smpl, AIFF MARK/INST)
    std::uint64_t bytes_read = 0;
};

// Bounded probe. Never decodes the whole file and never hashes it.
// Errors: FILE_MISSING, ACCESS_DENIED, UNSUPPORTED_CODEC (not audio / unknown), CORRUPT_AUDIO.
Result<ProbeResult> probe_file(const std::filesystem::path& file, const ProbeLimits& limits = {});

// Extension pre-filter used by enumeration (lowercase, without dot). Suffix is only a hint:
// the probe identifies the actual container.
bool is_candidate_extension(std::string_view ext_lower) noexcept;

// Streaming decoder producing interleaved float32 at the source rate and channel count.
class Decoder {
public:
    virtual ~Decoder() = default;
    virtual int channels() const = 0;
    virtual int sample_rate() const = 0;
    virtual std::optional<std::int64_t> total_frames() const = 0;
    virtual bool frames_exact() const = 0;
    // Reads up to `frames` frames into out (frames*channels floats). Returns 0 at end of stream.
    virtual Result<std::size_t> read(float* out, std::size_t frames) = 0;
    virtual Result<void> seek(std::int64_t frame) = 0;
    virtual std::int64_t position() const = 0;
    virtual std::string codec() const = 0;
};

Result<std::unique_ptr<Decoder>> open_decoder(const std::filesystem::path& file);

// Codecs this build can decode, for capability reports and UNSUPPORTED_CODEC messages.
std::vector<std::string> decoder_capabilities();

// Filename heuristics: proposals tagged as 'filename' evidence, never accepted silently.
struct FilenameHints {
    std::optional<std::int64_t> bpm_milli;
    std::optional<MusicalKey> key;
};
FilenameHints filename_hints(std::string_view stem_utf8);

}  // namespace mso::media
