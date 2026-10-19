/// @file
/// mso-worker: the analysis child process (docs/02_ARCHITECTURE.md section 3, docs/04_CONTRACTS.md
/// section 4). It reads framed supervisor messages from stdin, writes framed replies to stdout,
/// analyzes one file at a time on a task thread and reports liveness every two seconds.
///
/// The worker never opens SQLite, never starts processes or shell commands and treats every path
/// it receives as data. Its only output besides the protocol stream is the waveform segment file
/// the supervisor named, written through a temporary sibling and an atomic rename.
///
/// Usage: mso-worker[.exe] [--self-test]
///   --self-test  replace the real analyzer with a synthetic one that never opens the input file;
///                used by tests to exercise cancellation, heartbeats and crash handling.
#ifdef _WIN32
#include <picosha2.h>
#include <windows.h>

#include <fcntl.h>
#include <io.h>
#endif

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <cstdio>
#include <format>
#include <functional>
#include <map>
#include <mutex>
#include <new>
#include <numbers>
#include <optional>
#include <stop_token>
#include <string>
#include <string_view>
#include <thread>
#include <utility>

#include "mso/analysis/analysis.hpp"
#include "mso/analysis/worker_process.hpp"
#include "mso/analysis/worker_protocol.hpp"
#include "mso/domain/core.hpp"
#include "mso/domain/error.hpp"
#include "mso/media/media.hpp"

namespace {

namespace analysis = mso::analysis;
namespace worker = mso::worker;
using mso::ErrorCode;
using mso::Result;

constexpr int kExitClean = 0;
constexpr int kExitProtocolViolation = 2;
constexpr int kExitChannelBroken = 3;
constexpr int kExitUsage = 64;

/// Progress frames are throttled to this period (10 Hz) whatever rate the analyzer reports at.
constexpr std::chrono::milliseconds kProgressInterval{100};

/// Signature shared by mso::analysis::analyze_file and the self-test analyzer.
using Analyzer = std::function<Result<analysis::AnalysisReport>(const analysis::AnalysisRequest&, std::stop_token,
                                                                const analysis::ProgressCallback&)>;

/// Builds a visitor from lambdas.
template <class... Handlers>
struct Overloaded : Handlers... {
    using Handlers::operator()...;
};

mso::Error error_of(ErrorCode code, std::string detail, bool retryable = false) {
    return mso::Error{code, retryable, std::move(detail), std::nullopt, std::nullopt};
}

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

private:
    worker::HandleStream stream_;
    std::mutex mutex_;
    bool broken_ = false;
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
    constexpr std::int64_t kFramesPerBin = 64;
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

/// Stand-in for analyze_file in --self-test mode. It never opens the input file; it simulates
/// `max_musical_seconds` of work in 20 ms blocks, honoring stop between blocks like the real
/// analyzer, and returns a synthetic report (with a waveform when that stage was requested).
Result<analysis::AnalysisReport> analyze_self_test(const analysis::AnalysisRequest& request, std::stop_token stop,
                                                   const analysis::ProgressCallback& progress) {
    using namespace std::chrono;
    constexpr milliseconds kBlock{20};
    const auto simulated = duration_cast<milliseconds>(duration<double>(request.max_musical_seconds));
    const auto started = steady_clock::now();
    for (auto elapsed = 0ms; elapsed < simulated;
         elapsed = duration_cast<milliseconds>(steady_clock::now() - started)) {
        if (stop.stop_requested()) return mso::fail(ErrorCode::Cancelled, "analysis cancelled");
        if (progress) progress(static_cast<double>(elapsed.count()) / static_cast<double>(simulated.count()));
        std::this_thread::sleep_for(kBlock);
    }
    analysis::AnalysisReport report;
    report.warnings.push_back("self-test: synthetic report; the input file was not read");
    if (std::ranges::contains(request.stages, analysis::Stage::Waveform)) {
        report.waveform = synthetic_waveform(request.waveform_bins);
    }
    return report;
}

// ---------------------------------------------------------------- task execution

/// Runs at most one AnalyzeTask at a time on its own thread with a stop token, and sends its
/// progress and outcome. Driven by the main thread only; active_job() may be called from any.
class TaskRunner {
public:
    /// What to do with the outcome of a task that is being stopped.
    enum class Outcome { Report, Discard };

    TaskRunner(OutputChannel& channel, Analyzer analyzer) : channel_(channel), analyzer_(std::move(analyzer)) {}
    TaskRunner(const TaskRunner&) = delete;
    TaskRunner& operator=(const TaskRunner&) = delete;
    ~TaskRunner() { stop(Outcome::Discard); }

    /// Starts `task`. While another task runs the request is refused with a retryable
    /// RESOURCE_LIMIT failure: the worker runs one task at a time.
    void start(worker::AnalyzeTask task, std::uint64_t request_sequence) {
        {
            std::lock_guard lock(mutex_);
            if (active_) {
                (void)channel_.send(worker::TaskFailed{worker::TaskFence::of(task), ErrorCode::ResourceLimit, true,
                                                       "worker already runs a task"},
                                    request_sequence);
                return;
            }
        }
        if (thread_.joinable()) thread_.join();  // the previous task has finished; only its last send may remain
        {
            std::lock_guard lock(mutex_);
            active_ = ActiveTask{task.job_id, task.cancel_epoch};
        }
        thread_ = std::jthread([this, task = std::move(task), request_sequence](std::stop_token stop) {
            run(stop, task, request_sequence);
        });
    }

    /// Requests stop of the active task when the cancel names it with a current or newer epoch.
    void cancel(const worker::Cancel& cancel) {
        std::lock_guard lock(mutex_);
        if (active_ && active_->job_id == cancel.job_id && cancel.cancel_epoch >= active_->cancel_epoch) {
            thread_.request_stop();
        }
    }

    std::optional<uuids::uuid> active_job() const {
        std::lock_guard lock(mutex_);
        return active_ ? std::optional(active_->job_id) : std::nullopt;
    }

    /// Stops the active task (if any) and waits for its thread.
    void stop(Outcome outcome) {
        report_outcome_ = outcome == Outcome::Report;
        thread_.request_stop();
        if (thread_.joinable()) thread_.join();
    }

private:
    struct ActiveTask {
        uuids::uuid job_id;
        std::int64_t cancel_epoch = 0;
    };

    void run(std::stop_token stop, const worker::AnalyzeTask& task, std::uint64_t request_sequence) {
        auto outcome = execute(task, stop, request_sequence);
        {
            // Cleared before sending so a supervisor that reacts instantly with the next task
            // never finds the worker still busy.
            std::lock_guard lock(mutex_);
            active_.reset();
        }
        if (report_outcome_) send_outcome(outcome, worker::TaskFence::of(task), request_sequence);
    }

    worker::WorkerMessage execute(const worker::AnalyzeTask& task, std::stop_token stop,
                                  std::uint64_t request_sequence) {
        const auto fence = worker::TaskFence::of(task);
        const auto failed = [&](const mso::Error& error) -> worker::WorkerMessage {
            return worker::TaskFailed{fence, error.code, error.retryable, error.safe_detail};
        };
        const auto progress = throttled_progress(task, request_sequence);

        Result<analysis::AnalysisReport> report = mso::fail(ErrorCode::Internal);
        try {
            report = analyzer_(worker::to_analysis_request(task), stop, progress);
        } catch (const std::bad_alloc&) {
            return failed(error_of(ErrorCode::ResourceLimit, "analysis ran out of memory"));
        } catch (const std::exception&) {
            return failed(error_of(ErrorCode::Internal, "analysis raised an unexpected exception"));
        }
        if (!report) return failed(report.error());

        worker::TaskResult result{fence, std::move(*report), std::nullopt};
        if (result.report.waveform && !task.waveform_output_path.empty()) {
            auto segment = write_waveform_segment(*result.report.waveform, task.waveform_output_path);
            if (!segment) return failed(segment.error());
            result.waveform_segment = std::move(*segment);
        }
        result.report.waveform.reset();  // the overview travels in the segment file, never inline
        return result;
    }

    /// Progress callback that forwards at most one frame per kProgressInterval.
    analysis::ProgressCallback throttled_progress(const worker::AnalyzeTask& task, std::uint64_t request_sequence) {
        return [this, job_id = task.job_id, cancel_epoch = task.cancel_epoch, request_sequence,
                last_sent = std::chrono::steady_clock::time_point{}](double fraction) mutable {
            const auto now = std::chrono::steady_clock::now();
            if (!std::isfinite(fraction) || now - last_sent < kProgressInterval) return;
            last_sent = now;
            (void)channel_.send(worker::Progress{job_id, cancel_epoch, std::clamp(fraction, 0.0, 1.0)},
                                request_sequence);
        };
    }

    /// Sends the outcome; a result that cannot be encoded is reported as an INTERNAL failure.
    void send_outcome(const worker::WorkerMessage& outcome, const worker::TaskFence& fence,
                      std::uint64_t request_sequence) {
        auto sent = channel_.send(outcome, request_sequence);
        if (!sent && sent.error().code == ErrorCode::InvalidArgument) {
            (void)channel_.send(worker::TaskFailed{fence, ErrorCode::Internal, false, sent.error().safe_detail},
                                request_sequence);
        }
    }

    OutputChannel& channel_;
    Analyzer analyzer_;
    mutable std::mutex mutex_;
    std::optional<ActiveTask> active_;
    std::atomic<bool> report_outcome_{true};
    std::jthread thread_;
};

// ---------------------------------------------------------------- liveness

/// Sends a Heartbeat every kHeartbeatInterval on its own thread, independent of task progress.
/// Heartbeats carry request sequence 0.
class HeartbeatPump {
public:
    HeartbeatPump(OutputChannel& channel, const TaskRunner& runner)
        : channel_(channel), runner_(runner), thread_([this](std::stop_token stop) { run(stop); }) {}

private:
    void run(std::stop_token stop) {
        std::mutex mutex;
        std::condition_variable_any wake;
        std::unique_lock lock(mutex);
        while (true) {
            wake.wait_for(lock, stop, worker::kHeartbeatInterval, [] { return false; });
            if (stop.stop_requested()) return;
            (void)channel_.send(worker::Heartbeat{runner_.active_job()}, 0);
        }
    }

    OutputChannel& channel_;
    const TaskRunner& runner_;
    std::jthread thread_;  // declared last: starts after the members it uses
};

// ---------------------------------------------------------------- startup

std::map<std::string, std::string> algorithm_versions() {
    using analysis::Stage;
    using Versions = analysis::AlgorithmVersions;
    const std::array<std::pair<Stage, std::string_view>, 6> versions{{{Stage::Waveform, Versions::kWaveform},
                                                                      {Stage::Loudness, Versions::kLoudness},
                                                                      {Stage::Tempo, Versions::kTempo},
                                                                      {Stage::Key, Versions::kKey},
                                                                      {Stage::ByteHash, Versions::kByteHash},
                                                                      {Stage::PcmHash, Versions::kPcmHash}}};
    std::map<std::string, std::string> by_stage;
    for (const auto& [stage, version] : versions)
        by_stage.emplace(analysis::to_string(stage), version);
    return by_stage;
}

worker::Handshake make_handshake(bool self_test) {
    worker::Handshake handshake;
    handshake.build_id = std::format("mso-worker protocol {}.{} clang {}.{}.{}", worker::kProtocolMajor,
                                     worker::kProtocolMinor, __clang_major__, __clang_minor__, __clang_patchlevel__);
    handshake.process_id = worker::current_process_id();
    handshake.codecs = mso::media::decoder_capabilities();
    handshake.algorithm_versions = algorithm_versions();
    handshake.self_test = self_test;
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

/// The protocol loop: handshake, then dispatch supervisor messages until Shutdown or end of
/// input. A malformed frame or message ends the worker with kExitProtocolViolation.
int run_worker(Analyzer analyzer, bool self_test) {
    worker::HandleStream input(worker::standard_input_handle());
    OutputChannel output(worker::standard_output_handle());
    if (!output.send(make_handshake(self_test), 0)) return kExitChannelBroken;

    TaskRunner runner(output, std::move(analyzer));
    HeartbeatPump heartbeat(output, runner);
    while (true) {
        auto frame = worker::read_frame(input);
        if (!frame) {
            runner.stop(TaskRunner::Outcome::Discard);
            return kExitProtocolViolation;
        }
        if (!*frame) {  // the supervisor closed our input: nobody is left to report to
            runner.stop(TaskRunner::Outcome::Discard);
            return kExitClean;
        }
        auto message = worker::decode_supervisor_message((*frame)->payload);
        if (!message) {
            runner.stop(TaskRunner::Outcome::Discard);
            return kExitProtocolViolation;
        }
        const auto request_sequence = (*frame)->request_sequence;
        const bool keep_running = std::visit(Overloaded{[&](worker::AnalyzeTask& task) {
                                                            runner.start(std::move(task), request_sequence);
                                                            return true;
                                                        },
                                                        [&](const worker::Cancel& cancel) {
                                                            runner.cancel(cancel);
                                                            return true;
                                                        },
                                                        [&](const worker::Shutdown&) {
                                                            runner.stop(TaskRunner::Outcome::Report);
                                                            return false;
                                                        }},
                                             *message);
        if (!keep_running) return kExitClean;
    }
}

}  // namespace

int main(int argument_count, char** arguments) {
    bool self_test = false;
    for (int index = 1; index < argument_count; ++index) {
        if (std::string_view(arguments[index]) != "--self-test") {
            std::fputs(std::format("usage: {} [--self-test]\n", worker::kWorkerExecutableName).c_str(), stderr);
            return kExitUsage;
        }
        self_test = true;
    }
    harden_process();
    return run_worker(self_test ? Analyzer(analyze_self_test) : Analyzer(analysis::analyze_file), self_test);
}
