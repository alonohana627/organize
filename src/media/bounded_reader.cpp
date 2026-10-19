#include "mso/media/bounded_reader.hpp"

#include <algorithm>
#include <utility>

namespace mso::media::detail {

Result<BoundedFileReader> BoundedFileReader::open(const std::filesystem::path& file, const ProbeLimits& limits) {
    auto handle = FileHandle::open_for_read(file);
    if (!handle) return std::unexpected(handle.error());
    const auto size = handle->size();
    if (!size) return std::unexpected(size.error());
    return BoundedFileReader(std::move(*handle), *size, limits);
}

BoundedFileReader::BoundedFileReader(FileHandle file, std::uint64_t file_size, const ProbeLimits& limits)
    : file_(std::move(file)),
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
    const auto received = file_.read_at(offset, bytes);
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
    return file_.read_at(offset, destination.first(length));
}

}  // namespace mso::media::detail
