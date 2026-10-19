#pragma once
/// @file
/// Thin RAII wrapper over SQLite. A Connection is thread-confined: it is used by one thread at a
/// time and never shared concurrently. No raw sqlite3 handle escapes the catalog module's public API.
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <functional>
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
/// call dominated catalog write time; statements are reset and returned here for reuse. Shared with
/// outstanding PreparedStatements so either side may be destroyed first.
class StatementCache {
public:
    StatementCache() = default;
    StatementCache(const StatementCache&) = delete;
    StatementCache& operator=(const StatementCache&) = delete;
    ~StatementCache() { clear(); }

    /// Takes an idle statement for `sql`, or nullptr when none is cached. Never allocates.
    sqlite3_stmt* take(std::string_view sql);
    /// Returns a statement for reuse after resetting it and clearing its bindings; finalizes it
    /// instead once the connection has closed or the cache is full.
    void give_back(std::string sql, sqlite3_stmt* statement) noexcept;
    /// Finalizes every idle statement; later returns are finalized immediately.
    void clear() noexcept;
    /// Number of idle statements currently cached (for tests and diagnostics).
    std::size_t idle_count() const noexcept { return idle_.size(); }

private:
    /// Transparent hash so lookups by string_view do not allocate a std::string.
    struct TextHash {
        using is_transparent = void;
        std::size_t operator()(std::string_view text) const noexcept { return std::hash<std::string_view>{}(text); }
    };

    std::unordered_multimap<std::string, sqlite3_stmt*, TextHash, std::equal_to<>> idle_;
    bool closed_ = false;
    static constexpr std::size_t kMaximumIdleStatements = 256;
};

/// A compiled SQL statement. Parameters are 1-based, result columns are 0-based (SQLite convention).
/// Bind calls never fail on their own: the first bind error is remembered and returned by the next
/// next_row()/run(), so a wrong parameter index can never silently run with NULL.
class PreparedStatement {
public:
    PreparedStatement() = default;
    /// Wraps a compiled statement; `cache` (optional) receives it back on destruction.
    PreparedStatement(sqlite3* connection, sqlite3_stmt* statement, std::shared_ptr<StatementCache> cache = {},
                      std::string sql = {})
        : connection_(connection), statement_(statement), cache_(std::move(cache)), sql_(std::move(sql)) {}
    PreparedStatement(PreparedStatement&& other) noexcept;
    PreparedStatement& operator=(PreparedStatement&& other) noexcept;
    PreparedStatement(const PreparedStatement&) = delete;
    PreparedStatement& operator=(const PreparedStatement&) = delete;
    ~PreparedStatement();

    /// Binds a 64-bit integer.
    PreparedStatement& bind(int parameter, std::int64_t value);
    /// Binds an int as a 64-bit integer.
    PreparedStatement& bind(int parameter, int value) { return bind(parameter, static_cast<std::int64_t>(value)); }
    /// Binds a double.
    PreparedStatement& bind(int parameter, double value);
    /// Binds UTF-8 text (copied by SQLite).
    PreparedStatement& bind(int parameter, std::string_view value);
    /// Binds a C string as UTF-8 text.
    PreparedStatement& bind(int parameter, const char* value) { return bind(parameter, std::string_view(value)); }
    /// Binds a std::string as UTF-8 text.
    PreparedStatement& bind(int parameter, const std::string& value) {
        return bind(parameter, std::string_view(value));
    }
    /// Binds a blob (copied by SQLite).
    PreparedStatement& bind_blob(int parameter, const void* data, std::size_t size);
    /// Binds SQL NULL.
    PreparedStatement& bind_null(int parameter);
    /// Binds the value, or NULL when the optional is empty.
    template <class T>
    PreparedStatement& bind(int parameter, const std::optional<T>& value) {
        return value ? bind(parameter, *value) : bind_null(parameter);
    }

    /// Steps once: true when a result row is available, false when the statement has finished.
    /// A pending bind error is reported here (and the statement reset) without stepping.
    Result<bool> next_row();
    /// Runs to completion ignoring result rows, then resets so the statement can be reused.
    Result<void> run();
    /// Resets the statement and clears bindings (and any remembered bind error).
    void reset() noexcept;
    /// True when the last failed step was a UNIQUE or PRIMARY KEY constraint violation.
    bool failed_on_unique_constraint() const noexcept;

    /// True when the column of the current row is NULL.
    bool column_is_null(int column) const;
    /// Integer value of a column of the current row.
    std::int64_t column_int64(int column) const;
    /// Floating-point value of a column of the current row.
    double column_double(int column) const;
    /// Text value (UTF-8) of a column of the current row; "" for NULL.
    std::string column_text(int column) const;
    /// Blob bytes of a column of the current row; empty for NULL.
    std::vector<std::uint8_t> column_blob(int column) const;
    /// Integer value, or nullopt for NULL.
    std::optional<std::int64_t> column_optional_int64(int column) const;
    /// Floating-point value, or nullopt for NULL.
    std::optional<double> column_optional_double(int column) const;
    /// Text value, or nullopt for NULL.
    std::optional<std::string> column_optional_text(int column) const;
    /// Number of result columns.
    int column_count() const;

private:
    void release() noexcept;  ///< returns the statement to the cache, or finalizes it
    void remember_bind_result(int result_code, int parameter) noexcept;

    sqlite3* connection_ = nullptr;
    sqlite3_stmt* statement_ = nullptr;
    std::shared_ptr<StatementCache> cache_;  ///< null for uncached statements
    std::string sql_;
    int bind_error_code_ = 0;       ///< first failed bind's result code since the last reset (0 = none)
    int bind_error_parameter_ = 0;  ///< parameter index of that bind
    int last_step_error_code_ = 0;  ///< extended result code of the last failed step
};

/// How a Connection is opened.
struct ConnectionOptions {
    bool create = false;             ///< create the file when it does not exist
    bool read_only = false;          ///< open read-only (no pragmas that write)
    int busy_timeout_ms = 2000;      ///< SQLite busy handler budget for ordinary statements
    int page_cache_kib = 16 * 1024;  ///< per connection; drawn from the global catalog memory budget
    bool write_ahead_log = true;     ///< WAL journal (writable connections only)
};

/// One SQLite database connection with its statement cache.
class Connection {
public:
    Connection() = default;
    Connection(Connection&& other) noexcept
        : handle_(other.handle_), cache_(std::move(other.cache_)), busy_timeout_ms_(other.busy_timeout_ms_) {
        other.handle_ = nullptr;
    }
    Connection& operator=(Connection&& other) noexcept;
    Connection(const Connection&) = delete;
    Connection& operator=(const Connection&) = delete;
    ~Connection();

    /// Opens (or creates) a database file and applies the catalog's connection pragmas. Every
    /// connection also gets the `new_uuid()` SQL function (stduuid v4, canonical lowercase text).
    static Result<Connection> open(const std::filesystem::path& file, const ConnectionOptions& options);
    /// True while the connection is open.
    bool is_open() const { return handle_ != nullptr; }
    /// Finalizes cached statements and closes the connection; idempotent.
    void close() noexcept;

    /// Compiles (or reuses a cached compilation of) one statement.
    Result<PreparedStatement> prepare(std::string_view sql);
    /// Runs a multi-statement script without parameters (migrations, pragmas, fixed maintenance).
    Result<void> execute_script(std::string_view sql);
    /// First column of the first row of a parameterless query; NOT_FOUND when there is no row.
    Result<std::int64_t> query_int64(std::string_view sql);
    /// Rowid of the most recent successful INSERT on this connection.
    std::int64_t last_insert_rowid() const;
    /// Rows changed by the most recent INSERT/UPDATE/DELETE on this connection.
    int changed_row_count() const;

    /// BEGIN IMMEDIATE with bounded jittered retry until `deadline`; DB_BUSY (retryable) once it
    /// passes. SQLite's own busy handler is suspended meanwhile so the wait is not doubled.
    Result<void> begin_immediate(std::chrono::milliseconds deadline = std::chrono::milliseconds(2000));
    /// Starts a deferred transaction (a consistent read snapshot from its first read on).
    Result<void> begin_deferred();
    /// Commits the open transaction.
    Result<void> commit();
    /// Rolls back the open transaction, if any.
    void rollback() noexcept;
    /// True while a transaction is open on this connection.
    bool in_transaction() const;

    /// PRAGMA integrity_check joined into one line: "ok" when healthy.
    Result<std::string> integrity_check();
    /// PRAGMA foreign_key_check rows, formatted for diagnostics.
    Result<std::vector<std::string>> foreign_key_violations();
    /// Online backup API: copies a consistent snapshot into a standalone (non-WAL) file. Gives up
    /// with DB_BUSY when the source stays locked for longer than `busy_deadline`.
    Result<void> backup_to(const std::filesystem::path& destination, int pages_per_step = 256,
                           std::chrono::milliseconds busy_deadline = std::chrono::seconds(30));

private:
    sqlite3* handle_ = nullptr;
    std::shared_ptr<StatementCache> cache_ = std::make_shared<StatementCache>();  ///< null once moved from
    int busy_timeout_ms_ = 2000;
};

/// RAII write transaction (BEGIN IMMEDIATE): rolls back unless commit() succeeded.
class WriteTransaction {
public:
    explicit WriteTransaction(Connection& connection) : connection_(connection) {}
    WriteTransaction(const WriteTransaction&) = delete;
    WriteTransaction& operator=(const WriteTransaction&) = delete;
    ~WriteTransaction() {
        if (active_) connection_.rollback();
    }
    /// Begins the transaction (bounded busy wait).
    Result<void> begin();
    /// Commits; on failure the destructor rolls back.
    Result<void> commit();

private:
    Connection& connection_;
    bool active_ = false;
};

/// RAII read transaction (deferred BEGIN … COMMIT): every statement inside sees one WAL snapshot,
/// so multi-statement reads are never torn by a concurrent commit. Ends on destruction.
class ReadTransaction {
public:
    explicit ReadTransaction(Connection& connection) : connection_(connection) {}
    ReadTransaction(const ReadTransaction&) = delete;
    ReadTransaction& operator=(const ReadTransaction&) = delete;
    ~ReadTransaction() {
        if (active_) connection_.rollback();
    }
    /// Begins the snapshot transaction.
    Result<void> begin();

private:
    Connection& connection_;
    bool active_ = false;
};

/// Maps an SQLite result code to the project's error taxonomy (busy → DB_BUSY retryable, corrupt →
/// DB_CORRUPT, full → DISK_FULL, permission → ACCESS_DENIED, constraint → INVALID_ARGUMENT).
Error make_sqlite_error(sqlite3* connection, int result_code, std::string_view operation);
/// Version string of the linked SQLite library.
std::string library_version();
/// True when SQLite was compiled with the option, e.g. "ENABLE_FTS5".
bool compiled_with(std::string_view option);

}  // namespace mso::sqlite
