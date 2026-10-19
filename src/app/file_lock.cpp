#include "mso/app/file_lock.hpp"

#include <format>
#include <thread>

#ifdef _WIN32
#include <windows.h>
#else
#include <fcntl.h>
#include <sys/file.h>
#include <unistd.h>
#include <cerrno>
#include <cstring>
#endif

namespace mso::app {

namespace {

/// Pause between attempts of a waiting acquire(); the lock is held only for short file writes.
constexpr auto kRetryInterval = std::chrono::milliseconds(10);

/// DB_BUSY for a lock held elsewhere.
Error busy_error(const std::filesystem::path& file) {
    return Error{ErrorCode::DbBusy, true,
                 std::format("'{}' is locked by another Music Sample Organizer process", path_to_utf8(file)),
                 std::nullopt, std::nullopt};
}

}  // namespace

Result<FileLock> FileLock::try_acquire(const std::filesystem::path& file) {
#ifdef _WIN32
    // Share everything: the lock, not the sharing mode, provides exclusion, so other processes can
    // still open the file to discover that it is locked.
    worker::UniqueHandle handle(CreateFileW(file.c_str(), GENERIC_READ | GENERIC_WRITE,
                                            FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
                                            OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr));
    if (!handle) {
        const DWORD error = GetLastError();
        if (error == ERROR_PATH_NOT_FOUND) return fail(ErrorCode::FileMissing, "the folder does not exist");
        return fail(ErrorCode::AccessDenied, std::format("cannot open lock file '{}' (Windows error {})",
                                                         path_to_utf8(file), static_cast<unsigned long>(error)));
    }
    // One byte at offset 0 of an empty file: reading or copying the (empty) file is unaffected.
    OVERLAPPED region{};
    if (!LockFileEx(handle.get(), LOCKFILE_EXCLUSIVE_LOCK | LOCKFILE_FAIL_IMMEDIATELY, 0, 1, 0, &region)) {
        const DWORD error = GetLastError();
        if (error == ERROR_LOCK_VIOLATION || error == ERROR_IO_PENDING) return std::unexpected(busy_error(file));
        return fail(ErrorCode::AccessDenied, std::format("cannot lock '{}' (Windows error {})", path_to_utf8(file),
                                                         static_cast<unsigned long>(error)));
    }
    return FileLock(std::move(handle));
#else
    worker::UniqueHandle handle(::open(file.c_str(), O_RDWR | O_CREAT | O_CLOEXEC, 0644));
    if (!handle) {
        if (errno == ENOENT) return fail(ErrorCode::FileMissing, "the folder does not exist");
        return fail(ErrorCode::AccessDenied,
                    std::format("cannot open lock file '{}': {}", path_to_utf8(file), std::strerror(errno)));
    }
    while (::flock(handle.get(), LOCK_EX | LOCK_NB) != 0) {
        if (errno == EINTR) continue;
        if (errno == EWOULDBLOCK) return std::unexpected(busy_error(file));
        return fail(ErrorCode::AccessDenied,
                    std::format("cannot lock '{}': {}", path_to_utf8(file), std::strerror(errno)));
    }
    return FileLock(std::move(handle));
#endif
}

Result<FileLock> FileLock::acquire(const std::filesystem::path& file, std::chrono::milliseconds timeout) {
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    while (true) {
        auto lock = try_acquire(file);
        if (lock || lock.error().code != ErrorCode::DbBusy || std::chrono::steady_clock::now() >= deadline) return lock;
        std::this_thread::sleep_for(kRetryInterval);
    }
}

}  // namespace mso::app
