#pragma once
/// @file
/// Wire protocol between the job supervisor and the mso-worker child process
/// (docs/04_CONTRACTS.md section 4). Platform-free: framing, the typed message schema, its JSON
/// encoding and the range checks both peers apply. Both sides link this file, so a schema change
/// is one edit plus a protocol bump.
///
/// Frame layout, all integers little-endian:
///
///     offset 0   4 bytes  magic "MSOW"
///     offset 4   uint16   protocol major
///     offset 6   uint16   protocol minor
///     offset 8   uint32   payload length (at most kMaximumPayloadBytes)
///     offset 12  uint64   request sequence
///     offset 20  payload  UTF-8 JSON object with a "type" member
///
/// Request sequences: the supervisor numbers its frames 1, 2, 3, ... (strictly increasing); the
/// worker answers a task's Progress, TaskResult and TaskFailed with the sequence of that task's
/// AnalyzeTask frame, and sends Handshake and Heartbeat with sequence 0.
///
/// Any malformed frame or message is a protocol violation: the receiver closes the channel and
/// fails the active job. Nothing in a payload is ever executed; file paths are opaque data.
#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <map>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

#include "mso/analysis/analysis.hpp"
#include "mso/analysis/limits.hpp"
#include "mso/domain/core.hpp"
#include "mso/domain/error.hpp"

namespace mso::worker {

// ---------------------------------------------------------------- protocol constants

/// First four bytes of every frame.
inline constexpr std::array<std::uint8_t, 4> kFrameMagic{'M', 'S', 'O', 'W'};
/// Peers must agree on the major version; a minor bump only adds optional members.
/// Minor 1 added Heartbeat::task_progress.
inline constexpr std::uint16_t kProtocolMajor = 1;
inline constexpr std::uint16_t kProtocolMinor = 1;  ///< see kProtocolMajor
/// Size of the fixed frame header (see the layout at the top of this file).
inline constexpr std::size_t kFrameHeaderBytes = 20;
/// Payloads above this are rejected from the header alone, before any allocation.
inline constexpr std::uint32_t kMaximumPayloadBytes = 1024 * 1024;
/// The worker reports liveness at this interval, also while a task is running.
inline constexpr std::chrono::milliseconds kHeartbeatInterval{2000};
/// Upper bound on TaskResult warnings; the analyzer emits at most a handful.
inline constexpr std::size_t kMaximumWarnings = 64;

// ---------------------------------------------------------------- framing

/// Decoded fixed-size frame header.
struct FrameHeader {
    std::uint16_t protocol_major = kProtocolMajor;
    std::uint16_t protocol_minor = kProtocolMinor;
    std::uint32_t payload_length = 0;
    std::uint64_t request_sequence = 0;
};

/// One complete frame: the sequence it belongs to and its JSON payload.
struct Frame {
    std::uint64_t request_sequence = 0;
    std::string payload;
};

/// Blocking source of bytes, such as a pipe. Implementations need not be thread-safe.
class ByteSource {
public:
    virtual ~ByteSource() = default;
    /// Reads at least one byte into `destination` unless the stream ended, in which case it
    /// returns 0. Errors mean the channel is unusable.
    [[nodiscard]] virtual Result<std::size_t> read_some(std::span<std::uint8_t> destination) = 0;
};

/// Blocking sink of bytes, such as a pipe. Implementations need not be thread-safe.
class ByteSink {
public:
    virtual ~ByteSink() = default;
    /// Writes every byte or fails; a partial write leaves the channel unusable.
    [[nodiscard]] virtual Result<void> write_all(std::span<const std::uint8_t> bytes) = 0;
};

/// Serializes one frame. Fails with INVALID_ARGUMENT when the payload exceeds kMaximumPayloadBytes.
[[nodiscard]] Result<std::vector<std::uint8_t>> encode_frame(std::uint64_t request_sequence, std::string_view payload);

/// Validates magic, protocol major and payload length of a raw header. Returns PARSE_ERROR for
/// anything malformed, so an oversized length is refused before its payload is allocated.
[[nodiscard]] Result<FrameHeader> parse_frame_header(std::span<const std::uint8_t, kFrameHeaderBytes> bytes);

/// Reads the next frame. Returns nullopt when the stream ends cleanly on a frame boundary and
/// PARSE_ERROR for a garbled, oversized or truncated frame.
[[nodiscard]] Result<std::optional<Frame>> read_frame(ByteSource& source);

/// Decodes every frame in `bytes`, in order, through read_frame(), exactly as a peer would read
/// them from its pipe: the whole buffer must be complete frames. PARSE_ERROR for the first garbled,
/// oversized or truncated frame (a buffer that ends inside a frame included). The frame layer's
/// entry point for fuzzing on raw bytes; a fuzz target passes each payload on to
/// decode_supervisor_message() and decode_worker_message(). Payload memory is bounded by
/// kMaximumPayloadBytes per frame.
[[nodiscard]] Result<std::vector<Frame>> decode_frames(std::span<const std::uint8_t> bytes);

/// Encodes and writes one frame.
[[nodiscard]] Result<void> write_frame(ByteSink& sink, std::uint64_t request_sequence, std::string_view payload);

// ---------------------------------------------------------------- messages: supervisor -> worker

/// Requests one analysis. The fence fields (job, lease, cancel epoch, content revision, profile
/// digest) are echoed in the outcome so the supervisor can reject stale results.
struct AnalyzeTask {
    uuids::uuid job_id;
    uuids::uuid lease_id;
    std::int64_t cancel_epoch = 0;
    std::int64_t content_revision = 0;
    std::string profile_digest;
    std::string file_path;  ///< UTF-8; opened read-only, never interpreted as a command
    std::vector<analysis::Stage> stages;
    int waveform_bins = analysis::kDefaultWaveformBins;             ///< in [1, kMaximumWaveformBins]
    double max_musical_seconds = analysis::kDefaultMusicalSeconds;  ///< in (0, kMaximumMusicalSeconds]
    /// UTF-8 destination of the waveform segment; required exactly when the Waveform stage is
    /// requested. The worker writes a sibling temporary file and renames it into place.
    std::string waveform_output_path;
};

/// Cooperative cancellation of the task with `job_id`; honored at analysis block granularity.
struct Cancel {
    uuids::uuid job_id;
    std::int64_t cancel_epoch = 0;
};

/// Asks the worker to cancel any task and exit cleanly.
struct Shutdown {};

// ---------------------------------------------------------------- messages: worker -> supervisor

/// Resource bounds the worker enforces on itself. The JSON member names keep the protocol's
/// original "max_*" spelling.
struct WorkerLimits {
    std::uint32_t maximum_payload_bytes = kMaximumPayloadBytes;
    int maximum_waveform_bins = analysis::kMaximumWaveformBins;
    double maximum_musical_seconds = analysis::kMaximumMusicalSeconds;
    int concurrent_tasks = 1;
    std::int64_t heartbeat_interval_ms = kHeartbeatInterval.count();
};

/// First message on every channel: who the worker is and what it can do.
struct Handshake {
    std::string build_id;
    std::uint16_t protocol_major = kProtocolMajor;
    std::uint16_t protocol_minor = kProtocolMinor;
    std::uint32_t process_id = 0;  ///< lets the supervisor confirm it talks to the child it spawned
    std::vector<std::string> codecs;
    std::map<std::string, std::string> algorithm_versions;  ///< stage name -> algorithm version
    WorkerLimits limits;
    bool self_test = false;  ///< true when the worker runs the synthetic analyzer instead of real DSP
};

/// Liveness signal; renews the supervisor's lease on the worker. While a task runs it also
/// carries the task's work counter (analysis::WorkCounter units), which proves that the task
/// itself, not only the heartbeat thread, is moving.
struct Heartbeat {
    std::optional<uuids::uuid> active_job_id;
    /// Units of work the active task has completed; absent when no task runs (and from peers
    /// speaking protocol minor 0).
    std::optional<std::uint64_t> task_progress;
};

/// Analysis progress of the active task in [0, 1].
struct Progress {
    uuids::uuid job_id;
    std::int64_t cancel_epoch = 0;
    double fraction = 0;
};

/// Identity of a task as echoed in its outcome. Two fences are equal only when every field is.
struct TaskFence {
    uuids::uuid job_id;
    uuids::uuid lease_id;
    std::int64_t cancel_epoch = 0;
    std::int64_t content_revision = 0;
    std::string profile_digest;

    /// The fence of `task`.
    [[nodiscard]] static TaskFence of(const AnalyzeTask& task);
    bool operator==(const TaskFence&) const = default;
};

/// A waveform segment file the worker wrote and flushed. It is operation-owned and temporary
/// until the supervisor validates it and publishes it under its immutable final name.
struct WaveformSegment {
    std::string path;  ///< UTF-8, equal to AnalyzeTask::waveform_output_path
    std::uint64_t byte_length = 0;
    std::string sha256;  ///< lowercase hex digest of the whole file
    int bins = 0;
    std::int64_t frames_per_bin = 0;
    std::int64_t frames_covered = 0;
    std::int64_t total_frames = 0;
    int sample_rate = 0;
    bool complete = false;
};

/// Successful outcome. `report.waveform` is always empty on the wire: the overview travels in
/// the segment file described by `waveform_segment`.
struct TaskResult {
    TaskFence fence;
    analysis::AnalysisReport report;
    std::optional<WaveformSegment> waveform_segment;
};

/// Failed or cancelled outcome. `code` crosses the wire as its stable name (mso::to_string).
struct TaskFailed {
    TaskFence fence;
    ErrorCode code = ErrorCode::Internal;
    bool retryable = false;
    std::string safe_detail;

    /// The failure as an ordinary Error.
    [[nodiscard]] Error to_error() const;
};

/// Everything the supervisor sends.
using SupervisorMessage = std::variant<AnalyzeTask, Cancel, Shutdown>;
/// Everything the worker sends.
using WorkerMessage = std::variant<Handshake, Heartbeat, Progress, TaskResult, TaskFailed>;

// ---------------------------------------------------------------- message encoding

/// JSON payload of a message. INVALID_ARGUMENT for text that is not valid UTF-8 or a non-finite
/// number, RESOURCE_LIMIT when memory runs out; no exception escapes.
[[nodiscard]] Result<std::string> encode_message(const SupervisorMessage& message);
/// JSON payload of a message; errors as for the supervisor overload.
[[nodiscard]] Result<std::string> encode_message(const WorkerMessage& message);

/// Parses and validates a payload, including validate_task() for an AnalyzeTask. Every schema or
/// range violation is PARSE_ERROR; no exception escapes.
[[nodiscard]] Result<SupervisorMessage> decode_supervisor_message(std::string_view payload);
/// Parses and validates a payload, including validate_result() for a TaskResult and the [0, 1]
/// range of Progress. Every schema or range violation is PARSE_ERROR; no exception escapes.
[[nodiscard]] Result<WorkerMessage> decode_worker_message(std::string_view payload);

/// Checks the invariants of a task (identities, bounds, stage list, output path). The worker
/// applies it on receipt and the supervisor before sending. INVALID_ARGUMENT on violation.
[[nodiscard]] Result<void> validate_task(const AnalyzeTask& task);

/// Checks that every value of a result is in range: loudness levels, tempo within the reported
/// BPM range, confidences in [0, 1], SHA-256 digests as 64 lowercase hex digits, non-negative
/// counts and evidence, a bounded warning list, and a waveform segment whose header fields are
/// consistent and whose byte length matches its bin count. The symmetric counterpart of
/// validate_task(); INVALID_ARGUMENT on violation.
[[nodiscard]] Result<void> validate_result(const TaskResult& result);

/// The analysis request a task describes.
[[nodiscard]] analysis::AnalysisRequest to_analysis_request(const AnalyzeTask& task);

/// The algorithm version of every analysis stage, keyed by stage name: what a worker built from
/// this source tree reports in its handshake and what the supervisor requires.
[[nodiscard]] std::map<std::string, std::string> current_algorithm_versions();

/// Inverse of mso::to_string(ErrorCode).
[[nodiscard]] std::optional<ErrorCode> parse_error_code(std::string_view text) noexcept;

}  // namespace mso::worker
