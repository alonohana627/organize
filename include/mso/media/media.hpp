#pragma once
/// @file
/// Media boundary: bounded metadata probes (import) and streaming float decoders (preview and
/// analysis). No SQLite, Qt or catalog dependencies. Every read is bounded, every size is checked in
/// 64 bits, and source files are only ever opened read-only with full sharing (invariant 11).
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "mso/domain/core.hpp"
#include "mso/domain/error.hpp"

namespace mso::media {

/// Limits for one probe_file() call (docs/01_REQUIREMENTS.md section 5.2).
struct ProbeLimits {
    /// Total bytes the probe may read from the file, including footer and tail reads.
    std::uint64_t max_bytes = 4ull * 1024 * 1024;
    /// Wall-clock budget; a read starting after it fails and the probe returns a partial result.
    std::chrono::milliseconds max_time{2000};
};

/// Text and musical tags embedded in the file by its container or tag block. Every text value is
/// valid UTF-8, trimmed, and at most 1 KiB; binary items (cover art, private frames) are never read.
struct EmbeddedTags {
    std::optional<std::string> title;       ///< ID3 TIT2, Vorbis TITLE, RIFF INAM, MP4 ©nam, APE Title, ...
    std::optional<std::string> artist;      ///< ID3 TPE1, Vorbis ARTIST, RIFF IART, MP4 ©ART, ...
    std::optional<std::string> album;       ///< ID3 TALB, Vorbis ALBUM, RIFF IPRD, MP4 ©alb, ...
    std::optional<std::string> genre;       ///< numeric ID3 genres are resolved to their names
    std::optional<std::string> comment;     ///< the first user comment (iTunes machine comments are skipped)
    std::optional<std::int64_t> bpm_milli;  ///< tempo in thousandths of a BPM (TBPM, BPM, acid chunk, tmpo)
    std::optional<MusicalKey> key;          ///< from KEY / INITIALKEY / TKEY (classic, Camelot or Open Key)
    std::vector<std::string> keywords;      ///< other short textual labels worth indexing (at most 32)
};

/// A loop region declared by the container (WAV smpl, AIFF MARK + INST).
struct LoopPoint {
    std::int64_t start_frame = 0;  ///< first frame of the loop
    std::int64_t end_frame = 0;    ///< one past the last frame of the loop (exclusive)
};

/// Everything a bounded probe learned about one file. Fields stay empty when unknown.
struct ProbeResult {
    /// Actual container, identified from the bytes: wav, rf64, w64, aiff, aifc, flac, mp3, ogg,
    /// opus, mp4, m4a, caf, wv, aac, wma, or an FFmpeg demuxer name on the fallback path.
    std::string format;
    /// Codec: pcm_s16, pcm_s24, pcm_s32, pcm_f32, pcm_f64, pcm_u8, flac, mp3, vorbis, opus, aac, alac, ...
    std::string codec;
    std::optional<std::int64_t> sample_rate;  ///< samples per second per channel
    std::optional<std::int64_t> channels;     ///< interleaved channel count
    std::optional<std::int64_t> bit_depth;    ///< only for PCM and lossless codecs; empty for lossy codecs
    bool float_samples = false;               ///< samples are IEEE floating point
    std::optional<std::int64_t> frames;       ///< exact source frame count, only when the container states it
    std::optional<std::int64_t> duration_us;  ///< duration in microseconds (exact or estimated)
    bool duration_exact = false;              ///< true only when derived from an exact frame count, never for estimates
    bool partial = false;                     ///< a probe limit was hit: some fields are unknown (PROBE_LIMIT evidence)
    EmbeddedTags tags;                        ///< embedded text and musical tags
    std::vector<LoopPoint> loops;             ///< container loop markers that fit inside the stream (at most 64)
    std::uint64_t bytes_read = 0;  ///< bytes the probe actually read (never more than ProbeLimits::max_bytes)
};

/// Bounded metadata probe. Identifies the container from its leading bytes (the extension is only a
/// hint), reads headers and tags within `limits`, and never decodes the audio or hashes the file.
/// Hitting a limit is not an error: the result comes back with `partial` set.
/// Thread-safe; may run concurrently on any thread.
/// Errors: FileMissing, AccessDenied (retryable while another process locks the file),
/// UnsupportedCodec (not audio, not a regular file, unknown or DRM-protected), CorruptAudio
/// (damaged essential headers).
Result<ProbeResult> probe_file(const std::filesystem::path& file, const ProbeLimits& limits = {});

/// Extension pre-filter used by enumeration: true for suffixes worth probing. Takes the extension
/// in lower case without the dot ("wav"). The suffix is only a hint; the probe identifies the
/// actual container.
bool is_candidate_extension(std::string_view extension_lowercase) noexcept;

/// Streaming decoder producing interleaved float32 at the source rate and channel count.
///
/// The channel count and sample rate are fixed for the decoder's lifetime, even when the stream
/// changes layout part-way (such frames are remixed and resampled to the opening layout).
/// A decoder is not thread-safe: use it from one thread at a time. Destroying it closes the file.
class Decoder {
public:
    virtual ~Decoder() = default;

    /// Channels per interleaved frame (at least 1).
    virtual int channels() const = 0;
    /// Frames per second (at least 1).
    virtual int sample_rate() const = 0;
    /// Length of the stream in frames, when known (an estimate unless frames_exact()).
    virtual std::optional<std::int64_t> total_frames() const = 0;
    /// True when total_frames() is the exact number of frames a full read delivers.
    virtual bool frames_exact() const = 0;

    /// Decodes up to `frames` frames into `out`, which must hold frames * channels() floats.
    /// Returns the number of frames written: fewer than requested only at the end, and 0 once the
    /// stream is exhausted. Never writes beyond frames * channels() floats.
    /// Errors: CorruptAudio for undecodable data, AccessDenied when the file can no longer be read.
    virtual Result<std::size_t> read(float* out, std::size_t frames) = 0;

    /// read(float*, std::size_t) over a span: decodes at most out.size() / channels() frames.
    Result<std::size_t> read(std::span<float> out) {
        const int channel_count = channels();
        return read(out.data(), out.size() / static_cast<std::size_t>(channel_count > 0 ? channel_count : 1));
    }

    /// Moves to `frame`, counted from the start of the stream. A negative frame fails with
    /// InvalidArgument. Seeking at or past the end is not an error on any backend: the decoder is
    /// positioned at the end (position() reports the exact end when the length is exact, otherwise
    /// the frame actually reached) and the next read() returns 0.
    /// Errors: CorruptAudio when the stream cannot be repositioned.
    virtual Result<void> seek(std::int64_t frame) = 0;
    /// Index of the next frame read() delivers.
    virtual std::int64_t position() const = 0;
    /// Codec name, in the vocabulary of ProbeResult::codec.
    virtual std::string codec() const = 0;
};

/// Opens a decoder for `file`. A bounded probe identifies the container and codec first; the backend
/// is chosen from that: miniaudio for WAV/AIFF/FLAC/MP3, stb_vorbis for Ogg Vorbis (FFmpeg's Vorbis
/// decoder drops the first 128 frames; FFmpeg still takes the files stb_vorbis refuses), and FFmpeg
/// when built in for everything else. The file is opened with full sharing, so other applications
/// can rename, move or delete it while it previews.
/// Errors: those of probe_file(), plus UnsupportedCodec when no built-in backend decodes the codec.
Result<std::unique_ptr<Decoder>> open_decoder(const std::filesystem::path& file);

/// Codec names this build can decode, for capability reports and UNSUPPORTED_CODEC messages.
std::vector<std::string> decoder_capabilities();

/// Tempo and key read from a file name. Proposals are tagged as 'filename' evidence and never
/// accepted silently.
struct FilenameHints {
    std::optional<std::int64_t> bpm_milli;  ///< tempo in thousandths of a BPM
    std::optional<MusicalKey> key;          ///< key, possibly with an unknown mode
};

/// Conservative filename heuristics: ambiguous or conflicting tokens produce no proposal.
/// `stem_utf8` is the file name without its extension, in UTF-8.
FilenameHints filename_hints(std::string_view stem_utf8);

}  // namespace mso::media
