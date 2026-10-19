#pragma once
/// @file
/// Minimal read-only file handle with positional reads. On Windows it opens through the wide
/// CreateFileW API (Unicode paths, sharing read/write/delete so probing and preview never block
/// other applications); elsewhere it uses POSIX open/pread.
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <span>

#include "mso/domain/error.hpp"

namespace mso::media::detail {

/// Owning, move-only handle to a regular file opened for reading.
class FileHandle {
public:
    /// Opens `file` for reading. Errors: FileMissing, AccessDenied (retryable for sharing
    /// violations), UnsupportedCodec when the path is a directory.
    static Result<FileHandle> open_for_read(const std::filesystem::path& file);

    FileHandle(FileHandle&& other) noexcept;
    FileHandle& operator=(FileHandle&& other) noexcept;
    FileHandle(const FileHandle&) = delete;
    FileHandle& operator=(const FileHandle&) = delete;
    ~FileHandle();

    /// Current length of the file in bytes.
    Result<std::uint64_t> size() const;

    /// Reads up to destination.size() bytes starting at `offset`. Returns the number of bytes
    /// read, which is smaller than requested only at end of file.
    Result<std::size_t> read_at(std::uint64_t offset, std::span<std::uint8_t> destination) const;

private:
#ifdef _WIN32
    explicit FileHandle(void* native_handle) noexcept : native_handle_(native_handle) {}
    void close() noexcept;
    void* native_handle_ = nullptr;
#else
    explicit FileHandle(int descriptor) noexcept : descriptor_(descriptor) {}
    void close() noexcept;
    int descriptor_ = -1;
#endif
};

}  // namespace mso::media::detail
