#pragma once
/// @file
/// Filesystem identity and stat evidence for reconciliation (docs/02_ARCHITECTURE.md §7).
/// Volume serial + 128-bit file ID identify a file across renames on the same volume; size and
/// modification time detect possible content changes. Neither is proof of equal bytes.
#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>

#include "mso/domain/error.hpp"

namespace mso::files {

/// Stat and identity of one file.
struct FileIdentity {
    std::int64_t byte_size = 0;
    std::int64_t mtime_ns = 0;             ///< last write time, nanoseconds since the Unix epoch
    std::optional<std::string> volume_id;  ///< hex volume serial number
    std::optional<std::string> file_id;    ///< 16 raw bytes of the NTFS/exFAT/ReFS file ID
};

/// Reads size, mtime and (where the filesystem supports it) volume + file ID without reading
/// file content. Errors: FILE_MISSING, ACCESS_DENIED.
Result<FileIdentity> read_file_identity(const std::filesystem::path& file);

/// True when the path is a reparse point (symlink, junction): not followed by default.
bool is_reparse_point(const std::filesystem::path& path);

}  // namespace mso::files
