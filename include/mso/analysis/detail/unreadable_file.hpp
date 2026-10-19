#pragma once
/// @file
/// Internal: why a file that analysis could not open or query cannot be read.
#include <cstdint>
#include <filesystem>

#include "mso/domain/error.hpp"

namespace mso::analysis::detail {

/// Why `file` cannot be read, asked after opening it or reading its attributes failed:
///  - FILE_MISSING when it definitely does not exist (its folder exists, the name does not);
///  - SOURCE_OFFLINE (retryable) when its drive, share or server cannot be reached, or its
///    existence cannot be checked at all;
///  - ACCESS_DENIED when it exists but cannot be opened.
/// On Windows the answer comes from GetFileAttributesW's own error code: libc++ reports an
/// unreachable server (ERROR_BAD_NETPATH) as "not found" without an error code, which would make an
/// offline share look like deleted files.
[[nodiscard]] Error unreadable_file_error(const std::filesystem::path& file);

#ifdef _WIN32
/// The verdict of unreadable_file_error() for the Win32 error code that GetFileAttributesW reported
/// (ERROR_SUCCESS: the file exists).
[[nodiscard]] Error unreadable_file_error_from_win32(std::uint32_t win32_error);
#endif

}  // namespace mso::analysis::detail
