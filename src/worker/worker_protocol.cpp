/// @file
/// Worker IPC framing, the JSON mapping of every message and the range checks of tasks and
/// results. Strict by design: integers must be integral and in range, numbers finite, members
/// present with the right type; anything else is PARSE_ERROR at the decode boundary. No exception
/// leaves this file.
#include "mso/worker/worker_protocol.hpp"

#include <algorithm>
#include <cmath>
#include <exception>
#include <format>
#include <limits>
#include <new>
#include <stdexcept>
#include <type_traits>
#include <utility>

#include <nlohmann/json.hpp>

#include "mso/analysis/byte_order.hpp"
#include "mso/analysis/loudness_meter.hpp"
#include "mso/analysis/tempo_estimator.hpp"

namespace mso::worker {

namespace {

// ---------------------------------------------------------------- frame layout

constexpr std::size_t kMagicOffset = 0;      ///< 4 bytes "MSOW"
constexpr std::size_t kMajorOffset = 4;      ///< uint16 protocol major
constexpr std::size_t kMinorOffset = 6;      ///< uint16 protocol minor
constexpr std::size_t kLengthOffset = 8;     ///< uint32 payload length
constexpr std::size_t kSequenceOffset = 12;  ///< uint64 request sequence

/// The `sizeof(T)` header bytes of a field at `offset`.
template <class T, std::size_t Offset>
std::span<std::uint8_t, sizeof(T)> field(std::span<std::uint8_t> header) {
    return header.subspan<Offset, sizeof(T)>();
}

/// Read-only counterpart of field().
template <class T, std::size_t Offset>
std::span<const std::uint8_t, sizeof(T)> field(std::span<const std::uint8_t, kFrameHeaderBytes> header) {
    return header.subspan<Offset, sizeof(T)>();
}

/// ByteSource over a caller's buffer: serves the bytes once, then reports end of stream.
class BufferSource final : public ByteSource {
public:
    explicit BufferSource(std::span<const std::uint8_t> bytes) noexcept : remaining_(bytes) {}

    Result<std::size_t> read_some(std::span<std::uint8_t> destination) override {
        const std::size_t count = std::min(destination.size(), remaining_.size());
        std::ranges::copy(remaining_.first(count), destination.begin());
        remaining_ = remaining_.subspan(count);
        return count;
    }

private:
    std::span<const std::uint8_t> remaining_;  ///< bytes not read yet
};

/// Reads until `destination` is full or the stream ends; returns how many bytes arrived.
Result<std::size_t> read_fully(ByteSource& source, std::span<std::uint8_t> destination) {
    std::size_t filled = 0;
    while (filled < destination.size()) {
        auto received = source.read_some(destination.subspan(filled));
        if (!received) return std::unexpected(received.error());
        if (*received == 0) break;
        filled += *received;
    }
    return filled;
}

// ---------------------------------------------------------------- strict JSON member access

/// Thrown inside this file for a schema or range violation and converted to an Error at the
/// encode/decode boundary; it never crosses the module API.
class SchemaViolation : public std::runtime_error {
public:
    using std::runtime_error::runtime_error;
};

/// Converts one JSON value to T without nlohmann's lenient coercions: integers must be integral
/// and in range, floating values finite, strings strings and booleans booleans.
template <class T>
T convert(const nlohmann::json& value, std::string_view name) {
    if constexpr (std::is_same_v<T, bool>) {
        if (!value.is_boolean()) throw SchemaViolation(std::format("'{}' must be a boolean", name));
        return value.get<bool>();
    } else if constexpr (std::is_integral_v<T>) {
        if (!value.is_number_integer()) throw SchemaViolation(std::format("'{}' must be an integer", name));
        const bool fits = value.is_number_unsigned() ? std::in_range<T>(value.get<std::uint64_t>())
                                                     : std::in_range<T>(value.get<std::int64_t>());
        if (!fits) throw SchemaViolation(std::format("'{}' is out of range", name));
        return value.is_number_unsigned() ? static_cast<T>(value.get<std::uint64_t>())
                                          : static_cast<T>(value.get<std::int64_t>());
    } else if constexpr (std::is_floating_point_v<T>) {
        if (!value.is_number()) throw SchemaViolation(std::format("'{}' must be a number", name));
        const auto number = value.get<T>();
        if (!std::isfinite(number)) throw SchemaViolation(std::format("'{}' must be finite", name));
        return number;
    } else if constexpr (std::is_same_v<T, std::string>) {
        if (!value.is_string()) throw SchemaViolation(std::format("'{}' must be a string", name));
        return value.get<std::string>();
    } else {
        static_assert(std::is_same_v<T, nlohmann::json>, "unsupported member type");
        return value;
    }
}

/// A member that must be present, converted strictly.
template <class T>
T required(const nlohmann::json& object, std::string_view name) {
    const auto found = object.find(name);
    if (found == object.end()) throw SchemaViolation(std::format("missing member '{}'", name));
    return convert<T>(*found, name);
}

/// A member that may be absent or null.
template <class T>
std::optional<T> optional_member(const nlohmann::json& object, std::string_view name) {
    const auto found = object.find(name);
    if (found == object.end() || found->is_null()) return std::nullopt;
    return convert<T>(*found, name);
}

/// A nested object that must be present.
const nlohmann::json& required_object(const nlohmann::json& object, std::string_view name) {
    const auto found = object.find(name);
    if (found == object.end() || !found->is_object()) {
        throw SchemaViolation(std::format("member '{}' must be an object", name));
    }
    return *found;
}

/// An optional nested object: absent or null yields nullptr.
const nlohmann::json* optional_object(const nlohmann::json& object, std::string_view name) {
    const auto found = object.find(name);
    if (found == object.end() || found->is_null()) return nullptr;
    if (!found->is_object()) throw SchemaViolation(std::format("member '{}' must be an object", name));
    return &*found;
}

/// An array of strings that must be present.
std::vector<std::string> required_strings(const nlohmann::json& object, std::string_view name) {
    const auto& array = required<nlohmann::json>(object, name);
    if (!array.is_array()) throw SchemaViolation(std::format("member '{}' must be an array", name));
    std::vector<std::string> strings;
    strings.reserve(array.size());
    for (const auto& element : array)
        strings.push_back(convert<std::string>(element, name));
    return strings;
}

/// A UUID string member that must be present.
uuids::uuid required_uuid(const nlohmann::json& object, std::string_view name) {
    const auto parsed = uuids::uuid::from_string(required<std::string>(object, name));
    if (!parsed) throw SchemaViolation(std::format("'{}' must be a UUID", name));
    return *parsed;
}

/// A UUID string member that may be absent or null.
std::optional<uuids::uuid> optional_uuid(const nlohmann::json& object, std::string_view name) {
    const auto text = optional_member<std::string>(object, name);
    if (!text) return std::nullopt;
    const auto parsed = uuids::uuid::from_string(*text);
    if (!parsed) throw SchemaViolation(std::format("'{}' must be a UUID", name));
    return parsed;
}

/// Refuses NaN and infinities on the way out: JSON cannot represent them.
nlohmann::json finite_number(double value, std::string_view name) {
    if (!std::isfinite(value)) throw SchemaViolation(std::format("'{}' is not finite", name));
    return value;
}

/// The value, or JSON null for an empty optional.
template <class T>
nlohmann::json nullable(const std::optional<T>& value) {
    return value ? nlohmann::json(*value) : nlohmann::json(nullptr);
}

// ---------------------------------------------------------------- range checks

/// Lowest and highest level a finite float sample can produce, in dBFS (the loudest finite float
/// is about +770 dBFS; anything beyond is a corrupted report).
constexpr double kLowestLevelDbfs = analysis::kSilenceDbfs;
constexpr double kHighestLevelDbfs = 800.0;

/// True for exactly 64 lowercase hexadecimal digits.
bool is_sha256_hex(std::string_view text) {
    return text.size() == 64 && std::ranges::all_of(text, [](char character) {
               return (character >= '0' && character <= '9') || (character >= 'a' && character <= 'f');
           });
}

/// True for a finite value within [0, 1].
bool is_unit_interval(double value) {
    return value >= 0.0 && value <= 1.0;
}

/// UTF-8 text that is not empty and holds no NUL.
bool is_clean_text(std::string_view text) {
    return !text.empty() && is_valid_utf8(text) && text.find('\0') == std::string_view::npos;
}

/// Range checks of every report field (see validate_result()).
Result<void> validate_report(const analysis::AnalysisReport& report) {
    if (report.waveform) return fail(ErrorCode::InvalidArgument, "result carries an inline waveform");
    if (report.loudness) {
        const auto& loudness = *report.loudness;
        const auto in_range = [](double level) { return level >= kLowestLevelDbfs && level <= kHighestLevelDbfs; };
        if (!in_range(loudness.peak_dbfs) || !in_range(loudness.rms_dbfs))
            return fail(ErrorCode::InvalidArgument, "loudness level out of range");
        if (loudness.rms_dbfs > loudness.peak_dbfs + 1e-6)
            return fail(ErrorCode::InvalidArgument, "RMS level above the peak level");
    }
    if (report.tempo) {
        const auto& tempo = *report.tempo;
        if (!is_unit_interval(tempo.confidence))
            return fail(ErrorCode::InvalidArgument, "tempo confidence out of range");
        // The estimator's reported range in the wire unit (thousandths of a BPM).
        constexpr auto kLowestMilliBpm = static_cast<std::int64_t>(analysis::kMinimumReportedBpm * 1000.0);
        constexpr auto kHighestMilliBpm = static_cast<std::int64_t>(analysis::kMaximumReportedBpm * 1000.0);
        if (tempo.milli_bpm && (*tempo.milli_bpm < kLowestMilliBpm || *tempo.milli_bpm > kHighestMilliBpm))
            return fail(ErrorCode::InvalidArgument, "tempo out of the reported range");
    }
    if (report.key) {
        const auto& key = *report.key;
        if (!is_unit_interval(key.confidence)) return fail(ErrorCode::InvalidArgument, "key confidence out of range");
        if (key.key && (key.key->pitch_class < 0 || key.key->pitch_class > 11 || key.key->mode == KeyMode::Unknown))
            return fail(ErrorCode::InvalidArgument, "invalid musical key");
    }
    if (report.hashes.byte_sha256 && !is_sha256_hex(*report.hashes.byte_sha256))
        return fail(ErrorCode::InvalidArgument, "byte hash is not a SHA-256 digest");
    if (report.hashes.pcm_sha256 && !is_sha256_hex(*report.hashes.pcm_sha256))
        return fail(ErrorCode::InvalidArgument, "PCM hash is not a SHA-256 digest");
    const auto& evidence = report.evidence;
    if (evidence.size_before < 0 || evidence.size_after < 0)
        return fail(ErrorCode::InvalidArgument, "input evidence sizes must not be negative");
    if (report.frames_decoded < 0) return fail(ErrorCode::InvalidArgument, "decoded frame count must not be negative");
    if (report.warnings.size() > kMaximumWarnings) return fail(ErrorCode::InvalidArgument, "too many warnings");
    if (!std::ranges::all_of(report.warnings, [](const std::string& warning) { return is_valid_utf8(warning); }))
        return fail(ErrorCode::InvalidArgument, "warning text is not UTF-8");
    return {};
}

/// Range and consistency checks of a waveform segment description.
Result<void> validate_segment(const WaveformSegment& segment) {
    if (!is_clean_text(segment.path))
        return fail(ErrorCode::InvalidArgument, "waveform segment path is not clean UTF-8");
    if (!is_sha256_hex(segment.sha256))
        return fail(ErrorCode::InvalidArgument, "waveform segment checksum is not a SHA-256 digest");
    if (segment.bins < 0 || segment.bins > analysis::kMaximumWaveformBins)
        return fail(ErrorCode::InvalidArgument, "waveform segment bin count out of range");
    if (segment.byte_length != analysis::encoded_waveform_size(static_cast<std::size_t>(segment.bins)))
        return fail(ErrorCode::InvalidArgument, "waveform segment length does not match its bin count");
    if (segment.sample_rate < 1 || segment.sample_rate > analysis::kMaximumSampleRate)
        return fail(ErrorCode::InvalidArgument, "waveform segment sample rate out of range");
    if (segment.frames_per_bin < 1 || segment.frames_covered < 0 || segment.total_frames < 0)
        return fail(ErrorCode::InvalidArgument, "waveform segment frame counts out of range");
    // The bins tile the covered frames exactly, as the segment decoder requires.
    const std::int64_t expected_bins =
        segment.frames_covered / segment.frames_per_bin + (segment.frames_covered % segment.frames_per_bin != 0);
    if (expected_bins != segment.bins)
        return fail(ErrorCode::InvalidArgument, "waveform segment bin count does not match its frames");
    if (segment.complete && segment.total_frames != segment.frames_covered)
        return fail(ErrorCode::InvalidArgument, "complete waveform segment must cover all frames");
    return {};
}

// ---------------------------------------------------------------- analysis report

/// Stage list -> JSON array of stage names.
nlohmann::json stages_to_json(const std::vector<analysis::Stage>& stages) {
    nlohmann::json array = nlohmann::json::array();
    for (const auto stage : stages)
        array.push_back(std::string(analysis::to_string(stage)));
    return array;
}

/// The "stages" member -> stage list; unknown names are a schema violation.
std::vector<analysis::Stage> stages_from_json(const nlohmann::json& object) {
    std::vector<analysis::Stage> stages;
    for (const auto& name : required_strings(object, "stages")) {
        const auto stage = analysis::parse_stage(name);
        if (!stage) throw SchemaViolation(std::format("unknown analysis stage '{}'", name));
        stages.push_back(*stage);
    }
    return stages;
}

/// AnalysisReport -> JSON (the waveform never travels inline).
nlohmann::json report_to_json(const analysis::AnalysisReport& report) {
    nlohmann::json json = nlohmann::json::object();
    json["loudness"] = nullptr;
    if (report.loudness) {
        json["loudness"] = nlohmann::json{{"peak_dbfs", finite_number(report.loudness->peak_dbfs, "peak_dbfs")},
                                          {"rms_dbfs", finite_number(report.loudness->rms_dbfs, "rms_dbfs")}};
    }
    json["tempo"] = report.tempo ? nlohmann::json{{"milli_bpm", nullable(report.tempo->milli_bpm)},
                                                  {"confidence", finite_number(report.tempo->confidence, "confidence")}}
                                 : nlohmann::json(nullptr);
    if (report.key) {
        nlohmann::json key = nullptr;
        if (report.key->key) {
            key = nlohmann::json{{"pitch_class", report.key->key->pitch_class},
                                 {"mode", std::string(to_string(report.key->key->mode))}};
        }
        json["key"] = nlohmann::json{{"key", key}, {"confidence", finite_number(report.key->confidence, "confidence")}};
    } else {
        json["key"] = nullptr;
    }
    json["hashes"] = nlohmann::json{{"byte_sha256", nullable(report.hashes.byte_sha256)},
                                    {"pcm_sha256", nullable(report.hashes.pcm_sha256)}};
    json["evidence"] = nlohmann::json{{"size_before", report.evidence.size_before},
                                      {"size_after", report.evidence.size_after},
                                      {"mtime_ns_before", report.evidence.mtime_ns_before},
                                      {"mtime_ns_after", report.evidence.mtime_ns_after}};
    json["frames_decoded"] = report.frames_decoded;
    json["warnings"] = report.warnings;
    return json;
}

/// JSON -> AnalysisReport (schema only; validate_result() checks the values).
analysis::AnalysisReport report_from_json(const nlohmann::json& json) {
    analysis::AnalysisReport report;
    if (const auto* loudness = optional_object(json, "loudness")) {
        report.loudness =
            analysis::LoudnessResult{required<double>(*loudness, "peak_dbfs"), required<double>(*loudness, "rms_dbfs")};
    }
    if (const auto* tempo = optional_object(json, "tempo")) {
        report.tempo = analysis::TempoResult{optional_member<std::int64_t>(*tempo, "milli_bpm"),
                                             required<double>(*tempo, "confidence")};
    }
    if (const auto* key_estimate = optional_object(json, "key")) {
        analysis::KeyResult key_result;
        key_result.confidence = required<double>(*key_estimate, "confidence");
        if (const auto* key = optional_object(*key_estimate, "key")) {
            const auto pitch_class = required<int>(*key, "pitch_class");
            const auto mode = parse_key_mode(required<std::string>(*key, "mode"));
            if (!mode) throw SchemaViolation("invalid key mode");
            key_result.key = MusicalKey{pitch_class, *mode};
        }
        report.key = key_result;
    }
    const auto& hashes = required_object(json, "hashes");
    report.hashes.byte_sha256 = optional_member<std::string>(hashes, "byte_sha256");
    report.hashes.pcm_sha256 = optional_member<std::string>(hashes, "pcm_sha256");
    const auto& evidence = required_object(json, "evidence");
    report.evidence.size_before = required<std::int64_t>(evidence, "size_before");
    report.evidence.size_after = required<std::int64_t>(evidence, "size_after");
    report.evidence.mtime_ns_before = required<std::int64_t>(evidence, "mtime_ns_before");
    report.evidence.mtime_ns_after = required<std::int64_t>(evidence, "mtime_ns_after");
    report.frames_decoded = required<std::int64_t>(json, "frames_decoded");
    report.warnings = required_strings(json, "warnings");
    return report;
}

// ---------------------------------------------------------------- message type tags

/// The "type" member of each message.
template <class T>
inline constexpr std::string_view kMessageType{};
template <>
inline constexpr std::string_view kMessageType<AnalyzeTask> = "analyze";
template <>
inline constexpr std::string_view kMessageType<Cancel> = "cancel";
template <>
inline constexpr std::string_view kMessageType<Shutdown> = "shutdown";
template <>
inline constexpr std::string_view kMessageType<Handshake> = "handshake";
template <>
inline constexpr std::string_view kMessageType<Heartbeat> = "heartbeat";
template <>
inline constexpr std::string_view kMessageType<Progress> = "progress";
template <>
inline constexpr std::string_view kMessageType<TaskResult> = "task_result";
template <>
inline constexpr std::string_view kMessageType<TaskFailed> = "task_failed";

}  // namespace

// ---------------------------------------------------------------- per-message JSON mapping
// nlohmann's customization points, found by argument-dependent lookup when a message is assigned
// to or read from a json value. They may throw SchemaViolation or nlohmann exceptions; the public
// encode/decode functions below are the exception boundary. Member names are the wire schema.

/// AnalyzeTask -> JSON.
void to_json(nlohmann::json& json, const AnalyzeTask& task) {
    json = nlohmann::json{{"job_id", uuids::to_string(task.job_id)},
                          {"lease_id", uuids::to_string(task.lease_id)},
                          {"cancel_epoch", task.cancel_epoch},
                          {"content_revision", task.content_revision},
                          {"profile_digest", task.profile_digest},
                          {"file_path", task.file_path},
                          {"stages", stages_to_json(task.stages)},
                          {"waveform_bins", task.waveform_bins},
                          {"max_musical_seconds", finite_number(task.max_musical_seconds, "max_musical_seconds")},
                          {"waveform_output_path", task.waveform_output_path}};
}

/// JSON -> AnalyzeTask (schema only; validate_task() checks the values).
void from_json(const nlohmann::json& json, AnalyzeTask& task) {
    task.job_id = required_uuid(json, "job_id");
    task.lease_id = required_uuid(json, "lease_id");
    task.cancel_epoch = required<std::int64_t>(json, "cancel_epoch");
    task.content_revision = required<std::int64_t>(json, "content_revision");
    task.profile_digest = required<std::string>(json, "profile_digest");
    task.file_path = required<std::string>(json, "file_path");
    task.stages = stages_from_json(json);
    task.waveform_bins = required<int>(json, "waveform_bins");
    task.max_musical_seconds = required<double>(json, "max_musical_seconds");
    task.waveform_output_path = required<std::string>(json, "waveform_output_path");
}

/// Cancel -> JSON.
void to_json(nlohmann::json& json, const Cancel& cancel) {
    json = nlohmann::json{{"job_id", uuids::to_string(cancel.job_id)}, {"cancel_epoch", cancel.cancel_epoch}};
}

/// JSON -> Cancel.
void from_json(const nlohmann::json& json, Cancel& cancel) {
    cancel.job_id = required_uuid(json, "job_id");
    cancel.cancel_epoch = required<std::int64_t>(json, "cancel_epoch");
}

/// Shutdown -> JSON (no members besides the type).
void to_json(nlohmann::json& json, const Shutdown&) {
    json = nlohmann::json::object();
}

/// JSON -> Shutdown.
void from_json(const nlohmann::json&, Shutdown&) {}

/// WorkerLimits -> JSON.
void to_json(nlohmann::json& json, const WorkerLimits& limits) {
    json = nlohmann::json{{"max_payload_bytes", limits.maximum_payload_bytes},
                          {"max_waveform_bins", limits.maximum_waveform_bins},
                          {"max_musical_seconds", finite_number(limits.maximum_musical_seconds, "max_musical_seconds")},
                          {"concurrent_tasks", limits.concurrent_tasks},
                          {"heartbeat_interval_ms", limits.heartbeat_interval_ms}};
}

/// JSON -> WorkerLimits.
void from_json(const nlohmann::json& json, WorkerLimits& limits) {
    limits.maximum_payload_bytes = required<std::uint32_t>(json, "max_payload_bytes");
    limits.maximum_waveform_bins = required<int>(json, "max_waveform_bins");
    limits.maximum_musical_seconds = required<double>(json, "max_musical_seconds");
    limits.concurrent_tasks = required<int>(json, "concurrent_tasks");
    limits.heartbeat_interval_ms = required<std::int64_t>(json, "heartbeat_interval_ms");
}

/// Handshake -> JSON.
void to_json(nlohmann::json& json, const Handshake& handshake) {
    nlohmann::json algorithm_versions = nlohmann::json::object();
    for (const auto& [stage, version] : handshake.algorithm_versions)
        algorithm_versions[stage] = version;
    json = nlohmann::json{{"build_id", handshake.build_id},
                          {"protocol_major", handshake.protocol_major},
                          {"protocol_minor", handshake.protocol_minor},
                          {"process_id", handshake.process_id},
                          {"codecs", handshake.codecs},
                          {"algorithm_versions", algorithm_versions},
                          {"limits", handshake.limits},
                          {"self_test", handshake.self_test}};
}

/// JSON -> Handshake.
void from_json(const nlohmann::json& json, Handshake& handshake) {
    handshake.build_id = required<std::string>(json, "build_id");
    handshake.protocol_major = required<std::uint16_t>(json, "protocol_major");
    handshake.protocol_minor = required<std::uint16_t>(json, "protocol_minor");
    handshake.process_id = required<std::uint32_t>(json, "process_id");
    handshake.codecs = required_strings(json, "codecs");
    handshake.algorithm_versions.clear();
    for (const auto& [stage, version] : required_object(json, "algorithm_versions").items()) {
        handshake.algorithm_versions.emplace(stage, convert<std::string>(version, stage));
    }
    required_object(json, "limits").get_to(handshake.limits);
    handshake.self_test = required<bool>(json, "self_test");
}

/// Heartbeat -> JSON.
void to_json(nlohmann::json& json, const Heartbeat& heartbeat) {
    const auto active_job_id =
        heartbeat.active_job_id.transform([](const uuids::uuid& id) { return uuids::to_string(id); });
    json = nlohmann::json{{"active_job_id", nullable(active_job_id)},
                          {"task_progress", nullable(heartbeat.task_progress)}};
}

/// JSON -> Heartbeat; "task_progress" is optional (protocol minor 1).
void from_json(const nlohmann::json& json, Heartbeat& heartbeat) {
    heartbeat.active_job_id = optional_uuid(json, "active_job_id");
    heartbeat.task_progress = optional_member<std::uint64_t>(json, "task_progress");
}

/// Progress -> JSON.
void to_json(nlohmann::json& json, const Progress& progress) {
    json = nlohmann::json{{"job_id", uuids::to_string(progress.job_id)},
                          {"cancel_epoch", progress.cancel_epoch},
                          {"fraction", finite_number(progress.fraction, "fraction")}};
}

/// JSON -> Progress, with the fraction checked to be within [0, 1].
void from_json(const nlohmann::json& json, Progress& progress) {
    progress.job_id = required_uuid(json, "job_id");
    progress.cancel_epoch = required<std::int64_t>(json, "cancel_epoch");
    progress.fraction = required<double>(json, "fraction");
    if (!is_unit_interval(progress.fraction)) throw SchemaViolation("'fraction' must be within [0, 1]");
}

/// TaskFence -> JSON.
void to_json(nlohmann::json& json, const TaskFence& fence) {
    json = nlohmann::json{{"job_id", uuids::to_string(fence.job_id)},
                          {"lease_id", uuids::to_string(fence.lease_id)},
                          {"cancel_epoch", fence.cancel_epoch},
                          {"content_revision", fence.content_revision},
                          {"profile_digest", fence.profile_digest}};
}

/// JSON -> TaskFence.
void from_json(const nlohmann::json& json, TaskFence& fence) {
    fence.job_id = required_uuid(json, "job_id");
    fence.lease_id = required_uuid(json, "lease_id");
    fence.cancel_epoch = required<std::int64_t>(json, "cancel_epoch");
    fence.content_revision = required<std::int64_t>(json, "content_revision");
    fence.profile_digest = required<std::string>(json, "profile_digest");
}

/// WaveformSegment -> JSON.
void to_json(nlohmann::json& json, const WaveformSegment& segment) {
    json = nlohmann::json{{"path", segment.path},
                          {"byte_length", segment.byte_length},
                          {"sha256", segment.sha256},
                          {"bins", segment.bins},
                          {"frames_per_bin", segment.frames_per_bin},
                          {"frames_covered", segment.frames_covered},
                          {"total_frames", segment.total_frames},
                          {"sample_rate", segment.sample_rate},
                          {"complete", segment.complete}};
}

/// JSON -> WaveformSegment (schema only; validate_result() checks the values).
void from_json(const nlohmann::json& json, WaveformSegment& segment) {
    segment.path = required<std::string>(json, "path");
    segment.byte_length = required<std::uint64_t>(json, "byte_length");
    segment.sha256 = required<std::string>(json, "sha256");
    segment.bins = required<int>(json, "bins");
    segment.frames_per_bin = required<std::int64_t>(json, "frames_per_bin");
    segment.frames_covered = required<std::int64_t>(json, "frames_covered");
    segment.total_frames = required<std::int64_t>(json, "total_frames");
    segment.sample_rate = required<int>(json, "sample_rate");
    segment.complete = required<bool>(json, "complete");
}

/// TaskResult -> JSON.
void to_json(nlohmann::json& json, const TaskResult& result) {
    json = nlohmann::json{{"fence", result.fence},
                          {"report", report_to_json(result.report)},
                          {"waveform_segment", nullable(result.waveform_segment)}};
}

/// JSON -> TaskResult (schema only; validate_result() checks the values).
void from_json(const nlohmann::json& json, TaskResult& result) {
    required_object(json, "fence").get_to(result.fence);
    result.report = report_from_json(required_object(json, "report"));
    result.waveform_segment.reset();
    if (const auto* segment = optional_object(json, "waveform_segment")) {
        result.waveform_segment = segment->get<WaveformSegment>();
    }
}

/// TaskFailed -> JSON.
void to_json(nlohmann::json& json, const TaskFailed& failure) {
    json = nlohmann::json{{"fence", failure.fence},
                          {"code", std::string(to_string(failure.code))},
                          {"retryable", failure.retryable},
                          {"safe_detail", failure.safe_detail}};
}

/// JSON -> TaskFailed.
void from_json(const nlohmann::json& json, TaskFailed& failure) {
    required_object(json, "fence").get_to(failure.fence);
    const auto code = parse_error_code(required<std::string>(json, "code"));
    if (!code) throw SchemaViolation("unknown error code");
    failure.code = *code;
    failure.retryable = required<bool>(json, "retryable");
    failure.safe_detail = required<std::string>(json, "safe_detail");
}

namespace {

/// Shared body of both encode_message overloads: the message's members plus its "type" tag.
template <class Variant>
Result<std::string> encode_variant(const Variant& message) {
    try {
        return std::visit(
            [](const auto& alternative) {
                nlohmann::json json = alternative;
                json["type"] = std::string(kMessageType<std::decay_t<decltype(alternative)>>);
                return json.dump();
            },
            message);
    } catch (const SchemaViolation& violation) {
        return fail(ErrorCode::InvalidArgument, std::format("cannot encode worker message: {}", violation.what()));
    } catch (const nlohmann::json::exception& exception) {
        // dump() throws type_error 316 for invalid UTF-8; the text names no payload content.
        return fail(ErrorCode::InvalidArgument,
                    std::format("cannot encode worker message (json error {})", exception.id));
    } catch (const std::bad_alloc&) {
        return fail(ErrorCode::ResourceLimit, "out of memory while encoding a worker message");
    } catch (const std::exception&) {
        return fail(ErrorCode::Internal, "unexpected failure while encoding a worker message");
    }
}

/// Converts `object` into the variant alternative whose type tag equals `type`.
template <class Variant, std::size_t... AlternativeIndex>
std::optional<Variant> decode_alternative(std::string_view type, const nlohmann::json& object,
                                          std::index_sequence<AlternativeIndex...>) {
    std::optional<Variant> decoded;
    const auto try_alternative = [&]<class Alternative>(std::type_identity<Alternative>) {
        if (!decoded && type == kMessageType<Alternative>) decoded = object.get<Alternative>();
    };
    (try_alternative(std::type_identity<std::variant_alternative_t<AlternativeIndex, Variant>>{}), ...);
    return decoded;
}

/// Shared body of both decode functions: parse, dispatch on "type", map every failure to
/// PARSE_ERROR (RESOURCE_LIMIT when memory runs out). Parser messages are summarized by byte
/// offset so payload text (which may hold a file path) never ends up in an error detail.
template <class Variant>
Result<Variant> decode_variant(std::string_view payload) {
    try {
        const nlohmann::json object = nlohmann::json::parse(payload, nullptr, /*allow_exceptions=*/false);
        if (object.is_discarded()) return fail(ErrorCode::ParseError, "worker message is not valid JSON");
        if (!object.is_object()) return fail(ErrorCode::ParseError, "worker message is not a JSON object");
        const auto type = required<std::string>(object, "type");
        auto decoded =
            decode_alternative<Variant>(type, object, std::make_index_sequence<std::variant_size_v<Variant>>{});
        if (!decoded) return fail(ErrorCode::ParseError, "unexpected worker message type");
        return std::move(*decoded);
    } catch (const SchemaViolation& violation) {
        return fail(ErrorCode::ParseError, std::format("malformed worker message: {}", violation.what()));
    } catch (const nlohmann::json::exception& exception) {
        return fail(ErrorCode::ParseError, std::format("malformed worker message (json error {})", exception.id));
    } catch (const std::bad_alloc&) {
        return fail(ErrorCode::ResourceLimit, "out of memory while decoding a worker message");
    } catch (const std::exception&) {
        return fail(ErrorCode::ParseError, "malformed worker message");
    }
}

}  // namespace

// ---------------------------------------------------------------- framing

Result<std::vector<std::uint8_t>> encode_frame(std::uint64_t request_sequence, std::string_view payload) {
    if (payload.size() > kMaximumPayloadBytes) {
        return fail(ErrorCode::InvalidArgument,
                    std::format("worker frame payload of {} bytes exceeds the {} byte limit", payload.size(),
                                kMaximumPayloadBytes));
    }
    std::vector<std::uint8_t> bytes(kFrameHeaderBytes + payload.size());
    const std::span<std::uint8_t> header(bytes.data(), kFrameHeaderBytes);
    std::ranges::copy(kFrameMagic, header.begin() + kMagicOffset);
    analysis::store_little_endian(field<std::uint16_t, kMajorOffset>(header), kProtocolMajor);
    analysis::store_little_endian(field<std::uint16_t, kMinorOffset>(header), kProtocolMinor);
    analysis::store_little_endian(field<std::uint32_t, kLengthOffset>(header),
                                  static_cast<std::uint32_t>(payload.size()));
    analysis::store_little_endian(field<std::uint64_t, kSequenceOffset>(header), request_sequence);
    std::ranges::copy(payload, bytes.begin() + kFrameHeaderBytes);
    return bytes;
}

Result<FrameHeader> parse_frame_header(std::span<const std::uint8_t, kFrameHeaderBytes> bytes) {
    if (!std::ranges::equal(bytes.subspan<kMagicOffset, kFrameMagic.size()>(), kFrameMagic)) {
        return fail(ErrorCode::ParseError, "worker frame has a bad magic number");
    }
    FrameHeader header;
    header.protocol_major = analysis::load_little_endian<std::uint16_t>(field<std::uint16_t, kMajorOffset>(bytes));
    header.protocol_minor = analysis::load_little_endian<std::uint16_t>(field<std::uint16_t, kMinorOffset>(bytes));
    header.payload_length = analysis::load_little_endian<std::uint32_t>(field<std::uint32_t, kLengthOffset>(bytes));
    header.request_sequence = analysis::load_little_endian<std::uint64_t>(field<std::uint64_t, kSequenceOffset>(bytes));
    if (header.protocol_major != kProtocolMajor) {
        return fail(ErrorCode::ParseError, std::format("worker protocol major {} is not the supported {}",
                                                       header.protocol_major, kProtocolMajor));
    }
    if (header.payload_length > kMaximumPayloadBytes) {
        return fail(ErrorCode::ParseError, std::format("worker frame announces {} payload bytes; the limit is {}",
                                                       header.payload_length, kMaximumPayloadBytes));
    }
    return header;
}

Result<std::optional<Frame>> read_frame(ByteSource& source) {
    std::array<std::uint8_t, kFrameHeaderBytes> header_bytes{};
    const auto header_received = read_fully(source, header_bytes);
    if (!header_received) return std::unexpected(header_received.error());
    if (*header_received == 0) return std::optional<Frame>{};
    if (*header_received < kFrameHeaderBytes)
        return fail(ErrorCode::ParseError, "worker channel ended inside a frame header");

    const auto header = parse_frame_header(header_bytes);
    if (!header) return std::unexpected(header.error());

    Frame frame{header->request_sequence, std::string(header->payload_length, '\0')};
    const auto payload_received =
        read_fully(source, std::span(reinterpret_cast<std::uint8_t*>(frame.payload.data()), frame.payload.size()));
    if (!payload_received) return std::unexpected(payload_received.error());
    if (*payload_received < frame.payload.size())
        return fail(ErrorCode::ParseError, "worker channel ended inside a frame payload");
    return std::optional<Frame>(std::move(frame));
}

Result<std::vector<Frame>> decode_frames(std::span<const std::uint8_t> bytes) {
    try {
        BufferSource source(bytes);
        std::vector<Frame> frames;
        while (true) {
            auto frame = read_frame(source);
            if (!frame) return std::unexpected(frame.error());
            if (!*frame) return frames;
            frames.push_back(std::move(**frame));
        }
    } catch (const std::bad_alloc&) {
        return fail(ErrorCode::ResourceLimit, "out of memory while decoding worker frames");
    }
}

Result<void> write_frame(ByteSink& sink, std::uint64_t request_sequence, std::string_view payload) {
    const auto bytes = encode_frame(request_sequence, payload);
    if (!bytes) return std::unexpected(bytes.error());
    return sink.write_all(*bytes);
}

// ---------------------------------------------------------------- messages

TaskFence TaskFence::of(const AnalyzeTask& task) {
    return TaskFence{task.job_id, task.lease_id, task.cancel_epoch, task.content_revision, task.profile_digest};
}

Error TaskFailed::to_error() const {
    return Error{code, retryable, safe_detail, std::nullopt, std::nullopt};
}

Result<std::string> encode_message(const SupervisorMessage& message) {
    return encode_variant(message);
}

Result<std::string> encode_message(const WorkerMessage& message) {
    return encode_variant(message);
}

Result<SupervisorMessage> decode_supervisor_message(std::string_view payload) {
    auto message = decode_variant<SupervisorMessage>(payload);
    if (!message) return message;
    if (const auto* task = std::get_if<AnalyzeTask>(&*message)) {
        if (auto valid = validate_task(*task); !valid) return fail(ErrorCode::ParseError, valid.error().safe_detail);
    }
    return message;
}

Result<WorkerMessage> decode_worker_message(std::string_view payload) {
    auto message = decode_variant<WorkerMessage>(payload);
    if (!message) return message;
    if (const auto* result = std::get_if<TaskResult>(&*message)) {
        if (auto valid = validate_result(*result); !valid)
            return fail(ErrorCode::ParseError, valid.error().safe_detail);
    }
    return message;
}

Result<void> validate_task(const AnalyzeTask& task) {
    if (task.job_id.is_nil() || task.lease_id.is_nil())
        return fail(ErrorCode::InvalidArgument, "task needs a job and a lease id");
    if (task.cancel_epoch < 0 || task.content_revision < 0) {
        return fail(ErrorCode::InvalidArgument, "task cancel epoch and content revision must not be negative");
    }
    if (!is_clean_text(task.file_path))
        return fail(ErrorCode::InvalidArgument, "task file path must be non-empty UTF-8 without NUL");
    if (task.waveform_bins < 1 || task.waveform_bins > analysis::kMaximumWaveformBins) {
        return fail(ErrorCode::InvalidArgument,
                    std::format("waveform bins must be within [1, {}]", analysis::kMaximumWaveformBins));
    }
    if (!std::isfinite(task.max_musical_seconds) || task.max_musical_seconds <= 0.0 ||
        task.max_musical_seconds > analysis::kMaximumMusicalSeconds) {
        return fail(ErrorCode::InvalidArgument,
                    std::format("max musical seconds must be within (0, {}]", analysis::kMaximumMusicalSeconds));
    }
    for (auto stage = task.stages.begin(); stage != task.stages.end(); ++stage) {
        if (std::find(std::next(stage), task.stages.end(), *stage) != task.stages.end()) {
            return fail(ErrorCode::InvalidArgument, "task lists an analysis stage twice");
        }
    }
    const bool wants_waveform = std::ranges::contains(task.stages, analysis::Stage::Waveform);
    if (wants_waveform && !is_clean_text(task.waveform_output_path)) {
        return fail(ErrorCode::InvalidArgument, "waveform stage needs an output path");
    }
    if (!wants_waveform && !task.waveform_output_path.empty()) {
        return fail(ErrorCode::InvalidArgument, "waveform output path given without the waveform stage");
    }
    return {};
}

Result<void> validate_result(const TaskResult& result) {
    if (result.fence.job_id.is_nil() || result.fence.lease_id.is_nil())
        return fail(ErrorCode::InvalidArgument, "result needs a job and a lease id");
    if (auto report = validate_report(result.report); !report) return report;
    if (result.waveform_segment) return validate_segment(*result.waveform_segment);
    return {};
}

analysis::AnalysisRequest to_analysis_request(const AnalyzeTask& task) {
    analysis::AnalysisRequest request;
    request.file = path_from_utf8(task.file_path);
    request.stages = task.stages;
    request.waveform_bins = task.waveform_bins;
    request.max_musical_seconds = task.max_musical_seconds;
    return request;
}

std::map<std::string, std::string> current_algorithm_versions() {
    std::map<std::string, std::string> by_stage;
    for (const analysis::Stage stage : analysis::kAllStages)
        by_stage.emplace(analysis::to_string(stage), analysis::algorithm_version(stage));
    return by_stage;
}

std::optional<ErrorCode> parse_error_code(std::string_view text) noexcept {
    // Every value of the underlying type is tried, so the lookup needs no knowledge of which
    // enumerator is last. to_string() maps values without an enumerator to "INTERNAL", which the
    // real ErrorCode::Internal reaches first.
    using Underlying = std::underlying_type_t<ErrorCode>;
    for (unsigned value = 0; value <= std::numeric_limits<Underlying>::max(); ++value) {
        const auto code = static_cast<ErrorCode>(value);
        if (to_string(code) == text) return code;
    }
    return std::nullopt;
}

}  // namespace mso::worker
