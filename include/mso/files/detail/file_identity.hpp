#pragma once
/// @file
/// Filesystem identity and stat evidence for reconciliation (docs/02_ARCHITECTURE.md §7), and the
/// classification of links and placeholders met during enumeration.
/// Volume id + file id identify a file across renames on the same volume; size and modification
/// time detect possible content changes. Neither is proof of equal bytes.
#include <array>
#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>

#include "mso/domain/error.hpp"

namespace mso::files::detail {

/// A 128-bit file identifier. Windows: the NTFS/ReFS/exFAT file ID (a 64-bit file index is
/// zero-extended); POSIX: the inode number, little-endian, zero-extended. Unique per volume.
using FileId = std::array<std::uint8_t, 16>;

/// Stat and identity of one file or directory.
struct FileIdentity {
    std::int64_t byte_size = 0;
    std::int64_t last_write_time_ns = 0;   ///< last write time, nanoseconds since the Unix epoch
    std::optional<std::string> volume_id;  ///< volume serial (Windows) or device number (POSIX), 16 hex digits
    std::optional<FileId> file_id;         ///< unique per volume; see FileId
};

/// Reads size, last write time and (where the filesystem supports it) volume and file id without
/// reading file content and without blocking writers. Links are followed, so the identity is the
/// target's. Works for directories too (cycle detection while following links).
/// Errors: FileMissing, AccessDenied (retryable while locked).
[[nodiscard]] Result<FileIdentity> read_file_identity(const std::filesystem::path& file);

/// What a directory entry is, beyond "file" or "directory", for the importer's link policy.
enum class LinkKind {
    None,              ///< an ordinary file or directory
    SymbolicLink,      ///< a symbolic link (file or directory)
    Junction,          ///< a Windows directory junction (mount point)
    CloudPlaceholder,  ///< a cloud-sync entry (OneDrive, iCloud...) that is available locally
    CloudOnly,         ///< a cloud-sync file whose data is not on this device: reading it would download it
    Deduplicated,      ///< a Windows data-deduplication file: reads like an ordinary file
    Compressed,  ///< a Windows Overlay Filter compressed file (compact /EXE, CompactGUI): reads like an ordinary file
    OtherReparsePoint  ///< another Windows reparse point whose behaviour is unknown
};

/// What classify_entry() learned about a directory entry.
struct EntryClassification {
    LinkKind kind = LinkKind::None;  ///< link or placeholder kind
    /// The entry is a directory, or a link or placeholder standing for one (from the entry's own
    /// attributes on Windows, so a cloud-only folder is never opened to find out).
    bool is_directory = false;
};

/// Classifies `path` without following it (on POSIX, a link's target type is read through it, but
/// nothing is opened for reading). Never throws; an entry that cannot be inspected is reported as
/// LinkKind::None and not a directory (the following open reports the real error).
[[nodiscard]] EntryClassification classify_entry(const std::filesystem::path& path) noexcept;

/// classify_entry(path).kind.
[[nodiscard]] LinkKind classify_link(const std::filesystem::path& path) noexcept;

/// The Windows rule behind classify_entry() for an entry that is a reparse point: what reparse
/// `tag` (WIN32_FIND_DATAW::dwReserved0) and file `attributes` mean for the importer. Data that
/// is elsewhere (offline or recall attributes on a cloud entry) is CloudOnly; deduplicated and
/// WOF-compressed files are ordinary files to read; an unknown tag is OtherReparsePoint. Plain
/// numbers, so the rule is testable on every platform.
[[nodiscard]] LinkKind classify_reparse_tag(std::uint32_t tag, std::uint32_t attributes, bool is_directory) noexcept;

}  // namespace mso::files::detail
