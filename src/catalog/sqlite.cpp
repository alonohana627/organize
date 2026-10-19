#include "mso/catalog/sqlite.hpp"

#include <sqlite3.h>

#include <chrono>
#include <format>
#include <random>
#include <thread>

#include "mso/domain/core.hpp"

namespace mso::sqlite {

Error make_sqlite_error(sqlite3* connection, int result_code, std::string_view operation) {
    const int primary_code = result_code & 0xFF;
    ErrorCode code = ErrorCode::Internal;
    bool retryable = false;
    switch (primary_code) {
        case SQLITE_BUSY:
        case SQLITE_LOCKED:
            code = ErrorCode::DbBusy;
            retryable = true;
            break;
        case SQLITE_CORRUPT:
        case SQLITE_NOTADB:
            code = ErrorCode::DbCorrupt;
            break;
        case SQLITE_FULL:
            code = ErrorCode::DiskFull;
            break;
        case SQLITE_CANTOPEN:
        case SQLITE_PERM:
        case SQLITE_READONLY:
            code = ErrorCode::AccessDenied;
            break;
        case SQLITE_CONSTRAINT:
            code = ErrorCode::InvalidArgument;
            break;
        default:
            break;
    }
    const char* message = connection ? sqlite3_errmsg(connection) : sqlite3_errstr(result_code);
    return Error{code, retryable, std::format("{}: {} (rc={})", operation, message, result_code), {}, {}};
}

std::string library_version() {
    return sqlite3_libversion();
}

bool compiled_with(std::string_view option) {
    const std::string name(option);
    return sqlite3_compileoption_used(name.c_str()) != 0;
}

// ---------------------------------------------------------------- StatementCache
sqlite3_stmt* StatementCache::take(std::string_view sql) {
    const auto found = idle_.find(std::string(sql));
    if (found == idle_.end()) return nullptr;
    sqlite3_stmt* statement = found->second;
    idle_.erase(found);
    return statement;
}

void StatementCache::give_back(std::string sql, sqlite3_stmt* statement) {
    sqlite3_reset(statement);
    sqlite3_clear_bindings(statement);
    if (closed_ || idle_.size() >= kMaximumIdleStatements) {
        sqlite3_finalize(statement);
        return;
    }
    idle_.emplace(std::move(sql), statement);
}

void StatementCache::clear() noexcept {
    for (auto& [sql, statement] : idle_)
        sqlite3_finalize(statement);
    idle_.clear();
    closed_ = true;
}

// ---------------------------------------------------------------- PreparedStatement
PreparedStatement::PreparedStatement(PreparedStatement&& other) noexcept
    : connection_(other.connection_),
      statement_(other.statement_),
      cache_(std::move(other.cache_)),
      sql_(std::move(other.sql_)) {
    other.statement_ = nullptr;
}

PreparedStatement& PreparedStatement::operator=(PreparedStatement&& other) noexcept {
    if (this != &other) {
        release();
        connection_ = other.connection_;
        statement_ = other.statement_;
        cache_ = std::move(other.cache_);
        sql_ = std::move(other.sql_);
        other.statement_ = nullptr;
    }
    return *this;
}

PreparedStatement::~PreparedStatement() {
    release();
}

void PreparedStatement::release() noexcept {
    if (!statement_) return;
    if (cache_)
        cache_->give_back(std::move(sql_), statement_);
    else
        sqlite3_finalize(statement_);
    statement_ = nullptr;
}

PreparedStatement& PreparedStatement::bind(int parameter, std::int64_t value) {
    sqlite3_bind_int64(statement_, parameter, value);
    return *this;
}

PreparedStatement& PreparedStatement::bind(int parameter, double value) {
    sqlite3_bind_double(statement_, parameter, value);
    return *this;
}

PreparedStatement& PreparedStatement::bind(int parameter, std::string_view value) {
    sqlite3_bind_text64(statement_, parameter, value.data(), value.size(), SQLITE_TRANSIENT, SQLITE_UTF8);
    return *this;
}

PreparedStatement& PreparedStatement::bind_blob(int parameter, const void* data, std::size_t size) {
    sqlite3_bind_blob64(statement_, parameter, data, size, SQLITE_TRANSIENT);
    return *this;
}

PreparedStatement& PreparedStatement::bind_null(int parameter) {
    sqlite3_bind_null(statement_, parameter);
    return *this;
}

Result<bool> PreparedStatement::next_row() {
    const int result_code = sqlite3_step(statement_);
    if (result_code == SQLITE_ROW) return true;
    if (result_code == SQLITE_DONE) return false;
    auto error = make_sqlite_error(connection_, result_code, "step");
    sqlite3_reset(statement_);
    return std::unexpected(std::move(error));
}

Result<void> PreparedStatement::run() {
    while (true) {
        auto has_row = next_row();
        if (!has_row) return std::unexpected(has_row.error());
        if (!*has_row) break;
    }
    reset();
    return {};
}

void PreparedStatement::reset() {
    sqlite3_reset(statement_);
    sqlite3_clear_bindings(statement_);
}

bool PreparedStatement::column_is_null(int column) const {
    return sqlite3_column_type(statement_, column) == SQLITE_NULL;
}

std::int64_t PreparedStatement::column_int64(int column) const {
    return sqlite3_column_int64(statement_, column);
}

double PreparedStatement::column_double(int column) const {
    return sqlite3_column_double(statement_, column);
}

std::string PreparedStatement::column_text(int column) const {
    const auto* text = sqlite3_column_text(statement_, column);
    const auto size = static_cast<std::size_t>(sqlite3_column_bytes(statement_, column));
    return text ? std::string(reinterpret_cast<const char*>(text), size) : std::string();
}

std::vector<std::uint8_t> PreparedStatement::column_blob(int column) const {
    const auto* data = static_cast<const std::uint8_t*>(sqlite3_column_blob(statement_, column));
    const auto size = static_cast<std::size_t>(sqlite3_column_bytes(statement_, column));
    return data ? std::vector<std::uint8_t>(data, data + size) : std::vector<std::uint8_t>{};
}

std::optional<std::int64_t> PreparedStatement::column_optional_int64(int column) const {
    if (column_is_null(column)) return std::nullopt;
    return column_int64(column);
}

std::optional<double> PreparedStatement::column_optional_double(int column) const {
    if (column_is_null(column)) return std::nullopt;
    return column_double(column);
}

std::optional<std::string> PreparedStatement::column_optional_text(int column) const {
    if (column_is_null(column)) return std::nullopt;
    return column_text(column);
}

int PreparedStatement::column_count() const {
    return sqlite3_column_count(statement_);
}

// ---------------------------------------------------------------- Connection
Connection& Connection::operator=(Connection&& other) noexcept {
    if (this != &other) {
        close();
        handle_ = other.handle_;
        cache_ = std::move(other.cache_);
        other.handle_ = nullptr;
        other.cache_ = std::make_shared<StatementCache>();
    }
    return *this;
}

Connection::~Connection() {
    close();
}

void Connection::close() noexcept {
    if (cache_) cache_->clear();
    if (handle_) {
        sqlite3_close_v2(handle_);
        handle_ = nullptr;
    }
}

Result<Connection> Connection::open(const std::filesystem::path& file, const ConnectionOptions& options) {
    Connection connection;
    int flags = SQLITE_OPEN_NOMUTEX | SQLITE_OPEN_EXRESCODE;
    if (options.read_only) {
        flags |= SQLITE_OPEN_READONLY;
    } else {
        flags |= SQLITE_OPEN_READWRITE;
        if (options.create) flags |= SQLITE_OPEN_CREATE;
    }
    const auto utf8_path = path_to_utf8(file);
    const int result_code = sqlite3_open_v2(utf8_path.c_str(), &connection.handle_, flags, nullptr);
    if (result_code != SQLITE_OK) {
        auto error = make_sqlite_error(connection.handle_, result_code, "open");
        connection.close();
        if (!std::filesystem::exists(file)) error.code = ErrorCode::NotFound;
        return std::unexpected(std::move(error));
    }
    sqlite3_busy_timeout(connection.handle_, options.busy_timeout_ms);

    // Memory accounting stays simple: explicit page cache, no mmap, temp data on disk.
    std::string pragmas =
        std::format("PRAGMA foreign_keys=ON; PRAGMA temp_store=FILE; PRAGMA cache_size=-{}; PRAGMA mmap_size=0;",
                    options.page_cache_kib);
    if (!options.read_only) {
        if (options.create) pragmas += "PRAGMA page_size=4096;";
        if (options.write_ahead_log) pragmas += "PRAGMA journal_mode=WAL;";
        pragmas += "PRAGMA synchronous=FULL;";
    }
    if (auto applied = connection.execute_script(pragmas); !applied) return std::unexpected(applied.error());
    return connection;
}

Result<PreparedStatement> Connection::prepare(std::string_view sql) {
    if (sqlite3_stmt* cached = cache_->take(sql)) return PreparedStatement(handle_, cached, cache_, std::string(sql));
    sqlite3_stmt* statement = nullptr;
    const int result_code = sqlite3_prepare_v3(handle_, sql.data(), static_cast<int>(sql.size()),
                                               SQLITE_PREPARE_PERSISTENT, &statement, nullptr);
    if (result_code != SQLITE_OK)
        return std::unexpected(
            make_sqlite_error(handle_, result_code, std::format("prepare [{}]", sql.substr(0, 160))));
    return PreparedStatement(handle_, statement, cache_, std::string(sql));
}

Result<void> Connection::execute_script(std::string_view sql) {
    const std::string script(sql);
    char* message = nullptr;
    const int result_code = sqlite3_exec(handle_, script.c_str(), nullptr, nullptr, &message);
    if (result_code != SQLITE_OK) {
        auto error = make_sqlite_error(handle_, result_code, "execute");
        if (message) {
            error.safe_detail += std::format(" / {}", message);
            sqlite3_free(message);
        }
        return std::unexpected(std::move(error));
    }
    return {};
}

Result<std::int64_t> Connection::query_int64(std::string_view sql) {
    auto statement = prepare(sql);
    if (!statement) return std::unexpected(statement.error());
    auto has_row = statement->next_row();
    if (!has_row) return std::unexpected(has_row.error());
    if (!*has_row) return fail(ErrorCode::NotFound, "query returned no row");
    return statement->column_int64(0);
}

std::int64_t Connection::last_insert_rowid() const {
    return sqlite3_last_insert_rowid(handle_);
}

int Connection::changed_row_count() const {
    return sqlite3_changes(handle_);
}

bool Connection::in_transaction() const {
    return handle_ && !sqlite3_get_autocommit(handle_);
}

Result<void> Connection::begin_immediate(int deadline_ms) {
    using Clock = std::chrono::steady_clock;
    const auto deadline = Clock::now() + std::chrono::milliseconds(deadline_ms);
    static thread_local std::minstd_rand jitter(std::random_device{}());
    while (true) {
        const int result_code = sqlite3_exec(handle_, "BEGIN IMMEDIATE", nullptr, nullptr, nullptr);
        if (result_code == SQLITE_OK) return {};
        const int primary_code = result_code & 0xFF;
        const bool busy = primary_code == SQLITE_BUSY || primary_code == SQLITE_LOCKED;
        if (!busy || Clock::now() >= deadline)
            return std::unexpected(make_sqlite_error(handle_, result_code, "begin immediate"));
        std::this_thread::sleep_for(std::chrono::milliseconds(5 + jitter() % 20));
    }
}

Result<void> Connection::commit() {
    const int result_code = sqlite3_exec(handle_, "COMMIT", nullptr, nullptr, nullptr);
    if (result_code != SQLITE_OK) return std::unexpected(make_sqlite_error(handle_, result_code, "commit"));
    return {};
}

void Connection::rollback() noexcept {
    if (in_transaction()) sqlite3_exec(handle_, "ROLLBACK", nullptr, nullptr, nullptr);
}

Result<std::string> Connection::integrity_check() {
    auto statement = prepare("PRAGMA integrity_check");
    if (!statement) return std::unexpected(statement.error());
    std::string report;
    while (true) {
        auto has_row = statement->next_row();
        if (!has_row) return std::unexpected(has_row.error());
        if (!*has_row) break;
        if (!report.empty()) report += "; ";
        report += statement->column_text(0);
    }
    return report;
}

Result<std::vector<std::string>> Connection::foreign_key_violations() {
    auto statement = prepare("PRAGMA foreign_key_check");
    if (!statement) return std::unexpected(statement.error());
    std::vector<std::string> violations;
    while (true) {
        auto has_row = statement->next_row();
        if (!has_row) return std::unexpected(has_row.error());
        if (!*has_row) break;
        violations.push_back(std::format("{} rowid={} references {}", statement->column_text(0),
                                         statement->column_int64(1), statement->column_text(2)));
    }
    return violations;
}

Result<void> Connection::backup_to(const std::filesystem::path& destination, int pages_per_step) {
    sqlite3* target = nullptr;
    const auto utf8_path = path_to_utf8(destination);
    int result_code = sqlite3_open_v2(utf8_path.c_str(), &target, SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE, nullptr);
    auto fail_and_close = [&](std::string_view operation) {
        auto error = make_sqlite_error(target, result_code, operation);
        sqlite3_close_v2(target);
        return std::unexpected(std::move(error));
    };
    if (result_code != SQLITE_OK) return fail_and_close("backup open");

    sqlite3_backup* backup = sqlite3_backup_init(target, "main", handle_, "main");
    if (!backup) {
        result_code = sqlite3_errcode(target);
        return fail_and_close("backup init");
    }
    do {
        result_code = sqlite3_backup_step(backup, pages_per_step);
        if (result_code == SQLITE_BUSY || result_code == SQLITE_LOCKED)
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
    } while (result_code == SQLITE_OK || result_code == SQLITE_BUSY || result_code == SQLITE_LOCKED);
    sqlite3_backup_finish(backup);
    if (result_code != SQLITE_DONE) return fail_and_close("backup step");

    // A backup is a standalone file: leave WAL mode so the single file is complete on its own.
    sqlite3_exec(target, "PRAGMA journal_mode=DELETE", nullptr, nullptr, nullptr);
    result_code = sqlite3_close_v2(target);
    if (result_code != SQLITE_OK) return std::unexpected(make_sqlite_error(nullptr, result_code, "backup close"));
    return {};
}

// ---------------------------------------------------------------- WriteTransaction
Result<void> WriteTransaction::begin() {
    auto began = connection_.begin_immediate();
    if (began) active_ = true;
    return began;
}

Result<void> WriteTransaction::commit() {
    auto committed = connection_.commit();
    if (committed) active_ = false;
    return committed;
}

}  // namespace mso::sqlite
