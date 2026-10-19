#pragma once
/// @file
/// Crash-safe replacement of small files (library.json): write a uniquely named temporary file next
/// to the target, flush it to stable storage, rename it over the target, then flush the directory
/// entry (POSIX). A failure leaves the previous file intact and removes the temporary file.
#include <filesystem>
#include <string_view>

#include "mso/domain/error.hpp"

namespace mso::catalog {

/// Atomically and durably replaces `target` with `contents` (FlushFileBuffers + MoveFileExW with
/// MOVEFILE_WRITE_THROUGH on Windows; fsync of the file, rename, fsync of the directory on POSIX).
[[nodiscard]] Result<void> write_file_atomically(const std::filesystem::path& target, std::string_view contents);

}  // namespace mso::catalog
