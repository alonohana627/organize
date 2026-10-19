#pragma once
// Thin RAII wrapper over SQLite. A Connection is thread-confined: it is used by one thread at a
// time and never shared concurrently. No raw sqlite3 handle escapes the catalog module's public API.
#include <cstdint>
#include <filesystem>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

#include "mso/domain/error.hpp"

struct sqlite3;
struct sqlite3_stmt;

namespace mso::sqlite {

/// Idle compiled statements of one connection, keyed by SQL text. Re-preparing the same SQL on every
/// call dominated catalog write time; statements are now reset and returned here for reuse. Shared
/// with outstanding PreparedStatements so either side may be destroyed first.
class StatementCache {
public:
    StatementCache() = default;
    StatementCache(const StatementCache&) = delete;
    StatementCache& operator=(const StatementCache&) = delete;
    ~StatementCache() { clear(); }

    /// Takes an idle statement for `sql`, or nullptr when none is cached.
    sqlite3_stmt* take(std::string_view sql);
    /// Returns a reset statement for reuse (finalized instead once the connection has closed).
    void give_back(std::string sql, sqlite3_stmt* statement);
    /// Finalizes every idle statement; later returns are finalized immediately.
    void clear() noexcept;

private:
    std::unordered_multimap<std::string, sqlite3_stmt*> idle_;
    bool closed_ = false;
    static constexpr std::size_t kMaximumIdleStatements = 256;
};

// A compiled SQL statement. Parameters are 1-based, result columns are 0-based (SQLite convention).
class PreparedStatement {
public:
    PreparedStatement() = default;
    PreparedStatement(sqlite3* connection, sqlite3_stmt* statement, std::shared_ptr<StatementCache> cache = {},
                      std::string sql = {})
        : connection_(connection), statement_(statement), cache_(std::move(cache)), sql_(std::move(sql)) {}
    PreparedStatement(PreparedStatement&& other) noexcept;
    PreparedStatement& operator=(PreparedStatement&& other) noexcept;
    PreparedStatement(const PreparedStatement&) = delete;
    PreparedStatement& operator=(const PreparedStatement&) = delete;
    ~PreparedStatement();

    PreparedStatement& bind(int parameter, std::int64_t value);
    PreparedStatement& bind(int parameter, int value) { return bind(parameter, static_cast<std::int64_t>(value)); }
    PreparedStatement& bind(int parameter, double value);
    PreparedStatement& bind(int parameter, std::string_view value);
    PreparedStatement& bind(int parameter, const char* value) { return bind(parameter, std::string_view(value)); }
    PreparedStatement& bind(int parameter, const std::string& value) {
        return bind(parameter, std::string_view(value));
    }
    PreparedStatement& bind_blob(int parameter, const void* data, std::size_t size);
    PreparedStatement& bind_null(int parameter);
    template <class T>
    PreparedStatement& bind(int parameter, const std::optional<T>& value) {
        return value ? bind(parameter, *value) : bind_null(parameter);
    }

    // True when a result row is available, false when the statement has finished.
    Result<bool> next_row();
    // Runs to completion ignoring result rows, then resets so the statement can be reused.
    Result<void> run();
    void reset();

    bool column_is_null(int column) const;
    std::int64_t column_int64(int column) const;
    double column_double(int column) const;
    std::string column_text(int column) const;
    std::vector<std::uint8_t> column_blob(int column) const;
    std::optional<std::int64_t> column_optional_int64(int column) const;
    std::optional<double> column_optional_double(int column) const;
    std::optional<std::string> column_optional_text(int column) const;
    int column_count() const;

private:
    void release() noexcept;  ///< returns the statement to the cache, or finalizes it

    sqlite3* connection_ = nullptr;
    sqlite3_stmt* statement_ = nullptr;
    std::shared_ptr<StatementCache> cache_;  ///< null for uncached statements
    std::string sql_;
};

struct ConnectionOptions {
    bool create = false;
    bool read_only = false;
    int busy_timeout_ms = 2000;
    int page_cache_kib = 16 * 1024;  // per connection; drawn from the global catalog memory budget
    bool write_ahead_log = true;
};

class Connection {
public:
    Connection() = default;
    Connection(Connection&& other) noexcept : handle_(other.handle_), cache_(std::move(other.cache_)) {
        other.handle_ = nullptr;
        other.cache_ = std::make_shared<StatementCache>();
    }
    Connection& operator=(Connection&& other) noexcept;
    Connection(const Connection&) = delete;
    Connection& operator=(const Connection&) = delete;
    ~Connection();

    static Result<Connection> open(const std::filesystem::path& file, const ConnectionOptions& options);
    bool is_open() const { return handle_ != nullptr; }
    void close() noexcept;

    /// Compiles (or reuses a cached compilation of) one statement.
    Result<PreparedStatement> prepare(std::string_view sql);
    Result<void> execute_script(std::string_view sql);       // multi-statement, no parameters
    Result<std::int64_t> query_int64(std::string_view sql);  // first column of the first row
    std::int64_t last_insert_rowid() const;
    int changed_row_count() const;

    // BEGIN IMMEDIATE with bounded jittered retry; DB_BUSY (retryable) once the deadline passes.
    Result<void> begin_immediate(int deadline_ms = 2000);
    Result<void> commit();
    void rollback() noexcept;
    bool in_transaction() const;

    Result<std::string> integrity_check();  // "ok" when healthy
    Result<std::vector<std::string>> foreign_key_violations();
    // Online backup API: copies a consistent snapshot into a standalone (non-WAL) file.
    Result<void> backup_to(const std::filesystem::path& destination, int pages_per_step = 256);

private:
    sqlite3* handle_ = nullptr;
    std::shared_ptr<StatementCache> cache_ = std::make_shared<StatementCache>();
};

// RAII write transaction: rolls back unless commit() succeeded.
class WriteTransaction {
public:
    explicit WriteTransaction(Connection& connection) : connection_(connection) {}
    WriteTransaction(const WriteTransaction&) = delete;
    WriteTransaction& operator=(const WriteTransaction&) = delete;
    ~WriteTransaction() {
        if (active_) connection_.rollback();
    }
    Result<void> begin();
    Result<void> commit();

private:
    Connection& connection_;
    bool active_ = false;
};

Error make_sqlite_error(sqlite3* connection, int result_code, std::string_view operation);
std::string library_version();
bool compiled_with(std::string_view option);  // e.g. "ENABLE_FTS5"

}  // namespace mso::sqlite
