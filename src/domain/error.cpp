#include "mso/domain/error.hpp"

#include <format>

namespace mso {

std::string_view to_string(ErrorCode code) noexcept {
    switch (code) {
        case ErrorCode::SourceOffline:
            return "SOURCE_OFFLINE";
        case ErrorCode::FileMissing:
            return "FILE_MISSING";
        case ErrorCode::AccessDenied:
            return "ACCESS_DENIED";
        case ErrorCode::InputChanged:
            return "INPUT_CHANGED";
        case ErrorCode::UnsupportedCodec:
            return "UNSUPPORTED_CODEC";
        case ErrorCode::CorruptAudio:
            return "CORRUPT_AUDIO";
        case ErrorCode::ProbeLimit:
            return "PROBE_LIMIT";
        case ErrorCode::DeviceLost:
            return "DEVICE_LOST";
        case ErrorCode::ModelMissing:
            return "MODEL_MISSING";
        case ErrorCode::ModelInvalid:
            return "MODEL_INVALID";
        case ErrorCode::IndexUnavailable:
            return "INDEX_UNAVAILABLE";
        case ErrorCode::ResourceLimit:
            return "RESOURCE_LIMIT";
        case ErrorCode::DiskFull:
            return "DISK_FULL";
        case ErrorCode::DbBusy:
            return "DB_BUSY";
        case ErrorCode::DbCorrupt:
            return "DB_CORRUPT";
        case ErrorCode::SchemaTooNew:
            return "SCHEMA_TOO_NEW";
        case ErrorCode::StaleCursor:
            return "STALE_CURSOR";
        case ErrorCode::HistoryConflict:
            return "HISTORY_CONFLICT";
        case ErrorCode::Cancelled:
            return "CANCELLED";
        case ErrorCode::WorkerCrash:
            return "WORKER_CRASH";
        case ErrorCode::IdempotencyMismatch:
            return "IDEMPOTENCY_MISMATCH";
        case ErrorCode::InvalidArgument:
            return "INVALID_ARGUMENT";
        case ErrorCode::ParseError:
            return "PARSE_ERROR";
        case ErrorCode::NotFound:
            return "NOT_FOUND";
        case ErrorCode::Internal:
            return "INTERNAL";
    }
    return "INTERNAL";
}

std::string Error::message() const {
    if (position) return std::format("{} at {}: {}", to_string(code), *position, safe_detail);
    if (safe_detail.empty()) return std::string(to_string(code));
    return std::format("{}: {}", to_string(code), safe_detail);
}

}  // namespace mso
