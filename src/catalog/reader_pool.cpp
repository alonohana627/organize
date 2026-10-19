#include "mso/catalog/detail/reader_pool.hpp"

#include <algorithm>
#include <format>

namespace mso::catalog::detail {

namespace {
constexpr int kReaderPageCacheKib = 8 * 1024;
}

ReaderPool::ReaderPool(std::filesystem::path catalog_file, int maximum_readers)
    : catalog_file_(std::move(catalog_file)), maximum_readers_(std::max(1, maximum_readers)) {}

Result<std::unique_ptr<sqlite::Connection>> ReaderPool::acquire(std::chrono::milliseconds timeout) {
    std::unique_lock lock(mutex_);
    const bool available = reader_available_.wait_for(
        lock, timeout, [&] { return closed_ || !idle_.empty() || open_readers_ < maximum_readers_; });
    if (closed_) return fail(ErrorCode::NotFound, "the library has been closed");
    if (!available)
        return fail(ErrorCode::ResourceLimit,
                    std::format("all {} catalog readers stayed busy for {} ms", maximum_readers_, timeout.count()),
                    /*retryable=*/true);
    if (!idle_.empty()) {
        auto connection = std::move(idle_.back());
        idle_.pop_back();
        return connection;
    }
    ++open_readers_;
    lock.unlock();
    auto connection =
        sqlite::Connection::open(catalog_file_, {.read_only = true, .page_cache_kib = kReaderPageCacheKib});
    if (!connection) {
        const std::lock_guard relock(mutex_);
        --open_readers_;
        reader_available_.notify_one();
        return std::unexpected(connection.error());
    }
    return std::make_unique<sqlite::Connection>(std::move(*connection));
}

void ReaderPool::give_back(std::unique_ptr<sqlite::Connection> connection) noexcept {
    if (!connection) return;
    connection->rollback();  // never return a connection with an open read transaction
    const std::lock_guard lock(mutex_);
    if (closed_) {
        --open_readers_;
        return;  // `connection` closes as it goes out of scope
    }
    try {
        idle_.push_back(std::move(connection));
    } catch (const std::bad_alloc&) {
        --open_readers_;  // dropping a connection only costs a reopen later
    }
    reader_available_.notify_one();
}

void ReaderPool::close() noexcept {
    std::vector<std::unique_ptr<sqlite::Connection>> closing;
    {
        const std::lock_guard lock(mutex_);
        closed_ = true;
        open_readers_ -= static_cast<int>(idle_.size());
        closing.swap(idle_);
    }
    reader_available_.notify_all();
}

}  // namespace mso::catalog::detail
