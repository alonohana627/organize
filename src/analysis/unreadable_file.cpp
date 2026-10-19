/// @file
/// Classification of files that analysis cannot open or query (see detail/unreadable_file.hpp).
#include "mso/analysis/detail/unreadable_file.hpp"

#include <system_error>

#ifdef _WIN32
#include <windows.h>

#include "mso/media/native_file.hpp"
#endif

namespace mso::analysis::detail {

#ifdef _WIN32

Error unreadable_file_error_from_win32(std::uint32_t win32_error) {
    switch (win32_error) {
        case ERROR_SUCCESS:
            return fail(ErrorCode::AccessDenied, "file cannot be opened for reading").error();
        case ERROR_FILE_NOT_FOUND:
        case ERROR_PATH_NOT_FOUND:
        case ERROR_INVALID_NAME:
        case ERROR_DIRECTORY:
            return fail(ErrorCode::FileMissing, "file not found").error();
        case ERROR_ACCESS_DENIED:
            return fail(ErrorCode::AccessDenied, "file cannot be opened for reading").error();
        case ERROR_SHARING_VIOLATION:
        case ERROR_LOCK_VIOLATION:
            return fail(ErrorCode::AccessDenied, "file is locked by another process", /*retryable=*/true).error();
        default:
            // An unreachable server or share (ERROR_BAD_NETPATH, ERROR_BAD_NET_NAME,
            // ERROR_NETNAME_DELETED), a drive that is not there or not ready (ERROR_INVALID_DRIVE,
            // ERROR_NOT_READY), a device or network error: the file may well still exist.
            return fail(ErrorCode::SourceOffline, "file cannot be reached", /*retryable=*/true).error();
    }
}

Error unreadable_file_error(const std::filesystem::path& file) {
    const std::filesystem::path native = media::native_path(file);
    if (GetFileAttributesW(native.c_str()) != INVALID_FILE_ATTRIBUTES) {
        return unreadable_file_error_from_win32(ERROR_SUCCESS);
    }
    return unreadable_file_error_from_win32(GetLastError());
}

#else

Error unreadable_file_error(const std::filesystem::path& file) {
    std::error_code error;
    const auto status = std::filesystem::status(file, error);
    if (status.type() == std::filesystem::file_type::not_found) {
        return fail(ErrorCode::FileMissing, "file not found").error();
    }
    if (error) {
        return fail(ErrorCode::SourceOffline, "file cannot be reached", /*retryable=*/true).error();
    }
    return fail(ErrorCode::AccessDenied, "file cannot be opened for reading").error();
}

#endif

}  // namespace mso::analysis::detail
