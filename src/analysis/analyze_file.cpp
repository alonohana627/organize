/// @file
/// analyze_file(): input evidence, byte hashing and the shared decode pass for one file.
///
/// Byte hashing and decoding each read the file once. Folding the byte hash into the decode pass
/// would need a decoder that reads through a caller-supplied byte stream, which media::open_decoder
/// does not offer; the second read is usually served by the OS page cache, and only slow network
/// shares pay for it twice.
#include <chrono>
#include <exception>
#include <format>
#include <memory>
#include <new>
#include <optional>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>

#include "mso/analysis/analysis.hpp"
#include "mso/analysis/byte_hash.hpp"
#include "mso/analysis/decoded_stream_analysis.hpp"
#include "mso/analysis/detail/unreadable_file.hpp"
#include "mso/analysis/progress_reporter.hpp"
#include "mso/media/media.hpp"

namespace mso::analysis {
namespace {

/// Share of overall progress given to byte hashing when a decode pass follows it.
constexpr double kByteHashProgressShare = 0.25;

/// Size and modification time of a file, the evidence compared before and after reading.
struct FileStamp {
    std::int64_t size = 0;
    std::int64_t modified_ns = 0;
};

/// Maps a failed filesystem query to the error taxonomy: a vanished path is FILE_MISSING, a
/// permission problem ACCESS_DENIED, and anything else (I/O error, unreachable share) a retryable
/// SOURCE_OFFLINE.
std::unexpected<Error> file_query_failure(const std::error_code& error, std::string_view what) {
    if (error == std::errc::no_such_file_or_directory || error == std::errc::not_a_directory)
        return fail(ErrorCode::FileMissing, "file not found");
    if (error == std::errc::permission_denied || error == std::errc::operation_not_permitted)
        return fail(ErrorCode::AccessDenied, std::format("cannot read the file's {}", what));
    return fail(ErrorCode::SourceOffline, std::format("cannot read the file's {}", what), /*retryable=*/true);
}

/// Reads the size and modification time of a regular file without throwing.
Result<FileStamp> read_file_stamp(const std::filesystem::path& file) {
    std::error_code error;
    const auto status = std::filesystem::status(file, error);
    // libc++ reports an unreachable server as "not found" without an error: ask the OS what it is.
    if (status.type() == std::filesystem::file_type::not_found)
        return std::unexpected(detail::unreadable_file_error(file));
    if (error) return file_query_failure(error, "attributes");
    if (!std::filesystem::is_regular_file(status)) return fail(ErrorCode::InvalidArgument, "not a regular file");

    const auto size = std::filesystem::file_size(file, error);
    if (error) return file_query_failure(error, "size");
    const auto modified = std::filesystem::last_write_time(file, error);
    if (error) return file_query_failure(error, "modification time");

    FileStamp stamp;
    stamp.size = static_cast<std::int64_t>(size);
    stamp.modified_ns = std::chrono::duration_cast<std::chrono::nanoseconds>(modified.time_since_epoch()).count();
    return stamp;
}

/// The body of analyze_file(); library code below it may throw, analyze_file() catches.
Result<AnalysisReport> analyze_file_or_throw(const AnalysisRequest& request, std::stop_token stop,
                                             const ProgressCallback& progress, WorkCounter* work_counter) {
    if (auto valid = validate_request(request); !valid) return std::unexpected(valid.error());
    const StageSelection stages = StageSelection::from(request.stages);

    const Result<FileStamp> before = read_file_stamp(request.file);
    if (!before) return std::unexpected(before.error());
    if (stop.stop_requested()) return fail(ErrorCode::Cancelled, "analysis cancelled");

    ProgressReporter reporter(progress, ProgressReporter::kDefaultInterval, work_counter);
    const double byte_hash_share = !stages.byte_hash ? 0.0 : stages.needs_decode() ? kByteHashProgressShare : 1.0;

    std::optional<std::string> byte_sha256;
    if (stages.byte_hash) {
        Result<std::string> digest =
            hash_file_bytes(request.file, before->size, stop, ProgressPhase(reporter, 0.0, byte_hash_share));
        if (!digest) return std::unexpected(digest.error());
        byte_sha256 = std::move(*digest);
    }

    AnalysisReport report;
    if (stages.needs_decode()) {
        Result<std::unique_ptr<media::Decoder>> decoder = media::open_decoder(request.file);
        if (!decoder) return std::unexpected(decoder.error());
        Result<AnalysisReport> decoded =
            analyze_decoded_stream(**decoder, request, stop, ProgressPhase(reporter, byte_hash_share, 1.0));
        if (!decoded) return std::unexpected(decoded.error());
        report = std::move(*decoded);
    }  // the decoder (and its file handle) is closed before the second stamp
    report.hashes.byte_sha256 = std::move(byte_sha256);

    const Result<FileStamp> after = read_file_stamp(request.file);
    if (!after) return fail(ErrorCode::InputChanged, "file disappeared during analysis");
    report.evidence.size_before = before->size;
    report.evidence.mtime_ns_before = before->modified_ns;
    report.evidence.size_after = after->size;
    report.evidence.mtime_ns_after = after->modified_ns;
    if (!report.evidence.unchanged()) return fail(ErrorCode::InputChanged, "file changed during analysis");

    reporter.finish();
    return report;
}

}  // namespace

Result<AnalysisReport> analyze_file(const AnalysisRequest& request, std::stop_token stop,
                                    const ProgressCallback& progress, WorkCounter* work_counter) {
    // No exception crosses the module boundary: allocation failure and library exceptions
    // (pocketfft, std::format, the progress callback) become ordinary errors. The final handler
    // exists because the callback is caller code that may throw anything.
    try {
        return analyze_file_or_throw(request, std::move(stop), progress, work_counter);
    } catch (const std::bad_alloc&) {
        return fail(ErrorCode::ResourceLimit, "out of memory during analysis");
    } catch (const std::exception& error) {
        return fail(ErrorCode::Internal, std::string("analysis failed: ") + error.what());
    } catch (...) {
        return fail(ErrorCode::Internal, "analysis failed with an unknown exception");
    }
}

}  // namespace mso::analysis
