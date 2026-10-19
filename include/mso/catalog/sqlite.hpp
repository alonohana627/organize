#pragma once
/// @file
/// Thin RAII wrapper over SQLite. A Connection is thread-confined: it is used by one thread at a
/// time and never shared concurrently. No raw sqlite3 handle escapes the catalog module's public API.
#include <chrono>
#include <concepts>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <memory>
#include <optional>
#include <stop_token>
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
/// outstanding PreparedStatements so either side may be destroyed first. A cached statement travels
/// out and back as its map node (SQL key included), so a cache hit allocates nothing.
class StatementCache {
    /// Transparent hash so lookups by string_view do not allocate a std::string.
    struct TextHash {
        using is_transparent = void;
        std::size_t operator()(std::string_view text) const noexcept { return std::hash<std::string_view>{}(text); }
    };
    using IdleStatements = std::unordered_multimap<std::string, sqlite3_stmt*, TextHash, std::equal_to<>>;

public:
    /// An idle statement taken out of the cache together with its SQL key; empty when none was cached.
    using Entry = IdleStatements::node_type;

    StatementCache() = default;
    StatementCache(const StatementCache&) = delete;
    StatementCache& operator=(const StatementCache&) = delete;
    ~StatementCache() { clear(); }

    /// Takes an idle statement for `sql` (an empty entry when none is cached). Never allocates.
    [[nodiscard]] Entry take(std::string_view sql);
    /// Returns a statement taken with take() after resetting it and clearing its bindings; finalizes
    /// it instead once the connection has closed or the cache is full. Reuses the entry's node.
    void give_back(Entry entry) noexcept;
    /// Returns a freshly compiled statement for its first reuse (allocates its node once).
    void give_back(std::string sql, sqlite3_stmt* statement) noexcept;
    /// Finalizes every idle statement; later returns are finalized immediately.
    void clear() noexcept;
    /// Number of idle statements currently cached (for tests and diagnostics).
    [[nodiscard]] std::size_t idle_count() const noexcept { return idle_.size(); }
    /// SQL text of every idle cached statement, one entry per cached compilation (for query-plan
    /// tests and diagnostics).
    [[nodiscard]] std::vector<std::string> idle_sql() const;
    /// Most idle statements one connection keeps; later returns are finalized instead.
    static constexpr std::size_t kMaximumIdleStatements = 256;

private:
    /// Resets a returned statement; false (and finalized) when it must not be cached.
    [[nodiscard]] bool prepare_for_reuse(sqlite3_stmt* statement) noexcept;

    IdleStatements idle_;
    bool closed_ = false;
};

/// A compiled SQL statement. Parameters are 1-based, result columns are 0-based (SQLite convention).
/// Bind calls never fail on their own: the first bind error is remembered and returned by the next
/// next_row()/run(), so a wrong parameter index can never silently run with NULL.
class PreparedStatement {
public:
    PreparedStatement() = default;
    /// Wraps a freshly compiled statement; `cache` (optional) receives it back on destruction.
    PreparedStatement(sqlite3* connection, sqlite3_stmt* statement, std::shared_ptr<StatementCache> cache = {},
                      std::string sql = {})
        : connection_(connection), statement_(statement), cache_(std::move(cache)), sql_(std::move(sql)) {}
    /// Wraps a statement taken from `cache` (non-empty entry); it goes back on destruction.
    PreparedStatement(sqlite3* connection, StatementCache::Entry entry, std::shared_ptr<StatementCache> cache)
        : connection_(connection),
          statement_(entry.mapped()),
          cache_(std::move(cache)),
          cache_entry_(std::move(entry)) {}
    PreparedStatement(PreparedStatement&& other) noexcept;
    PreparedStatement& operator=(PreparedStatement&& other) noexcept;
    PreparedStatement(const PreparedStatement&) = delete;
    PreparedStatement& operator=(const PreparedStatement&) = delete;
    ~PreparedStatement();

    /// Binds a 64-bit integer.
    PreparedStatement& bind(int parameter, std::int64_t value);
    /// Binds any other integer type (int, bool, `long` vs `long long`, chrono counts) as a 64-bit
    /// integer. A template so that no integer type is ambiguous between the overloads: std::int64_t
    /// is `long` on Linux but `long long` on Windows, while chrono durations use `long long` on both.
    template <std::integral Integer>
        requires(!std::same_as<Integer, std::int64_t>)
    PreparedStatement& bind(int parameter, Integer value) {
        return bind(parameter, static_cast<std::int64_t>(value));
    }
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
    [[nodiscard]] Result<bool> next_row();
    /// Runs to completion ignoring result rows, then resets so the statement can be reused.
    [[nodiscard]] Result<void> run();
    /// Resets the statement and clears bindings (and any remembered bind error).
    void reset() noexcept;
    /// True when the last failed step was a UNIQUE or PRIMARY KEY constraint violation.
    [[nodiscard]] bool failed_on_unique_constraint() const noexcept;

    /// True when the column of the current row is NULL.
    [[nodiscard]] bool column_is_null(int column) const;
    /// Integer value of a column of the current row.
    [[nodiscard]] std::int64_t column_int64(int column) const;
    /// Floating-point value of a column of the current row.
    [[nodiscard]] double column_double(int column) const;
    /// Text value (UTF-8) of a column of the current row; "" for NULL.
    [[nodiscard]] std::string column_text(int column) const;
    /// Blob bytes of a column of the current row; empty for NULL.
    [[nodiscard]] std::vector<std::uint8_t> column_blob(int column) const;
    /// Integer value, or nullopt for NULL.
    [[nodiscard]] std::optional<std::int64_t> column_optional_int64(int column) const;
    /// Floating-point value, or nullopt for NULL.
    [[nodiscard]] std::optional<double> column_optional_double(int column) const;
    /// Text value, or nullopt for NULL.
    [[nodiscard]] std::optional<std::string> column_optional_text(int column) const;
    /// Number of result columns.
    [[nodiscard]] int column_count() const;

private:
    void release() noexcept;  ///< returns the statement to the cache, or finalizes it
    void remember_bind_result(int result_code, int parameter) noexcept;

    sqlite3* connection_ = nullptr;
    sqlite3_stmt* statement_ = nullptr;
    std::shared_ptr<StatementCache> cache_;  ///< null for uncached statements
    std::string sql_;                        ///< SQL of a freshly compiled statement (first return to the cache)
    StatementCache::Entry cache_entry_;      ///< node of a statement taken from the cache (empty otherwise)
    int bind_error_code_ = 0;                ///< first failed bind's result code since the last reset (0 = none)
    int bind_error_parameter_ = 0;           ///< parameter index of that bind
    int last_step_error_code_ = 0;           ///< extended result code of the last failed step
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
    /// connection also gets the SQL functions `new_uuid()` (stduuid v4, canonical lowercase text),
    /// `fold_key(text)` and `path_key(text, case_sensitive)` (mso::fold_key and mso::path_key).
    [[nodiscard]] static Result<Connection> open(const std::filesystem::path& file, const ConnectionOptions& options);
    /// True while the connection is open.
    [[nodiscard]] bool is_open() const { return handle_ != nullptr; }
    /// Finalizes cached statements and closes the connection; idempotent.
    void close() noexcept;

    /// Compiles (or reuses a cached compilation of) one statement.
    [[nodiscard]] Result<PreparedStatement> prepare(std::string_view sql);
    /// Runs a multi-statement script without parameters (migrations, pragmas, fixed maintenance).
    [[nodiscard]] Result<void> execute_script(std::string_view sql);
    /// First column of the first row of a parameterless query; NOT_FOUND when there is no row.
    [[nodiscard]] Result<std::int64_t> query_int64(std::string_view sql);
    /// Rowid of the most recent successful INSERT on this connection.
    [[nodiscard]] std::int64_t last_insert_rowid() const;
    /// Rows changed by the most recent INSERT/UPDATE/DELETE on this connection.
    [[nodiscard]] int changed_row_count() const;

    /// BEGIN IMMEDIATE with bounded jittered retry until `deadline`; DB_BUSY (retryable) once it
    /// passes. SQLite's own busy handler is suspended meanwhile so the wait is not doubled.
    [[nodiscard]] Result<void> begin_immediate(std::chrono::milliseconds deadline = std::chrono::milliseconds(2000));
    /// Starts a deferred transaction (a consistent read snapshot from its first read on).
    [[nodiscard]] Result<void> begin_deferred();
    /// Commits the open transaction.
    [[nodiscard]] Result<void> commit();
    /// Rolls back the open transaction, if any.
    void rollback() noexcept;
    /// True while a transaction is open on this connection.
    [[nodiscard]] bool in_transaction() const;

    /// SQL text of the compiled statements idle in this connection's cache (query-plan tests and
    /// diagnostics: what this connection actually ran). At most StatementCache::kMaximumIdleStatements.
    [[nodiscard]] std::vector<std::string> cached_statement_sql() const;
    /// Number of compiled statements idle in this connection's cache.
    [[nodiscard]] std::size_t cached_statement_count() const noexcept { return cache_ ? cache_->idle_count() : 0; }

    /// PRAGMA integrity_check joined into one line: "ok" when healthy. A stop request interrupts the
    /// check within a few thousand virtual-machine steps (CANCELLED).
    [[nodiscard]] Result<std::string> integrity_check(std::stop_token stop = {});
    /// PRAGMA foreign_key_check rows, formatted for diagnostics. A stop request interrupts it as it
    /// does integrity_check (CANCELLED).
    [[nodiscard]] Result<std::vector<std::string>> foreign_key_violations(std::stop_token stop = {});
    /// Online backup API: copies a consistent snapshot into a standalone (non-WAL) file. Gives up
    /// with DB_BUSY when the source stays locked for longer than `busy_deadline`, and with CANCELLED
    /// when `stop` is requested (checked between steps of `pages_per_step` pages). On any failure the
    /// destination is left incomplete; the caller removes it. When this connection holds an open read
    /// transaction, every step copies that one snapshot, so writers on other connections never
    /// restart the copy.
    [[nodiscard]] Result<void> backup_to(const std::filesystem::path& destination, int pages_per_step = 256,
                                         std::chrono::milliseconds busy_deadline = std::chrono::seconds(30),
                                         std::stop_token stop = {});

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
    [[nodiscard]] Result<void> begin();
    /// Commits; on failure the destructor rolls back.
    [[nodiscard]] Result<void> commit();

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
    [[nodiscard]] Result<void> begin();

private:
    Connection& connection_;
    bool active_ = false;
};

/// Maps an SQLite result code to the project's error taxonomy (busy → DB_BUSY retryable, corrupt →
/// DB_CORRUPT, full → DISK_FULL, permission → ACCESS_DENIED, constraint → INVALID_ARGUMENT,
/// interrupt → CANCELLED).
Error make_sqlite_error(sqlite3* connection, int result_code, std::string_view operation);
/// Version string of the linked SQLite library.
std::string library_version();
/// True when SQLite was compiled with the option, e.g. "ENABLE_FTS5".
bool compiled_with(std::string_view option);

}  // namespace mso::sqlite
