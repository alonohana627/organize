#include "mso/catalog/durable_file.hpp"

#include <algorithm>
#include <format>
#include <memory>
#include <type_traits>

#include "mso/domain/core.hpp"

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#else
#include <fcntl.h>
#include <unistd.h>
#include <cerrno>
#include <cstring>
#endif

namespace mso::catalog {

namespace {

#ifdef _WIN32

ErrorCode error_code_for(DWORD error) {
    switch (error) {
        case ERROR_DISK_FULL:
        case ERROR_HANDLE_DISK_FULL:
            return ErrorCode::DiskFull;
        case ERROR_FILE_NOT_FOUND:
        case ERROR_PATH_NOT_FOUND:
            return ErrorCode::NotFound;
        default:
            return ErrorCode::AccessDenied;
    }
}

std::unexpected<Error> windows_failure(std::string_view operation, const std::filesystem::path& file) {
    const DWORD error = GetLastError();
    return fail(error_code_for(error),
                std::format("{} {} failed (Win32 error {})", operation, path_to_utf8(file), error));
}

/// Closes a Win32 file handle when the owning unique_ptr ends.
struct HandleCloser {
    void operator()(HANDLE handle) const noexcept { CloseHandle(handle); }
};
using UniqueHandle = std::unique_ptr<std::remove_pointer_t<HANDLE>, HandleCloser>;

Result<void> write_and_flush(const std::filesystem::path& file, std::string_view contents) {
    UniqueHandle handle(
        CreateFileW(file.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_NEW, FILE_ATTRIBUTE_NORMAL, nullptr));
    if (handle.get() == INVALID_HANDLE_VALUE) {
        handle.release();  // nothing to close
        return windows_failure("create", file);
    }
    std::size_t written_total = 0;
    while (written_total < contents.size()) {
        const auto remaining = contents.size() - written_total;
        const auto chunk = static_cast<DWORD>(std::min<std::size_t>(remaining, 1u << 30));
        DWORD written = 0;
        if (!WriteFile(handle.get(), contents.data() + written_total, chunk, &written, nullptr))
            return windows_failure("write", file);
        written_total += written;
    }
    if (!FlushFileBuffers(handle.get())) return windows_failure("flush", file);
    return {};
}

Result<void> replace_durably(const std::filesystem::path& temporary, const std::filesystem::path& target) {
    if (!MoveFileExW(temporary.c_str(), target.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH))
        return windows_failure("replace", target);
    return {};
}

void remove_quietly(const std::filesystem::path& file) noexcept {
    DeleteFileW(file.c_str());
}

#else

ErrorCode error_code_for(int error) {
    switch (error) {
        case ENOSPC:
        case EDQUOT:
            return ErrorCode::DiskFull;
        case ENOENT:
            return ErrorCode::NotFound;
        default:
            return ErrorCode::AccessDenied;
    }
}

std::unexpected<Error> posix_failure(std::string_view operation, const std::filesystem::path& file) {
    const int error = errno;
    return fail(error_code_for(error),
                std::format("{} {} failed: {}", operation, path_to_utf8(file), std::strerror(error)));
}

/// Closes a POSIX file descriptor when the owning scope ends.
class UniqueDescriptor {
public:
    explicit UniqueDescriptor(int descriptor) : descriptor_(descriptor) {}
    ~UniqueDescriptor() {
        if (descriptor_ >= 0) ::close(descriptor_);
    }
    UniqueDescriptor(const UniqueDescriptor&) = delete;
    UniqueDescriptor& operator=(const UniqueDescriptor&) = delete;
    int get() const { return descriptor_; }

private:
    int descriptor_;
};

Result<void> write_and_flush(const std::filesystem::path& file, std::string_view contents) {
    const UniqueDescriptor descriptor(::open(file.c_str(), O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC, 0644));
    if (descriptor.get() < 0) return posix_failure("create", file);
    std::size_t written_total = 0;
    while (written_total < contents.size()) {
        const auto written =
            ::write(descriptor.get(), contents.data() + written_total, contents.size() - written_total);
        if (written < 0) {
            if (errno == EINTR) continue;
            return posix_failure("write", file);
        }
        written_total += static_cast<std::size_t>(written);
    }
    if (::fsync(descriptor.get()) != 0) return posix_failure("fsync", file);
    return {};
}

Result<void> replace_durably(const std::filesystem::path& temporary, const std::filesystem::path& target) {
    if (::rename(temporary.c_str(), target.c_str()) != 0) return posix_failure("replace", target);
    // The rename is durable only once the directory entry is flushed too.
    auto directory = target.parent_path();
    if (directory.empty()) directory = ".";
    const UniqueDescriptor directory_descriptor(::open(directory.c_str(), O_RDONLY | O_DIRECTORY | O_CLOEXEC));
    if (directory_descriptor.get() < 0) return posix_failure("open directory", directory);
    if (::fsync(directory_descriptor.get()) != 0) return posix_failure("fsync directory", directory);
    return {};
}

void remove_quietly(const std::filesystem::path& file) noexcept {
    ::unlink(file.c_str());
}

#endif

}  // namespace

Result<void> write_file_atomically(const std::filesystem::path& target, std::string_view contents) {
    auto temporary = target;
    temporary += std::format(".{}.tmp", uuids::to_string(new_uuid()));
    if (auto written = write_and_flush(temporary, contents); !written) {
        remove_quietly(temporary);
        return written;
    }
    if (auto replaced = replace_durably(temporary, target); !replaced) {
        remove_quietly(temporary);
        return replaced;
    }
    return {};
}

}  // namespace mso::catalog
