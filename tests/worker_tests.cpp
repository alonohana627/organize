// Worker IPC: framing, message schema, and the supervised mso-worker child process.
#include <doctest.h>
#include <picosha2.h>

#ifdef _WIN32
#include <windows.h>

#include <tlhelp32.h>
#else
#include <signal.h>
#endif

#include <algorithm>
#include <chrono>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <numbers>
#include <string>
#include <thread>
#include <vector>

#include "mso/analysis/analysis.hpp"
#include "mso/analysis/worker_process.hpp"
#include "mso/analysis/worker_protocol.hpp"
#include "mso/domain/core.hpp"

namespace {

using namespace std::chrono_literals;
namespace worker = mso::worker;
namespace analysis = mso::analysis;
using mso::ErrorCode;

// ---------------------------------------------------------------- in-memory streams

/// Serves a fixed byte string in small chunks, so the reader's partial-read handling is
/// exercised, and remembers how much was consumed.
class MemorySource final : public worker::ByteSource {
public:
    explicit MemorySource(std::vector<std::uint8_t> bytes, std::size_t chunk_size = 7)
        : bytes_(std::move(bytes)), chunk_size_(chunk_size) {}

    mso::Result<std::size_t> read_some(std::span<std::uint8_t> destination) override {
        const auto count = std::min({destination.size(), chunk_size_, bytes_.size() - consumed_});
        std::copy_n(bytes_.begin() + static_cast<std::ptrdiff_t>(consumed_), count, destination.begin());
        consumed_ += count;
        return count;
    }

    std::size_t consumed() const { return consumed_; }

private:
    std::vector<std::uint8_t> bytes_;
    std::size_t chunk_size_;
    std::size_t consumed_ = 0;
};

class MemorySink final : public worker::ByteSink {
public:
    mso::Result<void> write_all(std::span<const std::uint8_t> bytes) override {
        bytes_.insert(bytes_.end(), bytes.begin(), bytes.end());
        return {};
    }

    const std::vector<std::uint8_t>& bytes() const { return bytes_; }

private:
    std::vector<std::uint8_t> bytes_;
};

std::vector<std::uint8_t> frame_bytes(std::uint64_t request_sequence, std::string_view payload) {
    auto bytes = worker::encode_frame(request_sequence, payload);
    REQUIRE(bytes.has_value());
    return *bytes;
}

// ---------------------------------------------------------------- message fixtures

worker::AnalyzeTask make_task(double seconds, std::vector<analysis::Stage> stages = {},
                              const std::filesystem::path& waveform_output = {}) {
    worker::AnalyzeTask task;
    task.job_id = mso::new_uuid();
    task.lease_id = mso::new_uuid();
    task.cancel_epoch = 3;
    task.content_revision = 17;
    task.profile_digest = "profile-sha256-0123";
    task.file_path = "C:/Samples/Kicks/kick \"hard\" & loud; del *.wav.wav";
    task.stages = std::move(stages);
    task.waveform_bins = 256;
    task.max_musical_seconds = seconds;
    if (!waveform_output.empty()) task.waveform_output_path = mso::path_to_utf8(waveform_output);
    return task;
}

worker::TaskResult make_result() {
    worker::TaskResult result;
    result.fence = worker::TaskFence::of(make_task(1.0));
    result.report.loudness = analysis::LoudnessResult{-6.02, -9.03};
    result.report.tempo = analysis::TempoResult{128000, 0.75};
    result.report.key = analysis::KeyResult{mso::MusicalKey{9, mso::KeyMode::Minor}, 0.5};
    result.report.hashes.byte_sha256 = std::string(64, 'a');
    result.report.evidence = analysis::InputEvidence{1000, 1000, 1234567890123, 1234567890123};
    result.report.frames_decoded = 44100;
    result.report.warnings = {"clipped", "short"};
    result.waveform_segment =
        worker::WaveformSegment{"C:/derived/w.seg", 4096, std::string(64, 'b'), 256, 172, 44100, 44100, 44100, true};
    return result;
}

// ---------------------------------------------------------------- process fixtures

/// The worker binary next to this test executable (or one directory up for a tests/ subfolder).
std::filesystem::path worker_executable() {
    const auto beside = worker::default_worker_executable();
    if (std::filesystem::exists(beside)) return beside;
    return beside.parent_path().parent_path() / worker::kWorkerExecutableName;
}

worker::WorkerOptions self_test_options() {
    worker::WorkerOptions options;
    options.executable = worker_executable();
    options.arguments = {"--self-test"};
    return options;
}

std::unique_ptr<worker::WorkerProcess> start_worker(const worker::WorkerOptions& options) {
    auto process = worker::WorkerProcess::start(options);
    REQUIRE_MESSAGE(process.has_value(), (process ? std::string() : process.error().message()));
    return std::move(*process);
}

/// The next event other than Progress, or nullopt after `timeout`.
std::optional<worker::WorkerEvent> next_outcome(worker::WorkerProcess& process, std::chrono::milliseconds timeout) {
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    while (std::chrono::steady_clock::now() < deadline) {
        const auto remaining =
            std::chrono::duration_cast<std::chrono::milliseconds>(deadline - std::chrono::steady_clock::now());
        auto event = process.next_event(std::max(remaining, 1ms));
        if (event && !std::holds_alternative<worker::Progress>(*event)) return event;
    }
    return std::nullopt;
}

template <class Event>
Event expect_event(worker::WorkerProcess& process, std::chrono::milliseconds timeout) {
    auto event = next_outcome(process, timeout);
    REQUIRE_MESSAGE(event.has_value(), "no event arrived in time");
    REQUIRE_MESSAGE(std::holds_alternative<Event>(*event), "unexpected event type, index ", event->index());
    return std::get<Event>(std::move(*event));
}

#ifdef _WIN32
/// Exit status kill_process() gives the worker, as WorkerExited reports it.
constexpr std::uint32_t kKilledExitCode = 0xBAD;

/// Freezes every thread of a process, simulating a hung worker.
void suspend_process(std::uint32_t process_id) {
    worker::UniqueHandle snapshot(CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0));
    REQUIRE(snapshot.valid());
    THREADENTRY32 entry{};
    entry.dwSize = sizeof entry;
    for (BOOL more = Thread32First(snapshot.get(), &entry); more; more = Thread32Next(snapshot.get(), &entry)) {
        if (entry.th32OwnerProcessID != process_id) continue;
        worker::UniqueHandle thread(OpenThread(THREAD_SUSPEND_RESUME, FALSE, entry.th32ThreadID));
        if (thread) SuspendThread(thread.get());
    }
}

/// Kills a process from outside the supervisor, simulating a crash.
void kill_process(std::uint32_t process_id) {
    worker::UniqueHandle process(OpenProcess(PROCESS_TERMINATE | SYNCHRONIZE, FALSE, process_id));
    REQUIRE(process.valid());
    REQUIRE(TerminateProcess(process.get(), kKilledExitCode));
    WaitForSingleObject(process.get(), 5000);
}
#else
constexpr std::uint32_t kKilledExitCode = 128 + SIGKILL;

void suspend_process(std::uint32_t process_id) {
    REQUIRE(::kill(static_cast<pid_t>(process_id), SIGSTOP) == 0);
}

void kill_process(std::uint32_t process_id) {
    REQUIRE(::kill(static_cast<pid_t>(process_id), SIGKILL) == 0);
}
#endif

/// A fresh directory under %TEMP%, removed with its contents on destruction.
class ScratchDirectory {
public:
    ScratchDirectory()
        : path_(std::filesystem::temp_directory_path() / ("mso-worker-tests-" + uuids::to_string(mso::new_uuid()))) {
        std::filesystem::create_directories(path_);
    }
    ScratchDirectory(const ScratchDirectory&) = delete;
    ScratchDirectory& operator=(const ScratchDirectory&) = delete;
    ~ScratchDirectory() {
        std::error_code ignored;
        std::filesystem::remove_all(path_, ignored);
    }

    const std::filesystem::path& path() const { return path_; }

private:
    std::filesystem::path path_;
};

std::string read_file(const std::filesystem::path& file) {
    std::ifstream stream(file, std::ios::binary);
    return {std::istreambuf_iterator<char>(stream), std::istreambuf_iterator<char>()};
}

/// One second of a 440 Hz sine at half scale: 16-bit PCM, mono, 44.1 kHz.
void write_sine_wav(const std::filesystem::path& file) {
    constexpr std::uint32_t kSampleRate = 44100;
    constexpr std::uint32_t kFrames = kSampleRate;
    std::string bytes;
    const auto put = [&](std::uint32_t value, int byte_count) {
        for (int index = 0; index < byte_count; ++index)
            bytes.push_back(static_cast<char>((value >> (8 * index)) & 0xFF));
    };
    bytes += "RIFF";
    put(36 + kFrames * 2, 4);
    bytes += "WAVEfmt ";
    put(16, 4);
    put(1, 2);  // PCM
    put(1, 2);  // mono
    put(kSampleRate, 4);
    put(kSampleRate * 2, 4);
    put(2, 2);
    put(16, 2);
    bytes += "data";
    put(kFrames * 2, 4);
    for (std::uint32_t frame = 0; frame < kFrames; ++frame) {
        const double sample = 0.5 * std::sin(2.0 * std::numbers::pi * 440.0 * frame / kSampleRate);
        put(static_cast<std::uint16_t>(static_cast<std::int16_t>(std::lround(sample * 32767.0))), 2);
    }
    std::ofstream(file, std::ios::binary).write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
}

}  // namespace

TEST_SUITE("analysis") {
    // ------------------------------------------------------------ framing

    TEST_CASE("worker frames round-trip through a chunked stream") {
        MemorySink sink;
        REQUIRE(worker::write_frame(sink, 1, R"({"type":"shutdown"})").has_value());
        REQUIRE(worker::write_frame(sink, 0xFFFF'FFFF'0000'0002ull, "").has_value());

        MemorySource source(sink.bytes());
        auto first = worker::read_frame(source);
        REQUIRE(first.has_value());
        REQUIRE(first->has_value());
        CHECK((*first)->request_sequence == 1);
        CHECK((*first)->payload == R"({"type":"shutdown"})");

        auto second = worker::read_frame(source);
        REQUIRE(second.has_value());
        REQUIRE(second->has_value());
        CHECK((*second)->request_sequence == 0xFFFF'FFFF'0000'0002ull);
        CHECK((*second)->payload.empty());

        auto end = worker::read_frame(source);
        REQUIRE(end.has_value());
        CHECK_FALSE(end->has_value());
    }

    TEST_CASE("worker frame header is MSOW, protocol version and little-endian fields") {
        const auto bytes = frame_bytes(0x0102030405060708ull, "{}");
        REQUIRE(bytes.size() == worker::kFrameHeaderBytes + 2);
        const std::vector<std::uint8_t> expected_header{
            'M', 'S', 'O', 'W', worker::kProtocolMajor, 0, worker::kProtocolMinor, 0, 2, 0, 0, 0, 8, 7, 6, 5,
            4,   3,   2,   1};
        CHECK(std::equal(expected_header.begin(), expected_header.end(), bytes.begin()));
    }

    TEST_CASE("worker frame reader rejects an oversized length before reading the payload") {
        auto bytes = frame_bytes(5, "{}");
        const std::uint32_t oversized = worker::kMaxPayloadBytes + 1;
        for (int index = 0; index < 4; ++index)
            bytes[8 + index] = static_cast<std::uint8_t>(oversized >> (8 * index));
        MemorySource source(bytes);
        const auto frame = worker::read_frame(source);
        REQUIRE_FALSE(frame.has_value());
        CHECK(frame.error().code == ErrorCode::ParseError);
        CHECK(source.consumed() == worker::kFrameHeaderBytes);
    }

    TEST_CASE("worker frame reader rejects garbled and truncated frames") {
        const auto valid = frame_bytes(9, R"({"type":"shutdown"})");
        const auto expect_parse_error = [](std::vector<std::uint8_t> bytes) {
            MemorySource source(std::move(bytes));
            const auto frame = worker::read_frame(source);
            REQUIRE_FALSE(frame.has_value());
            CHECK(frame.error().code == ErrorCode::ParseError);
        };

        SUBCASE("bad magic") {
            auto bytes = valid;
            bytes[0] = 'X';
            expect_parse_error(bytes);
        }
        SUBCASE("other protocol major") {
            auto bytes = valid;
            bytes[4] = worker::kProtocolMajor + 1;
            expect_parse_error(bytes);
        }
        SUBCASE("stream ends inside the header") {
            expect_parse_error(std::vector<std::uint8_t>(valid.begin(), valid.begin() + 11));
        }
        SUBCASE("stream ends inside the payload") {
            expect_parse_error(std::vector<std::uint8_t>(valid.begin(), valid.end() - 1));
        }
        SUBCASE("random bytes") {
            expect_parse_error(std::vector<std::uint8_t>(64, 0xA5));
        }
    }

    TEST_CASE("encode_frame refuses payloads above 1 MiB") {
        CHECK(worker::encode_frame(1, std::string(worker::kMaxPayloadBytes, ' ')).has_value());
        const auto oversized = worker::encode_frame(1, std::string(worker::kMaxPayloadBytes + 1, ' '));
        REQUIRE_FALSE(oversized.has_value());
        CHECK(oversized.error().code == ErrorCode::InvalidArgument);
    }

    // ------------------------------------------------------------ messages

    TEST_CASE("analyze task round-trips with its fence and treats the path as plain data") {
        const auto task = make_task(42.5, {analysis::Stage::Waveform, analysis::Stage::Key}, "C:/derived/out.seg");
        const auto payload = worker::encode_message(worker::SupervisorMessage{task});
        REQUIRE(payload.has_value());
        const auto decoded = worker::decode_supervisor_message(*payload);
        REQUIRE(decoded.has_value());
        const auto* round_tripped = std::get_if<worker::AnalyzeTask>(&*decoded);
        REQUIRE(round_tripped != nullptr);
        CHECK(worker::TaskFence::of(*round_tripped) == worker::TaskFence::of(task));
        CHECK(round_tripped->file_path == task.file_path);
        CHECK(round_tripped->stages == task.stages);
        CHECK(round_tripped->waveform_bins == task.waveform_bins);
        CHECK(round_tripped->max_musical_seconds == task.max_musical_seconds);
        CHECK(round_tripped->waveform_output_path == task.waveform_output_path);
    }

    TEST_CASE("task result keeps every report field across the wire") {
        const auto result = make_result();
        const auto payload = worker::encode_message(worker::WorkerMessage{result});
        REQUIRE(payload.has_value());
        const auto decoded = worker::decode_worker_message(*payload);
        REQUIRE(decoded.has_value());
        const auto* round_tripped = std::get_if<worker::TaskResult>(&*decoded);
        REQUIRE(round_tripped != nullptr);
        CHECK(round_tripped->fence == result.fence);
        CHECK(round_tripped->report.loudness->peak_dbfs == result.report.loudness->peak_dbfs);
        CHECK(round_tripped->report.loudness->rms_dbfs == result.report.loudness->rms_dbfs);
        CHECK(round_tripped->report.tempo->milli_bpm == result.report.tempo->milli_bpm);
        CHECK(round_tripped->report.tempo->confidence == result.report.tempo->confidence);
        CHECK(round_tripped->report.key->key == result.report.key->key);
        CHECK(round_tripped->report.hashes.byte_sha256 == result.report.hashes.byte_sha256);
        CHECK_FALSE(round_tripped->report.hashes.pcm_sha256.has_value());
        CHECK(round_tripped->report.evidence.mtime_ns_after == result.report.evidence.mtime_ns_after);
        CHECK(round_tripped->report.frames_decoded == result.report.frames_decoded);
        CHECK(round_tripped->report.warnings == result.report.warnings);
        REQUIRE(round_tripped->waveform_segment.has_value());
        CHECK(round_tripped->waveform_segment->sha256 == result.waveform_segment->sha256);
        CHECK(round_tripped->waveform_segment->bins == 256);
        CHECK(round_tripped->waveform_segment->complete);
    }

    TEST_CASE("task failure carries its stable error code") {
        const worker::TaskFailed failure{worker::TaskFence::of(make_task(1.0)), ErrorCode::UnsupportedCodec, false,
                                         "no decoder for alac"};
        const auto payload = worker::encode_message(worker::WorkerMessage{failure});
        REQUIRE(payload.has_value());
        CHECK(payload->find(R"("code":"UNSUPPORTED_CODEC")") != std::string::npos);
        const auto decoded = worker::decode_worker_message(*payload);
        REQUIRE(decoded.has_value());
        const auto error = std::get<worker::TaskFailed>(*decoded).to_error();
        CHECK(error.code == ErrorCode::UnsupportedCodec);
        CHECK(error.safe_detail == "no decoder for alac");
    }

    TEST_CASE("malformed messages are parse errors, never exceptions") {
        const auto valid_task = *worker::encode_message(worker::SupervisorMessage{make_task(1.0)});
        const auto with = [&](std::string_view from, std::string_view to) {
            auto changed = valid_task;
            const auto at = changed.find(from);
            REQUIRE(at != std::string::npos);
            return changed.replace(at, from.size(), to);
        };
        const std::vector<std::string> malformed{
            "not json",
            "[1,2,3]",
            R"({"type":"launch_missiles"})",
            R"({"type":"cancel","job_id":"not-a-uuid","cancel_epoch":1})",
            with(R"("cancel_epoch":3)", R"("cancel_epoch":3.5)"),
            with(R"("waveform_bins":256)", R"("waveform_bins":0)"),
            with(R"("stages":[])", R"("stages":["telepathy"])"),
            with(R"("lease_id")", R"("unrelated")"),
            std::string("{\"type\":\"cancel\",\"job_id\":\"\xff\xfe\"}"),
        };
        for (const auto& payload : malformed) {
            const auto decoded = worker::decode_supervisor_message(payload);
            REQUIRE_FALSE(decoded.has_value());
            CHECK(decoded.error().code == ErrorCode::ParseError);
        }
        const auto progress = worker::decode_worker_message(
            R"({"type":"progress","job_id":"00000000-0000-4000-8000-000000000001","cancel_epoch":0,"fraction":1.5})");
        REQUIRE_FALSE(progress.has_value());
        CHECK(progress.error().code == ErrorCode::ParseError);
    }

    TEST_CASE("task validation rejects inconsistent waveform output") {
        auto task = make_task(1.0, {analysis::Stage::Waveform});
        const auto missing_output = worker::validate_task(task);
        REQUIRE_FALSE(missing_output.has_value());
        CHECK(missing_output.error().code == ErrorCode::InvalidArgument);
        task.stages.clear();
        task.waveform_output_path = "C:/derived/out.seg";
        CHECK_FALSE(worker::validate_task(task).has_value());
        task.waveform_output_path.clear();
        CHECK(worker::validate_task(task).has_value());
        task.stages = {analysis::Stage::Key, analysis::Stage::Key};
        CHECK_FALSE(worker::validate_task(task).has_value());
    }

    // ------------------------------------------------------------ supervised process

    TEST_CASE("worker handshake reports protocol, process identity and job limits") {
        auto process = start_worker(self_test_options());
        const auto handshake = process->handshake();
        CHECK(handshake.protocol_major == worker::kProtocolMajor);
        CHECK(handshake.process_id == process->process_id());
        CHECK(handshake.self_test);
        CHECK(handshake.algorithm_versions.at("waveform") == analysis::AlgorithmVersions::kWaveform);
        CHECK(handshake.limits.concurrent_tasks == 1);

        const auto limits = process->limits_status();
        CHECK(limits.memory_limit_applied);
        CHECK(limits.memory_limit_bytes == 2ull * 1024 * 1024 * 1024);
#ifdef _WIN32
        CHECK(limits.kill_on_job_close);
        // Either the 40% hard cap is in force or the fallback is explained.
        CHECK((limits.cpu_rate_cap_applied ? limits.cpu_rate_percent == 40 : !limits.detail.empty()));
#else
        CHECK_FALSE(limits.cpu_rate_cap_applied);
        CHECK_FALSE(limits.detail.empty());
#endif
        CHECK(process->is_running());
    }

    TEST_CASE("self-test task reports progress and publishes a complete waveform segment") {
        ScratchDirectory scratch;
        const auto segment_path = scratch.path() / mso::path_from_utf8("waveform \u00e9\u00e8.seg");
        auto process = start_worker(self_test_options());
        const auto task = make_task(0.5, {analysis::Stage::Waveform}, segment_path);
        REQUIRE(process->submit(task).has_value());

        bool saw_progress = false;
        std::optional<worker::TaskResult> result;
        const auto deadline = std::chrono::steady_clock::now() + 10s;
        while (!result && std::chrono::steady_clock::now() < deadline) {
            auto event = process->next_event(100ms);
            if (!event) continue;
            if (std::holds_alternative<worker::Progress>(*event)) saw_progress = true;
            if (auto* finished = std::get_if<worker::TaskResult>(&*event)) result = std::move(*finished);
            REQUIRE_FALSE(std::holds_alternative<worker::TaskFailed>(*event));
        }
        REQUIRE(result.has_value());
        CHECK(saw_progress);
        CHECK(result->fence == worker::TaskFence::of(task));
        CHECK_FALSE(result->report.waveform.has_value());
        REQUIRE(result->waveform_segment.has_value());

        const auto& segment = *result->waveform_segment;
        CHECK(segment.bins == 256);
        CHECK(segment.complete);
        CHECK_FALSE(std::filesystem::exists(std::filesystem::path(segment_path) += ".partial"));
        const auto bytes = read_file(segment_path);
        CHECK(bytes.size() == segment.byte_length);
        CHECK(picosha2::hash256_hex_string(bytes) == segment.sha256);
        const auto overview = analysis::decode_waveform(std::vector<std::uint8_t>(bytes.begin(), bytes.end()));
        REQUIRE(overview.has_value());
        CHECK(overview->maximum.size() == 256);
        CHECK_FALSE(process->is_busy());
    }

    TEST_CASE("worker heartbeats during a long task, refuses a second task and honors cancel") {
        auto process = start_worker(self_test_options());
        const auto long_task = make_task(60.0);
        REQUIRE(process->submit(long_task).has_value());
        const auto second = process->submit(make_task(1.0));
        REQUIRE_FALSE(second.has_value());
        CHECK(second.error().code == ErrorCode::InvalidArgument);

        std::this_thread::sleep_for(4500ms);
        CHECK(process->heartbeats_received() >= 2);

        const auto cancel_started = std::chrono::steady_clock::now();
        REQUIRE(process->cancel(long_task.job_id, long_task.cancel_epoch + 1).has_value());
        const auto failure = expect_event<worker::TaskFailed>(*process, 3s);
        CHECK(failure.code == ErrorCode::Cancelled);
        CHECK(failure.fence == worker::TaskFence::of(long_task));
        CHECK(std::chrono::steady_clock::now() - cancel_started < 2s);

        // The worker survives a cooperative cancel and takes the next task.
        CHECK(process->is_running());
        const auto next_task = make_task(0.1);
        REQUIRE(process->submit(next_task).has_value());
        const auto result = expect_event<worker::TaskResult>(*process, 5s);
        CHECK(result.fence == worker::TaskFence::of(next_task));
    }

    TEST_CASE("a worker that ignores cancellation is terminated after the grace period") {
        auto options = self_test_options();
        options.cancel_grace = 1000ms;
        auto process = start_worker(options);
        const auto task = make_task(60.0);
        REQUIRE(process->submit(task).has_value());
        suspend_process(process->process_id());

        REQUIRE(process->cancel(task.job_id, task.cancel_epoch).has_value());
        const auto failure = expect_event<worker::TaskFailed>(*process, 5s);
        CHECK(failure.code == ErrorCode::Cancelled);
        const auto exited = expect_event<worker::WorkerExited>(*process, 5s);
        REQUIRE(exited.failure.has_value());
        CHECK(exited.failure->code == ErrorCode::Cancelled);
        CHECK_FALSE(process->is_running());
    }

    TEST_CASE("a hung worker loses its heartbeat lease") {
        auto options = self_test_options();
        options.lease_deadline = 1500ms;
        auto process = start_worker(options);
        suspend_process(process->process_id());

        const auto exited = expect_event<worker::WorkerExited>(*process, 6s);
        REQUIRE(exited.failure.has_value());
        CHECK(exited.failure->code == ErrorCode::WorkerCrash);
        CHECK(exited.failure->retryable);
        const auto refused = process->submit(make_task(1.0));
        REQUIRE_FALSE(refused.has_value());
        CHECK(refused.error().code == ErrorCode::WorkerCrash);
    }

    TEST_CASE("a killed worker is reported as WORKER_CRASH and restarts cleanly") {
        auto process = start_worker(self_test_options());
        const auto task = make_task(60.0);
        REQUIRE(process->submit(task).has_value());
        const auto first_process_id = process->process_id();
        kill_process(first_process_id);

        const auto failure = expect_event<worker::TaskFailed>(*process, 5s);
        CHECK(failure.code == ErrorCode::WorkerCrash);
        CHECK(failure.retryable);
        CHECK(failure.fence == worker::TaskFence::of(task));
        const auto exited = expect_event<worker::WorkerExited>(*process, 5s);
        REQUIRE(exited.failure.has_value());
        CHECK(exited.failure->code == ErrorCode::WorkerCrash);
        CHECK(exited.exit_code == kKilledExitCode);

        REQUIRE(process->restart().has_value());
        CHECK(process->is_running());
        CHECK(process->process_id() != first_process_id);
        const auto retry = make_task(0.1);
        REQUIRE(process->submit(retry).has_value());
        CHECK(expect_event<worker::TaskResult>(*process, 5s).fence == worker::TaskFence::of(retry));
    }

    TEST_CASE("shutdown ends the worker cleanly and reports the task in flight as cancelled") {
        auto process = start_worker(self_test_options());
        const auto task = make_task(60.0);
        REQUIRE(process->submit(task).has_value());
        process->shutdown();

        const auto failure = expect_event<worker::TaskFailed>(*process, 5s);
        CHECK(failure.code == ErrorCode::Cancelled);
        const auto exited = expect_event<worker::WorkerExited>(*process, 5s);
        CHECK_FALSE(exited.failure.has_value());
        CHECK(exited.exit_code == 0);
        CHECK_FALSE(process->is_running());
        const auto refused = process->submit(make_task(1.0));
        REQUIRE_FALSE(refused.has_value());
        CHECK(refused.error().code == ErrorCode::WorkerCrash);
    }

    TEST_CASE("a missing worker executable fails to start") {
        auto options = self_test_options();
        options.executable = worker_executable().parent_path() / "no-such-worker";
        const auto process = worker::WorkerProcess::start(options);
        REQUIRE_FALSE(process.has_value());
        CHECK(process.error().code == ErrorCode::WorkerCrash);
    }

    TEST_CASE("real analysis of a generated WAV runs in the worker") {
        ScratchDirectory scratch;
        const auto wav_path = scratch.path() / mso::path_from_utf8("sine 440 \u266a.wav");
        const auto segment_path = scratch.path() / "sine.seg";
        write_sine_wav(wav_path);

        worker::WorkerOptions options;
        options.executable = worker_executable();
        auto process = start_worker(options);
        CHECK_FALSE(process->handshake().self_test);

        auto task = make_task(30.0, {analysis::Stage::Waveform, analysis::Stage::Loudness, analysis::Stage::ByteHash},
                              segment_path);
        task.file_path = mso::path_to_utf8(wav_path);
        task.waveform_bins = 128;
        REQUIRE(process->submit(task).has_value());
        auto event = next_outcome(*process, 20s);
        REQUIRE(event.has_value());
        if (const auto* failure = std::get_if<worker::TaskFailed>(&*event))
            FAIL("analysis failed: ", failure->to_error().message());
        const auto& result = std::get<worker::TaskResult>(*event);

        REQUIRE(result.report.loudness.has_value());
        CHECK(result.report.loudness->peak_dbfs == doctest::Approx(20.0 * std::log10(0.5)).epsilon(0.01));
        CHECK(result.report.hashes.byte_sha256 == picosha2::hash256_hex_string(read_file(wav_path)));
        CHECK(result.report.evidence.unchanged());
        CHECK(result.report.evidence.size_before == static_cast<std::int64_t>(std::filesystem::file_size(wav_path)));
        REQUIRE(result.waveform_segment.has_value());
        const auto bytes = read_file(segment_path);
        const auto overview = analysis::decode_waveform(std::vector<std::uint8_t>(bytes.begin(), bytes.end()));
        REQUIRE(overview.has_value());
        CHECK(overview->maximum.size() == 128);
        CHECK(*std::ranges::max_element(overview->maximum) == doctest::Approx(0.5).epsilon(0.01));

        SUBCASE("a missing input file is a terminal FILE_MISSING failure") {
            auto missing = make_task(30.0, {analysis::Stage::Loudness});
            missing.file_path = mso::path_to_utf8(scratch.path() / "gone.wav");
            REQUIRE(process->submit(missing).has_value());
            const auto failure = expect_event<worker::TaskFailed>(*process, 10s);
            CHECK(failure.code == ErrorCode::FileMissing);
            CHECK(process->is_running());
        }
    }
}
