#pragma once
/// @file
/// Small platform helpers for every module that opens source files directly (the media file handle,
/// the importer's identity reads and its directory listings): extended-length paths on Windows, an
/// owning Win32 handle, and one mapping from native error codes to the error taxonomy. Public so
/// that `files` (which declares `media` as a dependency) never includes a media `detail/` header.
#include <cstdint>
#include <filesystem>
#include <string_view>

#include "mso/domain/error.hpp"

namespace mso::media {

/// Path to hand to native file APIs. On Windows a path whose absolute form is too long for the
/// classic MAX_PATH limit is made absolute and given the extended-length prefix ("\\?\C:\..." or
/// "\\?\UNC\server\share\..."), so it opens even in processes that are not long-path aware. A
/// relative path is judged by the absolute path it resolves to, not by its own length. Short
/// paths, already-prefixed paths and every path on POSIX are returned unchanged.
[[nodiscard]] std::filesystem::path native_path(const std::filesystem::path& path);

#ifdef _WIN32

/// Owning Win32 handle (closed with CloseHandle). Holds a HANDLE as `void*` so this header does
/// not pull in <windows.h>. Both nullptr and INVALID_HANDLE_VALUE count as "no handle".
class UniqueWin32Handle {
public:
    UniqueWin32Handle() noexcept = default;
    /// Takes ownership of `handle` (which may be INVALID_HANDLE_VALUE).
    explicit UniqueWin32Handle(void* handle) noexcept : handle_(handle) {}
    UniqueWin32Handle(UniqueWin32Handle&& other) noexcept;
    UniqueWin32Handle& operator=(UniqueWin32Handle&& other) noexcept;
    UniqueWin32Handle(const UniqueWin32Handle&) = delete;
    UniqueWin32Handle& operator=(const UniqueWin32Handle&) = delete;
    ~UniqueWin32Handle();

    /// True when a handle is held.
    [[nodiscard]] bool valid() const noexcept;
    /// The raw handle, still owned by this object.
    [[nodiscard]] void* get() const noexcept { return handle_; }
    /// Gives up ownership and returns the raw handle; the object becomes empty.
    [[nodiscard]] void* release() noexcept;

private:
    /// Closes the held handle, if any, and leaves the object empty.
    void reset() noexcept;
    void* handle_ = nullptr;
};

/// Maps a Win32 error code from opening or querying a file to the error taxonomy:
/// not-found family -> FileMissing; sharing and lock violations -> AccessDenied (retryable);
/// everything else -> AccessDenied naming the code. `operation` names what failed ("open").
[[nodiscard]] Error error_from_win32(std::uint32_t win32_error, std::string_view operation);

#else

/// Maps an errno value from opening or querying a file to the error taxonomy:
/// ENOENT/ENOTDIR/ENAMETOOLONG/ELOOP -> FileMissing; EAGAIN/EBUSY -> AccessDenied (retryable);
/// everything else -> AccessDenied naming the errno. `operation` names what failed ("open").
[[nodiscard]] Error error_from_errno(int error_number, std::string_view operation);

#endif

}  // namespace mso::media
