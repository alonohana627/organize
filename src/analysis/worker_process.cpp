#include "mso/analysis/worker_process.hpp"

#include <algorithm>
#include <atomic>
#include <condition_variable>
#include <deque>
#include <format>
#include <mutex>
#include <stop_token>
#include <thread>
#include <utility>

#include "mso/analysis/worker_platform.hpp"

namespace mso::worker {

namespace {

using Clock = std::chrono::steady_clock;

/// Smallest accepted memory cap; below this the worker cannot even load.
constexpr std::uint64_t kMinimumMemoryLimitBytes = 64ull * 1024 * 1024;
/// Outgoing frames waiting for the writer thread. Each is a small control message.
constexpr std::size_t kMaxQueuedFrames = 64;
/// Supervisor events waiting for the owner. Progress beyond this is dropped; outcomes never are.
constexpr std::size_t kMaxQueuedEvents = 1024;
/// Period of the watchdog that enforces handshake, lease and cancellation deadlines.
constexpr std::chrono::milliseconds kWatchdogPeriod{100};
/// How long to wait for a dead or closing worker to finish exiting.
constexpr std::chrono::milliseconds kExitWait{5000};
/// How long the reader may stay blocked after the child has exited (possible on Windows when
/// another process inherited the pipe) before its read is cancelled.
constexpr std::chrono::milliseconds kOrphanedReadGrace{500};
/// After a malformed frame, how long to wait to tell a dying worker from a misbehaving one.
constexpr std::chrono::milliseconds kViolationExitProbe{200};

Error error_of(ErrorCode code, std::string detail, bool retryable = false) {
    return Error{code, retryable, std::move(detail), std::nullopt, std::nullopt};
}

/// Builds a visitor from lambdas.
template <class... Handlers>
struct Overloaded : Handlers... {
    using Handlers::operator()...;
};

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
    return {};
}

/// Writes frames to the worker on a dedicated thread, so a worker that stopped reading can
/// never block a supervisor caller; the watchdog's termination then unblocks the write.
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
            if (queue_.size() >= kMaxQueuedFrames)
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
    void run(std::stop_token stop) {
        while (true) {
            std::vector<std::uint8_t> frame;
            {
                std::unique_lock lock(mutex_);
                if (!pending_.wait(lock, stop, [&] { return !queue_.empty(); })) return;
                frame = std::move(queue_.front());
                queue_.pop_front();
            }
            if (!pipe_.write_all(frame)) {
                std::lock_guard lock(mutex_);
                broken_ = true;
                queue_.clear();
                return;
            }
        }
    }

    HandleStream pipe_;
    std::mutex mutex_;
    std::condition_variable_any pending_;
    std::deque<std::vector<std::uint8_t>> queue_;
    bool broken_ = false;
    std::jthread thread_;  // declared last: starts after, and stops before, the members it uses
};

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

class WorkerProcess::EventQueue {
public:
    /// Appends an event. When the queue is full a Progress event is dropped (the incoming one,
    /// or the oldest queued one to make room for an outcome); outcomes and exits never are.
    void push(WorkerEvent event) {
        {
            std::lock_guard lock(mutex_);
            if (events_.size() >= kMaxQueuedEvents) {
                if (std::holds_alternative<Progress>(event)) return;
                const auto oldest_progress = std::ranges::find_if(
                    events_, [](const WorkerEvent& queued) { return std::holds_alternative<Progress>(queued); });
                if (oldest_progress != events_.end()) events_.erase(oldest_progress);
            }
            events_.push_back(std::move(event));
        }
        available_.notify_one();
    }

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
/// enforces deadlines by terminating the child, and the FrameSender writes. Every way the process
/// can end funnels into on_channel_closed(), which reports it exactly once.
class WorkerProcess::Session {
public:
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

    Result<void> submit(const AnalyzeTask& task);
    Result<void> cancel(const uuids::uuid& job_id, std::int64_t cancel_epoch);
    void shutdown();

private:
    /// The task the worker currently owns, as the supervisor sent it.
    struct TaskInFlight {
        TaskFence fence;
        std::optional<Clock::time_point> cancel_deadline;  ///< set once cancel() was called
    };

    Session(const WorkerOptions& options, EventQueue& events, std::unique_ptr<platform::ChildProcess> child);

    /// Blocks until the handshake arrived or the process ended; the watchdog bounds the wait.
    Result<void> await_handshake();
    /// Encodes `message` with the next request sequence and queues it for the writer thread.
    Result<void> send(const SupervisorMessage& message);

    void read_loop();
    void watch_loop(std::stop_token stop);
    /// Applies one incoming message. An error is a protocol violation. Takes the lock.
    Result<void> handle_message(WorkerMessage message);
    /// The following three require mutex_ to be held.
    Result<void> accept_handshake(Handshake handshake);
    void accept_outcome(WorkerEvent outcome, const TaskFence& fence);
    std::optional<Error> overdue_deadline(Clock::time_point now) const;

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
    const auto executable = options.executable.empty() ? default_worker_executable() : options.executable;
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

Result<void> WorkerProcess::Session::await_handshake() {
    std::unique_lock lock(mutex_);
    state_changed_.wait(lock, [&] { return handshake_.has_value() || exited_; });
    if (handshake_) return {};
    const auto failure = exit_failure_.value_or(error_of(ErrorCode::WorkerCrash, "worker exited", true));
    return fail(ErrorCode::WorkerCrash, std::format("worker handshake failed: {}", failure.message()),
                failure.retryable);
}

Result<void> WorkerProcess::Session::send(const SupervisorMessage& message) {
    const auto payload = encode_message(message);
    if (!payload) return std::unexpected(payload.error());
    std::uint64_t request_sequence = 0;
    {
        std::lock_guard lock(mutex_);
        request_sequence = next_sequence_++;
    }
    auto frame = encode_frame(request_sequence, *payload);
    if (!frame) return std::unexpected(frame.error());
    return sender_.post(std::move(*frame));
}

Result<void> WorkerProcess::Session::submit(const AnalyzeTask& task) {
    if (auto valid = validate_task(task); !valid) return valid;
    {
        std::lock_guard lock(mutex_);
        if (exited_ || shutting_down_ || termination_reason_) {
            return fail(ErrorCode::WorkerCrash, "worker is not running", true);
        }
        if (task_) return fail(ErrorCode::InvalidArgument, "the worker already has a task in flight");
        task_ = TaskInFlight{TaskFence::of(task), std::nullopt};
    }
    auto sent = send(task);
    if (!sent) {
        std::lock_guard lock(mutex_);
        task_.reset();
    }
    return sent;
}

Result<void> WorkerProcess::Session::cancel(const uuids::uuid& job_id, std::int64_t cancel_epoch) {
    {
        std::lock_guard lock(mutex_);
        if (!task_ || task_->fence.job_id != job_id)
            return fail(ErrorCode::NotFound, "no task with that job id is in flight");
        if (!task_->cancel_deadline) task_->cancel_deadline = Clock::now() + options_.cancel_grace;
    }
    // Even if this frame cannot be delivered the armed deadline still ends the task.
    return send(Cancel{job_id, cancel_epoch});
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
        (void)send(Shutdown{});
        if (!child_->wait_for_exit(options_.shutdown_grace)) child_->terminate();
    }
    if (reader_.joinable()) reader_.join();
}

void WorkerProcess::Session::read_loop() {
    child_->register_reader_thread();
    HandleStream channel(child_->from_child());
    while (true) {
        auto frame = read_frame(channel);
        if (!frame) {
            // Garbage from a live worker is a violation; a frame cut short by the worker's
            // death is just the crash itself.
            const bool worker_alive = !child_->wait_for_exit(kViolationExitProbe);
            on_channel_closed(worker_alive && frame.error().code == ErrorCode::ParseError ? std::optional(frame.error())
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
        if (auto accepted = handle_message(std::move(*message)); !accepted) {
            on_channel_closed(accepted.error());
            return;
        }
    }
}

Result<void> WorkerProcess::Session::handle_message(WorkerMessage message) {
    std::lock_guard lock(mutex_);
    last_frame_at_ = Clock::now();
    if (!handshake_ && !std::holds_alternative<Handshake>(message)) {
        return fail(ErrorCode::ParseError, "worker sent a message before its handshake");
    }
    return std::visit(Overloaded{[&](Handshake& handshake) { return accept_handshake(std::move(handshake)); },
                                 [&](Heartbeat&) -> Result<void> {
                                     heartbeats_.fetch_add(1);
                                     return {};
                                 },
                                 [&](Progress& progress) -> Result<void> {
                                     if (task_ && !task_->cancel_deadline && task_->fence.job_id == progress.job_id)
                                         events_.push(progress);
                                     return {};
                                 },
                                 [&](TaskResult& result) -> Result<void> {
                                     const auto fence = result.fence;
                                     accept_outcome(std::move(result), fence);
                                     return {};
                                 },
                                 [&](TaskFailed& failure) -> Result<void> {
                                     const auto fence = failure.fence;
                                     accept_outcome(std::move(failure), fence);
                                     return {};
                                 }},
                      message);
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
    handshake_ = std::move(handshake);
    state_changed_.notify_all();
    return {};
}

void WorkerProcess::Session::accept_outcome(WorkerEvent outcome, const TaskFence& fence) {
    if (!task_ || task_->fence != fence) return;  // stale or foreign outcome: the fence discards it
    if (task_->cancel_deadline && std::holds_alternative<TaskResult>(outcome)) {
        // Cancelled work is never published by a late message.
        outcome = TaskFailed{fence, ErrorCode::Cancelled, false, "result arrived after cancellation and was discarded"};
    }
    events_.push(std::move(outcome));
    task_.reset();
    state_changed_.notify_all();
}

void WorkerProcess::Session::watch_loop(std::stop_token stop) {
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
        if (auto overdue = overdue_deadline(now)) {
            lock.unlock();
            terminate(std::move(*overdue));
            lock.lock();
        }
    }
}

std::optional<Error> WorkerProcess::Session::overdue_deadline(Clock::time_point now) const {
    if (!handshake_) {
        if (now - spawned_at_ <= options_.handshake_timeout) return std::nullopt;
        return error_of(ErrorCode::WorkerCrash,
                        std::format("worker sent no handshake within {} ms", options_.handshake_timeout.count()), true);
    }
    if (now - last_frame_at_ > options_.lease_deadline) {
        return error_of(ErrorCode::WorkerCrash,
                        std::format("worker sent no heartbeat within its {} ms lease", options_.lease_deadline.count()),
                        true);
    }
    if (task_ && task_->cancel_deadline && now >= *task_->cancel_deadline) {
        return error_of(ErrorCode::Cancelled,
                        std::format("worker did not stop within {} ms of cancellation and was terminated",
                                    options_.cancel_grace.count()));
    }
    return std::nullopt;
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
