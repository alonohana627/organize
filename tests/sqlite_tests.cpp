// The catalog's SQLite wrapper: statement cache, moves, bind errors, the busy path, read snapshots,
// online backup and the new_uuid() SQL function.
#include <doctest.h>

#include <chrono>

#include "mso/catalog/sqlite.hpp"
#include "test_support.hpp"

using namespace mso;
using mso::test::ScratchDirectory;

namespace {

sqlite::Connection open_database(const std::filesystem::path& file) {
    auto connection = sqlite::Connection::open(file, {.create = true});
    REQUIRE_MESSAGE(connection.has_value(), connection.error().message());
    return std::move(*connection);
}

std::int64_t scalar(sqlite::Connection& connection, std::string_view sql) {
    auto value = connection.query_int64(sql);
    REQUIRE_MESSAGE(value.has_value(), value.error().message());
    return *value;
}

}  // namespace

TEST_SUITE("catalog") {
    TEST_CASE("cached statements are reused with their bindings cleared") {
        ScratchDirectory scratch;
        auto connection = open_database(scratch / "cache.sqlite");
        {
            auto statement = connection.prepare("SELECT ?1");
            REQUIRE(statement.has_value());
            statement->bind(1, std::int64_t{42});
            auto has_row = statement->next_row();
            REQUIRE(has_row.has_value());
            REQUIRE(*has_row);
            CHECK(statement->column_int64(0) == 42);
        }  // returned to the cache mid-iteration: it must come back reset
        auto again = connection.prepare("SELECT ?1");
        REQUIRE(again.has_value());
        auto has_row = again->next_row();
        REQUIRE(has_row.has_value());
        REQUIRE(*has_row);
        CHECK(again->column_is_null(0));  // the old binding did not leak into the reuse
    }

    TEST_CASE("the statement cache keeps idle statements and finalizes them after closing") {
        sqlite::StatementCache cache;
        CHECK(cache.take("SELECT 1") == nullptr);
        ScratchDirectory scratch;
        auto connection = open_database(scratch / "idle.sqlite");
        for (int round = 0; round < 3; ++round)
            CHECK(scalar(connection, "SELECT 7") == 7);
        auto outstanding = connection.prepare("SELECT 8");
        REQUIRE(outstanding.has_value());
        connection.close();  // the statement outlives its connection and is finalized on release
        CHECK_FALSE(connection.is_open());
        CHECK_FALSE(connection.prepare("SELECT 1").has_value());
    }

    TEST_CASE("moved connections and statements stay usable; moved-from ones fail cleanly") {
        ScratchDirectory scratch;
        auto original = open_database(scratch / "move.sqlite");
        sqlite::Connection moved(std::move(original));
        CHECK(moved.is_open());
        CHECK_FALSE(original.is_open());  // NOLINT(bugprone-use-after-move): the moved-from state is under test
        CHECK_FALSE(original.prepare("SELECT 1").has_value());
        CHECK(scalar(moved, "SELECT 3") == 3);

        sqlite::Connection assigned;
        assigned = std::move(moved);
        auto statement = assigned.prepare("SELECT 5");
        REQUIRE(statement.has_value());
        sqlite::PreparedStatement other = std::move(*statement);
        auto has_row = other.next_row();
        REQUIRE(has_row.has_value());
        REQUIRE(*has_row);
        CHECK(other.column_int64(0) == 5);
    }

    TEST_CASE("a bind error is reported instead of running with NULL") {
        ScratchDirectory scratch;
        auto connection = open_database(scratch / "bind.sqlite");
        REQUIRE(connection.execute_script("CREATE TABLE item(value INTEGER)").has_value());
        auto insert = connection.prepare("INSERT INTO item(value) VALUES(?1)");
        REQUIRE(insert.has_value());
        insert->bind(2, std::int64_t{1});  // there is no parameter 2
        auto ran = insert->run();
        REQUIRE_FALSE(ran.has_value());
        CHECK(ran.error().safe_detail.find("bind parameter 2") != std::string::npos);
        CHECK(scalar(connection, "SELECT count(*) FROM item") == 0);
        insert->bind(1, std::int64_t{9});  // the error was cleared by the reset
        CHECK(insert->run().has_value());
        CHECK(scalar(connection, "SELECT value FROM item") == 9);
    }

    TEST_CASE("unique violations are distinguishable from other failures") {
        ScratchDirectory scratch;
        auto connection = open_database(scratch / "unique.sqlite");
        REQUIRE(connection.execute_script("CREATE TABLE item(value INTEGER UNIQUE CHECK(value>0))").has_value());
        auto insert = connection.prepare("INSERT INTO item(value) VALUES(?1)");
        REQUIRE(insert.has_value());
        REQUIRE(insert->bind(1, std::int64_t{1}).run().has_value());
        CHECK_FALSE(insert->bind(1, std::int64_t{1}).run().has_value());
        CHECK(insert->failed_on_unique_constraint());
        CHECK_FALSE(insert->bind(1, std::int64_t{-1}).run().has_value());
        CHECK_FALSE(insert->failed_on_unique_constraint());  // a CHECK failure is not a conflict
    }

    TEST_CASE("BEGIN IMMEDIATE gives up with a retryable DB_BUSY within its deadline") {
        ScratchDirectory scratch;
        auto holder = open_database(scratch / "busy.sqlite");
        auto contender = open_database(scratch / "busy.sqlite");
        REQUIRE(holder.begin_immediate().has_value());
        const auto started = std::chrono::steady_clock::now();
        auto began = contender.begin_immediate(std::chrono::milliseconds(100));
        const auto waited = std::chrono::steady_clock::now() - started;
        REQUIRE_FALSE(began.has_value());
        CHECK(began.error().code == ErrorCode::DbBusy);
        CHECK(began.error().retryable);
        // The busy handler (2 s) is suspended during the retry loop, so the wait is not doubled.
        CHECK(waited < std::chrono::milliseconds(1500));
        holder.rollback();
        REQUIRE(contender.begin_immediate().has_value());
        CHECK(contender.commit().has_value());
    }

    TEST_CASE("a read transaction sees one snapshot across statements") {
        ScratchDirectory scratch;
        auto writer = open_database(scratch / "snapshot.sqlite");
        REQUIRE(writer.execute_script("CREATE TABLE item(value INTEGER); INSERT INTO item VALUES(1)").has_value());
        auto reader = sqlite::Connection::open(scratch / "snapshot.sqlite", {.read_only = true});
        REQUIRE(reader.has_value());
        {
            sqlite::ReadTransaction snapshot(*reader);
            REQUIRE(snapshot.begin().has_value());
            CHECK(scalar(*reader, "SELECT count(*) FROM item") == 1);
            REQUIRE(writer.execute_script("INSERT INTO item VALUES(2)").has_value());
            CHECK(scalar(*reader, "SELECT count(*) FROM item") == 1);  // still the first snapshot
        }
        CHECK_FALSE(reader->in_transaction());
        CHECK(scalar(*reader, "SELECT count(*) FROM item") == 2);
    }

    TEST_CASE("a write transaction rolls back unless committed") {
        ScratchDirectory scratch;
        auto connection = open_database(scratch / "rollback.sqlite");
        REQUIRE(connection.execute_script("CREATE TABLE item(value INTEGER)").has_value());
        {
            sqlite::WriteTransaction transaction(connection);
            REQUIRE(transaction.begin().has_value());
            REQUIRE(connection.execute_script("INSERT INTO item VALUES(1)").has_value());
        }
        CHECK(scalar(connection, "SELECT count(*) FROM item") == 0);
        {
            sqlite::WriteTransaction transaction(connection);
            REQUIRE(transaction.begin().has_value());
            REQUIRE(connection.execute_script("INSERT INTO item VALUES(1)").has_value());
            REQUIRE(transaction.commit().has_value());
        }
        CHECK(scalar(connection, "SELECT count(*) FROM item") == 1);
    }

    TEST_CASE("online backup writes a complete standalone copy") {
        ScratchDirectory scratch;
        auto connection = open_database(scratch / "source.sqlite");
        REQUIRE(connection
                    .execute_script("CREATE TABLE item(value INTEGER);"
                                    "WITH RECURSIVE n(i) AS (SELECT 1 UNION ALL SELECT i+1 FROM n WHERE i<5000) "
                                    "INSERT INTO item SELECT i FROM n")
                    .has_value());
        REQUIRE(connection.backup_to(scratch / "copy.sqlite", 8).has_value());
        auto copy = sqlite::Connection::open(scratch / "copy.sqlite", {.read_only = true, .write_ahead_log = false});
        REQUIRE(copy.has_value());
        CHECK(scalar(*copy, "SELECT count(*) FROM item") == 5000);
        auto mode = copy->prepare("PRAGMA journal_mode");
        REQUIRE(mode.has_value());
        REQUIRE(mode->next_row().value_or(false));
        CHECK(mode->column_text(0) == "delete");
        CHECK(copy->integrity_check().value_or("") == "ok");
    }

    TEST_CASE("new_uuid() mints distinct canonical UUIDs per row") {
        ScratchDirectory scratch;
        auto connection = open_database(scratch / "uuid.sqlite");
        REQUIRE(connection
                    .execute_script("CREATE TABLE item(id TEXT UNIQUE);"
                                    "WITH RECURSIVE n(i) AS (SELECT 1 UNION ALL SELECT i+1 FROM n WHERE i<100) "
                                    "INSERT INTO item SELECT new_uuid() FROM n")
                    .has_value());
        CHECK(scalar(connection, "SELECT count(DISTINCT id) FROM item") == 100);
        auto first = connection.prepare("SELECT id FROM item LIMIT 1");
        REQUIRE(first.has_value());
        REQUIRE(first->next_row().value_or(false));
        CHECK(uuids::uuid::from_string(first->column_text(0)).has_value());
    }

    TEST_CASE("opening a missing database read-only reports NOT_FOUND") {
        ScratchDirectory scratch;
        auto missing = sqlite::Connection::open(scratch / "absent.sqlite", {.read_only = true});
        REQUIRE_FALSE(missing.has_value());
        CHECK(missing.error().code == ErrorCode::NotFound);
    }
}  // TEST_SUITE("catalog")
