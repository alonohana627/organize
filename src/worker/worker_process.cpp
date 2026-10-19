/// @file
/// Supervisor side of the worker protocol: spawning through the platform layer, the handshake,
/// fencing of outcomes, and the watchdog that enforces the handshake, lease, task-stall and
/// cancellation deadlines.
#include "mso/worker/worker_process.hpp"

#include <algorithm>
#include <atomic>
#include <condition_variable>
#include <deque>
#include <exception>
#include <format>
#include <mutex>
#include <stop_token>
#include <thread>
#include <utility>

#include "mso/domain/overloaded.hpp"
#include "mso/worker/detail/worker_platform.hpp"

namespace mso::worker {

namespace {

using Clock = std::chrono::steady_clock;
using mso::Overloaded;

/// Smallest accepted memory cap; below this the worker cannot even load.
constexpr std::uint64_t kMinimumMemoryLimitBytes = 64ull * 1024 * 1024;
/// Outgoing frames waiting for the writer thread. Each is a small control message.
constexpr std::size_t kMaximumQueuedFrames = 64;
/// Supervisor events waiting for the owner. Progress beyond this is dropped; outcomes never are.
constexpr std::size_t kMaximumQueuedEvents = 1024;
/// Period of the watchdog that enforces handshake, lease, stall and cancellation deadlines.
constexpr std::chrono::milliseconds kWatchdogPeriod{100};
/// How long to wait for a dead or closing worker to finish exiting.
constexpr std::chrono::milliseconds kExitWait{5000};
/// How long the reader may stay blocked after the child has exited (possible on Windows when
/// another process inherited the pipe) before its read is cancelled.
constexpr std::chrono::milliseconds kOrphanedReadGrace{500};
/// After a malformed frame, how long to wait to tell a dying worker from a misbehaving one.
constexpr std::chrono::milliseconds kViolationExitProbe{200};
/// Deadlines that must outlast at least two heartbeats, so one late heartbeat never kills a
/// healthy worker.
constexpr std::chrono::milliseconds kShortestHeartbeatDeadline = 2 * kHeartbeatInterval;

/// The Error value of a failure, for places that store rather than return it.
Error error_of(ErrorCode code, std::string detail, bool retryable = false) {
    return fail(code, std::move(detail), retryable).error();
}

/// Rejects option values that would make supervision meaningless or unsafe.
Result<void> validate_options(const WorkerOptions& options) {
    if (options.cpu_rate_percent > 100)
        return fail(ErrorCode::InvalidArgument, "CPU rate cap must be within [0, 100] percent");
    if (options.memory_limit_bytes < kMinimumMemoryLimitBytes) {
        return fail(ErrorCode::InvalidArgument,
                    std::format("worker memory limit must be at least {}",
                                format_byte_size(static_cast<std::int64_t>(kMinimumMemoryLimitBytes))));
    }
    for (const auto& argument : options.arguments) {
        const bool plain_token = !argument.empty() && std::ranges::all_of(argument, [](char character) {
            return character > ' ' && character < 0x7F && character != '"' && character != '\'';
        });
        if (!plain_token)
            return fail(ErrorCode::InvalidArgument, "worker arguments must be printable ASCII tokens without quotes");
    }
    const auto positive = [](std::chrono::milliseconds duration) {
        return duration > std::chrono::milliseconds::zero();
    };
    if (!positive(options.handshake_timeout) || !positive(options.cancel_grace) || !positive(options.shutdown_grace))
        return fail(ErrorCode::InvalidArgument, "worker handshake, cancel and shutdown timeouts must be positive");
    if (options.lease_deadline < kShortestHeartbeatDeadline ||
        options.task_stall_deadline < kShortestHeartbeatDeadline) {
        return fail(ErrorCode::InvalidArgument,
                    std::format("worker lease and stall deadlines must be at least {} ms (two heartbeats)",
                                kShortestHeartbeatDeadline.count()));
    }
    return {};
}

/// Writes frames to the worker on a dedicated thread, so a worker that stopped reading can
/// never block a supervisor caller; the watchdog's termination then unblocks the write.
///
/// A mutex and a deque on purpose: frames come from any supervisor thread (submit, cancel,
/// shutdown), so the vendored single-producer moodycamel::ReaderWriterQueue does not fit, and the
/// sender also needs a "channel broken" state and a stop_token-aware blocking wait that a
/// lock-free queue would have to rebuild around it. At most 64 small control frames per task
/// make this no hot path.
class FrameSender {
public:
    explicit FrameSender(NativeHandle pipe) : pipe_(pipe), thread_([this](std::stop_token stop) { run(stop); }) {}
    FrameSender(const FrameSender&) = delete;
    FrameSender& operator=(const FrameSender&) = delete;
    ~FrameSender() { stop(); }

    /// Queues one encoded frame. WORKER_CRASH once the channel broke, RESOURCE_LIMIT when the
    /// backlog is full.
    Result<void> post(std::vector<std::uint8_t> frame) {
        {
            std::lock_guard lock(mutex_);
            if (broken_) return fail(ErrorCode::WorkerCrash, "worker channel is closed", true);
            if (queue_.size() >= kMaximumQueuedFrames)
                return fail(ErrorCode::ResourceLimit, "worker channel backlog is full", true);
            queue_.push_back(std::move(frame));
        }
        pending_.notify_one();
        return {};
    }

    /// Stops the thread; frames still queued are dropped.
    void stop() {
        thread_.request_stop();
        if (thread_.joinable()) thread_.join();
    }

private:
    /// Thread body: writes queued frames in order until stopped or the pipe breaks.
    void run(std::stop_token stop) {
        try {
            while (true) {
                std::vector<std::uint8_t> frame;
                {
                    std::unique_lock lock(mutex_);
                    if (!pending_.wait(lock, stop, [&] { return !queue_.empty(); })) return;
                    frame = std::move(queue_.front());
                    queue_.pop_front();
                }
                if (!pipe_.write_all(frame)) {
                    mark_broken();
                    return;
                }
            }
        } catch (const std::exception&) {
            // Only allocation can throw here; the channel is unusable either way, and the
            // watchdog reports the worker once it stops answering.
            mark_broken();
        }
    }

    /// Records that the pipe is unusable and drops queued frames.
    void mark_broken() {
        std::lock_guard lock(mutex_);
        broken_ = true;
        queue_.clear();
    }

    HandleStream pipe_;
    std::mutex mutex_;
    std::condition_variable_any pending_;
    std::deque<std::vector<std::uint8_t>> queue_;
    bool broken_ = false;
    std::jthread thread_;  // declared last: starts after, and stops before, the members it uses
};

/// Whether the stages a task requested and the parts a result carries correspond: each result
/// part is present exactly when its stage was requested, and the waveform segment names the
/// task's output path and holds no more bins than the task asked for.
Result<void> validate_result_for_task(const TaskResult& result, const std::vector<analysis::Stage>& stages,
                                      const std::string& waveform_output_path, int waveform_bins) {
    const auto requested = [&](analysis::Stage stage) { return std::ranges::contains(stages, stage); };
    const auto& report = result.report;
    const bool matches = report.loudness.has_value() == requested(analysis::Stage::Loudness) &&
                         report.tempo.has_value() == requested(analysis::Stage::Tempo) &&
                         report.key.has_value() == requested(analysis::Stage::Key) &&
                         report.hashes.byte_sha256.has_value() == requested(analysis::Stage::ByteHash) &&
                         report.hashes.pcm_sha256.has_value() == requested(analysis::Stage::PcmHash) &&
                         result.waveform_segment.has_value() == requested(analysis::Stage::Waveform);
    if (!matches) return fail(ErrorCode::ParseError, "result parts do not match the requested stages");
    if (result.waveform_segment && result.waveform_segment->path != waveform_output_path)
        return fail(ErrorCode::ParseError, "waveform segment names a path the task did not ask for");
    if (result.waveform_segment && result.waveform_segment->bins > waveform_bins)
        return fail(ErrorCode::ParseError, "waveform segment holds more bins than the task asked for");
    return {};
}

}  // namespace

// ---------------------------------------------------------------- UniqueHandle (shared part)

UniqueHandle& UniqueHandle::operator=(UniqueHandle&& other) noexcept {
    if (this != &other) reset(other.release());
    return *this;
}

NativeHandle UniqueHandle::release() noexcept {
    return std::exchange(handle_, kNoNativeHandle);
}

// ---------------------------------------------------------------- WorkerProcess::EventQueue

/// Bounded queue of events for the owner of a WorkerProcess, with a drop policy instead of
/// back-pressure: the reader thread must never block on a slow consumer.
///
/// A mutex and a deque on purpose: the drop policy removes the oldest Progress event from the
/// middle of the queue to make room for an outcome, which no lock-free FIFO (the vendored
/// moodycamel queues included) supports, and the consumer needs a timed blocking pop.
class WorkerProcess::EventQueue {
public:
    /// Appends an event. When the queue is full a Progress event is dropped (the incoming one,
    /// or the oldest queued one to make room for an outcome); outcomes and exits never are.
    void push(WorkerEvent event) {
        {
            std::lock_guard lock(mutex_);
            if (events_.size() >= kMaximumQueuedEvents) {
                if (std::holds_alternative<Progress>(event)) return;
                const auto oldest_progress = std::ranges::find_if(
                    events_, [](const WorkerEvent& queued) { return std::holds_alternative<Progress>(queued); });
                if (oldest_progress != events_.end()) events_.erase(oldest_progress);
            }
            events_.push_back(std::move(event));
        }
        available_.notify_one();
    }

    /// The oldest event, waiting up to `timeout`; nullopt on timeout.
    std::optional<WorkerEvent> pop(std::chrono::milliseconds timeout) {
        std::unique_lock lock(mutex_);
        if (!available_.wait_for(lock, timeout, [&] { return !events_.empty(); })) return std::nullopt;
        auto event = std::move(events_.front());
        events_.pop_front();
        return event;
    }

private:
    std::mutex mutex_;
    std::condition_variable available_;
    std::deque<WorkerEvent> events_;
};

// ---------------------------------------------------------------- WorkerProcess::Session

/// One spawned worker and three threads: the reader turns frames into events, the watchdog
/// enforces deadlines by cancelling or terminating, and the FrameSender writes. Every way the
/// process can end funnels into on_channel_closed(), which reports it exactly once.
class WorkerProcess::Session {
public:
    /// Spawns, confines and handshakes a worker.
    static Result<std::unique_ptr<Session>> launch(const WorkerOptions& options, EventQueue& events);

    /// Graceful shutdown, then joins every thread.
    ~Session();
    Session(const Session&) = delete;
    Session& operator=(const Session&) = delete;

    Handshake handshake() const {
        std::lock_guard lock(mutex_);
        return handshake_.value_or(Handshake{});
    }
    const LimitsStatus& limits_status() const { return child_->limits(); }
    std::uint32_t process_id() const { return child_->process_id(); }
    bool is_running() const {
        std::lock_guard lock(mutex_);
        return !exited_ && !shutting_down_ && !termination_reason_;
    }
    bool is_busy() const {
        std::lock_guard lock(mutex_);
        return task_.has_value();
    }
    std::uint64_t heartbeats_received() const { return heartbeats_.load(); }
    std::optional<TaskActivity> task_activity() const;

    Result<void> submit(const AnalyzeTask& task);
    Result<void> cancel(const uuids::uuid& job_id, std::int64_t cancel_epoch);
    void shutdown();

private:
    /// The task the worker currently owns, as the supervisor sent it.
    struct TaskInFlight {
        TaskFence fence;
        std::uint64_t request_sequence = 0;   ///< sequence of the AnalyzeTask frame; outcomes echo it
        std::vector<analysis::Stage> stages;  ///< the stages requested, which the result must match
        std::string waveform_output_path;     ///< the only path a waveform segment may name
        int waveform_bins = 0;                ///< upper bound of the segment's bin count
        std::uint64_t work_units = 0;         ///< last work counter the worker reported
        Clock::time_point last_progress_at;
        std::optional<Clock::time_point> cancel_deadline;  ///< set once a cancel was sent
        std::optional<Error> stall_failure;                ///< set when the watchdog cancelled a stall
    };

    /// What the watchdog decided, carried out after it released the lock.
    struct WatchdogAction {
        std::optional<Error> terminate_reason;
        std::optional<Cancel> stall_cancel;
    };

    Session(const WorkerOptions& options, EventQueue& events, std::unique_ptr<platform::ChildProcess> child);

    /// Blocks until the handshake arrived or the process ended; the watchdog bounds the wait.
    Result<void> await_handshake();
    /// Encodes `message`, numbers it with the next request sequence and queues it for the writer.
    /// Requires mutex_, which keeps sequence order and queue order identical across threads.
    Result<std::uint64_t> send_locked(const SupervisorMessage& message);
    /// send_locked() under the lock.
    Result<std::uint64_t> send(const SupervisorMessage& message);

    /// Reader thread body.
    void read_loop();
    /// Watchdog thread body.
    void watch_loop(std::stop_token stop);
    /// Applies one incoming message that arrived with `request_sequence`. An error is a protocol
    /// violation. Takes the lock.
    Result<void> handle_message(WorkerMessage message, std::uint64_t request_sequence);
    /// The following require mutex_ to be held.
    Result<void> check_sequence(const WorkerMessage& message, std::uint64_t request_sequence) const;
    Result<void> accept_handshake(Handshake handshake);
    void accept_heartbeat(const Heartbeat& heartbeat);
    void accept_progress(const Progress& progress);
    Result<void> accept_outcome(WorkerEvent outcome, const TaskFence& fence);
    WatchdogAction check_deadlines(Clock::time_point now);

    /// Records why the worker is being killed (the first reason wins) and terminates it.
    void terminate(Error reason);
    /// Reports the end of the process: the in-flight task's outcome, then WorkerExited.
    void on_channel_closed(std::optional<Error> violation);

    const WorkerOptions options_;
    EventQueue& events_;
    const std::unique_ptr<platform::ChildProcess> child_;
    const Clock::time_point spawned_at_;

    mutable std::mutex mutex_;
    std::condition_variable_any state_changed_;
    std::optional<Handshake> handshake_;
    std::optional<TaskInFlight> task_;
    Clock::time_point last_frame_at_;
    std::optional<Clock::time_point> process_gone_at_;
    std::optional<Error> termination_reason_;
    std::optional<Error> exit_failure_;
    bool shutting_down_ = false;
    bool exited_ = false;
    std::uint64_t next_sequence_ = 1;
    std::mutex shutdown_mutex_;
    std::atomic<std::uint64_t> heartbeats_{0};

    FrameSender sender_;
    std::jthread reader_;
    std::jthread watchdog_;
};

Result<std::unique_ptr<WorkerProcess::Session>> WorkerProcess::Session::launch(const WorkerOptions& options,
                                                                               EventQueue& events) {
    if (auto valid = validate_options(options); !valid) return std::unexpected(valid.error());
    // Absolute before spawning: the POSIX child changes its working directory before exec, so a
    // relative path would resolve against the wrong directory there.
    std::error_code path_error;
    const auto executable = std::filesystem::absolute(
        options.executable.empty() ? default_worker_executable() : options.executable, path_error);
    if (path_error) return fail(ErrorCode::WorkerCrash, "cannot resolve the worker executable path");
    std::error_code status_error;
    if (!std::filesystem::is_regular_file(executable, status_error)) {
        return fail(ErrorCode::WorkerCrash, std::format("worker executable {} not found", path_to_utf8(executable)));
    }
    auto child = platform::ChildProcess::spawn(executable, options);
    if (!child) return std::unexpected(child.error());

    std::unique_ptr<Session> session(new Session(options, events, std::move(*child)));
    if (auto ready = session->await_handshake(); !ready) return std::unexpected(ready.error());
    return session;
}

WorkerProcess::Session::Session(const WorkerOptions& options, EventQueue& events,
                                std::unique_ptr<platform::ChildProcess> child)
    : options_(options),
      events_(events),
      child_(std::move(child)),
      spawned_at_(Clock::now()),
      last_frame_at_(spawned_at_),
      sender_(child_->to_child()) {
    reader_ = std::jthread([this] { read_loop(); });
    watchdog_ = std::jthread([this](std::stop_token stop) { watch_loop(stop); });
}

WorkerProcess::Session::~Session() {
    shutdown();
    watchdog_.request_stop();
    if (watchdog_.joinable()) watchdog_.join();
    sender_.stop();
}

std::optional<TaskActivity> WorkerProcess::Session::task_activity() const {
    std::lock_guard lock(mutex_);
    if (!task_) return std::nullopt;
    return TaskActivity{task_->fence.job_id, task_->work_units, Clock::now() - task_->last_progress_at,
                        task_->cancel_deadline.has_value()};
}

Result<void> WorkerProcess::Session::await_handshake() {
    std::unique_lock lock(mutex_);
    state_changed_.wait(lock, [&] { return handshake_.has_value() || exited_; });
    if (handshake_) return {};
    const auto failure = exit_failure_.value_or(error_of(ErrorCode::WorkerCrash, "worker exited", true));
    return fail(ErrorCode::WorkerCrash, std::format("worker handshake failed: {}", failure.message()),
                failure.retryable);
}

Result<std::uint64_t> WorkerProcess::Session::send_locked(const SupervisorMessage& message) {
    const auto payload = encode_message(message);
    if (!payload) return std::unexpected(payload.error());
    auto frame = encode_frame(next_sequence_, *payload);
    if (!frame) return std::unexpected(frame.error());
    if (auto posted = sender_.post(std::move(*frame)); !posted) return std::unexpected(posted.error());
    return next_sequence_++;
}

Result<std::uint64_t> WorkerProcess::Session::send(const SupervisorMessage& message) {
    std::lock_guard lock(mutex_);
    return send_locked(message);
}

Result<void> WorkerProcess::Session::submit(const AnalyzeTask& task) {
    if (auto valid = validate_task(task); !valid) return valid;
    std::lock_guard lock(mutex_);
    if (exited_ || shutting_down_ || termination_reason_) {
        return fail(ErrorCode::WorkerCrash, "worker is not running", true);
    }
    if (task_) return fail(ErrorCode::InvalidArgument, "the worker already has a task in flight");
    // Registered before the frame can reach the worker, so an instant outcome finds its task.
    const auto sequence = send_locked(task);
    if (!sequence) return std::unexpected(sequence.error());
    task_ =
        TaskInFlight{TaskFence::of(task), *sequence,    task.stages, task.waveform_output_path, task.waveform_bins, 0,
                     Clock::now(),        std::nullopt, std::nullopt};
    return {};
}

Result<void> WorkerProcess::Session::cancel(const uuids::uuid& job_id, std::int64_t cancel_epoch) {
    std::lock_guard lock(mutex_);
    if (!task_ || task_->fence.job_id != job_id)
        return fail(ErrorCode::NotFound, "no task with that job id is in flight");
    if (!task_->cancel_deadline) task_->cancel_deadline = Clock::now() + options_.cancel_grace;
    // Even if this frame cannot be delivered the armed deadline still ends the task.
    const auto sent = send_locked(Cancel{job_id, cancel_epoch});
    if (!sent) return std::unexpected(sent.error());
    return {};
}

void WorkerProcess::Session::shutdown() {
    std::lock_guard serialized(shutdown_mutex_);
    bool already_exited = false;
    {
        std::lock_guard lock(mutex_);
        already_exited = exited_;
        shutting_down_ = true;
    }
    if (!already_exited) {
        // A Shutdown frame that cannot be queued (channel already broken) changes nothing: the
        // bounded wait below and the termination after it end the process either way.
        (void)send(Shutdown{});
        if (!child_->wait_for_exit(options_.shutdown_grace)) child_->terminate();
    }
    if (reader_.joinable()) reader_.join();
}

void WorkerProcess::Session::read_loop() {
    try {
        child_->register_reader_thread();
        HandleStream channel(child_->from_child());
        while (true) {
            auto frame = read_frame(channel);
            if (!frame) {
                // Garbage from a live worker is a violation; a frame cut short by the worker's
                // death is just the crash itself.
                const bool worker_alive = !child_->wait_for_exit(kViolationExitProbe);
                on_channel_closed(worker_alive && frame.error().code == ErrorCode::ParseError
                                      ? std::optional(frame.error())
                                      : std::nullopt);
                return;
            }
            if (!*frame) {
                on_channel_closed(std::nullopt);
                return;
            }
            auto message = decode_worker_message((*frame)->payload);
            if (!message) {
                on_channel_closed(message.error());
                return;
            }
            if (auto accepted = handle_message(std::move(*message), (*frame)->request_sequence); !accepted) {
                on_channel_closed(accepted.error());
                return;
            }
        }
    } catch (const std::exception&) {
        // Allocation failure while reading; the channel can no longer be trusted.
        on_channel_closed(error_of(ErrorCode::ResourceLimit, "supervisor ran out of memory reading the worker"));
    }
}

Result<void> WorkerProcess::Session::handle_message(WorkerMessage message, std::uint64_t request_sequence) {
    std::lock_guard lock(mutex_);
    last_frame_at_ = Clock::now();
    if (!handshake_ && !std::holds_alternative<Handshake>(message)) {
        return fail(ErrorCode::ParseError, "worker sent a message before its handshake");
    }
    if (auto sequence = check_sequence(message, request_sequence); !sequence) return sequence;
    return std::visit(Overloaded{[&](Handshake& handshake) { return accept_handshake(std::move(handshake)); },
                                 [&](Heartbeat& heartbeat) -> Result<void> {
                                     accept_heartbeat(heartbeat);
                                     return {};
                                 },
                                 [&](Progress& progress) -> Result<void> {
                                     accept_progress(progress);
                                     return {};
                                 },
                                 [&](TaskResult& result) -> Result<void> {
                                     const auto fence = result.fence;
                                     return accept_outcome(std::move(result), fence);
                                 },
                                 [&](TaskFailed& failure) -> Result<void> {
                                     const auto fence = failure.fence;
                                     return accept_outcome(std::move(failure), fence);
                                 }},
                      message);
}

Result<void> WorkerProcess::Session::check_sequence(const WorkerMessage& message,
                                                    std::uint64_t request_sequence) const {
    const bool unsolicited = std::holds_alternative<Handshake>(message) || std::holds_alternative<Heartbeat>(message);
    if (unsolicited) {
        if (request_sequence != 0) return fail(ErrorCode::ParseError, "handshake and heartbeat must carry sequence 0");
        return {};
    }
    if (request_sequence == 0 || request_sequence >= next_sequence_)
        return fail(ErrorCode::ParseError, "task message names a request the supervisor never sent");
    const auto job_id = std::visit(Overloaded{[](const Progress& progress) { return progress.job_id; },
                                              [](const TaskResult& result) { return result.fence.job_id; },
                                              [](const TaskFailed& failure) { return failure.fence.job_id; },
                                              [](const auto&) { return uuids::uuid{}; }},
                                   message);
    if (task_ && task_->fence.job_id == job_id && task_->request_sequence != request_sequence)
        return fail(ErrorCode::ParseError, "task message carries another request's sequence");
    return {};
}

Result<void> WorkerProcess::Session::accept_handshake(Handshake handshake) {
    if (handshake_) return fail(ErrorCode::ParseError, "worker sent a second handshake");
    if (handshake.protocol_major != kProtocolMajor) {
        return fail(ErrorCode::ParseError,
                    std::format("worker speaks protocol {}.{}; the supervisor requires major {}",
                                handshake.protocol_major, handshake.protocol_minor, kProtocolMajor));
    }
    if (handshake.process_id != child_->process_id()) {
        return fail(ErrorCode::ParseError, std::format("handshake names process {} but the supervisor spawned {}",
                                                       handshake.process_id, child_->process_id()));
    }
    if (handshake.self_test != options_.self_test) {
        return fail(ErrorCode::ParseError, handshake.self_test
                                               ? "worker runs in self-test mode, which was not requested"
                                               : "worker does not run the requested self-test mode");
    }
    // Results are stamped with this build's algorithm versions, so a stale worker binary would
    // publish old output under new identities.
    if (handshake.algorithm_versions != current_algorithm_versions())
        return fail(ErrorCode::ParseError, "worker algorithm versions differ from the supervisor's");
    const std::chrono::milliseconds heartbeat_interval{handshake.limits.heartbeat_interval_ms};
    if (heartbeat_interval <= std::chrono::milliseconds::zero() || 2 * heartbeat_interval > options_.lease_deadline ||
        2 * heartbeat_interval > options_.task_stall_deadline) {
        return fail(ErrorCode::ParseError, "worker heartbeat interval does not fit the supervisor's deadlines");
    }
    if (handshake.limits.concurrent_tasks < 1) return fail(ErrorCode::ParseError, "worker accepts no tasks");
    // The supervisor sizes frames and validates tasks and results with its own constants; a worker
    // that enforces other bounds would refuse valid tasks or produce results the supervisor rejects.
    const WorkerLimits expected;
    if (handshake.limits.maximum_payload_bytes != expected.maximum_payload_bytes ||
        handshake.limits.maximum_waveform_bins != expected.maximum_waveform_bins ||
        handshake.limits.maximum_musical_seconds != expected.maximum_musical_seconds) {
        return fail(ErrorCode::ParseError, "worker resource limits differ from the supervisor's");
    }
    handshake_ = std::move(handshake);
    state_changed_.notify_all();
    return {};
}

void WorkerProcess::Session::accept_heartbeat(const Heartbeat& heartbeat) {
    heartbeats_.fetch_add(1);
    if (!task_ || heartbeat.active_job_id != task_->fence.job_id || !heartbeat.task_progress) return;
    if (*heartbeat.task_progress != task_->work_units) {
        task_->work_units = *heartbeat.task_progress;
        task_->last_progress_at = Clock::now();
    }
}

void WorkerProcess::Session::accept_progress(const Progress& progress) {
    if (!task_ || task_->fence.job_id != progress.job_id || task_->fence.cancel_epoch != progress.cancel_epoch) return;
    task_->last_progress_at = Clock::now();
    if (!task_->cancel_deadline) events_.push(progress);
}

Result<void> WorkerProcess::Session::accept_outcome(WorkerEvent outcome, const TaskFence& fence) {
    if (!task_ || task_->fence != fence) return {};  // stale or foreign outcome: the fence discards it
    if (const auto* result = std::get_if<TaskResult>(&outcome)) {
        if (auto valid =
                validate_result_for_task(*result, task_->stages, task_->waveform_output_path, task_->waveform_bins);
            !valid)
            return valid;
    }
    if (task_->stall_failure) {
        // Work that stalled is never published, whatever arrives after the stall cancel.
        const Error& stall = *task_->stall_failure;
        outcome = TaskFailed{fence, stall.code, stall.retryable, stall.safe_detail};
    } else if (task_->cancel_deadline && std::holds_alternative<TaskResult>(outcome)) {
        // Cancelled work is never published by a late message.
        outcome = TaskFailed{fence, ErrorCode::Cancelled, false, "result arrived after cancellation and was discarded"};
    }
    events_.push(std::move(outcome));
    task_.reset();
    state_changed_.notify_all();
    return {};
}

void WorkerProcess::Session::watch_loop(std::stop_token stop) {
    try {
        std::unique_lock lock(mutex_);
        while (!state_changed_.wait_for(lock, stop, kWatchdogPeriod, [&] { return exited_; })) {
            if (stop.stop_requested()) return;
            lock.unlock();
            child_->poll();
            const bool process_gone = child_->wait_for_exit(std::chrono::milliseconds{0}).has_value();
            lock.lock();
            if (exited_) return;
            const auto now = Clock::now();
            if (process_gone) {
                if (!process_gone_at_) process_gone_at_ = now;
                if (now - *process_gone_at_ > kOrphanedReadGrace) child_->cancel_blocked_read();
            }
            if (termination_reason_) continue;
            WatchdogAction action = check_deadlines(now);
            if (action.stall_cancel) {
                // A Cancel that cannot be queued is covered by the armed cancel deadline, which
                // terminates the worker if the task does not end.
                (void)send_locked(*action.stall_cancel);
            }
            if (action.terminate_reason) {
                lock.unlock();
                terminate(std::move(*action.terminate_reason));
                lock.lock();
            }
        }
    } catch (const std::exception&) {
        // Only allocation can fail here. Without a watchdog the deadlines are unenforceable, so
        // the worker is ended rather than left unsupervised.
        terminate(error_of(ErrorCode::ResourceLimit, "worker watchdog ran out of memory"));
    }
}

WorkerProcess::Session::WatchdogAction WorkerProcess::Session::check_deadlines(Clock::time_point now) {
    WatchdogAction action;
    if (!handshake_) {
        if (now - spawned_at_ > options_.handshake_timeout) {
            action.terminate_reason = error_of(
                ErrorCode::WorkerCrash,
                std::format("worker sent no handshake within {} ms", options_.handshake_timeout.count()), true);
        }
        return action;
    }
    if (now - last_frame_at_ > options_.lease_deadline) {
        action.terminate_reason = error_of(
            ErrorCode::WorkerCrash,
            std::format("worker sent no heartbeat within its {} ms lease", options_.lease_deadline.count()), true);
        return action;
    }
    if (!task_) return action;
    if (!task_->cancel_deadline && now - task_->last_progress_at > options_.task_stall_deadline) {
        // Heartbeats arrive but the task does not move: one call inside it is stuck. Ask it to
        // stop; the cancel deadline below terminates the worker if that does not help.
        task_->stall_failure = error_of(
            ErrorCode::WorkerCrash,
            std::format("analysis made no progress for {} ms and was stopped", options_.task_stall_deadline.count()),
            true);
        task_->cancel_deadline = now + options_.cancel_grace;
        action.stall_cancel = Cancel{task_->fence.job_id, task_->fence.cancel_epoch};
        return action;
    }
    if (task_->cancel_deadline && now >= *task_->cancel_deadline) {
        action.terminate_reason =
            task_->stall_failure
                ? *task_->stall_failure
                : error_of(ErrorCode::Cancelled,
                           std::format("worker did not stop within {} ms of cancellation and was terminated",
                                       options_.cancel_grace.count()));
    }
    return action;
}

void WorkerProcess::Session::terminate(Error reason) {
    {
        std::lock_guard lock(mutex_);
        if (exited_) return;
        if (!termination_reason_) termination_reason_ = std::move(reason);
    }
    child_->terminate();
}

void WorkerProcess::Session::on_channel_closed(std::optional<Error> violation) {
    if (violation) {
        terminate(
            error_of(ErrorCode::WorkerCrash, std::format("worker protocol violation: {}", violation->safe_detail)));
    }
    auto exit_code = child_->wait_for_exit(kExitWait);
    if (!exit_code) {
        // The worker closed its output but kept running: it can no longer be supervised.
        terminate(error_of(ErrorCode::WorkerCrash, "worker closed its output channel", true));
        exit_code = child_->wait_for_exit(kExitWait);
    }
    child_->poll();

    std::lock_guard lock(mutex_);
    std::optional<Error> failure = termination_reason_;
    if (!failure && !shutting_down_) {
        failure =
            child_->memory_limit_hit()
                ? error_of(
                      ErrorCode::ResourceLimit,
                      std::format("worker exceeded its memory limit of {}",
                                  format_byte_size(static_cast<std::int64_t>(child_->limits().memory_limit_bytes))))
                : error_of(ErrorCode::WorkerCrash,
                           std::format("worker exited unexpectedly with code {:#x}", exit_code.value_or(0)), true);
    }
    if (task_) {
        const auto task_failure =
            failure.value_or(error_of(ErrorCode::Cancelled, "worker shut down before the task finished"));
        events_.push(TaskFailed{task_->fence, task_failure.code, task_failure.retryable, task_failure.safe_detail});
        task_.reset();
    }
    exit_failure_ = failure;
    exited_ = true;
    events_.push(WorkerExited{failure, exit_code.value_or(0)});
    state_changed_.notify_all();
}

// ---------------------------------------------------------------- WorkerProcess

WorkerProcess::WorkerProcess(WorkerOptions options)
    : options_(std::move(options)), events_(std::make_unique<EventQueue>()) {}

WorkerProcess::~WorkerProcess() = default;

Result<std::unique_ptr<WorkerProcess>> WorkerProcess::start(WorkerOptions options) {
    std::unique_ptr<WorkerProcess> process(new WorkerProcess(std::move(options)));
    if (auto started = process->restart(); !started) return std::unexpected(started.error());
    return process;
}

Result<void> WorkerProcess::restart() {
    session_.reset();
    auto session = Session::launch(options_, *events_);
    if (!session) return std::unexpected(session.error());
    session_ = std::move(*session);
    return {};
}

Handshake WorkerProcess::handshake() const {
    return session_ ? session_->handshake() : Handshake{};
}

LimitsStatus WorkerProcess::limits_status() const {
    return session_ ? session_->limits_status() : LimitsStatus{};
}

std::uint32_t WorkerProcess::process_id() const {
    return session_ ? session_->process_id() : 0;
}

bool WorkerProcess::is_running() const {
    return session_ && session_->is_running();
}

bool WorkerProcess::is_busy() const {
    return session_ && session_->is_busy();
}

std::uint64_t WorkerProcess::heartbeats_received() const {
    return session_ ? session_->heartbeats_received() : 0;
}

std::optional<TaskActivity> WorkerProcess::task_activity() const {
    return session_ ? session_->task_activity() : std::nullopt;
}

Result<void> WorkerProcess::submit(const AnalyzeTask& task) {
    if (!session_) return fail(ErrorCode::WorkerCrash, "worker is not running", true);
    return session_->submit(task);
}

Result<void> WorkerProcess::cancel(const uuids::uuid& job_id, std::int64_t cancel_epoch) {
    if (!session_) return fail(ErrorCode::NotFound, "no task with that job id is in flight");
    return session_->cancel(job_id, cancel_epoch);
}

std::optional<WorkerEvent> WorkerProcess::next_event(std::chrono::milliseconds timeout) {
    return events_->pop(timeout);
}

void WorkerProcess::shutdown() {
    if (session_) session_->shutdown();
}

}  // namespace mso::worker
