#pragma once
/// @file
/// The preview engine's output thread: owns the audio backend and the open endpoint, serves device
/// requests, watches the endpoint, and re-opens a lost one by itself with backoff.
#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <future>
#include <memory>
#include <mutex>
#include <optional>
#include <stop_token>
#include <thread>
#include <type_traits>
#include <vector>

#include "mso/audio/detail/output_device.hpp"
#include "mso/audio/detail/output_watchdog.hpp"
#include "mso/audio/detail/playback_renderer.hpp"
#include "mso/audio/detail/shared_playback_state.hpp"
#include "mso/audio/detail/wakeup_signal.hpp"
#include "mso/audio/preview_engine.hpp"
#include "mso/domain/error.hpp"

namespace mso::audio::detail {

/// When the output thread retries a lost endpoint: `first_delay` after the loss, then twice as long
/// after every failed attempt, never waiting longer than `longest_delay`.
struct OutputRetrySchedule {
    std::chrono::milliseconds first_delay{500};       ///< wait before the first attempt
    std::chrono::milliseconds longest_delay{10'000};  ///< cap of the doubling

    /// Wait before the attempt that follows `failed_attempts` failures (0 for the first attempt).
    [[nodiscard]] std::chrono::milliseconds delay_after(std::uint32_t failed_attempts) const noexcept;
};

/// How the output thread opens and watches endpoints.
struct OutputSupervisorOptions {
    OutputBackend backend = OutputBackend::System;  ///< System or Null (Offline has no output thread)
    OutputDeviceId device;                          ///< endpoint to open first; empty = system default
    OutputDeviceSettings device_settings;           ///< rate and period of every endpoint opened
    OutputRetrySchedule retry;                      ///< automatic re-open after a loss
    OutputWatchdogSettings watchdog;                ///< when an open endpoint counts as lost
};

/// Runs every miniaudio context and device call of a System or Null engine on one thread of its own:
/// backend creation, enumeration, open, start, stop and close. That keeps the backend's per-thread
/// setup balanced whichever threads create, use and destroy the engine (WASAPI initializes COM on
/// the thread that creates the context and uninitializes it on the thread that destroys it).
///
/// Requests (list, select, restart) run there in the order they were made, at most
/// kMaximumQueuedRequests waiting at a time. The blocking forms wait for the result (enumerating or
/// re-opening WASAPI endpoints takes hundreds of milliseconds, sometimes seconds); the *_async forms
/// return at once and hand the result to a completion on the output thread.
///
/// Loss detection: the thread looks at the open endpoint (OutputWatchdog) every
/// OutputWatchdogSettings::check_interval and whenever the backend reports an event. A stop that the
/// backend announces is not a loss by itself: Windows stops a system-default endpoint while it moves
/// it to a new default device. An endpoint is lost when it stays stopped (at once for an explicitly
/// chosen one, after the reroute grace for the system default) or when its callback stalls, which
/// also catches an unplug whose failed stop announced nothing. A rerouted endpoint keeps playing and
/// its new name, period and latency are published.
///
/// When the endpoint is lost (SharedPlaybackState::device_lost), the thread keeps it if it works
/// again by the time of the next attempt, and otherwise re-opens the selected endpoint, falling back
/// to the system default, on the OutputRetrySchedule until one starts. A new or recovered endpoint
/// is published like a device switch: the decoder thread re-anchors at the audible position and
/// restores the transport (see PlaybackController).
///
/// The renderer never runs on two callbacks: a lost endpoint is closed, and a working one stopped,
/// before its replacement starts.
class OutputSupervisor final : public OutputDeviceListener {
public:
    /// Requests that may wait for the output thread at once; more are refused with RESOURCE_LIMIT.
    static constexpr std::size_t kMaximumQueuedRequests = 32;

    /// Starts the output thread, initializes the backend there and opens `options.device` (falling
    /// back to the system default). Fails with DEVICE_LOST when the audio subsystem itself is
    /// unavailable. An endpoint that cannot be opened is not a failure: it is recorded as lost and
    /// retried automatically. `renderer`, `shared` and `decoder_wakeup` must outlive the supervisor.
    [[nodiscard]] static Result<std::unique_ptr<OutputSupervisor>> start(const OutputSupervisorOptions& options,
                                                                         PlaybackRenderer& renderer,
                                                                         SharedPlaybackState& shared,
                                                                         WakeupSignal& decoder_wakeup);

    /// Completes the requests still waiting with CANCELLED, closes the endpoint and the backend on
    /// the output thread, then joins it.
    ~OutputSupervisor();
    OutputSupervisor(const OutputSupervisor&) = delete;
    OutputSupervisor& operator=(const OutputSupervisor&) = delete;

    /// Playback endpoints of the backend. Blocks until the output thread has enumerated them.
    [[nodiscard]] Result<std::vector<OutputDeviceInfo>> list_devices();

    /// Switches to `device` and remembers it as the selection. A working current endpoint keeps
    /// playing when the new one fails to open, and is restarted when it fails to start. Blocks until
    /// the output thread has done it.
    [[nodiscard]] Result<void> select(const OutputDeviceId& device);

    /// Re-opens the selected endpoint (or the system default when it is gone) whether or not the
    /// current one still works, and asks the decoder thread to bring the transport back exactly as
    /// it was, Playing included. On failure the endpoint counts as lost and automatic retries go on.
    /// Blocks until the output thread has done it.
    [[nodiscard]] Result<void> restart();

    /// list_devices() without waiting; PreviewEngine::list_output_devices_async() says when and where
    /// `on_done` runs.
    void list_devices_async(DeviceRequestCompletion<std::vector<OutputDeviceInfo>> on_done);
    /// select() without waiting (see list_devices_async()).
    void select_async(OutputDeviceId device, DeviceRequestCompletion<void> on_done);
    /// restart() without waiting (see list_devices_async()).
    void restart_async(DeviceRequestCompletion<void> on_done);

    /// Reports the endpoint as lost: raises SharedPlaybackState::device_lost and wakes the decoder
    /// thread (which flushes and remembers the audible position) and the output thread (which
    /// schedules the re-open). The output thread calls it when its watchdog finds the endpoint
    /// stopped or stalled. Safe from any thread.
    void on_device_lost() noexcept;

    /// Records a reroute and wakes the output thread to look at the endpoint at once. Called on
    /// miniaudio's threads.
    void on_output_device_event(OutputDeviceEvent event) noexcept override;

private:
    using Clock = std::chrono::steady_clock;

    OutputSupervisor(const OutputSupervisorOptions& options, PlaybackRenderer& renderer, SharedPlaybackState& shared,
                     WakeupSignal& decoder_wakeup);

    /// Runs `work` on the output thread and returns its result (exceptions included) to the caller.
    /// Refused with RESOURCE_LIMIT when the queue is full, and with INVALID_ARGUMENT on the output
    /// thread itself (it would wait for itself).
    template <class Work>
    std::invoke_result_t<Work&> run_on_output_thread(Work work);
    /// Queues `work` for the output thread and hands its result to `on_done` there (an empty
    /// `on_done` discards it). When the work cannot be queued, `on_done` receives the error on the
    /// calling thread before this returns.
    template <class Value, class Work>
    void post_to_output_thread(Work work, DeviceRequestCompletion<Value> on_done);
    /// Output thread: runs one request's work, or answers CANCELLED once the thread is shutting down.
    template <class Work>
    std::invoke_result_t<Work&> run_request(Work& work);

    /// Thread body: requests, then the retry schedule and the watchdog, then sleep until the next
    /// of these is due.
    void run(std::stop_token stop);
    /// Runs the queued requests in order until none is left or `stop` is requested.
    void run_requests(const std::stop_token& stop);
    /// Keeps (see recover_in_place()) or re-opens a lost endpoint when its attempt is due; keeps the
    /// schedule otherwise.
    void retry_if_due();
    /// Looks at the open endpoint: adopts a finished reroute, and reports the endpoint lost when the
    /// watchdog says so.
    void watch_device();
    /// Time until the next retry attempt or watchdog look is due, or a bounded idle wait.
    [[nodiscard]] std::chrono::microseconds time_until_next_wake() const;

    /// Creates the backend context and opens the first endpoint.
    [[nodiscard]] Result<void> initialize();
    /// The work of list_devices().
    [[nodiscard]] Result<std::vector<OutputDeviceInfo>> list_devices_now();
    /// The work of select().
    [[nodiscard]] Result<void> select_now(const OutputDeviceId& device);
    /// The work of restart().
    [[nodiscard]] Result<void> restart_now();
    /// Opens and starts `device` in place of the current endpoint (see select()).
    [[nodiscard]] Result<void> switch_to(const OutputDeviceId& device);
    /// switch_to() the selection, then the system default when the selection fails.
    [[nodiscard]] Result<void> reopen_selected();
    /// Keeps a lost endpoint that works again (running, its callback advancing since the loss) and
    /// is of the selected kind (the chosen endpoint, or the default while the default is selected)
    /// instead of closing it: the backend restarted a rerouted default endpoint after the grace, or
    /// a stalled driver recovered. True when it was kept.
    [[nodiscard]] bool recover_in_place();
    /// Starts watching the open endpoint.
    void start_watching();
    /// Publishes a rerouted endpoint's new name, period and latency (its stream rate never changes).
    void adopt_reroute();
    /// Publishes the open endpoint's facts and tells the decoder thread to adopt them.
    void publish_device();
    /// Records that no endpoint is usable.
    void mark_device_lost() noexcept;

    const OutputSupervisorOptions options_;       ///< backend, first endpoint, settings, retry and watchdog
    PlaybackRenderer& renderer_;                  ///< what every endpoint's callback runs
    SharedPlaybackState& shared_;                 ///< device status for the decoder thread and snapshots
    WakeupSignal& decoder_wakeup_;                ///< wakes the decoder thread on device changes
    WakeupSignal wakeup_;                         ///< wakes this thread: requests, device events, shutdown
    std::atomic<bool> reroute_announced_{false};  ///< the backend rerouted the endpoint (set on its thread)

    std::mutex requests_mutex_;                        ///< guards requests_ only; never held while one runs
    std::deque<std::packaged_task<void()>> requests_;  ///< waiting requests, at most kMaximumQueuedRequests

    // Output-thread state.
    std::unique_ptr<OutputContext> context_;          ///< the backend; null until initialize()
    std::unique_ptr<OutputDevice> device_;            ///< the open endpoint; null while none is open
    OutputDeviceId selected_;                         ///< endpoint the user chose (empty = system default)
    OutputWatchdog watchdog_;                         ///< decides when the open endpoint is lost
    bool reroute_pending_ = false;                    ///< a reroute was announced; adopted once it runs
    std::uint32_t failed_attempts_ = 0;               ///< failed re-opens since the loss
    std::optional<Clock::time_point> next_attempt_;   ///< due re-open, while lost
    std::optional<std::uint64_t> callbacks_at_loss_;  ///< the lost endpoint's callback count, for recovery
    bool cancelling_requests_ = false;                ///< shutting down: requests are answered CANCELLED

    std::jthread thread_;  ///< last member: starts after, and is joined before, everything above
};

}  // namespace mso::audio::detail
