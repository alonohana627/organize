#include "mso/media/file_handle.hpp"

#include <algorithm>
#include <format>
#include <limits>
#include <system_error>
#include <utility>

#ifdef _WIN32
#include <windows.h>
#else
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>
#include <cerrno>
#endif

namespace mso::media::detail {

namespace {

/// Directories cannot be opened as files; report them as "not audio" rather than access denied.
bool path_is_directory(const std::filesystem::path& file) {
    std::error_code ignored;
    return std::filesystem::is_directory(file, ignored);
}

}  // namespace

#ifdef _WIN32

namespace {

/// Maps a CreateFileW failure to the media error taxonomy.
Error open_error(DWORD win32_error, const std::filesystem::path& file) {
    switch (win32_error) {
        case ERROR_FILE_NOT_FOUND:
        case ERROR_PATH_NOT_FOUND:
        case ERROR_INVALID_NAME:
        case ERROR_INVALID_DRIVE:
        case ERROR_BAD_NETPATH:
        case ERROR_BAD_NET_NAME:
            return fail(ErrorCode::FileMissing, "file does not exist").error();
        case ERROR_SHARING_VIOLATION:
        case ERROR_LOCK_VIOLATION:
            return fail(ErrorCode::AccessDenied, "file is locked by another process", true).error();
        case ERROR_ACCESS_DENIED:
            if (path_is_directory(file)) return fail(ErrorCode::UnsupportedCodec, "path is a directory").error();
            return fail(ErrorCode::AccessDenied, "access to the file was denied").error();
        default:
            return fail(ErrorCode::AccessDenied, std::format("cannot open file (Win32 error {})", win32_error)).error();
    }
}

}  // namespace

Result<FileHandle> FileHandle::open_for_read(const std::filesystem::path& file) {
    HANDLE handle = CreateFileW(file.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                                nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (handle == INVALID_HANDLE_VALUE) return std::unexpected(open_error(GetLastError(), file));
    return FileHandle(handle);
}

void FileHandle::close() noexcept {
    if (native_handle_ != nullptr) CloseHandle(native_handle_);
    native_handle_ = nullptr;
}

FileHandle::FileHandle(FileHandle&& other) noexcept : native_handle_(std::exchange(other.native_handle_, nullptr)) {}

FileHandle& FileHandle::operator=(FileHandle&& other) noexcept {
    if (this != &other) {
        close();
        native_handle_ = std::exchange(other.native_handle_, nullptr);
    }
    return *this;
}

FileHandle::~FileHandle() {
    close();
}

Result<std::uint64_t> FileHandle::size() const {
    LARGE_INTEGER length{};
    if (!GetFileSizeEx(native_handle_, &length)) {
        return fail(ErrorCode::AccessDenied, std::format("cannot read file size (Win32 error {})", GetLastError()));
    }
    return static_cast<std::uint64_t>(length.QuadPart);
}

Result<std::size_t> FileHandle::read_at(std::uint64_t offset, std::span<std::uint8_t> destination) const {
    std::size_t total = 0;
    while (total < destination.size()) {
        const std::uint64_t position = offset + total;
        OVERLAPPED overlapped{};
        overlapped.Offset = static_cast<DWORD>(position & 0xFFFFFFFFu);
        overlapped.OffsetHigh = static_cast<DWORD>(position >> 32);
        const auto request = static_cast<DWORD>(
            std::min<std::size_t>(destination.size() - total, std::numeric_limits<DWORD>::max() / 2));
        DWORD received = 0;
        if (!ReadFile(native_handle_, destination.data() + total, request, &received, &overlapped)) {
            const DWORD error = GetLastError();
            if (error == ERROR_HANDLE_EOF) break;
            return fail(ErrorCode::AccessDenied, std::format("read failed (Win32 error {})", error), true);
        }
        if (received == 0) break;
        total += received;
    }
    return total;
}

#else

Result<FileHandle> FileHandle::open_for_read(const std::filesystem::path& file) {
    const int descriptor = ::open(file.c_str(), O_RDONLY | O_CLOEXEC);
    if (descriptor < 0) {
        if (errno == ENOENT || errno == ENOTDIR) return fail(ErrorCode::FileMissing, "file does not exist");
        return fail(ErrorCode::AccessDenied, std::format("cannot open file (errno {})", errno));
    }
    if (path_is_directory(file)) {
        ::close(descriptor);
        return fail(ErrorCode::UnsupportedCodec, "path is a directory");
    }
    return FileHandle(descriptor);
}

void FileHandle::close() noexcept {
    if (descriptor_ >= 0) ::close(descriptor_);
    descriptor_ = -1;
}

FileHandle::FileHandle(FileHandle&& other) noexcept : descriptor_(std::exchange(other.descriptor_, -1)) {}

FileHandle& FileHandle::operator=(FileHandle&& other) noexcept {
    if (this != &other) {
        close();
        descriptor_ = std::exchange(other.descriptor_, -1);
    }
    return *this;
}

FileHandle::~FileHandle() {
    close();
}

Result<std::uint64_t> FileHandle::size() const {
    struct stat status{};
    if (::fstat(descriptor_, &status) != 0) return fail(ErrorCode::AccessDenied, "cannot read file size");
    return static_cast<std::uint64_t>(status.st_size);
}

Result<std::size_t> FileHandle::read_at(std::uint64_t offset, std::span<std::uint8_t> destination) const {
    std::size_t total = 0;
    while (total < destination.size()) {
        const ssize_t received = ::pread(descriptor_, destination.data() + total, destination.size() - total,
                                         static_cast<off_t>(offset + total));
        if (received < 0) {
            if (errno == EINTR) continue;
            return fail(ErrorCode::AccessDenied, std::format("read failed (errno {})", errno), true);
        }
        if (received == 0) break;
        total += static_cast<std::size_t>(received);
    }
    return total;
}

#endif

}  // namespace mso::media::detail
