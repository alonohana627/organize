#pragma once
/// @file
/// Bounded pool of read-only connections of one library. Shared (std::shared_ptr) between the
/// Library and every outstanding ReaderLease, so a lease that outlives its Library stays valid and
/// simply closes its connection when it ends. Internal to the catalog module.
#include <chrono>
#include <condition_variable>
#include <filesystem>
#include <memory>
#include <mutex>
#include <vector>

#include "mso/catalog/sqlite.hpp"

namespace mso::catalog::detail {

/// Thread-safe pool of reader connections (at most `maximum_readers` open at once).
class ReaderPool {
public:
    ReaderPool(std::filesystem::path catalog_file, int maximum_readers);
    ReaderPool(const ReaderPool&) = delete;
    ReaderPool& operator=(const ReaderPool&) = delete;

    /// Takes an idle connection or opens a new one. Waits at most `timeout` while every reader is
    /// leased (RESOURCE_LIMIT, retryable, afterwards); NOT_FOUND once the pool is closed.
    [[nodiscard]] Result<std::unique_ptr<sqlite::Connection>> acquire(std::chrono::milliseconds timeout);
    /// Returns a connection (its read transaction rolled back); destroys it once the pool is closed.
    void give_back(std::unique_ptr<sqlite::Connection> connection) noexcept;
    /// Closes idle connections and refuses further leases; leased connections close when returned.
    void close() noexcept;

private:
    const std::filesystem::path catalog_file_;
    const int maximum_readers_;
    std::mutex mutex_;
    std::condition_variable reader_available_;
    std::vector<std::unique_ptr<sqlite::Connection>> idle_;
    int open_readers_ = 0;
    bool closed_ = false;
};

}  // namespace mso::catalog::detail
