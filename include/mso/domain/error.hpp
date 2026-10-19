#pragma once
// Stable error taxonomy (docs/04_CONTRACTS.md section 6) and the Result alias.
#include <cstdint>
#include <expected>
#include <optional>
#include <string>
#include <string_view>

namespace mso {

enum class ErrorCode : std::uint8_t {
    SourceOffline,
    FileMissing,
    AccessDenied,
    InputChanged,
    UnsupportedCodec,
    CorruptAudio,
    ProbeLimit,
    DeviceLost,
    ModelMissing,
    ModelInvalid,
    IndexUnavailable,
    ResourceLimit,
    DiskFull,
    DbBusy,
    DbCorrupt,
    SchemaTooNew,
    StaleCursor,
    HistoryConflict,
    Cancelled,
    WorkerCrash,
    IdempotencyMismatch,
    InvalidArgument,
    ParseError,
    NotFound,
    Internal,
};

std::string_view to_string(ErrorCode code) noexcept;

struct Error {
    ErrorCode code = ErrorCode::Internal;
    bool retryable = false;
    std::string safe_detail;
    std::optional<std::int64_t> sample_id;  // library-local; never escapes library scope in UI DTOs
    std::optional<std::uint32_t> position;  // parse errors: code-point offset in the query

    std::string message() const;
};

template <class T>
using Result = std::expected<T, Error>;

inline std::unexpected<Error> fail(ErrorCode code, std::string detail = {}, bool retryable = false) {
    return std::unexpected(Error{code, retryable, std::move(detail), std::nullopt, std::nullopt});
}

}  // namespace mso
