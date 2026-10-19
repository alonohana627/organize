#include "mso/media/detail/native_file_support.hpp"

#include <format>
#include <string>
#include <system_error>
#include <utility>

#ifdef _WIN32
#include <windows.h>
#else
#include <cerrno>
#endif

namespace mso::media::detail {

#ifdef _WIN32

namespace {

/// Classic Win32 path limit. CreateDirectoryW stops 12 characters earlier (room for an 8.3 name), so
/// anything at or beyond that gets the extended-length prefix.
constexpr std::size_t kClassicPathLimit = MAX_PATH - 12;

}  // namespace

std::filesystem::path native_path(const std::filesystem::path& path) {
    const std::wstring& text = path.native();
    if (text.size() < kClassicPathLimit || text.starts_with(LR"(\\?\)") || text.starts_with(LR"(\\.\)")) {
        return path;
    }
    // The prefix turns off all path normalisation, so the path must already be absolute, use only
    // backslashes and contain no "." or ".." components.
    std::error_code error;
    std::filesystem::path absolute = std::filesystem::absolute(path, error);
    if (error) return path;
    absolute = absolute.lexically_normal();
    absolute.make_preferred();
    const std::wstring& normal = absolute.native();
    if (normal.starts_with(LR"(\\)")) return std::filesystem::path(LR"(\\?\UNC\)" + normal.substr(2));
    return std::filesystem::path(LR"(\\?\)" + normal);
}

UniqueWin32Handle::UniqueWin32Handle(UniqueWin32Handle&& other) noexcept
    : handle_(std::exchange(other.handle_, nullptr)) {}

UniqueWin32Handle& UniqueWin32Handle::operator=(UniqueWin32Handle&& other) noexcept {
    if (this != &other) {
        reset();
        handle_ = std::exchange(other.handle_, nullptr);
    }
    return *this;
}

UniqueWin32Handle::~UniqueWin32Handle() {
    reset();
}

bool UniqueWin32Handle::valid() const noexcept {
    return handle_ != nullptr && handle_ != INVALID_HANDLE_VALUE;
}

void* UniqueWin32Handle::release() noexcept {
    return std::exchange(handle_, nullptr);
}

void UniqueWin32Handle::reset() noexcept {
    if (valid()) CloseHandle(handle_);
    handle_ = nullptr;
}

Error error_from_win32(std::uint32_t win32_error, std::string_view operation) {
    switch (win32_error) {
        case ERROR_FILE_NOT_FOUND:
        case ERROR_PATH_NOT_FOUND:
        case ERROR_INVALID_NAME:
        case ERROR_INVALID_DRIVE:
        case ERROR_BAD_NETPATH:
        case ERROR_BAD_NET_NAME:
        case ERROR_NOT_READY:
        case ERROR_FILENAME_EXCED_RANGE:
            return fail(ErrorCode::FileMissing, "file does not exist").error();
        case ERROR_SHARING_VIOLATION:
        case ERROR_LOCK_VIOLATION:
            return fail(ErrorCode::AccessDenied, "file is locked by another process", true).error();
        case ERROR_ACCESS_DENIED:
            return fail(ErrorCode::AccessDenied, std::format("{}: access denied", operation)).error();
        default:
            return fail(ErrorCode::AccessDenied, std::format("{} failed (Win32 error {})", operation, win32_error))
                .error();
    }
}

#else

std::filesystem::path native_path(const std::filesystem::path& path) {
    return path;
}

Error error_from_errno(int error_number, std::string_view operation) {
    switch (error_number) {
        case ENOENT:
        case ENOTDIR:
        case ENAMETOOLONG:
        case ELOOP:
            return fail(ErrorCode::FileMissing, "file does not exist").error();
        case EAGAIN:
        case EBUSY:
            return fail(ErrorCode::AccessDenied, "file is busy", true).error();
        case EACCES:
        case EPERM:
            return fail(ErrorCode::AccessDenied, std::format("{}: access denied", operation)).error();
        default:
            return fail(ErrorCode::AccessDenied, std::format("{} failed (errno {})", operation, error_number)).error();
    }
}

#endif

}  // namespace mso::media::detail
