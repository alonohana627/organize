#pragma once
/// @file
/// The probe's only way to touch a file: every read is charged against a byte budget and checked
/// against a wall-clock deadline (docs/01_REQUIREMENTS.md section 5.2).
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <optional>
#include <span>
#include <vector>

#include "mso/domain/error.hpp"
#include "mso/media/detail/file_handle.hpp"
#include "mso/media/media.hpp"

namespace mso::media::detail {

/// Random-access reader that refuses to exceed ProbeLimits, over a file or over bytes in memory
/// (probe_memory(), used by the fuzz targets).
///
/// A read that would pass the byte budget or start after the deadline fails with
/// ErrorCode::ProbeLimit without touching the file; probe_file() turns that into a partial result.
///
/// Small reads from a file are served from a read-ahead window of kReadAheadBytes, so a parser
/// walking headers costs one system call per window instead of one per header (on a network share
/// each call is a round trip). The budget charges only the bytes handed to the parser; a window
/// never extends past the end of the file or beyond what the parser could still be given, and the
/// bytes read ahead (beyond each request) total at most the byte budget, so scattered small reads
/// cannot pull more than twice ProbeLimits::max_bytes off the disk.
class BoundedFileReader {
public:
    /// Size of the read-ahead window for reads smaller than it.
    static constexpr std::size_t kReadAheadBytes = 16 * 1024;

    /// Opens `file` and starts the deadline clock.
    [[nodiscard]] static Result<BoundedFileReader> open(const std::filesystem::path& file, const ProbeLimits& limits);
    /// Reads from `bytes`, which must outlive the reader, and starts the deadline clock.
    [[nodiscard]] static BoundedFileReader over_memory(std::span<const std::uint8_t> bytes, const ProbeLimits& limits);

    /// Length of the file (or buffer) when it was opened.
    [[nodiscard]] std::uint64_t file_size() const noexcept { return file_size_; }
    /// Bytes charged against the budget so far: those handed to the parsers.
    [[nodiscard]] std::uint64_t bytes_read() const noexcept { return bytes_read_; }
    /// Bytes read from the file so far, read-ahead included (0 over memory); at most twice the
    /// byte budget.
    [[nodiscard]] std::uint64_t bytes_fetched() const noexcept { return bytes_fetched_; }
    /// Bytes that may still be read before the budget is exhausted.
    [[nodiscard]] std::uint64_t remaining_budget() const noexcept;
    /// When reads start failing with ProbeLimit; also bounds work done between reads (FFmpeg analysis).
    [[nodiscard]] std::chrono::steady_clock::time_point deadline() const noexcept { return deadline_; }

    /// Reads exactly `length` bytes at `offset`. Errors: ProbeLimit (budget or deadline),
    /// CorruptAudio when the range extends past the end of the file.
    [[nodiscard]] Result<std::vector<std::uint8_t>> read_exact(std::uint64_t offset, std::size_t length);

    /// Reads up to `length` bytes at `offset`, stopping at the end of the file (an empty vector at
    /// or past it). Only the bytes the file actually holds there are charged, all or nothing.
    [[nodiscard]] Result<std::vector<std::uint8_t>> read_available(std::uint64_t offset, std::size_t length);

    /// Like read_available() but also shrinks the request to the remaining budget, so it only
    /// fails when the budget is already exhausted. For optional data such as file-tail tags.
    [[nodiscard]] Result<std::vector<std::uint8_t>> read_within_budget(std::uint64_t offset, std::size_t length);

    /// Fills `destination` from `offset` with whatever the file holds there (for library
    /// adapters that pull bytes through a callback). Returns the number of bytes copied.
    [[nodiscard]] Result<std::size_t> read_into(std::uint64_t offset, std::span<std::uint8_t> destination);

private:
    BoundedFileReader(std::optional<FileHandle> file, std::span<const std::uint8_t> memory, std::uint64_t file_size,
                      const ProbeLimits& limits);

    /// Reserves `length` bytes of budget; ProbeLimit when that is not possible.
    [[nodiscard]] Result<void> charge(std::uint64_t length);
    /// Copies the bytes at `offset` (already charged and inside the file) into `destination` from
    /// memory, the read-ahead window or the file. Returns the number copied, fewer only when the
    /// file shrank after it was opened.
    [[nodiscard]] Result<std::size_t> fetch(std::uint64_t offset, std::span<std::uint8_t> destination);

    std::optional<FileHandle> file_;        ///< the file; empty when reading from memory
    std::span<const std::uint8_t> memory_;  ///< the buffer when reading from memory
    std::uint64_t file_size_ = 0;
    std::uint64_t byte_budget_ = 0;
    std::uint64_t bytes_read_ = 0;
    std::chrono::steady_clock::time_point deadline_;
    std::vector<std::uint8_t> window_;  ///< read-ahead bytes of the file, starting at window_offset_
    std::uint64_t window_offset_ = 0;
    std::uint64_t read_ahead_bytes_ = 0;  ///< bytes read into windows beyond their requests, at most byte_budget_
    std::uint64_t bytes_fetched_ = 0;     ///< bytes read from the file, requests and read-ahead together
};

}  // namespace mso::media::detail
