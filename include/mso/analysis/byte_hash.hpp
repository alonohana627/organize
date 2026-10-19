#pragma once
/// @file
/// Streaming SHA-256 of a file's raw bytes (algorithm AlgorithmVersions::kByteHash).
#include <cstdint>
#include <filesystem>
#include <stop_token>
#include <string>

#include "mso/analysis/progress_reporter.hpp"
#include "mso/domain/error.hpp"

namespace mso::analysis {

/// Hashes the file in 1 MiB chunks and returns the lowercase hex digest. Records one unit of work
/// per chunk.
///
/// `expected_size` only scales progress. Errors: FILE_MISSING (the file definitely does not exist),
/// ACCESS_DENIED (it exists but cannot be opened), SOURCE_OFFLINE (retryable: its existence cannot
/// be checked, or a read failed) and CANCELLED (checked before every chunk).
[[nodiscard]] Result<std::string> hash_file_bytes(const std::filesystem::path& file, std::int64_t expected_size,
                                                  std::stop_token stop, const ProgressPhase& progress);

}  // namespace mso::analysis
