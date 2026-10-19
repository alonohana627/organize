#pragma once
/// @file
/// Case sensitivity of root folders. A root records whether its volume distinguishes file names by
/// case; location keys are then built with mso::path_key(relative_path, root.case_sensitive), so on
/// case-sensitive roots (Linux defaults) `Kick.wav` and `kick.wav` stay two locations.
#include <filesystem>

#include "mso/domain/error.hpp"

namespace mso::catalog {

/// The case sensitivity assumed when no directory can be examined: false on Windows and macOS,
/// true on other POSIX systems.
[[nodiscard]] bool default_case_sensitivity() noexcept;

/// Examines a directory without writing to it: finds an entry whose name has a case variant and
/// checks whether that variant resolves to the same file. Falls back to the directory's own name,
/// then to default_case_sensitivity(). NOT_FOUND / ACCESS_DENIED when the directory is unusable.
[[nodiscard]] Result<bool> detect_case_sensitivity(const std::filesystem::path& directory);

}  // namespace mso::catalog
