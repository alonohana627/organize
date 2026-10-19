#include "mso/analysis/worker_protocol.hpp"

#include <algorithm>
#include <cmath>
#include <concepts>
#include <format>
#include <stdexcept>
#include <type_traits>
#include <utility>

#include <nlohmann/json.hpp>

namespace mso::worker {

namespace {

// ---------------------------------------------------------------- frame layout

constexpr std::size_t kMagicOffset = 0;
constexpr std::size_t kMajorOffset = 4;
constexpr std::size_t kMinorOffset = 6;
constexpr std::size_t kLengthOffset = 8;
constexpr std::size_t kSequenceOffset = 12;

template <std::unsigned_integral T>
void store_little_endian(std::uint8_t* destination, T value) {
    for (std::size_t index = 0; index < sizeof(T); ++index) {
        destination[index] = static_cast<std::uint8_t>(value >> (8 * index));
    }
}

template <std::unsigned_integral T>
T load_little_endian(const std::uint8_t* source) {
    T value = 0;
    for (std::size_t index = 0; index < sizeof(T); ++index) {
        value = static_cast<T>(value | (static_cast<T>(source[index]) << (8 * index)));
    }
    return value;
}

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

template <class T>
T required(const nlohmann::json& object, std::string_view name) {
    const auto found = object.find(name);
    if (found == object.end()) throw SchemaViolation(std::format("missing member '{}'", name));
    return convert<T>(*found, name);
}

template <class T>
std::optional<T> optional_member(const nlohmann::json& object, std::string_view name) {
    const auto found = object.find(name);
    if (found == object.end() || found->is_null()) return std::nullopt;
    return convert<T>(*found, name);
}

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

std::vector<std::string> required_strings(const nlohmann::json& object, std::string_view name) {
    const auto& array = required<nlohmann::json>(object, name);
    if (!array.is_array()) throw SchemaViolation(std::format("member '{}' must be an array", name));
    std::vector<std::string> strings;
    strings.reserve(array.size());
    for (const auto& element : array)
        strings.push_back(convert<std::string>(element, name));
    return strings;
}

uuids::uuid required_uuid(const nlohmann::json& object, std::string_view name) {
    const auto parsed = uuids::uuid::from_string(required<std::string>(object, name));
    if (!parsed) throw SchemaViolation(std::format("'{}' must be a UUID", name));
    return *parsed;
}

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

template <class T>
nlohmann::json nullable(const std::optional<T>& value) {
    return value ? nlohmann::json(*value) : nlohmann::json(nullptr);
}

// ---------------------------------------------------------------- analysis report

nlohmann::json stages_to_json(const std::vector<analysis::Stage>& stages) {
    nlohmann::json array = nlohmann::json::array();
    for (const auto stage : stages)
        array.push_back(std::string(analysis::to_string(stage)));
    return array;
}

std::vector<analysis::Stage> stages_from_json(const nlohmann::json& object) {
    std::vector<analysis::Stage> stages;
    for (const auto& name : required_strings(object, "stages")) {
        const auto stage = analysis::parse_stage(name);
        if (!stage) throw SchemaViolation(std::format("unknown analysis stage '{}'", name));
        stages.push_back(*stage);
    }
    return stages;
}

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
            if (pitch_class < 0 || pitch_class > 11 || !mode) throw SchemaViolation("invalid musical key");
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
// Found by nlohmann through argument-dependent lookup. They may throw; the public encode/decode
// functions below are the exception boundary.

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

void to_json(nlohmann::json& json, const Cancel& cancel) {
    json = nlohmann::json{{"job_id", uuids::to_string(cancel.job_id)}, {"cancel_epoch", cancel.cancel_epoch}};
}

void from_json(const nlohmann::json& json, Cancel& cancel) {
    cancel.job_id = required_uuid(json, "job_id");
    cancel.cancel_epoch = required<std::int64_t>(json, "cancel_epoch");
}

void to_json(nlohmann::json& json, const Shutdown&) {
    json = nlohmann::json::object();
}

void from_json(const nlohmann::json&, Shutdown&) {}

void to_json(nlohmann::json& json, const WorkerLimits& limits) {
    json = nlohmann::json{{"max_payload_bytes", limits.max_payload_bytes},
                          {"max_waveform_bins", limits.max_waveform_bins},
                          {"max_musical_seconds", finite_number(limits.max_musical_seconds, "max_musical_seconds")},
                          {"concurrent_tasks", limits.concurrent_tasks},
                          {"heartbeat_interval_ms", limits.heartbeat_interval_ms}};
}

void from_json(const nlohmann::json& json, WorkerLimits& limits) {
    limits.max_payload_bytes = required<std::uint32_t>(json, "max_payload_bytes");
    limits.max_waveform_bins = required<int>(json, "max_waveform_bins");
    limits.max_musical_seconds = required<double>(json, "max_musical_seconds");
    limits.concurrent_tasks = required<int>(json, "concurrent_tasks");
    limits.heartbeat_interval_ms = required<std::int64_t>(json, "heartbeat_interval_ms");
}

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

void to_json(nlohmann::json& json, const Heartbeat& heartbeat) {
    const auto active_job_id =
        heartbeat.active_job_id.transform([](const uuids::uuid& id) { return uuids::to_string(id); });
    json = nlohmann::json{{"active_job_id", nullable(active_job_id)}};
}

void from_json(const nlohmann::json& json, Heartbeat& heartbeat) {
    heartbeat.active_job_id = optional_uuid(json, "active_job_id");
}

void to_json(nlohmann::json& json, const Progress& progress) {
    json = nlohmann::json{{"job_id", uuids::to_string(progress.job_id)},
                          {"cancel_epoch", progress.cancel_epoch},
                          {"fraction", finite_number(progress.fraction, "fraction")}};
}

void from_json(const nlohmann::json& json, Progress& progress) {
    progress.job_id = required_uuid(json, "job_id");
    progress.cancel_epoch = required<std::int64_t>(json, "cancel_epoch");
    progress.fraction = required<double>(json, "fraction");
    if (progress.fraction < 0.0 || progress.fraction > 1.0) throw SchemaViolation("'fraction' must be within [0, 1]");
}

void to_json(nlohmann::json& json, const TaskFence& fence) {
    json = nlohmann::json{{"job_id", uuids::to_string(fence.job_id)},
                          {"lease_id", uuids::to_string(fence.lease_id)},
                          {"cancel_epoch", fence.cancel_epoch},
                          {"content_revision", fence.content_revision},
                          {"profile_digest", fence.profile_digest}};
}

void from_json(const nlohmann::json& json, TaskFence& fence) {
    fence.job_id = required_uuid(json, "job_id");
    fence.lease_id = required_uuid(json, "lease_id");
    fence.cancel_epoch = required<std::int64_t>(json, "cancel_epoch");
    fence.content_revision = required<std::int64_t>(json, "content_revision");
    fence.profile_digest = required<std::string>(json, "profile_digest");
}

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

void to_json(nlohmann::json& json, const TaskResult& result) {
    json = nlohmann::json{{"fence", result.fence},
                          {"report", report_to_json(result.report)},
                          {"waveform_segment", nullable(result.waveform_segment)}};
}

void from_json(const nlohmann::json& json, TaskResult& result) {
    required_object(json, "fence").get_to(result.fence);
    result.report = report_from_json(required_object(json, "report"));
    result.waveform_segment.reset();
    if (const auto* segment = optional_object(json, "waveform_segment")) {
        result.waveform_segment = segment->get<WaveformSegment>();
    }
}

void to_json(nlohmann::json& json, const TaskFailed& failure) {
    json = nlohmann::json{{"fence", failure.fence},
                          {"code", std::string(to_string(failure.code))},
                          {"retryable", failure.retryable},
                          {"safe_detail", failure.safe_detail}};
}

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
        return fail(ErrorCode::InvalidArgument, std::format("cannot encode worker message: {}", exception.what()));
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
/// PARSE_ERROR. Parser messages are summarized by byte offset so payload text (which may hold a
/// file path) never ends up in an error detail.
template <class Variant>
Result<Variant> decode_variant(std::string_view payload) {
    try {
        const nlohmann::json object = nlohmann::json::parse(payload);
        if (!object.is_object()) return fail(ErrorCode::ParseError, "worker message is not a JSON object");
        const auto type = required<std::string>(object, "type");
        auto decoded =
            decode_alternative<Variant>(type, object, std::make_index_sequence<std::variant_size_v<Variant>>{});
        if (!decoded) return fail(ErrorCode::ParseError, "unexpected worker message type");
        return std::move(*decoded);
    } catch (const nlohmann::json::parse_error& error) {
        return fail(ErrorCode::ParseError, std::format("worker message is not valid JSON (byte {})", error.byte));
    } catch (const SchemaViolation& violation) {
        return fail(ErrorCode::ParseError, std::format("malformed worker message: {}", violation.what()));
    } catch (const nlohmann::json::exception& exception) {
        return fail(ErrorCode::ParseError, std::format("malformed worker message (json error {})", exception.id));
    }
}

}  // namespace

// ---------------------------------------------------------------- framing

Result<std::vector<std::uint8_t>> encode_frame(std::uint64_t request_sequence, std::string_view payload) {
    if (payload.size() > kMaxPayloadBytes) {
        return fail(ErrorCode::InvalidArgument,
                    std::format("worker frame payload of {} bytes exceeds the {} byte limit", payload.size(),
                                kMaxPayloadBytes));
    }
    std::vector<std::uint8_t> bytes(kFrameHeaderBytes + payload.size());
    std::ranges::copy(kFrameMagic, bytes.begin() + kMagicOffset);
    store_little_endian(bytes.data() + kMajorOffset, kProtocolMajor);
    store_little_endian(bytes.data() + kMinorOffset, kProtocolMinor);
    store_little_endian(bytes.data() + kLengthOffset, static_cast<std::uint32_t>(payload.size()));
    store_little_endian(bytes.data() + kSequenceOffset, request_sequence);
    std::ranges::copy(payload, bytes.begin() + kFrameHeaderBytes);
    return bytes;
}

Result<FrameHeader> parse_frame_header(std::span<const std::uint8_t, kFrameHeaderBytes> bytes) {
    if (!std::ranges::equal(bytes.subspan<kMagicOffset, kFrameMagic.size()>(), kFrameMagic)) {
        return fail(ErrorCode::ParseError, "worker frame has a bad magic number");
    }
    FrameHeader header;
    header.protocol_major = load_little_endian<std::uint16_t>(bytes.data() + kMajorOffset);
    header.protocol_minor = load_little_endian<std::uint16_t>(bytes.data() + kMinorOffset);
    header.payload_length = load_little_endian<std::uint32_t>(bytes.data() + kLengthOffset);
    header.request_sequence = load_little_endian<std::uint64_t>(bytes.data() + kSequenceOffset);
    if (header.protocol_major != kProtocolMajor) {
        return fail(ErrorCode::ParseError, std::format("worker protocol major {} is not the supported {}",
                                                       header.protocol_major, kProtocolMajor));
    }
    if (header.payload_length > kMaxPayloadBytes) {
        return fail(ErrorCode::ParseError, std::format("worker frame announces {} payload bytes; the limit is {}",
                                                       header.payload_length, kMaxPayloadBytes));
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
    return decode_variant<WorkerMessage>(payload);
}

Result<void> validate_task(const AnalyzeTask& task) {
    const auto is_clean_text = [](std::string_view text) {
        return !text.empty() && is_valid_utf8(text) && text.find('\0') == std::string_view::npos;
    };
    if (task.job_id.is_nil() || task.lease_id.is_nil())
        return fail(ErrorCode::InvalidArgument, "task needs a job and a lease id");
    if (task.cancel_epoch < 0 || task.content_revision < 0) {
        return fail(ErrorCode::InvalidArgument, "task cancel epoch and content revision must not be negative");
    }
    if (!is_clean_text(task.file_path))
        return fail(ErrorCode::InvalidArgument, "task file path must be non-empty UTF-8 without NUL");
    if (task.waveform_bins < 1 || task.waveform_bins > kMaxWaveformBins) {
        return fail(ErrorCode::InvalidArgument, std::format("waveform bins must be within [1, {}]", kMaxWaveformBins));
    }
    if (!std::isfinite(task.max_musical_seconds) || task.max_musical_seconds <= 0.0 ||
        task.max_musical_seconds > kMaxMusicalSeconds) {
        return fail(ErrorCode::InvalidArgument,
                    std::format("max musical seconds must be within (0, {}]", kMaxMusicalSeconds));
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

analysis::AnalysisRequest to_analysis_request(const AnalyzeTask& task) {
    analysis::AnalysisRequest request;
    request.file = path_from_utf8(task.file_path);
    request.stages = task.stages;
    request.waveform_bins = task.waveform_bins;
    request.max_musical_seconds = task.max_musical_seconds;
    return request;
}

std::optional<ErrorCode> parse_error_code(std::string_view text) noexcept {
    for (int value = 0; value <= static_cast<int>(ErrorCode::Internal); ++value) {
        const auto code = static_cast<ErrorCode>(value);
        if (to_string(code) == text) return code;
    }
    return std::nullopt;
}

}  // namespace mso::worker
