#pragma once
/// @file
/// Minimal read-only file handle with positional reads. On Windows it opens through the wide
/// CreateFileW API (Unicode and extended-length paths, sharing read/write/delete so probing and
/// preview never stop other applications from renaming, moving or deleting the file); elsewhere it
/// uses POSIX open/pread and never blocks on FIFOs or devices.
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <span>
#include <utility>

#include "mso/domain/error.hpp"

#ifdef _WIN32
#include "mso/media/detail/native_file_support.hpp"
#endif

namespace mso::media::detail {

/// Owning, move-only handle to a regular file opened for reading.
class FileHandle {
public:
    /// Opens `file` for reading. Only regular files are accepted.
    /// Errors: FileMissing, AccessDenied (retryable for sharing violations), UnsupportedCodec when
    /// the path is a directory, FIFO, device or other non-regular file.
    static Result<FileHandle> open_for_read(const std::filesystem::path& file);

    FileHandle(FileHandle&& other) noexcept;
    FileHandle& operator=(FileHandle&& other) noexcept;
    FileHandle(const FileHandle&) = delete;
    FileHandle& operator=(const FileHandle&) = delete;
    ~FileHandle();

    /// Current length of the file in bytes.
    Result<std::uint64_t> size() const;

    /// Reads up to destination.size() bytes starting at `offset`. Returns the number of bytes
    /// read, which is smaller than requested only at end of file. Positional: concurrent reads
    /// through the same handle do not disturb each other.
    Result<std::size_t> read_at(std::uint64_t offset, std::span<std::uint8_t> destination) const;

    /// Transfers the open file to a C stdio stream positioned at the start (for libraries that
    /// only accept a FILE*). The stream keeps the same sharing mode; the caller must fclose() it.
    /// This handle is empty afterwards, whether or not the transfer succeeds.
    /// Errors: ResourceLimit when the C runtime cannot wrap the handle.
    Result<std::FILE*> release_to_stdio();

private:
#ifdef _WIN32
    explicit FileHandle(UniqueWin32Handle handle) noexcept : handle_(std::move(handle)) {}
    UniqueWin32Handle handle_;
#else
    explicit FileHandle(int descriptor) noexcept : descriptor_(descriptor) {}
    void close() noexcept;
    int descriptor_ = -1;
#endif
};

}  // namespace mso::media::detail
