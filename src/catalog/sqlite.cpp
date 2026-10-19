#include "mso/catalog/sqlite.hpp"

#include <sqlite3.h>

#include <exception>
#include <format>
#include <random>
#include <thread>

#include "mso/domain/core.hpp"

namespace mso::sqlite {

namespace {

/// SQL function `new_uuid()`: a fresh RFC 4122 v4 UUID from stduuid (mso::new_uuid) as canonical
/// lowercase text, so set-based INSERT … SELECT statements can mint row identities.
void new_uuid_sql_function(sqlite3_context* context, int /*argument_count*/, sqlite3_value** /*arguments*/) {
    try {
        const std::string text = uuids::to_string(new_uuid());
        sqlite3_result_text(context, text.c_str(), static_cast<int>(text.size()), SQLITE_TRANSIENT);
    } catch (const std::exception& error) {
        sqlite3_result_error(context, error.what(), -1);  // never let an exception cross the C boundary
    }
}

/// The text of a TEXT argument (UTF-8 bytes as stored), or nullopt for NULL and other types.
std::optional<std::string_view> text_argument(sqlite3_value* value) {
    if (sqlite3_value_type(value) != SQLITE_TEXT) return std::nullopt;
    const auto* text = sqlite3_value_text(value);
    const int size = sqlite3_value_bytes(value);
    return std::string_view(reinterpret_cast<const char*>(text), static_cast<std::size_t>(size));
}

/// Returns a computed key as the function result (NULL input gives NULL).
void return_key(sqlite3_context* context, const std::optional<std::string>& key) {
    if (!key) {
        sqlite3_result_null(context);
        return;
    }
    sqlite3_result_text(context, key->data(), static_cast<int>(key->size()), SQLITE_TRANSIENT);
}

/// SQL function `fold_key(text)`: mso::fold_key, so stored name keys can be rebuilt set-based.
void fold_key_sql_function(sqlite3_context* context, int /*argument_count*/, sqlite3_value** arguments) {
    try {
        const auto text = text_argument(arguments[0]);
        return_key(context, text ? std::optional<std::string>(fold_key(*text)) : std::nullopt);
    } catch (const std::exception& error) {
        sqlite3_result_error(context, error.what(), -1);  // never let an exception cross the C boundary
    }
}

/// SQL function `path_key(text, case_sensitive)`: mso::path_key, so stored path keys can be rebuilt
/// set-based.
void path_key_sql_function(sqlite3_context* context, int /*argument_count*/, sqlite3_value** arguments) {
    try {
        const auto text = text_argument(arguments[0]);
        const bool case_sensitive = sqlite3_value_int64(arguments[1]) != 0;
        return_key(context, text ? std::optional<std::string>(path_key(*text, case_sensitive)) : std::nullopt);
    } catch (const std::exception& error) {
        sqlite3_result_error(context, error.what(), -1);
    }
}

/// Restores a connection's configured busy timeout when a scope that suspended it ends.
class BusyTimeoutSuspension {
public:
    BusyTimeoutSuspension(sqlite3* handle, int configured_ms) : handle_(handle), configured_ms_(configured_ms) {
        sqlite3_busy_timeout(handle_, 0);
    }
    ~BusyTimeoutSuspension() { sqlite3_busy_timeout(handle_, configured_ms_); }
    BusyTimeoutSuspension(const BusyTimeoutSuspension&) = delete;
    BusyTimeoutSuspension& operator=(const BusyTimeoutSuspension&) = delete;

private:
    sqlite3* handle_;
    int configured_ms_;
};

/// Interrupts the connection's statements once `stop` is requested, by a progress handler checked
/// every kStepsPerCheck virtual-machine steps; removed again when the scope ends.
class StopInterruption {
public:
    StopInterruption(sqlite3* handle, std::stop_token stop) : handle_(handle), stop_(std::move(stop)) {
        if (stop_.stop_possible()) sqlite3_progress_handler(handle_, kStepsPerCheck, &requested, &stop_);
    }
    ~StopInterruption() {
        if (stop_.stop_possible()) sqlite3_progress_handler(handle_, 0, nullptr, nullptr);
    }
    StopInterruption(const StopInterruption&) = delete;
    StopInterruption& operator=(const StopInterruption&) = delete;

private:
    static constexpr int kStepsPerCheck = 4096;
    /// Progress callback: non-zero interrupts the running statement (SQLITE_INTERRUPT).
    static int requested(void* stop) noexcept { return static_cast<std::stop_token*>(stop)->stop_requested() ? 1 : 0; }

    sqlite3* handle_;
    std::stop_token stop_;
};

bool is_busy(int result_code) {
    const int primary_code = result_code & 0xFF;
    return primary_code == SQLITE_BUSY || primary_code == SQLITE_LOCKED;
}

}  // namespace

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
        case SQLITE_INTERRUPT:
            code = ErrorCode::Cancelled;
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
StatementCache::Entry StatementCache::take(std::string_view sql) {
    const auto found = idle_.find(sql);
    if (found == idle_.end()) return {};
    return idle_.extract(found);
}

bool StatementCache::prepare_for_reuse(sqlite3_stmt* statement) noexcept {
    sqlite3_reset(statement);
    sqlite3_clear_bindings(statement);
    if (closed_ || idle_.size() >= kMaximumIdleStatements) {
        sqlite3_finalize(statement);
        return false;
    }
    return true;
}

void StatementCache::give_back(Entry entry) noexcept {
    if (entry.empty() || !prepare_for_reuse(entry.mapped())) return;
    sqlite3_stmt* statement = entry.mapped();
    try {
        idle_.insert(std::move(entry));  // may grow the bucket array, never the node
    } catch (const std::bad_alloc&) {
        sqlite3_finalize(statement);  // caching is an optimization; dropping the statement is safe
    }
}

void StatementCache::give_back(std::string sql, sqlite3_stmt* statement) noexcept {
    if (!prepare_for_reuse(statement)) return;
    try {
        idle_.emplace(std::move(sql), statement);
    } catch (const std::bad_alloc&) {
        sqlite3_finalize(statement);  // caching is an optimization; dropping the statement is safe
    }
}

std::vector<std::string> StatementCache::idle_sql() const {
    std::vector<std::string> texts;
    texts.reserve(idle_.size());
    for (const auto& [sql, statement] : idle_)
        texts.push_back(sql);
    return texts;
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
      sql_(std::move(other.sql_)),
      cache_entry_(std::move(other.cache_entry_)),
      bind_error_code_(other.bind_error_code_),
      bind_error_parameter_(other.bind_error_parameter_),
      last_step_error_code_(other.last_step_error_code_) {
    other.statement_ = nullptr;
}

PreparedStatement& PreparedStatement::operator=(PreparedStatement&& other) noexcept {
    if (this != &other) {
        release();
        connection_ = other.connection_;
        statement_ = other.statement_;
        cache_ = std::move(other.cache_);
        sql_ = std::move(other.sql_);
        cache_entry_ = std::move(other.cache_entry_);
        bind_error_code_ = other.bind_error_code_;
        bind_error_parameter_ = other.bind_error_parameter_;
        last_step_error_code_ = other.last_step_error_code_;
        other.statement_ = nullptr;
    }
    return *this;
}

PreparedStatement::~PreparedStatement() {
    release();
}

void PreparedStatement::release() noexcept {
    if (!statement_) return;
    if (!cache_)
        sqlite3_finalize(statement_);
    else if (!cache_entry_.empty())
        cache_->give_back(std::move(cache_entry_));
    else
        cache_->give_back(std::move(sql_), statement_);
    statement_ = nullptr;
}

void PreparedStatement::remember_bind_result(int result_code, int parameter) noexcept {
    if (result_code != SQLITE_OK && bind_error_code_ == 0) {
        bind_error_code_ = result_code;
        bind_error_parameter_ = parameter;
    }
}

PreparedStatement& PreparedStatement::bind(int parameter, std::int64_t value) {
    remember_bind_result(sqlite3_bind_int64(statement_, parameter, value), parameter);
    return *this;
}

PreparedStatement& PreparedStatement::bind(int parameter, double value) {
    remember_bind_result(sqlite3_bind_double(statement_, parameter, value), parameter);
    return *this;
}

PreparedStatement& PreparedStatement::bind(int parameter, std::string_view value) {
    remember_bind_result(
        sqlite3_bind_text64(statement_, parameter, value.data(), value.size(), SQLITE_TRANSIENT, SQLITE_UTF8),
        parameter);
    return *this;
}

PreparedStatement& PreparedStatement::bind_blob(int parameter, const void* data, std::size_t size) {
    remember_bind_result(sqlite3_bind_blob64(statement_, parameter, data, size, SQLITE_TRANSIENT), parameter);
    return *this;
}

PreparedStatement& PreparedStatement::bind_null(int parameter) {
    remember_bind_result(sqlite3_bind_null(statement_, parameter), parameter);
    return *this;
}

Result<bool> PreparedStatement::next_row() {
    if (bind_error_code_ != 0) {
        auto error =
            make_sqlite_error(nullptr, bind_error_code_, std::format("bind parameter {}", bind_error_parameter_));
        reset();
        return std::unexpected(std::move(error));
    }
    last_step_error_code_ = 0;
    const int result_code = sqlite3_step(statement_);
    if (result_code == SQLITE_ROW) return true;
    if (result_code == SQLITE_DONE) return false;
    last_step_error_code_ = result_code;
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

void PreparedStatement::reset() noexcept {
    sqlite3_reset(statement_);
    sqlite3_clear_bindings(statement_);
    bind_error_code_ = 0;
    bind_error_parameter_ = 0;
}

bool PreparedStatement::failed_on_unique_constraint() const noexcept {
    return last_step_error_code_ == SQLITE_CONSTRAINT_UNIQUE || last_step_error_code_ == SQLITE_CONSTRAINT_PRIMARYKEY;
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
        busy_timeout_ms_ = other.busy_timeout_ms_;
        other.handle_ = nullptr;
    }
    return *this;
}

Connection::~Connection() {
    close();
}

void Connection::close() noexcept {
    if (cache_) cache_->clear();
    if (handle_) {
        sqlite3_close_v2(handle_);  // deferred until outstanding statements are finalized
        handle_ = nullptr;
    }
}

Result<Connection> Connection::open(const std::filesystem::path& file, const ConnectionOptions& options) {
    Connection connection;
    connection.busy_timeout_ms_ = options.busy_timeout_ms;
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
        std::error_code exists_error;
        if (!std::filesystem::exists(file, exists_error) && !exists_error) error.code = ErrorCode::NotFound;
        return std::unexpected(std::move(error));
    }
    sqlite3_busy_timeout(connection.handle_, options.busy_timeout_ms);
    const int function_code =
        sqlite3_create_function_v2(connection.handle_, "new_uuid", 0, SQLITE_UTF8 | SQLITE_INNOCUOUS, nullptr,
                                   &new_uuid_sql_function, nullptr, nullptr, nullptr);
    if (function_code != SQLITE_OK)
        return std::unexpected(make_sqlite_error(connection.handle_, function_code, "register new_uuid()"));
    constexpr int kKeyFunctionFlags = SQLITE_UTF8 | SQLITE_DETERMINISTIC | SQLITE_INNOCUOUS;
    const int fold_code = sqlite3_create_function_v2(connection.handle_, "fold_key", 1, kKeyFunctionFlags, nullptr,
                                                     &fold_key_sql_function, nullptr, nullptr, nullptr);
    if (fold_code != SQLITE_OK)
        return std::unexpected(make_sqlite_error(connection.handle_, fold_code, "register fold_key()"));
    const int path_code = sqlite3_create_function_v2(connection.handle_, "path_key", 2, kKeyFunctionFlags, nullptr,
                                                     &path_key_sql_function, nullptr, nullptr, nullptr);
    if (path_code != SQLITE_OK)
        return std::unexpected(make_sqlite_error(connection.handle_, path_code, "register path_key()"));

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
    if (!handle_ || !cache_) return fail(ErrorCode::Internal, "prepare on a closed connection");
    if (auto cached = cache_->take(sql); !cached.empty()) return PreparedStatement(handle_, std::move(cached), cache_);
    sqlite3_stmt* statement = nullptr;
    const int result_code = sqlite3_prepare_v3(handle_, sql.data(), static_cast<int>(sql.size()),
                                               SQLITE_PREPARE_PERSISTENT, &statement, nullptr);
    if (result_code != SQLITE_OK)
        return std::unexpected(
            make_sqlite_error(handle_, result_code, std::format("prepare [{}]", utf8_prefix(sql, 160))));
    return PreparedStatement(handle_, statement, cache_, std::string(sql));
}

Result<void> Connection::execute_script(std::string_view sql) {
    if (!handle_) return fail(ErrorCode::Internal, "execute on a closed connection");
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

Result<void> Connection::begin_immediate(std::chrono::milliseconds deadline) {
    if (!handle_) return fail(ErrorCode::Internal, "begin on a closed connection");
    using Clock = std::chrono::steady_clock;
    const auto give_up_at = Clock::now() + deadline;
    static thread_local std::minstd_rand jitter(std::random_device{}());
    // Our own jittered loop is the only wait: SQLite's busy handler would add its full timeout to
    // every attempt and double the real wait.
    const BusyTimeoutSuspension suspended(handle_, busy_timeout_ms_);
    while (true) {
        const int result_code = sqlite3_exec(handle_, "BEGIN IMMEDIATE", nullptr, nullptr, nullptr);
        if (result_code == SQLITE_OK) return {};
        if (!is_busy(result_code) || Clock::now() >= give_up_at)
            return std::unexpected(make_sqlite_error(handle_, result_code, "begin immediate"));
        std::this_thread::sleep_for(std::chrono::milliseconds(5 + jitter() % 20));
    }
}

Result<void> Connection::begin_deferred() {
    if (!handle_) return fail(ErrorCode::Internal, "begin on a closed connection");
    const int result_code = sqlite3_exec(handle_, "BEGIN", nullptr, nullptr, nullptr);
    if (result_code != SQLITE_OK) return std::unexpected(make_sqlite_error(handle_, result_code, "begin"));
    return {};
}

Result<void> Connection::commit() {
    const int result_code = sqlite3_exec(handle_, "COMMIT", nullptr, nullptr, nullptr);
    if (result_code != SQLITE_OK) return std::unexpected(make_sqlite_error(handle_, result_code, "commit"));
    return {};
}

void Connection::rollback() noexcept {
    if (in_transaction()) sqlite3_exec(handle_, "ROLLBACK", nullptr, nullptr, nullptr);
}

std::vector<std::string> Connection::cached_statement_sql() const {
    return cache_ ? cache_->idle_sql() : std::vector<std::string>{};
}

Result<std::string> Connection::integrity_check(std::stop_token stop) {
    if (!handle_) return fail(ErrorCode::Internal, "integrity check on a closed connection");
    if (stop.stop_requested()) return fail(ErrorCode::Cancelled, "the integrity check was cancelled");
    const StopInterruption interruption(handle_, stop);
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

Result<std::vector<std::string>> Connection::foreign_key_violations(std::stop_token stop) {
    if (!handle_) return fail(ErrorCode::Internal, "foreign key check on a closed connection");
    if (stop.stop_requested()) return fail(ErrorCode::Cancelled, "the foreign key check was cancelled");
    const StopInterruption interruption(handle_, stop);
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

Result<void> Connection::backup_to(const std::filesystem::path& destination, int pages_per_step,
                                   std::chrono::milliseconds busy_deadline, std::stop_token stop) {
    if (!handle_) return fail(ErrorCode::Internal, "backup from a closed connection");
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
    using Clock = std::chrono::steady_clock;
    std::optional<Clock::time_point> busy_since;
    while (true) {
        if (stop.stop_requested()) {
            sqlite3_backup_finish(backup);
            sqlite3_close_v2(target);
            return fail(ErrorCode::Cancelled, "the backup was cancelled");
        }
        result_code = sqlite3_backup_step(backup, pages_per_step);
        if (result_code == SQLITE_OK) {
            busy_since.reset();
            continue;
        }
        if (!is_busy(result_code)) break;
        const auto now = Clock::now();
        if (!busy_since) busy_since = now;
        if (now - *busy_since >= busy_deadline) break;  // bounded: never retry BUSY forever
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    sqlite3_backup_finish(backup);
    if (result_code != SQLITE_DONE) return fail_and_close("backup step");

    // A backup is a standalone file: leave WAL mode so the single file is complete on its own.
    result_code = sqlite3_exec(target, "PRAGMA journal_mode=DELETE", nullptr, nullptr, nullptr);
    if (result_code != SQLITE_OK) return fail_and_close("backup journal mode");
    result_code = sqlite3_close_v2(target);
    if (result_code != SQLITE_OK) return std::unexpected(make_sqlite_error(nullptr, result_code, "backup close"));
    return {};
}

// ---------------------------------------------------------------- transactions
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

Result<void> ReadTransaction::begin() {
    auto began = connection_.begin_deferred();
    if (began) active_ = true;
    return began;
}

}  // namespace mso::sqlite
