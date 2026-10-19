/// @file
/// mso-worker: the analysis child process (docs/02_ARCHITECTURE.md section 3, docs/04_CONTRACTS.md
/// section 4). It reads framed supervisor messages from stdin, writes framed replies to stdout,
/// analyzes one file at a time on a task thread and reports liveness, including the task's work
/// counter, every two seconds.
///
/// The worker never opens SQLite, never starts processes or shell commands and treats every path
/// it receives as data. Its only output besides the protocol stream is the waveform segment file
/// the supervisor named, written through a temporary sibling and an atomic rename.
///
/// Usage: mso-worker[.exe] [--self-test [--self-test-fault NAME]]
///   --self-test        replace the real analyzer with a synthetic one that never opens the input
///                      file; used by tests to exercise cancellation, heartbeats and crash handling.
///   --self-test-fault  additionally misbehave in one named way (see kFaultNames) so tests can
///                      check that the supervisor detects it. Only valid with --self-test, which a
///                      production supervisor refuses at the handshake.
#ifdef _WIN32
#include <windows.h>

#include <fcntl.h>
#include <io.h>
#endif

#include <picosha2.h>
#include <CLI/CLI11.hpp>

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <cstdio>
#include <cstdlib>
#include <exception>
#include <format>
#include <functional>
#include <iostream>
#include <map>
#include <memory>
#include <mutex>
#include <new>
#include <numbers>
#include <optional>
#include <stop_token>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

#include "mso/analysis/analysis.hpp"
#include "mso/analysis/progress_reporter.hpp"
#include "mso/domain/core.hpp"
#include "mso/domain/error.hpp"
#include "mso/domain/overloaded.hpp"
#include "mso/media/media.hpp"
#include "mso/worker/worker_process.hpp"
#include "mso/worker/worker_protocol.hpp"

namespace {

namespace analysis = mso::analysis;
namespace worker = mso::worker;
using mso::ErrorCode;
using mso::Overloaded;
using mso::Result;

// Exit statuses; the supervisor reports any status other than a clean one as a crash.
constexpr int kExitClean = 0;              ///< Shutdown received, or the supervisor closed stdin
constexpr int kExitInternalFailure = 1;    ///< a worker thread could not continue
constexpr int kExitProtocolViolation = 2;  ///< malformed frame or message from the supervisor
constexpr int kExitChannelBroken = 3;      ///< the handshake could not be written
constexpr int kExitUsage = 64;             ///< invalid command line (EX_USAGE)

/// Once the worker is told to stop (Shutdown, end of input, a protocol violation), how long it
/// waits for its task thread to end. A thread stuck in a decoder or file read never ends; the
/// worker then exits without it instead of waiting forever. Half the supervisor's default
/// shutdown_grace (2 s), so the worker's own exit, with its own exit status, comes well before the
/// supervisor would terminate it.
constexpr std::chrono::milliseconds kTaskStopDeadline{1000};

/// The compiler that built this worker, for the handshake's build id.
#if defined(__clang__)
constexpr std::string_view kCompiler = "clang " __clang_version__;
#elif defined(__GNUC__)
constexpr std::string_view kCompiler = "gcc " __VERSION__;
#else
constexpr std::string_view kCompiler = "unknown compiler";
#endif

// ---------------------------------------------------------------- self-test faults

/// Ways a --self-test worker can misbehave on purpose, one per supervisor safeguard under test.
enum class Fault {
    None,
    NoHandshake,             ///< never sends a handshake (handshake timeout)
    WrongProcessId,          ///< handshake names another process
    MessageBeforeHandshake,  ///< a heartbeat precedes the handshake
    SecondHandshake,         ///< sends its handshake twice
    GarbageAfterHandshake,   ///< writes bytes that are not a frame after a valid handshake
    StaleAlgorithm,          ///< handshake reports an outdated tempo algorithm
    UnsequencedOutcome,      ///< sends task outcomes with request sequence 0
    StallTask,               ///< the task blocks forever, ignoring cancellation; heartbeats continue
    SilentTask,              ///< the task stops making progress but honors cancellation
    ExhaustMemory,           ///< the task allocates until the memory cap stops it
    ForeignLimits,           ///< handshake reports resource limits other than the supervisor's
    ExcessWaveformBins,      ///< the waveform segment holds more bins than the task asked for
};

/// Command-line names of the faults.
const std::map<std::string, Fault> kFaultNames{
    {"no-handshake", Fault::NoHandshake},
    {"wrong-process-id", Fault::WrongProcessId},
    {"message-before-handshake", Fault::MessageBeforeHandshake},
    {"second-handshake", Fault::SecondHandshake},
    {"garbage-after-handshake", Fault::GarbageAfterHandshake},
    {"stale-algorithm", Fault::StaleAlgorithm},
    {"unsequenced-outcome", Fault::UnsequencedOutcome},
    {"stall-task", Fault::StallTask},
    {"silent-task", Fault::SilentTask},
    {"exhaust-memory", Fault::ExhaustMemory},
    {"foreign-limits", Fault::ForeignLimits},
    {"excess-waveform-bins", Fault::ExcessWaveformBins},
};

/// Signature shared by mso::analysis::analyze_file and the self-test analyzer.
using Analyzer = std::function<Result<analysis::AnalysisReport>(
    const analysis::AnalysisRequest&, std::stop_token, const analysis::ProgressCallback&, analysis::WorkCounter*)>;

// ---------------------------------------------------------------- protocol output

/// Serializes frames from the main, task and heartbeat threads onto stdout. Once a write fails
/// the supervisor is gone and every later send fails fast.
class OutputChannel {
public:
    explicit OutputChannel(worker::NativeHandle output) : stream_(output) {}

    /// Encodes and writes one message. INVALID_ARGUMENT means the message could not be encoded;
    /// WORKER_CRASH means the channel is closed.
    Result<void> send(const worker::WorkerMessage& message, std::uint64_t request_sequence) {
        const auto payload = worker::encode_message(message);
        if (!payload) return std::unexpected(payload.error());
        std::lock_guard lock(mutex_);
        if (broken_) return mso::fail(ErrorCode::WorkerCrash, "supervisor channel is closed");
        auto written = worker::write_frame(stream_, request_sequence, *payload);
        if (!written) broken_ = true;
        return written;
    }

    /// Writes raw bytes that are not a frame (fault injection only).
    Result<void> send_garbage() {
        /// Not a frame: the magic is wrong.
        constexpr std::array<std::uint8_t, 32> kGarbage{'n', 'o', 't', ' ', 'a', ' ', 'f', 'r', 'a', 'm', 'e'};
        std::lock_guard lock(mutex_);
        return stream_.write_all(kGarbage);
    }

private:
    worker::HandleStream stream_;  ///< stdout
    std::mutex mutex_;             ///< one frame at a time, whole frames only
    bool broken_ = false;          ///< a write failed; the supervisor is gone
};

// ---------------------------------------------------------------- waveform segment output

/// Encodes `overview` into the segment file at `output_path_utf8` and describes the result.
Result<worker::WaveformSegment> write_waveform_segment(const analysis::WaveformOverview& overview,
                                                       const std::string& output_path_utf8) {
    const auto bytes = analysis::encode_waveform(overview);
    if (auto written = worker::write_file_atomically(mso::path_from_utf8(output_path_utf8), bytes); !written) {
        return std::unexpected(written.error());
    }
    worker::WaveformSegment segment;
    segment.path = output_path_utf8;
    segment.byte_length = bytes.size();
    segment.sha256 = picosha2::hash256_hex_string(bytes.begin(), bytes.end());
    segment.bins = static_cast<int>(overview.minimum.size());
    segment.frames_per_bin = overview.frames_per_bin;
    segment.frames_covered = overview.frames_covered;
    segment.total_frames = overview.total_frames;
    segment.sample_rate = overview.sample_rate;
    segment.complete = overview.complete;
    return segment;
}

// ---------------------------------------------------------------- self-test analyzer

/// A deterministic sine-shaped overview with `bins` bins.
analysis::WaveformOverview synthetic_waveform(int bins) {
    constexpr std::int64_t kFramesPerBin = 64;  ///< any positive width makes a consistent segment
    analysis::WaveformOverview overview;
    overview.frames_per_bin = kFramesPerBin;
    overview.total_frames = kFramesPerBin * bins;
    overview.frames_covered = overview.total_frames;
    overview.sample_rate = 48000;
    overview.complete = true;
    for (int bin = 0; bin < bins; ++bin) {
        const auto level = static_cast<float>(std::abs(std::sin(2.0 * std::numbers::pi * bin / bins)));
        overview.minimum.push_back(-level);
        overview.maximum.push_back(level);
        overview.rms.push_back(level * static_cast<float>(std::numbers::sqrt2 / 2.0));
    }
    return overview;
}

/// A synthetic report with a plausible value for every requested stage.
analysis::AnalysisReport synthetic_report(const analysis::AnalysisRequest& request) {
    const auto requested = [&](analysis::Stage stage) { return std::ranges::contains(request.stages, stage); };
    analysis::AnalysisReport report;
    report.warnings.push_back("self-test: synthetic report; the input file was not read");
    const std::string digest = picosha2::hash256_hex_string(std::string("mso-worker self-test"));
    if (requested(analysis::Stage::Waveform)) report.waveform = synthetic_waveform(request.waveform_bins);
    if (requested(analysis::Stage::Loudness)) report.loudness = analysis::LoudnessResult{-6.02, -9.03};
    if (requested(analysis::Stage::Tempo)) report.tempo = analysis::TempoResult{120'000, 0.5};
    if (requested(analysis::Stage::Key)) report.key = analysis::KeyResult{std::nullopt, 0.0};
    if (requested(analysis::Stage::ByteHash)) report.hashes.byte_sha256 = digest;
    if (requested(analysis::Stage::PcmHash)) report.hashes.pcm_sha256 = digest;
    return report;
}

/// Blocks the calling thread forever, deaf to cancellation: a decoder stuck in a system call.
[[noreturn]] void block_forever() {
    std::mutex mutex;
    std::condition_variable never_notified;
    std::unique_lock lock(mutex);
    while (true)
        never_notified.wait(lock);
}

/// Allocates and touches 16 MiB blocks until allocation fails, then lets std::bad_alloc escape
/// to the task barrier (which reports RESOURCE_LIMIT). Gives up with INTERNAL after 8 GiB, which
/// means no memory cap was enforced.
Result<analysis::AnalysisReport> exhaust_memory() {
    constexpr std::size_t kBlockBytes = 16u << 20;  ///< 16 MiB per allocation
    constexpr std::size_t kMaximumBlocks = 512;     ///< 8 GiB in total
    std::vector<std::unique_ptr<char[]>> blocks;
    blocks.reserve(kMaximumBlocks);
    for (std::size_t block = 0; block < kMaximumBlocks; ++block) {
        auto memory = std::make_unique<char[]>(kBlockBytes);  // value-initialized: every page is touched
        blocks.push_back(std::move(memory));
    }
    return mso::fail(ErrorCode::Internal, "self-test: allocated 8 GiB without reaching the memory cap");
}

/// Stand-in for analyze_file in --self-test mode. It never opens the input file; it simulates
/// `max_musical_seconds` of work in 20 ms blocks, honoring stop and counting one unit of work
/// per block, with progress throttled by the same ProgressReporter as the real analyzer, and
/// returns a synthetic report. `fault` replaces the work with a misbehaviour.
Result<analysis::AnalysisReport> analyze_self_test(const analysis::AnalysisRequest& request, std::stop_token stop,
                                                   const analysis::ProgressCallback& progress,
                                                   analysis::WorkCounter* work_counter, Fault fault) {
    using namespace std::chrono;
    constexpr milliseconds kBlock{20};  ///< simulated duration of one decoded block
    if (fault == Fault::StallTask) block_forever();
    if (fault == Fault::ExhaustMemory) return exhaust_memory();

    analysis::ProgressReporter reporter(progress, analysis::ProgressReporter::kDefaultInterval, work_counter);
    const auto simulated = duration_cast<milliseconds>(duration<double>(request.max_musical_seconds));
    const auto started = steady_clock::now();
    for (auto elapsed = 0ms; fault == Fault::SilentTask || elapsed < simulated;
         elapsed = duration_cast<milliseconds>(steady_clock::now() - started)) {
        if (stop.stop_requested()) return mso::fail(ErrorCode::Cancelled, "analysis cancelled");
        if (fault != Fault::SilentTask) {
            reporter.record_work();
            reporter.report(static_cast<double>(elapsed.count()) / static_cast<double>(simulated.count()));
        }
        std::this_thread::sleep_for(kBlock);  // the simulated work itself
    }
    auto report = synthetic_report(request);
    if (fault == Fault::ExcessWaveformBins && report.waveform)
        report.waveform = synthetic_waveform(request.waveform_bins + 1);
    reporter.finish();
    return report;
}

// ---------------------------------------------------------------- task execution

/// Runs at most one AnalyzeTask at a time on one long-lived task thread, and sends its progress
/// and outcome. The protocol thread only hands tasks over and never joins or waits for the task
/// thread while it runs, so Cancel and Shutdown are handled even while an outcome is being
/// written to a slow pipe.
class TaskRunner {
public:
    /// What to do with the outcome of a task that is being stopped.
    enum class Outcome { Report, Discard };

    /// What the heartbeat reports about the active task.
    struct ActiveTask {
        uuids::uuid job_id;            ///< the task's job
        std::uint64_t work_units = 0;  ///< its work counter (analysis::WorkCounter units)
    };

    TaskRunner(OutputChannel& channel, Analyzer analyzer, Fault fault)
        : channel_(channel),
          analyzer_(std::move(analyzer)),
          fault_(fault),
          thread_([this](std::stop_token stop) { run(stop); }) {}
    TaskRunner(const TaskRunner&) = delete;
    TaskRunner& operator=(const TaskRunner&) = delete;
    /// Stops the task thread. A thread stuck beyond kTaskStopDeadline cannot be joined, and
    /// destroying the members it uses would be undefined, so the process ends instead.
    ~TaskRunner() {
        if (!stop(Outcome::Discard, kTaskStopDeadline)) std::_Exit(kExitInternalFailure);
    }

    /// Hands `task` to the task thread. While another task runs the request is refused with a
    /// retryable RESOURCE_LIMIT failure: the worker runs one task at a time.
    void start(worker::AnalyzeTask task, std::uint64_t request_sequence) {
        bool accepted = false;
        {
            std::lock_guard lock(mutex_);
            if (!active_) {
                active_ = Active{task.job_id, task.cancel_epoch};
                task_stop_ = std::stop_source();
                work_units_.store(0, std::memory_order_relaxed);
                pending_ = Pending{std::move(task), request_sequence};
                accepted = true;
            }
        }
        if (accepted) {
            task_ready_.notify_one();
            return;
        }
        // Refused: answered from this thread. A failed send means the supervisor is gone, which
        // the protocol loop notices on its next read.
        (void)channel_.send(worker::TaskFailed{worker::TaskFence::of(task), ErrorCode::ResourceLimit, true,
                                               "worker already runs a task"},
                            request_sequence);
    }

    /// Requests stop of the active task when the cancel names it with a current or newer epoch.
    void cancel(const worker::Cancel& cancel) {
        std::lock_guard lock(mutex_);
        if (active_ && active_->job_id == cancel.job_id && cancel.cancel_epoch >= active_->cancel_epoch) {
            task_stop_.request_stop();
        }
    }

    /// The active task and its work counter, for the heartbeat; nullopt when idle.
    std::optional<ActiveTask> active() const {
        std::lock_guard lock(mutex_);
        if (!active_) return std::nullopt;
        return ActiveTask{active_->job_id, work_units_.load(std::memory_order_relaxed)};
    }

    /// Stops the active task (if any) and the task thread, and waits up to `deadline` for the
    /// thread to end. Returns false when it did not: the task is stuck in a call that ignores
    /// cancellation, the thread still runs, and the caller must end the process (std::_Exit)
    /// rather than destroy this runner. Idempotent.
    [[nodiscard]] bool stop(Outcome outcome, std::chrono::milliseconds deadline) {
        report_outcome_ = outcome == Outcome::Report;
        {
            std::lock_guard lock(mutex_);
            task_stop_.request_stop();
        }
        thread_.request_stop();
        {
            std::unique_lock lock(mutex_);
            if (!thread_finished_changed_.wait_for(lock, deadline, [&] { return thread_finished_; })) return false;
        }
        if (thread_.joinable()) thread_.join();
        return true;
    }

private:
    /// The task the heartbeat reports and Cancel addresses.
    struct Active {
        uuids::uuid job_id;             ///< the task's job
        std::int64_t cancel_epoch = 0;  ///< a Cancel with an older epoch is ignored
    };
    /// A task handed over by the protocol thread and not yet picked up by the task thread.
    struct Pending {
        worker::AnalyzeTask task;            ///< the task as received
        std::uint64_t request_sequence = 0;  ///< sequence of its AnalyzeTask frame, echoed in replies
    };

    /// Task thread body: runs tasks until stopped, then tells stop() that the thread is done.
    void run(std::stop_token thread_stop) {
        run_tasks(thread_stop);
        {
            std::lock_guard lock(mutex_);
            thread_finished_ = true;
        }
        thread_finished_changed_.notify_all();
    }

    /// Waits for a handed-over task, runs it, reports it; returns once the thread is stopped.
    void run_tasks(const std::stop_token& thread_stop) {
        try {
            while (true) {
                Pending next;
                std::stop_token task_stop;
                {
                    std::unique_lock lock(mutex_);
                    if (!task_ready_.wait(lock, thread_stop, [&] { return pending_.has_value(); })) return;
                    next = std::move(*pending_);
                    pending_.reset();
                    task_stop = task_stop_.get_token();
                }
                auto outcome = execute(next.task, task_stop, next.request_sequence);
                {
                    // Cleared before sending so a supervisor that reacts instantly with the next
                    // task never finds the worker still busy.
                    std::lock_guard lock(mutex_);
                    active_.reset();
                }
                if (report_outcome_) send_outcome(outcome, worker::TaskFence::of(next.task), next.request_sequence);
            }
        } catch (const std::exception&) {
            // Only allocation can fail outside execute()'s own barrier. A worker without a task
            // thread is useless; the supervisor reports the exit as a retryable crash.
            std::_Exit(kExitInternalFailure);
        }
    }

    /// Runs the analyzer and turns its report into the outcome message, writing the waveform
    /// segment when one was requested. Exception barrier around the analyzer.
    worker::WorkerMessage execute(const worker::AnalyzeTask& task, const std::stop_token& stop,
                                  std::uint64_t request_sequence) {
        const auto fence = worker::TaskFence::of(task);
        const auto failed = [&](const mso::Error& error) -> worker::WorkerMessage {
            return worker::TaskFailed{fence, error.code, error.retryable, error.safe_detail};
        };
        const auto progress = forward_progress(task, request_sequence);

        Result<analysis::AnalysisReport> report = mso::fail(ErrorCode::Internal);
        try {
            report = analyzer_(worker::to_analysis_request(task), stop, progress, &work_units_);
        } catch (const std::bad_alloc&) {
            return failed(mso::fail(ErrorCode::ResourceLimit, "analysis ran out of memory").error());
        } catch (const std::exception&) {
            return failed(mso::fail(ErrorCode::Internal, "analysis raised an unexpected exception").error());
        }
        if (!report) return failed(report.error());

        worker::TaskResult result{fence, std::move(*report), std::nullopt};
        if (result.report.waveform && !task.waveform_output_path.empty()) {
            auto segment = write_waveform_segment(*result.report.waveform, task.waveform_output_path);
            if (!segment) return failed(segment.error());
            result.waveform_segment = std::move(*segment);
        }
        result.report.waveform.reset();  // the overview travels in the segment file, never inline
        if (auto valid = worker::validate_result(result); !valid) {
            return failed(
                mso::fail(ErrorCode::Internal, "analysis produced an out-of-range result: " + valid.error().safe_detail)
                    .error());
        }
        return result;
    }

    /// Progress callback that sends every fraction it receives as a Progress frame. Both analyzers
    /// already throttle through analysis::ProgressReporter (10 Hz), so throttling again here would
    /// only drop the final 1.0.
    analysis::ProgressCallback forward_progress(const worker::AnalyzeTask& task, std::uint64_t request_sequence) {
        return [this, job_id = task.job_id, cancel_epoch = task.cancel_epoch, request_sequence](double fraction) {
            if (!std::isfinite(fraction)) return;
            // A lost progress frame is harmless; a broken channel ends the worker via the main loop.
            (void)channel_.send(worker::Progress{job_id, cancel_epoch, std::clamp(fraction, 0.0, 1.0)},
                                request_sequence);
        };
    }

    /// Sends the outcome; a result that cannot be encoded is reported as an INTERNAL failure.
    void send_outcome(const worker::WorkerMessage& outcome, const worker::TaskFence& fence,
                      std::uint64_t request_sequence) {
        const std::uint64_t sequence = fault_ == Fault::UnsequencedOutcome ? 0 : request_sequence;
        auto sent = channel_.send(outcome, sequence);
        if (!sent && sent.error().code == ErrorCode::InvalidArgument) {
            // If even this fails the channel is broken, which the protocol loop notices.
            (void)channel_.send(worker::TaskFailed{fence, ErrorCode::Internal, false, sent.error().safe_detail},
                                sequence);
        }
    }

    OutputChannel& channel_;                           ///< where progress and outcomes go
    Analyzer analyzer_;                                ///< analyze_file or the self-test analyzer
    const Fault fault_;                                ///< self-test misbehaviour, if any
    mutable std::mutex mutex_;                         ///< guards every member below except the atomics
    std::condition_variable_any task_ready_;           ///< signals pending_ to the task thread
    std::condition_variable thread_finished_changed_;  ///< signals thread_finished_ to stop()
    std::optional<Active> active_;                     ///< the task in progress (handed over or running)
    std::optional<Pending> pending_;                   ///< the task waiting for the task thread
    std::stop_source task_stop_;                       ///< cancels the active task
    bool thread_finished_ = false;                     ///< the task thread left run_tasks()
    analysis::WorkCounter work_units_{0};              ///< the active task's work counter
    std::atomic<bool> report_outcome_{true};           ///< false once stopping with Outcome::Discard
    std::jthread thread_;  // declared last: starts after, and stops before, the members it uses
};

// ---------------------------------------------------------------- liveness

/// Sends a Heartbeat every kHeartbeatInterval on its own thread, with the active task's work
/// counter, so the supervisor can tell a live worker with a stuck task from a busy one.
/// Heartbeats carry request sequence 0.
class HeartbeatPump {
public:
    HeartbeatPump(OutputChannel& channel, const TaskRunner& runner)
        : channel_(channel), runner_(runner), thread_([this](std::stop_token stop) { run(stop); }) {}

private:
    /// Thread body: one heartbeat per interval until stopped.
    void run(std::stop_token stop) {
        try {
            std::mutex mutex;
            std::condition_variable_any wake;
            std::unique_lock lock(mutex);
            while (true) {
                wake.wait_for(lock, stop, worker::kHeartbeatInterval, [] { return false; });
                if (stop.stop_requested()) return;
                const auto active = runner_.active();
                worker::Heartbeat heartbeat;
                if (active) {
                    heartbeat.active_job_id = active->job_id;
                    heartbeat.task_progress = active->work_units;
                }
                // A failed send means the supervisor is gone; the protocol loop ends the worker.
                (void)channel_.send(heartbeat, 0);
            }
        } catch (const std::exception&) {
            // Without heartbeats the supervisor would kill the worker at its lease deadline anyway.
            std::_Exit(kExitInternalFailure);
        }
    }

    OutputChannel& channel_;
    const TaskRunner& runner_;
    std::jthread thread_;  // declared last: starts after the members it uses
};

// ---------------------------------------------------------------- startup

/// The handshake this worker sends, altered by the faults that concern it.
worker::Handshake make_handshake(bool self_test, Fault fault) {
    worker::Handshake handshake;
    handshake.build_id =
        std::format("mso-worker protocol {}.{} {}", worker::kProtocolMajor, worker::kProtocolMinor, kCompiler);
    handshake.process_id = worker::current_process_id();
    handshake.codecs = mso::media::decoder_capabilities();
    handshake.algorithm_versions = worker::current_algorithm_versions();
    handshake.self_test = self_test;
    if (fault == Fault::WrongProcessId) handshake.process_id += 1;
    if (fault == Fault::StaleAlgorithm) handshake.algorithm_versions["tempo"] = "tempo-stale-0";
    if (fault == Fault::ForeignLimits) handshake.limits.maximum_waveform_bins /= 2;
    return handshake;
}

/// Windows: no error dialogs (the worker is headless), DLLs only from the application and
/// system directories (never from a sample folder or the current directory), and untranslated
/// binary standard streams. POSIX needs none of this: streams are binary, the supervisor sets the
/// working directory and limits, and HandleStream turns SIGPIPE into an ordinary write error.
void harden_process() {
#ifdef _WIN32
    SetErrorMode(SEM_FAILCRITICALERRORS | SEM_NOGPFAULTERRORBOX | SEM_NOOPENFILEERRORBOX);
    SetDefaultDllDirectories(LOAD_LIBRARY_SEARCH_DEFAULT_DIRS);
    _setmode(_fileno(stdin), _O_BINARY);
    _setmode(_fileno(stdout), _O_BINARY);
#endif
}

/// Sends the handshake as `fault` dictates; false when the channel is already broken.
bool introduce(OutputChannel& output, bool self_test, Fault fault) {
    if (fault == Fault::NoHandshake) return true;
    if (fault == Fault::MessageBeforeHandshake && !output.send(worker::Heartbeat{}, 0)) return false;
    const auto handshake = make_handshake(self_test, fault);
    if (!output.send(handshake, 0)) return false;
    if (fault == Fault::SecondHandshake && !output.send(handshake, 0)) return false;
    if (fault == Fault::GarbageAfterHandshake && !output.send_garbage()) return false;
    return true;
}

/// Stops `runner` and returns `exit_code`. When the task thread is stuck in a call that ignores
/// cancellation, it can be neither joined nor destroyed, so the process ends at once with
/// `exit_code` (nothing is buffered: every frame was written straight to the pipe).
int stop_and_exit(TaskRunner& runner, TaskRunner::Outcome outcome, int exit_code) {
    if (!runner.stop(outcome, kTaskStopDeadline)) std::_Exit(exit_code);
    return exit_code;
}

/// The protocol loop: handshake, then dispatch supervisor messages until Shutdown or end of
/// input. A malformed frame or message, or a request sequence that does not increase, ends the
/// worker with kExitProtocolViolation. The worker always exits within kTaskStopDeadline of being
/// told to stop, even when its task is stuck.
int run_worker(Analyzer analyzer, bool self_test, Fault fault) {
    worker::HandleStream input(worker::standard_input_handle());
    OutputChannel output(worker::standard_output_handle());
    if (!introduce(output, self_test, fault)) return kExitChannelBroken;

    TaskRunner runner(output, std::move(analyzer), fault);
    std::optional<HeartbeatPump> heartbeat;
    if (fault != Fault::NoHandshake) heartbeat.emplace(output, runner);
    std::uint64_t last_sequence = 0;
    while (true) {
        auto frame = worker::read_frame(input);
        if (!frame) return stop_and_exit(runner, TaskRunner::Outcome::Discard, kExitProtocolViolation);
        if (!*frame) {  // the supervisor closed our input: nobody is left to report to
            return stop_and_exit(runner, TaskRunner::Outcome::Discard, kExitClean);
        }
        const auto request_sequence = (*frame)->request_sequence;
        auto message = worker::decode_supervisor_message((*frame)->payload);
        if (!message || request_sequence <= last_sequence)
            return stop_and_exit(runner, TaskRunner::Outcome::Discard, kExitProtocolViolation);
        last_sequence = request_sequence;
        const bool keep_running = std::visit(Overloaded{[&](worker::AnalyzeTask& task) {
                                                            runner.start(std::move(task), request_sequence);
                                                            return true;
                                                        },
                                                        [&](const worker::Cancel& cancel) {
                                                            runner.cancel(cancel);
                                                            return true;
                                                        },
                                                        [&](const worker::Shutdown&) { return false; }},
                                             *message);
        if (!keep_running) return stop_and_exit(runner, TaskRunner::Outcome::Report, kExitClean);
    }
}

/// Parsed command line.
struct CommandLine {
    bool self_test = false;
    Fault fault = Fault::None;
};

/// Parses the command line with CLI11; nullopt (after printing usage to stderr) when invalid.
std::optional<CommandLine> parse_command_line(int argument_count, char** arguments, int& exit_code) {
    CLI::App application{"Music Sample Organizer analysis worker (started by the application, not by hand)",
                         std::string(worker::kWorkerExecutableName)};
    CommandLine command_line;
    std::string fault_name;
    auto* self_test = application.add_flag("--self-test", command_line.self_test,
                                           "Run the synthetic analyzer instead of real DSP (tests only)");
    std::vector<std::string> fault_choices;
    for (const auto& [name, fault] : kFaultNames)
        fault_choices.push_back(name);
    application.add_option("--self-test-fault", fault_name, "Misbehave in one named way (tests only)")
        ->needs(self_test)
        ->check(CLI::IsMember(fault_choices));
    try {
        application.parse(argument_count, arguments);
    } catch (const CLI::ParseError& error) {
        // Help and errors both go to stderr: stdout is the protocol channel, where CLI11's default
        // (help on std::cout) would look like a garbled frame to a supervisor.
        exit_code = error.get_exit_code() == 0 ? kExitClean : kExitUsage;
        (void)application.exit(error, std::cerr, std::cerr);  // the exit code is chosen above
        return std::nullopt;
    }
    if (!fault_name.empty()) command_line.fault = kFaultNames.at(fault_name);
    return command_line;
}

}  // namespace

int main(int argument_count, char** arguments) {
    // Exception barrier: nothing escapes to std::terminate.
    try {
        int exit_code = kExitUsage;
        const auto command_line = parse_command_line(argument_count, arguments, exit_code);
        if (!command_line) return exit_code;
        harden_process();
        const Fault fault = command_line->fault;
        Analyzer analyzer =
            command_line->self_test
                ? Analyzer([fault](const analysis::AnalysisRequest& request, std::stop_token stop,
                                   const analysis::ProgressCallback& progress, analysis::WorkCounter* work_counter) {
                      return analyze_self_test(request, std::move(stop), progress, work_counter, fault);
                  })
                : Analyzer(analysis::analyze_file);
        return run_worker(std::move(analyzer), command_line->self_test, fault);
    } catch (const std::exception&) {
        return kExitInternalFailure;
    }
}
