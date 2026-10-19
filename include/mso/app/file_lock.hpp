#pragma once
/// @file
/// Exclusive inter-process advisory lock on a file: `LockFileEx` on Windows, `flock` on POSIX.
/// The operating system drops the lock when the handle closes or the process dies, so a crash
/// never leaves a stale lock behind. Used for the per-library `library.lock` (one writing process
/// per library: the desktop app and the CLI can never both write a catalog) and for
/// `workspace.lock` (serialized read-merge-write of workspace.json).
///
/// The lock is per handle, not per process: a second acquisition of the same file inside one
/// process fails too, exactly as it would from another process.
#include <chrono>
#include <filesystem>

#include "mso/domain/error.hpp"
#include "mso/worker/worker_process.hpp"

namespace mso::app {

/// An exclusive advisory lock on one file, held until the object is destroyed (see the file comment).
class FileLock {
public:
    /// File name of the per-library lock inside a library folder.
    static constexpr std::string_view kLibraryLockFileName = "library.lock";

    /// Creates `file` when absent and locks it without waiting. DB_BUSY (retryable) when another
    /// handle holds the lock; ACCESS_DENIED or FILE_MISSING when the file cannot be opened.
    [[nodiscard]] static Result<FileLock> try_acquire(const std::filesystem::path& file);

    /// Like try_acquire(), retrying every few milliseconds until `timeout` has elapsed.
    [[nodiscard]] static Result<FileLock> acquire(const std::filesystem::path& file, std::chrono::milliseconds timeout);

    FileLock() = default;
    FileLock(FileLock&&) noexcept = default;
    FileLock& operator=(FileLock&&) noexcept = default;
    FileLock(const FileLock&) = delete;
    FileLock& operator=(const FileLock&) = delete;
    /// Releases the lock (closing the handle releases it).
    ~FileLock() = default;

    /// True while this object holds a lock.
    bool held() const noexcept { return handle_.valid(); }

private:
    explicit FileLock(worker::UniqueHandle handle) noexcept : handle_(std::move(handle)) {}
    worker::UniqueHandle handle_;
};

}  // namespace mso::app
