// BoundedFileReader: budgeted, deadline-checked reads from a file (through a read-ahead window) or memory.
#include "mso/media/detail/bounded_reader.hpp"

#include <algorithm>
#include <cstring>
#include <utility>

namespace mso::media::detail {

Result<BoundedFileReader> BoundedFileReader::open(const std::filesystem::path& file, const ProbeLimits& limits) {
    auto handle = FileHandle::open_for_read(file);
    if (!handle) return std::unexpected(handle.error());
    const auto size = handle->size();
    if (!size) return std::unexpected(size.error());
    return BoundedFileReader(std::move(*handle), {}, *size, limits);
}

BoundedFileReader BoundedFileReader::over_memory(std::span<const std::uint8_t> bytes, const ProbeLimits& limits) {
    return BoundedFileReader(std::nullopt, bytes, bytes.size(), limits);
}

BoundedFileReader::BoundedFileReader(std::optional<FileHandle> file, std::span<const std::uint8_t> memory,
                                     std::uint64_t file_size, const ProbeLimits& limits)
    : file_(std::move(file)),
      memory_(memory),
      file_size_(file_size),
      byte_budget_(limits.max_bytes),
      deadline_(std::chrono::steady_clock::now() + limits.max_time) {}

std::uint64_t BoundedFileReader::remaining_budget() const noexcept {
    return byte_budget_ > bytes_read_ ? byte_budget_ - bytes_read_ : 0;
}

Result<void> BoundedFileReader::charge(std::uint64_t length) {
    if (std::chrono::steady_clock::now() >= deadline_) return fail(ErrorCode::ProbeLimit, "probe time limit reached");
    if (length > remaining_budget()) return fail(ErrorCode::ProbeLimit, "probe byte budget reached");
    bytes_read_ += length;
    return {};
}

Result<std::size_t> BoundedFileReader::fetch(std::uint64_t offset, std::span<std::uint8_t> destination) {
    if (destination.empty()) return std::size_t{0};
    if (!file_) {
        // Every caller clamps its range to file_size_, which is the buffer's size.
        std::memcpy(destination.data(), memory_.data() + offset, destination.size());
        return destination.size();
    }
    const bool in_window = offset >= window_offset_ && offset - window_offset_ <= window_.size() &&
                           window_.size() - (offset - window_offset_) >= destination.size();
    if (!in_window) {
        if (destination.size() >= kReadAheadBytes) return file_->read_at(offset, destination);  // large: direct
        // Refill with the request plus read-ahead, never past the end of the file and never more
        // than the parser could still be given (the request itself is already charged).
        const std::uint64_t still_affordable = remaining_budget() + destination.size();
        const auto window_length = static_cast<std::size_t>(
            std::min({static_cast<std::uint64_t>(kReadAheadBytes), file_size_ - offset, still_affordable}));
        window_.resize(window_length);
        const auto received = file_->read_at(offset, window_);
        if (!received) {
            window_.clear();
            return std::unexpected(received.error());
        }
        window_.resize(*received);
        window_offset_ = offset;
    }
    const auto start = static_cast<std::size_t>(offset - window_offset_);
    const std::size_t copied = std::min(destination.size(), window_.size() - start);
    std::memcpy(destination.data(), window_.data() + start, copied);
    return copied;
}

Result<std::vector<std::uint8_t>> BoundedFileReader::read_exact(std::uint64_t offset, std::size_t length) {
    if (offset > file_size_ || file_size_ - offset < length) {
        return fail(ErrorCode::CorruptAudio, "file is truncated inside a header");
    }
    return read_available(offset, length);
}

Result<std::vector<std::uint8_t>> BoundedFileReader::read_available(std::uint64_t offset, std::size_t length) {
    const std::uint64_t available = offset < file_size_ ? file_size_ - offset : 0;
    const auto present = static_cast<std::size_t>(std::min<std::uint64_t>(length, available));
    if (auto charged = charge(present); !charged) return std::unexpected(charged.error());
    std::vector<std::uint8_t> bytes(present);
    const auto received = fetch(offset, bytes);
    if (!received) return std::unexpected(received.error());
    bytes.resize(*received);
    return bytes;
}

Result<std::vector<std::uint8_t>> BoundedFileReader::read_within_budget(std::uint64_t offset, std::size_t length) {
    const std::uint64_t available = offset < file_size_ ? file_size_ - offset : 0;
    const std::uint64_t affordable = std::min({static_cast<std::uint64_t>(length), available, remaining_budget()});
    if (affordable == 0 && length > 0 && available > 0) return fail(ErrorCode::ProbeLimit, "probe byte budget reached");
    return read_available(offset, static_cast<std::size_t>(affordable));
}

Result<std::size_t> BoundedFileReader::read_into(std::uint64_t offset, std::span<std::uint8_t> destination) {
    const std::uint64_t available = offset < file_size_ ? file_size_ - offset : 0;
    const auto length = static_cast<std::size_t>(std::min<std::uint64_t>(destination.size(), available));
    if (auto charged = charge(length); !charged) return std::unexpected(charged.error());
    return fetch(offset, destination.first(length));
}

}  // namespace mso::media::detail
