/// @file
/// Streaming raw-byte SHA-256.
#include "mso/analysis/byte_hash.hpp"

#include <picosha2.h>

#include <fstream>
#include <vector>

#include "mso/analysis/detail/unreadable_file.hpp"
#include "mso/domain/core.hpp"

namespace mso::analysis {
namespace {

/// Bytes read and hashed per step: bounds memory and cancellation latency; one unit of work.
constexpr std::size_t kChunkBytes = 1 << 20;

}  // namespace

Result<std::string> hash_file_bytes(const std::filesystem::path& file, std::int64_t expected_size, std::stop_token stop,
                                    const ProgressPhase& progress) {
    std::ifstream stream(file, std::ios::binary);
    if (!stream) {
        // Only a definite "does not exist" is FILE_MISSING; an unreachable share or drive (or an
        // existence check that fails) may well still hold the file, so the job is retried.
        return std::unexpected(detail::unreadable_file_error(file));
    }

    picosha2::hash256_one_by_one hasher;
    std::vector<char> chunk(kChunkBytes);
    std::int64_t bytes_read = 0;
    while (true) {
        if (stop.stop_requested()) return fail(ErrorCode::Cancelled, "byte hash cancelled");
        stream.read(chunk.data(), static_cast<std::streamsize>(chunk.size()));
        const std::streamsize count = stream.gcount();
        if (count > 0) {
            hasher.process(chunk.begin(), chunk.begin() + count);
            bytes_read += count;
            progress.record_work();
            if (expected_size > 0)
                progress.report(static_cast<double>(bytes_read) / static_cast<double>(expected_size));
        }
        if (stream.eof()) break;
        if (!stream) return fail(ErrorCode::SourceOffline, "read failed while hashing", /*retryable=*/true);
    }

    hasher.finish();
    return picosha2::get_hash_hex_string(hasher);
}

}  // namespace mso::analysis
