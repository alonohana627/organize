// Worker IPC: framing, message schema and range checks, atomic segment writes, and the supervised
// mso-worker child process, including every misbehaviour the supervisor must detect (driven by the
// worker's --self-test-fault modes). Timing is never asserted by sleeping: tests wait for events or
// poll with deadlines derived from the configured intervals.
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
#include <limits>
#include <string>
#include <vector>

#include "mso/analysis/analysis.hpp"
#include "mso/domain/core.hpp"
#include "mso/worker/worker_process.hpp"
#include "mso/worker/worker_protocol.hpp"
#include "test_support.hpp"

namespace {

using namespace std::chrono_literals;
namespace worker = mso::worker;
namespace analysis = mso::analysis;
using mso::ErrorCode;
using mso::test::ScratchDirectory;

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

/// Collects everything written to it.
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

/// A consistent waveform segment description of `bins` bins of 172 frames each.
worker::WaveformSegment make_segment(int bins) {
    worker::WaveformSegment segment;
    segment.path = "C:/derived/w.seg";
    segment.byte_length = analysis::encoded_waveform_size(static_cast<std::size_t>(bins));
    segment.sha256 = std::string(64, 'b');
    segment.bins = bins;
    segment.frames_per_bin = 172;
    segment.frames_covered = 172LL * bins;
    segment.total_frames = segment.frames_covered;
    segment.sample_rate = 44100;
    segment.complete = true;
    return segment;
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
    result.waveform_segment = make_segment(256);
    return result;
}

// ---------------------------------------------------------------- process fixtures

/// Options for a self-test worker, optionally misbehaving in the named way.
worker::WorkerOptions self_test_options(std::string_view fault = {}) {
    worker::WorkerOptions options;
    options.executable = mso::test::worker_executable();
    options.self_test = true;
    if (!fault.empty()) options.arguments = {"--self-test-fault", std::string(fault)};
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

/// The start error of a worker that must fail to start.
mso::Error expect_start_failure(const worker::WorkerOptions& options) {
    auto process = worker::WorkerProcess::start(options);
    REQUIRE_FALSE(process.has_value());
    return process.error();
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

}  // namespace

TEST_SUITE("worker") {
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
        const std::uint32_t oversized = worker::kMaximumPayloadBytes + 1;
        for (int index = 0; index < 4; ++index)
            bytes[8 + static_cast<std::size_t>(index)] = static_cast<std::uint8_t>(oversized >> (8 * index));
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
        CHECK(worker::encode_frame(1, std::string(worker::kMaximumPayloadBytes, ' ')).has_value());
        const auto oversized = worker::encode_frame(1, std::string(worker::kMaximumPayloadBytes + 1, ' '));
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

    TEST_CASE("heartbeats carry the active task and its work counter") {
        const worker::Heartbeat busy{mso::new_uuid(), 4711};
        const auto payload = worker::encode_message(worker::WorkerMessage{busy});
        REQUIRE(payload.has_value());
        const auto decoded = worker::decode_worker_message(*payload);
        REQUIRE(decoded.has_value());
        const auto& heartbeat = std::get<worker::Heartbeat>(*decoded);
        CHECK(heartbeat.active_job_id == busy.active_job_id);
        CHECK(heartbeat.task_progress == 4711u);

        // A protocol-minor-0 heartbeat without the member is still valid.
        const auto old_style = worker::decode_worker_message(R"({"type":"heartbeat","active_job_id":null})");
        REQUIRE(old_style.has_value());
        CHECK_FALSE(std::get<worker::Heartbeat>(*old_style).task_progress.has_value());
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

    TEST_CASE("every error code round-trips through its stable name") {
        for (const ErrorCode code : {ErrorCode::SourceOffline, ErrorCode::FileMissing, ErrorCode::ResourceLimit,
                                     ErrorCode::WorkerCrash, ErrorCode::Cancelled, ErrorCode::Internal}) {
            CAPTURE(mso::to_string(code));
            CHECK(worker::parse_error_code(mso::to_string(code)) == code);
        }
        CHECK_FALSE(worker::parse_error_code("TELEPATHY_FAILED").has_value());
        CHECK_FALSE(worker::parse_error_code("").has_value());
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
        task.stages.clear();
        task.waveform_bins = analysis::kMaximumWaveformBins + 1;
        CHECK_FALSE(worker::validate_task(task).has_value());
    }

    TEST_CASE("result validation rejects out-of-range values, and decoding applies it") {
        CHECK(worker::validate_result(make_result()).has_value());

        const auto rejected = [](const std::function<void(worker::TaskResult&)>& corrupt) {
            auto result = make_result();
            corrupt(result);
            const auto valid = worker::validate_result(result);
            if (valid) return false;
            CHECK(valid.error().code == ErrorCode::InvalidArgument);
            // The same result from a worker is a protocol violation, not a value to publish.
            const auto payload = worker::encode_message(worker::WorkerMessage{result});
            if (payload) {
                const auto decoded = worker::decode_worker_message(*payload);
                REQUIRE_FALSE(decoded.has_value());
                CHECK(decoded.error().code == ErrorCode::ParseError);
            }
            return true;
        };
        CHECK(rejected([](auto& result) { result.report.tempo->confidence = 1.5; }));
        CHECK(rejected([](auto& result) { result.report.tempo->milli_bpm = 20'000; }));
        CHECK(rejected([](auto& result) { result.report.tempo->milli_bpm = 999'000; }));
        CHECK(rejected([](auto& result) { result.report.key->confidence = -0.1; }));
        CHECK(rejected([](auto& result) { result.report.key->key = mso::MusicalKey{12, mso::KeyMode::Major}; }));
        CHECK(rejected([](auto& result) { result.report.key->key = mso::MusicalKey{3, mso::KeyMode::Unknown}; }));
        CHECK(rejected([](auto& result) { result.report.loudness->rms_dbfs = 3.0; }));  // above the peak
        CHECK(rejected([](auto& result) { result.report.loudness->peak_dbfs = -1000.0; }));
        CHECK(rejected([](auto& result) { result.report.hashes.byte_sha256 = std::string(64, 'G'); }));
        CHECK(rejected([](auto& result) { result.report.hashes.pcm_sha256 = std::string(63, 'a'); }));
        CHECK(rejected([](auto& result) { result.report.evidence.size_after = -1; }));
        CHECK(rejected([](auto& result) { result.report.frames_decoded = -5; }));
        CHECK(rejected(
            [](auto& result) { result.report.warnings.assign(worker::kMaximumWarnings + 1, std::string("w")); }));
        CHECK(rejected([](auto& result) { result.waveform_segment->byte_length += 1; }));
        CHECK(rejected([](auto& result) { result.waveform_segment->bins = analysis::kMaximumWaveformBins + 1; }));
        CHECK(rejected([](auto& result) { result.waveform_segment->frames_covered += 172; }));
        CHECK(rejected([](auto& result) { result.waveform_segment->sample_rate = 0; }));
        CHECK(rejected([](auto& result) { result.waveform_segment->total_frames += 1; }));
        CHECK(rejected([](auto& result) { result.waveform_segment->path.clear(); }));
    }

    // ------------------------------------------------------------ atomic segment files

    TEST_CASE("write_file_atomically publishes complete files only") {
        ScratchDirectory scratch("worker-atomic");
        const auto destination = scratch.file("segment \u00e9.seg");
        auto partial = destination;
        partial += ".partial";

        const std::vector<std::uint8_t> first(100'000, 0x5A);
        REQUIRE(worker::write_file_atomically(destination, first).has_value());
        CHECK(mso::test::read_file(destination) == std::string(first.begin(), first.end()));
        CHECK_FALSE(std::filesystem::exists(partial));

        // Replacing an existing file swaps the whole content.
        const std::vector<std::uint8_t> second{1, 2, 3};
        REQUIRE(worker::write_file_atomically(destination, second).has_value());
        CHECK(mso::test::read_file(destination) == std::string(second.begin(), second.end()));

        // A missing directory fails cleanly and leaves nothing behind.
        const auto unreachable = scratch.file("missing-directory/segment.seg");
        const auto failed = worker::write_file_atomically(unreachable, second);
        REQUIRE_FALSE(failed.has_value());
        CHECK(failed.error().code == ErrorCode::FileMissing);
        CHECK_FALSE(std::filesystem::exists(scratch.file("missing-directory")));
    }

    // ------------------------------------------------------------ options

    TEST_CASE("worker options that would make supervision unsafe are refused") {
        const auto refused = [](const std::function<void(worker::WorkerOptions&)>& change) {
            auto options = self_test_options();
            change(options);
            const auto error = expect_start_failure(options);
            return error.code == ErrorCode::InvalidArgument;
        };
        CHECK(refused([](auto& options) { options.lease_deadline = worker::kHeartbeatInterval; }));
        CHECK(refused([](auto& options) { options.task_stall_deadline = worker::kHeartbeatInterval; }));
        CHECK(refused([](auto& options) { options.cancel_grace = 0ms; }));
        CHECK(refused([](auto& options) { options.shutdown_grace = -1ms; }));
        CHECK(refused([](auto& options) { options.handshake_timeout = 0ms; }));
        CHECK(refused([](auto& options) { options.memory_limit_bytes = 1024; }));
        CHECK(refused([](auto& options) { options.cpu_rate_percent = 101; }));
        CHECK(refused([](auto& options) { options.arguments = {"two words"}; }));
    }

    // ------------------------------------------------------------ supervised process

    TEST_CASE("worker handshake reports protocol, process identity, algorithms and job limits") {
        auto process = start_worker(self_test_options());
        const auto handshake = process->handshake();
        CHECK(handshake.protocol_major == worker::kProtocolMajor);
        CHECK(handshake.protocol_minor == worker::kProtocolMinor);
        CHECK(handshake.process_id == process->process_id());
        CHECK(handshake.self_test);
        CHECK(handshake.algorithm_versions == worker::current_algorithm_versions());
        CHECK(handshake.algorithm_versions.at("waveform") == analysis::AlgorithmVersions::kWaveform);
        CHECK(handshake.limits.concurrent_tasks == 1);
        CHECK(handshake.limits.maximum_waveform_bins == analysis::kMaximumWaveformBins);

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
        CHECK_FALSE(process->task_activity().has_value());
    }

    TEST_CASE("a relative worker path is resolved before the worker starts") {
        std::error_code error;
        const auto relative = std::filesystem::relative(mso::test::worker_executable(), error);
        REQUIRE_FALSE(error);
        REQUIRE(relative.is_relative());
        auto options = self_test_options();
        options.executable = relative;
        auto process = start_worker(options);
        CHECK(process->is_running());
    }

    TEST_CASE("self-test mode is refused unless it was requested") {
        auto options = self_test_options();
        options.self_test = false;
        options.arguments = {"--self-test"};
        const auto error = expect_start_failure(options);
        CHECK(error.code == ErrorCode::WorkerCrash);
        CHECK(error.safe_detail.find("self-test") != std::string::npos);
    }

    TEST_CASE("an invalid handshake keeps the worker from starting") {
        for (const std::string_view fault : {"wrong-process-id", "message-before-handshake", "stale-algorithm"}) {
            CAPTURE(fault);
            const auto error = expect_start_failure(self_test_options(fault));
            CHECK(error.code == ErrorCode::WorkerCrash);
        }
    }

    TEST_CASE("a worker that never handshakes is terminated at the handshake timeout") {
        auto options = self_test_options("no-handshake");
        options.handshake_timeout = 1000ms;
        const auto started = std::chrono::steady_clock::now();
        const auto error = expect_start_failure(options);
        const auto elapsed = std::chrono::steady_clock::now() - started;
        CHECK(error.code == ErrorCode::WorkerCrash);
        CHECK(error.safe_detail.find("handshake") != std::string::npos);
        CHECK(elapsed >= options.handshake_timeout);
        CHECK(elapsed < options.handshake_timeout + 10s);  // the watchdog, not a lease, ended it
    }

    TEST_CASE("garbage, a second handshake or an unsequenced outcome from a live worker is a protocol violation") {
        for (const std::string_view fault : {"garbage-after-handshake", "second-handshake"}) {
            CAPTURE(fault);
            auto process = start_worker(self_test_options(fault));
            const auto exited = expect_event<worker::WorkerExited>(*process, 10s);
            REQUIRE(exited.failure.has_value());
            CHECK(exited.failure->code == ErrorCode::WorkerCrash);
            CHECK(exited.failure->safe_detail.find("protocol violation") != std::string::npos);
            CHECK_FALSE(process->is_running());
        }

        auto process = start_worker(self_test_options("unsequenced-outcome"));
        const auto task = make_task(0.1);
        REQUIRE(process->submit(task).has_value());
        const auto failure = expect_event<worker::TaskFailed>(*process, 10s);
        CHECK(failure.code == ErrorCode::WorkerCrash);
        CHECK(failure.fence == worker::TaskFence::of(task));
        const auto exited = expect_event<worker::WorkerExited>(*process, 10s);
        REQUIRE(exited.failure.has_value());
        CHECK(exited.failure->safe_detail.find("protocol violation") != std::string::npos);
    }

    TEST_CASE("self-test task reports progress and publishes a complete waveform segment") {
        ScratchDirectory scratch("worker-segment");
        const auto segment_path = scratch.file("waveform \u00e9\u00e8.seg");
        auto process = start_worker(self_test_options());
        const auto task = make_task(0.5, {analysis::Stage::Waveform, analysis::Stage::Loudness}, segment_path);
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
        CHECK(result->report.loudness.has_value());
        REQUIRE(result->waveform_segment.has_value());

        const auto& segment = *result->waveform_segment;
        CHECK(segment.bins == 256);
        CHECK(segment.complete);
        auto partial = segment_path;
        partial += ".partial";
        CHECK_FALSE(std::filesystem::exists(partial));
        const auto bytes = mso::test::read_file(segment_path);
        CHECK(bytes.size() == segment.byte_length);
        CHECK(picosha2::hash256_hex_string(bytes) == segment.sha256);
        const auto overview = analysis::decode_waveform(std::vector<std::uint8_t>(bytes.begin(), bytes.end()));
        REQUIRE(overview.has_value());
        CHECK(overview->maximum.size() == 256);
        CHECK_FALSE(process->is_busy());
    }

    TEST_CASE("worker heartbeats with task progress during a long task, refuses a second task and honors cancel") {
        auto process = start_worker(self_test_options());
        const auto long_task = make_task(60.0);
        REQUIRE(process->submit(long_task).has_value());
        const auto second = process->submit(make_task(1.0));
        REQUIRE_FALSE(second.has_value());
        CHECK(second.error().code == ErrorCode::InvalidArgument);

        // Two heartbeats, the later one carrying a moving work counter.
        const auto heartbeat_wait = 3 * worker::kHeartbeatInterval;
        CHECK(mso::test::wait_until([&] { return process->heartbeats_received() >= 2; }, 2 * heartbeat_wait));
        CHECK(mso::test::wait_until(
            [&] {
                const auto activity = process->task_activity();
                return activity && activity->work_units > 0;
            },
            heartbeat_wait));
        const auto activity = process->task_activity();
        REQUIRE(activity.has_value());
        CHECK(activity->job_id == long_task.job_id);
        CHECK_FALSE(activity->cancelling);

        const auto cancel_started = std::chrono::steady_clock::now();
        REQUIRE(process->cancel(long_task.job_id, long_task.cancel_epoch + 1).has_value());
        const auto failure = expect_event<worker::TaskFailed>(*process, 3s);
        CHECK(failure.code == ErrorCode::Cancelled);
        CHECK(failure.fence == worker::TaskFence::of(long_task));
        // Cooperative: the task ended well before the grace period would have forced it.
        CHECK(std::chrono::steady_clock::now() - cancel_started < worker::WorkerOptions{}.cancel_grace);

        // The worker survives a cooperative cancel and takes the next task.
        CHECK(process->is_running());
        const auto next_task = make_task(0.1);
        REQUIRE(process->submit(next_task).has_value());
        const auto result = expect_event<worker::TaskResult>(*process, 5s);
        CHECK(result.fence == worker::TaskFence::of(next_task));
    }

    TEST_CASE("a task thread stuck in a call is cancelled, then its worker terminated, while heartbeats run") {
        auto options = self_test_options("stall-task");
        options.task_stall_deadline = 2 * worker::kHeartbeatInterval;
        options.cancel_grace = 500ms;
        auto process = start_worker(options);
        const auto task = make_task(60.0);
        REQUIRE(process->submit(task).has_value());

        const auto failure = expect_event<worker::TaskFailed>(*process, options.task_stall_deadline + 15s);
        CHECK(failure.code == ErrorCode::WorkerCrash);
        CHECK(failure.retryable);
        CHECK(failure.fence == worker::TaskFence::of(task));
        CHECK(failure.safe_detail.find("no progress") != std::string::npos);
        // The process was alive all along: its heartbeats kept the lease, only the task was stuck.
        CHECK(process->heartbeats_received() >= 1);
        const auto exited = expect_event<worker::WorkerExited>(*process, 10s);
        REQUIRE(exited.failure.has_value());
        CHECK(exited.failure->code == ErrorCode::WorkerCrash);
        CHECK_FALSE(process->is_running());
    }

    TEST_CASE("a stalled task that honors cancellation fails retryably and the worker keeps running") {
        auto options = self_test_options("silent-task");
        options.task_stall_deadline = 2 * worker::kHeartbeatInterval;
        auto process = start_worker(options);
        const auto task = make_task(60.0);
        REQUIRE(process->submit(task).has_value());

        const auto failure = expect_event<worker::TaskFailed>(*process, options.task_stall_deadline + 15s);
        CHECK(failure.code == ErrorCode::WorkerCrash);
        CHECK(failure.retryable);
        CHECK(failure.safe_detail.find("no progress") != std::string::npos);
        CHECK(process->is_running());
        CHECK_FALSE(process->is_busy());
    }

    TEST_CASE("a worker that exceeds its memory cap fails the task with RESOURCE_LIMIT") {
        auto options = self_test_options("exhaust-memory");
#ifdef _WIN32
        options.memory_limit_bytes = 256ull * 1024 * 1024;
#else
        // RLIMIT_AS counts every mapping (shared libraries, thread stacks), so it needs headroom.
        options.memory_limit_bytes = 1024ull * 1024 * 1024;
#endif
        auto process = start_worker(options);
        REQUIRE(process->limits_status().memory_limit_applied);
        REQUIRE(process->submit(make_task(1.0)).has_value());
        const auto failure = expect_event<worker::TaskFailed>(*process, 60s);
        CHECK(failure.code == ErrorCode::ResourceLimit);
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
        options.lease_deadline = 2 * worker::kHeartbeatInterval;
        auto process = start_worker(options);
        suspend_process(process->process_id());

        const auto exited = expect_event<worker::WorkerExited>(*process, options.lease_deadline + 5s);
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
        options.executable = mso::test::worker_executable().parent_path() / "no-such-worker";
        const auto error = expect_start_failure(options);
        CHECK(error.code == ErrorCode::WorkerCrash);
    }

    TEST_CASE("real analysis of a generated WAV runs in the worker") {
        ScratchDirectory scratch("worker-real");
        const auto wav_path = scratch.file("sine 440 \u266a.wav");
        const auto segment_path = scratch.file("sine.seg");
        mso::test::write_sine_wav(wav_path, 1.0);

        worker::WorkerOptions options;
        options.executable = mso::test::worker_executable();
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
        CHECK(result.report.hashes.byte_sha256 == picosha2::hash256_hex_string(mso::test::read_file(wav_path)));
        CHECK(result.report.evidence.unchanged());
        CHECK(result.report.evidence.size_before == static_cast<std::int64_t>(std::filesystem::file_size(wav_path)));
        REQUIRE(result.waveform_segment.has_value());
        const auto bytes = mso::test::read_file(segment_path);
        const auto overview = analysis::decode_waveform(std::vector<std::uint8_t>(bytes.begin(), bytes.end()));
        REQUIRE(overview.has_value());
        CHECK(overview->maximum.size() == 128);
        CHECK(*std::ranges::max_element(overview->maximum) == doctest::Approx(0.5).epsilon(0.01));

        SUBCASE("a missing input file is a terminal FILE_MISSING failure") {
            auto missing = make_task(30.0, {analysis::Stage::Loudness});
            missing.file_path = mso::path_to_utf8(scratch.file("gone.wav"));
            REQUIRE(process->submit(missing).has_value());
            const auto failure = expect_event<worker::TaskFailed>(*process, 10s);
            CHECK(failure.code == ErrorCode::FileMissing);
            CHECK(process->is_running());
        }
    }
}
