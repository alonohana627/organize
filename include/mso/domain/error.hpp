#pragma once
/// @file
/// The stable error taxonomy (docs/04_CONTRACTS.md section 6) and the `Result` alias every fallible
/// project API returns. Codes are part of the external contract (CLI JSON, worker protocol, UI
/// messages): add new codes at the end and never renumber or rename an existing one.
#include <cstdint>
#include <expected>
#include <optional>
#include <string>
#include <string_view>

namespace mso {

/// Stable, user-facing error categories. `to_string` gives the wire name (e.g. "STALE_CURSOR").
enum class ErrorCode : std::uint8_t {
    SourceOffline,        ///< the root or drive holding the file is not reachable
    FileMissing,          ///< the file is no longer at its recorded location
    AccessDenied,         ///< the operating system refused access
    InputChanged,         ///< the file or sample changed while it was being processed
    UnsupportedCodec,     ///< no decoder handles this format or codec
    CorruptAudio,         ///< the audio data is malformed
    ProbeLimit,           ///< a bounded probe or parser ran out of its budget
    DeviceLost,           ///< the audio output device disappeared
    ModelMissing,         ///< a required analysis model file is absent
    ModelInvalid,         ///< an analysis model file failed validation
    IndexUnavailable,     ///< a search or analysis index cannot be used right now
    ResourceLimit,        ///< a memory, size or count limit would be exceeded
    DiskFull,             ///< the destination volume has no space left
    DbBusy,               ///< SQLite stayed locked past the retry deadline (usually retryable)
    DbCorrupt,            ///< the catalog database failed an integrity check
    SchemaTooNew,         ///< the catalog was written by a newer version of the application
    StaleCursor,          ///< a paging cursor no longer matches the query, sort or library set
    HistoryConflict,      ///< undo/redo cannot apply because the data changed since
    Cancelled,            ///< the operation was cancelled by the user or by shutdown
    WorkerCrash,          ///< the analysis worker process exited unexpectedly
    IdempotencyMismatch,  ///< an operation id was reused with a different command
    InvalidArgument,      ///< the caller passed a value outside the API's contract
    ParseError,           ///< user text (query, key, setting) could not be parsed
    NotFound,             ///< the requested record does not exist
    Internal,             ///< a bug or an unexpected library failure
};

/// Wire name of an error code ("SOURCE_OFFLINE", ..., "INTERNAL"); "INTERNAL" for out-of-range values.
std::string_view to_string(ErrorCode code) noexcept;

/// A failure with a stable code and a detail that is safe to show to the user (no secrets, no raw SQL).
struct Error {
    ErrorCode code = ErrorCode::Internal;   ///< stable category
    bool retryable = false;                 ///< true when the same call may succeed later unchanged
    std::string safe_detail;                ///< human-readable explanation, safe for logs and the UI
    std::optional<std::int64_t> sample_id;  ///< library-local sample id; never leaves library scope in UI DTOs
    std::optional<std::uint32_t> position;  ///< parse errors: code-point offset into the parsed text

    /// "CODE: detail", "CODE at position: detail" for parse errors, or just "CODE" without a detail.
    std::string message() const;
};

/// The return type of every fallible operation: a value or an `Error`.
template <class T>
using Result = std::expected<T, Error>;

/// Builds the error half of a `Result` from a code, a safe detail and the retry hint.
inline std::unexpected<Error> fail(ErrorCode code, std::string detail = {}, bool retryable = false) {
    return std::unexpected(Error{code, retryable, std::move(detail), std::nullopt, std::nullopt});
}

}  // namespace mso
