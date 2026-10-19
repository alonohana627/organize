#pragma once
/// @file
/// The preview decoder thread: transport state machine and PCM production.
#include <cstdint>
#include <filesystem>
#include <memory>
#include <mutex>
#include <optional>
#include <stop_token>
#include <thread>
#include <variant>

#include "mso/audio/command_mailbox.hpp"
#include "mso/audio/pcm_ring.hpp"
#include "mso/audio/shared_playback_state.hpp"
#include "mso/audio/source_stream.hpp"
#include "mso/audio/wakeup_signal.hpp"
#include "mso/media/media.hpp"

namespace mso::audio {

/// Where a new source comes from: a file the decoder thread opens, or an already-opened decoder.
using SourceOrigin = std::variant<std::filesystem::path, std::unique_ptr<media::Decoder>>;

/// Owns the engine's decoder thread. Each pass it adopts the newest open() request, follows device
/// changes, applies mailbox commands in order, notices the end of the source, and tops the ring up
/// to the prebuffer target for the current epoch. All decoding and file I/O of preview happen here.
///
/// Every change that makes buffered audio wrong (open, seek, stop, loop or speed change, device
/// change, error) starts a new PCM epoch; the renderer drops blocks of older epochs unheard.
class PlaybackController {
public:
    /// Starts the thread. `prebuffer_milliseconds` bounds the audio decoded ahead of the callback.
    PlaybackController(PcmRing& ring, SharedPlaybackState& shared, CommandMailbox& mailbox, WakeupSignal& wakeup,
                       std::uint32_t prebuffer_milliseconds);

    /// Stops and joins the thread; bounded by one decoder read.
    ~PlaybackController();
    PlaybackController(const PlaybackController&) = delete;
    PlaybackController& operator=(const PlaybackController&) = delete;

    /// Control thread: replaces any not-yet-adopted request (only the newest selection matters).
    /// The caller has already published `generation` as the requested generation.
    void request_source(std::uint64_t generation, SourceOrigin origin);

private:
    struct SourceRequest {
        std::uint64_t generation = 0;
        SourceOrigin origin;
    };

    void run(std::stop_token stop);

    void adopt_requested_source();
    Result<std::unique_ptr<SourceStream>> open_stream(SourceOrigin origin);
    void publish_source_facts();
    void follow_output_device();
    void apply(const TransportCommand& command);
    void play();
    void pause();
    void stop();
    void seek_to_fraction(double fraction);
    void set_loop(const std::optional<LoopRegion>& region);
    void set_speed(double ratio);
    void set_pitch(double semitones);
    void set_playback_mode(PlaybackMode mode);
    void handle_end_of_stream();
    /// Writes at most one pass of blocks; true when more could be written right away.
    bool produce();

    /// Re-anchors the source at `source_frame` and starts a new epoch there.
    void restart_at(std::int64_t source_frame);
    void begin_epoch(std::int64_t start_frame);
    void fail_source(const Error& error);
    void set_state(PreviewState state);

    /// True while an endpoint is open and has not been lost.
    bool output_ready() const noexcept;
    /// What the listener hears now: the callback's position in the current epoch, else its start.
    std::int64_t audible_frame() const noexcept;
    /// Frames of the current epoch written but not yet played.
    std::int64_t buffered_frames() const noexcept;
    std::int64_t prebuffer_target_frames() const noexcept;

    PcmRing& ring_;
    SharedPlaybackState& shared_;
    CommandMailbox& mailbox_;
    WakeupSignal& wakeup_;
    const std::uint32_t prebuffer_milliseconds_;

    // Decoder-thread state.
    std::unique_ptr<SourceStream> stream_;
    PlaybackShape shape_;
    PreviewState state_ = PreviewState::Empty;
    std::uint64_t epoch_ = 0;
    std::int64_t produced_frames_ = 0;
    bool end_of_stream_written_ = false;
    std::uint32_t output_rate_ = 48000;  ///< production rate until a device reports its own
    std::uint64_t seen_output_configuration_ = 0;
    bool device_loss_handled_ = false;

    std::mutex request_mutex_;  ///< guards pending_request_ only; never held during I/O
    std::optional<SourceRequest> pending_request_;

    std::jthread thread_;  ///< last member: starts after, and is joined before, everything above
};

}  // namespace mso::audio
