// miniaudio-backed streaming decoder (dr_wav, dr_flac, dr_mp3), reading through our own FileHandle.
#include <algorithm>
#include <array>
#include <cstddef>
#include <format>
#include <limits>
#include <new>
#include <optional>
#include <type_traits>

#include <miniaudio.h>

#include "mso/domain/core.hpp"
#include "mso/media/detail/decoder_backends.hpp"
#include "mso/media/detail/file_handle.hpp"

namespace mso::media::detail {

namespace {

/// Codecs dr_wav decodes from WAV-family and AIFF containers.
constexpr std::array<std::string_view, 11> kWaveCodecs = {
    "pcm_u8",  "pcm_s8",   "pcm_s16",   "pcm_s24",  "pcm_s32",   "pcm_f32",
    "pcm_f64", "pcm_alaw", "pcm_mulaw", "adpcm_ms", "adpcm_ima",
};
constexpr std::array<std::string_view, 5> kWaveContainers = {"wav", "rf64", "w64", "aiff", "aifc"};
constexpr std::array<std::string_view, 3> kMpegCodecs = {"mp3", "mp2", "mp1"};

/// True when `value` is one of the entries of `list`.
bool contains(const auto& list, std::string_view value) {
    return std::ranges::find(list, value) != list.end();
}

/// The miniaudio decoding backend for a probed stream; ma_encoding_format_unknown if none.
ma_encoding_format encoding_for(const ProbeResult& probe) {
    if (contains(kWaveContainers, probe.format) && contains(kWaveCodecs, probe.codec)) return ma_encoding_format_wav;
    if (probe.format == "flac" && probe.codec == "flac") return ma_encoding_format_flac;
    if (probe.format == "mp3" && contains(kMpegCodecs, probe.codec)) return ma_encoding_format_mp3;
    return ma_encoding_format_unknown;
}

/// Maps a miniaudio result to the error taxonomy; `action` names what failed.
Error error_for(ma_result result, std::string_view action) {
    switch (result) {
        case MA_DOES_NOT_EXIST:
            return fail(ErrorCode::FileMissing, "file does not exist").error();
        case MA_ACCESS_DENIED:
            return fail(ErrorCode::AccessDenied, "access to the file was denied").error();
        case MA_IO_ERROR:
            return fail(ErrorCode::AccessDenied, std::format("{} failed: the file could not be read", action), true)
                .error();
        case MA_OUT_OF_MEMORY:
            return fail(ErrorCode::ResourceLimit, "out of memory while decoding").error();
        default:
            return fail(ErrorCode::CorruptAudio, std::format("{} failed: {}", action, ma_result_description(result)))
                .error();
    }
}

/// A file opened through PreopenedFileSystem, as miniaudio sees it (an ma_vfs_file).
struct OpenFile {
    FileHandle handle;
    std::uint64_t size = 0;
    std::uint64_t position = 0;
};

/// A miniaudio virtual file system whose only file is one FileHandle opened beforehand, so the
/// decoder reads with our sharing mode (read/write/delete) and our path handling. The path given to
/// ma_decoder_init_vfs is only a label; the first open hands over the pre-opened handle.
struct PreopenedFileSystem {
    ma_vfs_callbacks callbacks;         ///< must stay the first member: miniaudio casts ma_vfs* to it
    std::optional<FileHandle> pending;  ///< the handle until miniaudio opens it
    std::uint64_t size = 0;
    /// The file miniaudio opened and has not closed yet. ma_decoder_init_vfs returns without closing
    /// it when a forced encoding fails to initialise, so the destructor releases it then.
    OpenFile* outstanding = nullptr;

    /// Gives the pre-opened handle to miniaudio. Write access and second opens are refused.
    static ma_result open(ma_vfs* file_system, const char* /*label*/, ma_uint32 mode, ma_vfs_file* file) {
        auto* self = static_cast<PreopenedFileSystem*>(file_system);
        if (file == nullptr) return MA_INVALID_ARGS;
        *file = nullptr;
        if ((mode & MA_OPEN_MODE_WRITE) != 0) return MA_ACCESS_DENIED;
        if (!self->pending) return MA_DOES_NOT_EXIST;
        auto* opened = new (std::nothrow) OpenFile{std::move(*self->pending), self->size, 0};
        if (opened == nullptr) return MA_OUT_OF_MEMORY;
        self->pending.reset();
        self->outstanding = opened;
        *file = opened;
        return MA_SUCCESS;
    }

    /// Closes the file handed out by open().
    static ma_result close(ma_vfs* file_system, ma_vfs_file file) {
        auto* self = static_cast<PreopenedFileSystem*>(file_system);
        if (self->outstanding == file) self->outstanding = nullptr;
        delete static_cast<OpenFile*>(file);
        return MA_SUCCESS;
    }

    /// stdio semantics: a short read is success; nothing at all at the end is MA_AT_END.
    static ma_result read(ma_vfs* /*file_system*/, ma_vfs_file file, void* destination, size_t byte_count,
                          size_t* bytes_read) {
        auto* opened = static_cast<OpenFile*>(file);
        if (bytes_read != nullptr) *bytes_read = 0;
        const auto received =
            opened->handle.read_at(opened->position, {static_cast<std::uint8_t*>(destination), byte_count});
        if (!received) return MA_IO_ERROR;
        opened->position += *received;
        if (bytes_read != nullptr) *bytes_read = *received;
        return *received == 0 && byte_count > 0 ? MA_AT_END : MA_SUCCESS;
    }

    /// Moves the read position like fseek (positions past the end are allowed).
    static ma_result seek(ma_vfs* /*file_system*/, ma_vfs_file file, ma_int64 offset, ma_seek_origin origin) {
        auto* opened = static_cast<OpenFile*>(file);
        std::int64_t base = 0;
        switch (origin) {
            case ma_seek_origin_start:
                base = 0;
                break;
            case ma_seek_origin_current:
                base = static_cast<std::int64_t>(opened->position);
                break;
            case ma_seek_origin_end:
                base = static_cast<std::int64_t>(opened->size);
                break;
        }
        std::int64_t target = 0;
        if (__builtin_add_overflow(base, offset, &target) || target < 0) return MA_BAD_SEEK;
        opened->position = static_cast<std::uint64_t>(target);
        return MA_SUCCESS;
    }

    /// Reports the read position.
    static ma_result tell(ma_vfs* /*file_system*/, ma_vfs_file file, ma_int64* cursor) {
        const auto* opened = static_cast<const OpenFile*>(file);
        if (cursor == nullptr || opened->position > static_cast<std::uint64_t>(std::numeric_limits<ma_int64>::max())) {
            return MA_INVALID_ARGS;
        }
        *cursor = static_cast<ma_int64>(opened->position);
        return MA_SUCCESS;
    }

    /// Reports the file size measured when it was opened.
    static ma_result info(ma_vfs* /*file_system*/, ma_vfs_file file, ma_file_info* information) {
        if (information == nullptr) return MA_INVALID_ARGS;
        information->sizeInBytes = static_cast<const OpenFile*>(file)->size;
        return MA_SUCCESS;
    }

    PreopenedFileSystem(FileHandle handle, std::uint64_t file_size)
        : callbacks{&PreopenedFileSystem::open,
                    nullptr,
                    &PreopenedFileSystem::close,
                    &PreopenedFileSystem::read,
                    nullptr,
                    &PreopenedFileSystem::seek,
                    &PreopenedFileSystem::tell,
                    &PreopenedFileSystem::info},
          pending(std::move(handle)),
          size(file_size) {}
    PreopenedFileSystem(const PreopenedFileSystem&) = delete;
    PreopenedFileSystem& operator=(const PreopenedFileSystem&) = delete;
    ~PreopenedFileSystem() { delete outstanding; }
};
// miniaudio casts the ma_vfs* it is given to ma_vfs_callbacks*: that is only valid for a
// standard-layout type whose first member is the callback table.
static_assert(std::is_standard_layout_v<PreopenedFileSystem>);
static_assert(offsetof(PreopenedFileSystem, callbacks) == 0);

/// Streams float32 frames from an ma_decoder. Not movable: miniaudio keeps pointers to the
/// decoder and to the file system.
class MiniaudioDecoder final : public Decoder {
public:
    MiniaudioDecoder(FileHandle file, std::uint64_t file_size) : file_system_(std::move(file), file_size) {}
    MiniaudioDecoder(const MiniaudioDecoder&) = delete;
    MiniaudioDecoder& operator=(const MiniaudioDecoder&) = delete;
    ~MiniaudioDecoder() override {
        if (initialized_) ma_decoder_uninit(&decoder_);
    }

    /// Opens the backend chosen from the probe, at the native rate and channel count.
    Result<void> open(const std::filesystem::path& file, const ProbeResult& probe) {
        const ma_encoding_format encoding = encoding_for(probe);
        ma_decoder_config config = ma_decoder_config_init(ma_format_f32, 0, 0);
        config.encodingFormat = encoding;
        const std::string label = path_to_utf8(file);
        const ma_result opened = ma_decoder_init_vfs(&file_system_, label.c_str(), &config, &decoder_);
        if (opened != MA_SUCCESS) return std::unexpected(error_for(opened, "opening the decoder"));
        initialized_ = true;

        ma_format format = ma_format_unknown;
        ma_uint32 channels = 0;
        ma_uint32 sample_rate = 0;
        const ma_result described = ma_decoder_get_data_format(&decoder_, &format, &channels, &sample_rate, nullptr, 0);
        constexpr auto kIntMaximum = static_cast<ma_uint32>(std::numeric_limits<int>::max());
        if (described != MA_SUCCESS || channels == 0 || sample_rate == 0 || channels > kIntMaximum ||
            sample_rate > kIntMaximum) {
            return fail(ErrorCode::CorruptAudio, "decoder reported no audio format");
        }
        channels_ = static_cast<int>(channels);
        sample_rate_ = static_cast<int>(sample_rate);
        codec_ = probe.codec;
        determine_length(encoding, probe);
        return {};
    }

    int channels() const override { return channels_; }
    int sample_rate() const override { return sample_rate_; }
    std::optional<std::int64_t> total_frames() const override { return total_frames_; }
    bool frames_exact() const override { return frames_exact_; }
    std::int64_t position() const override { return position_; }
    std::string codec() const override { return codec_; }

    Result<std::size_t> read(float* out, std::size_t frames) override {
        if (frames == 0 || at_end_) return std::size_t{0};
        ma_uint64 frames_read = 0;
        const ma_result result = ma_decoder_read_pcm_frames(&decoder_, out, frames, &frames_read);
        if (result != MA_SUCCESS && result != MA_AT_END) return std::unexpected(error_for(result, "decoding"));
        position_ += static_cast<std::int64_t>(frames_read);
        return static_cast<std::size_t>(frames_read);
    }

    Result<void> seek(std::int64_t frame) override {
        if (frame < 0) return fail(ErrorCode::InvalidArgument, "seek position is negative");
        at_end_ = false;
        if (frames_exact_ && total_frames_ && frame >= *total_frames_) {
            at_end_ = true;  // at or past the exact end: positioned at the end, reads return 0
            position_ = *total_frames_;
            return {};
        }
        const ma_result result = ma_decoder_seek_to_pcm_frame(&decoder_, static_cast<ma_uint64>(frame));
        if (result != MA_SUCCESS) {
            // Without an exact length, a target past the end is only discovered by the backend.
            if (!frames_exact_ && total_frames_ && frame >= *total_frames_) {
                at_end_ = true;
                position_ = frame;
                return {};
            }
            return std::unexpected(error_for(result, "seeking"));
        }
        // Report where the backend actually landed: an estimate-based target may lie past the end.
        ma_uint64 cursor = 0;
        position_ = ma_decoder_get_cursor_in_pcm_frames(&decoder_, &cursor) == MA_SUCCESS
                        ? static_cast<std::int64_t>(std::min<ma_uint64>(cursor, static_cast<ma_uint64>(frame)))
                        : frame;
        if (position_ < frame) at_end_ = true;
        return {};
    }

private:
    /// WAV and FLAC lengths come from headers. For MP3, asking dr_mp3 without a Xing/Info frame
    /// count would scan the whole file, so the probe's exact count or estimate is used instead.
    void determine_length(ma_encoding_format encoding, const ProbeResult& probe) {
        if (encoding == ma_encoding_format_mp3) {
            if (probe.frames) {
                total_frames_ = probe.frames;
                frames_exact_ = true;
            } else if (probe.duration_us && *probe.duration_us > 0) {
                // An estimate from the bitrate; absurd values saturate instead of overflowing.
                std::int64_t scaled = 0;
                total_frames_ = __builtin_mul_overflow(*probe.duration_us, std::int64_t{sample_rate_}, &scaled)
                                    ? std::numeric_limits<std::int64_t>::max() / 1'000'000
                                    : scaled / 1'000'000;
            }
            return;
        }
        ma_uint64 length = 0;
        if (ma_decoder_get_length_in_pcm_frames(&decoder_, &length) != MA_SUCCESS) return;
        if (length == 0 && encoding == ma_encoding_format_flac) return;  // STREAMINFO total 0: unknown
        total_frames_ = static_cast<std::int64_t>(length);
        frames_exact_ = true;
    }

    PreopenedFileSystem file_system_;  ///< declared first: the decoder reads through it until uninit
    ma_decoder decoder_{};
    bool initialized_ = false;
    bool at_end_ = false;  ///< positioned at the end by a seek; read() returns 0 until the next seek
    int channels_ = 0;
    int sample_rate_ = 0;
    std::optional<std::int64_t> total_frames_;
    bool frames_exact_ = false;
    std::int64_t position_ = 0;
    std::string codec_;
};

}  // namespace

bool miniaudio_can_decode(const ProbeResult& probe) {
    return encoding_for(probe) != ma_encoding_format_unknown;
}

std::vector<std::string> miniaudio_codecs() {
    std::vector<std::string> codecs(kWaveCodecs.begin(), kWaveCodecs.end());
    codecs.emplace_back("flac");
    codecs.insert(codecs.end(), kMpegCodecs.begin(), kMpegCodecs.end());
    return codecs;
}

Result<std::unique_ptr<Decoder>> open_miniaudio_decoder(const std::filesystem::path& file, const ProbeResult& probe) {
    auto handle = FileHandle::open_for_read(file);
    if (!handle) return std::unexpected(handle.error());
    const auto size = handle->size();
    if (!size) return std::unexpected(size.error());
    auto decoder = std::make_unique<MiniaudioDecoder>(std::move(*handle), *size);
    if (auto opened = decoder->open(file, probe); !opened) return std::unexpected(opened.error());
    return std::unique_ptr<Decoder>(std::move(decoder));
}

}  // namespace mso::media::detail
