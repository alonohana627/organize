#pragma once
/// @file
/// The probe's only way to touch a file: every read is charged against a byte budget and checked
/// against a wall-clock deadline (docs/01_REQUIREMENTS.md section 5.2).
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <span>
#include <vector>

#include "mso/domain/error.hpp"
#include "mso/media/detail/file_handle.hpp"
#include "mso/media/media.hpp"

namespace mso::media::detail {

/// Random-access reader that refuses to exceed ProbeLimits.
///
/// A read that would pass the byte budget or start after the deadline fails with
/// ErrorCode::ProbeLimit without touching the file; probe_file() turns that into a partial result.
class BoundedFileReader {
public:
    /// Opens `file` and starts the deadline clock.
    static Result<BoundedFileReader> open(const std::filesystem::path& file, const ProbeLimits& limits);

    /// Length of the file when it was opened.
    std::uint64_t file_size() const noexcept { return file_size_; }
    /// Bytes charged against the budget so far.
    std::uint64_t bytes_read() const noexcept { return bytes_read_; }
    /// Bytes that may still be read before the budget is exhausted.
    std::uint64_t remaining_budget() const noexcept;

    /// Reads exactly `length` bytes at `offset`. Errors: ProbeLimit (budget or deadline),
    /// CorruptAudio when the range extends past the end of the file.
    Result<std::vector<std::uint8_t>> read_exact(std::uint64_t offset, std::size_t length);

    /// Reads up to `length` bytes at `offset`, stopping at the end of the file (an empty vector at
    /// or past it). Only the bytes the file actually holds there are charged, all or nothing.
    Result<std::vector<std::uint8_t>> read_available(std::uint64_t offset, std::size_t length);

    /// Like read_available() but also shrinks the request to the remaining budget, so it only
    /// fails when the budget is already exhausted. For optional data such as file-tail tags.
    Result<std::vector<std::uint8_t>> read_within_budget(std::uint64_t offset, std::size_t length);

    /// Fills `destination` from `offset` with whatever the file holds there (for library
    /// adapters that pull bytes through a callback). Returns the number of bytes copied.
    Result<std::size_t> read_into(std::uint64_t offset, std::span<std::uint8_t> destination);

private:
    BoundedFileReader(FileHandle file, std::uint64_t file_size, const ProbeLimits& limits);

    /// Reserves `length` bytes of budget; ProbeLimit when that is not possible.
    Result<void> charge(std::uint64_t length);

    FileHandle file_;
    std::uint64_t file_size_ = 0;
    std::uint64_t byte_budget_ = 0;
    std::uint64_t bytes_read_ = 0;
    std::chrono::steady_clock::time_point deadline_;
};

}  // namespace mso::media::detail
