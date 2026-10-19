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

#include "mso/audio/detail/command_mailbox.hpp"
#include "mso/audio/detail/pcm_ring.hpp"
#include "mso/audio/detail/shared_playback_state.hpp"
#include "mso/audio/detail/source_stream.hpp"
#include "mso/audio/detail/wakeup_signal.hpp"
#include "mso/media/media.hpp"

namespace mso::audio::detail {

/// Where a new source comes from: a file the decoder thread opens, or an already-opened decoder.
using SourceOrigin = std::variant<std::filesystem::path, std::unique_ptr<media::Decoder>>;

/// Owns the engine's decoder thread. Each pass it adopts the newest open() request, follows device
/// changes, applies mailbox commands in order, notices the end of the source, and tops the ring up
/// to the prebuffer target for the current epoch. All decoding and file I/O of preview happen here.
///
/// Every change that makes buffered audio wrong (open, seek, stop, loop/speed/pitch/mode change,
/// device change, error) starts a new PCM epoch; the renderer drops blocks of older epochs unheard.
/// The thread body is an exception barrier: anything thrown by a decoder or library fails the
/// current source (INTERNAL / RESOURCE_LIMIT in the snapshot) instead of terminating the process.
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
    /// A pending open() for the decoder thread.
    struct SourceRequest {
        std::uint64_t generation = 0;  ///< generation the request was published under
        SourceOrigin origin;           ///< what to open
    };

    /// Thread body: passes until stop is requested, each behind an exception barrier.
    void run(std::stop_token stop);
    /// One pass; true when more production is possible right away (no sleep).
    bool run_pass();
    /// Turns an exception escaping a pass into a failed source reporting `code`.
    void fail_from_exception(ErrorCode code) noexcept;

    /// Opens the newest pending source, if any.
    void adopt_requested_source();
    /// Opens the decoder (file or given) and wraps it in a SourceStream.
    Result<std::unique_ptr<SourceStream>> open_stream(SourceOrigin origin);
    /// Publishes rate, channels and length of the current source (zeros without one).
    void publish_source_facts();
    /// Publishes speed, pitch, mode, seam fades and the effective loop.
    void publish_settings();
    /// Reacts to a lost endpoint and to a newly opened one.
    void follow_output_device();
    /// Applies one mailbox command.
    void apply(const TransportCommand& command);
    void play();   ///< Play command
    void pause();  ///< Pause command
    void stop();   ///< Stop command: rewinds to 0
    /// Seek to a fraction of the known length (ignored when the length is unknown, except 0).
    void seek_to_fraction(double fraction);
    void set_loop(const std::optional<LoopRegion>& region);  ///< SetLoop command
    void set_speed(double ratio);                            ///< SetSpeed command
    void set_pitch(double semitones);                        ///< SetPitch command
    void set_playback_mode(PlaybackMode mode);               ///< SetPlaybackMode command
    void set_seam_fades(bool enabled);                       ///< SetSeamFades command
    /// Stops at the end of the source once the callback has played its last block.
    void handle_end_of_stream();
    /// Writes at most one pass of blocks; true when more could be written right away.
    bool produce();

    /// Re-anchors the source at `source_frame` and starts a new epoch there.
    void restart_at(std::int64_t source_frame);
    /// Starts a new epoch beginning at `start_frame` (flushes everything buffered).
    void begin_epoch(std::int64_t start_frame) noexcept;
    /// Switches the callback's output on or off (published with the epoch, see TransportGate).
    void set_output_enabled(bool enabled) noexcept;
    /// Drops the source, flushes, and reports `code`; replay needs a new open().
    void fail_source(ErrorCode code) noexcept;
    /// Publishes the transport state.
    void set_state(PreviewState state) noexcept;

    /// True while an endpoint is open and has not been lost.
    bool output_ready() const noexcept;
    /// What the listener hears now: the callback's audible position in the current epoch, else its start.
    std::int64_t audible_frame() const noexcept;
    /// Frames of the current epoch written but not yet played.
    std::int64_t buffered_frames() const noexcept;
    /// Prebuffer goal in frames at the current output rate.
    std::int64_t prebuffer_target_frames() const noexcept;

    PcmRing& ring_;                               ///< blocks to the callback
    SharedPlaybackState& shared_;                 ///< published status
    CommandMailbox& mailbox_;                     ///< transport commands from the control thread
    WakeupSignal& wakeup_;                        ///< wakes this thread early
    const std::uint32_t prebuffer_milliseconds_;  ///< prebuffer goal

    // Decoder-thread state.
    std::unique_ptr<SourceStream> stream_;         ///< current source; null when empty or failed
    PlaybackShape shape_;                          ///< loop/speed/pitch/mode/seam settings
    PreviewState state_ = PreviewState::Empty;     ///< transport state
    TransportGate gate_;                           ///< epoch and output switch last published
    std::int64_t produced_frames_ = 0;             ///< frames written for the current epoch
    bool end_of_stream_written_ = false;           ///< the current epoch's last block is in the ring
    std::uint32_t output_rate_ = 48000;            ///< production rate until a device reports its own
    std::uint64_t seen_output_configuration_ = 0;  ///< last output_configuration adopted
    /// State to return to once a lost endpoint is replaced (Playing comes back as Paused).
    std::optional<PreviewState> state_before_device_loss_;

    std::mutex request_mutex_;  ///< guards pending_request_ only; never held during I/O or destruction
    std::optional<SourceRequest> pending_request_;  ///< newest open() not yet adopted

    std::jthread thread_;  ///< last member: starts after, and is joined before, everything above
};

}  // namespace mso::audio::detail
