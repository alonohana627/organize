#include "mso/media/detail/file_handle.hpp"

#include <algorithm>
#include <cstdint>
#include <format>
#include <limits>
#include <system_error>
#include <utility>

#include "mso/media/detail/native_file_support.hpp"

#ifdef _WIN32
#include <fcntl.h>
#include <io.h>
#include <windows.h>
#else
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>
#include <cerrno>
#endif

namespace mso::media::detail {

#ifdef _WIN32

namespace {

/// Directories cannot be opened as files; report them as "not audio" rather than access denied.
bool path_is_directory(const std::filesystem::path& file) {
    std::error_code ignored;
    return std::filesystem::is_directory(native_path(file), ignored);
}

}  // namespace

Result<FileHandle> FileHandle::open_for_read(const std::filesystem::path& file) {
    UniqueWin32Handle handle(CreateFileW(native_path(file).c_str(), GENERIC_READ,
                                         FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING,
                                         FILE_ATTRIBUTE_NORMAL, nullptr));
    if (!handle.valid()) {
        const DWORD error = GetLastError();
        if (error == ERROR_ACCESS_DENIED && path_is_directory(file)) {
            return fail(ErrorCode::UnsupportedCodec, "path is a directory");
        }
        return std::unexpected(error_from_win32(error, "opening the file"));
    }
    // Pipes and character devices (\\.\pipe\..., CON) open fine but are not files to probe.
    if (GetFileType(handle.get()) != FILE_TYPE_DISK) return fail(ErrorCode::UnsupportedCodec, "not a regular file");
    return FileHandle(std::move(handle));
}

FileHandle::FileHandle(FileHandle&& other) noexcept = default;

FileHandle& FileHandle::operator=(FileHandle&& other) noexcept = default;

FileHandle::~FileHandle() = default;

Result<std::uint64_t> FileHandle::size() const {
    LARGE_INTEGER length{};
    if (!GetFileSizeEx(handle_.get(), &length)) {
        return std::unexpected(error_from_win32(GetLastError(), "reading the file size"));
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
        if (!ReadFile(handle_.get(), destination.data() + total, request, &received, &overlapped)) {
            const DWORD error = GetLastError();
            if (error == ERROR_HANDLE_EOF) break;
            return fail(ErrorCode::AccessDenied, std::format("read failed (Win32 error {})", error), true);
        }
        if (received == 0) break;
        total += received;
    }
    return total;
}

Result<std::FILE*> FileHandle::release_to_stdio() {
    void* const native = handle_.release();
    if (native == nullptr) return fail(ErrorCode::Internal, "file handle is empty");
    const int descriptor = _open_osfhandle(reinterpret_cast<std::intptr_t>(native), _O_RDONLY | _O_BINARY);
    if (descriptor < 0) {
        CloseHandle(native);  // not adopted by the C runtime, so still ours to close
        return fail(ErrorCode::ResourceLimit, "cannot wrap the file handle in a C runtime descriptor");
    }
    std::FILE* stream = _fdopen(descriptor, "rb");
    if (stream == nullptr) {
        _close(descriptor);  // also closes the Win32 handle
        return fail(ErrorCode::ResourceLimit, "cannot open a C stdio stream over the file");
    }
    std::rewind(stream);  // positional reads moved the shared file pointer
    return stream;
}

#else

Result<FileHandle> FileHandle::open_for_read(const std::filesystem::path& file) {
    // O_NONBLOCK: opening a FIFO for reading would otherwise wait forever for a writer. It is
    // cleared again once fstat() shows a regular file (checked on the descriptor, not the path).
    const int descriptor = ::open(file.c_str(), O_RDONLY | O_CLOEXEC | O_NOCTTY | O_NONBLOCK);
    if (descriptor < 0) {
        if (errno == EISDIR) return fail(ErrorCode::UnsupportedCodec, "path is a directory");
        return std::unexpected(error_from_errno(errno, "opening the file"));
    }
    FileHandle handle(descriptor);
    struct stat status{};
    if (::fstat(descriptor, &status) != 0) return std::unexpected(error_from_errno(errno, "inspecting the file"));
    if (S_ISDIR(status.st_mode)) return fail(ErrorCode::UnsupportedCodec, "path is a directory");
    if (!S_ISREG(status.st_mode)) return fail(ErrorCode::UnsupportedCodec, "not a regular file");
    const int flags = ::fcntl(descriptor, F_GETFL);
    if (flags < 0 || ::fcntl(descriptor, F_SETFL, flags & ~O_NONBLOCK) != 0) {
        return std::unexpected(error_from_errno(errno, "configuring the file"));
    }
    return handle;
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
    if (::fstat(descriptor_, &status) != 0) return std::unexpected(error_from_errno(errno, "reading the file size"));
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

Result<std::FILE*> FileHandle::release_to_stdio() {
    const int descriptor = std::exchange(descriptor_, -1);
    if (descriptor < 0) return fail(ErrorCode::Internal, "file handle is empty");
    std::FILE* stream = ::fdopen(descriptor, "rb");
    if (stream == nullptr) {
        ::close(descriptor);  // fdopen did not adopt it
        return fail(ErrorCode::ResourceLimit, "cannot open a C stdio stream over the file");
    }
    std::rewind(stream);
    return stream;
}

#endif

}  // namespace mso::media::detail
