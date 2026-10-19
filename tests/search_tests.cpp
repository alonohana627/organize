// Search: query language, injection safety, hard filters, keyset paging with nulls, federation.
#include <doctest.h>

#include <algorithm>
#include <numeric>

#include <set>

#include "mso/search/search_service.hpp"
#include "test_support.hpp"

using namespace mso;
using namespace mso::search;
using mso::test::ScratchDirectory;

namespace {

/// Twelve samples: three without a known duration, names deliberately interleaved.
std::vector<catalog::ScannedFile> paging_fixture() {
    std::vector<catalog::ScannedFile> files;
    for (int index = 0; index < 12; ++index) {
        auto file = test::synthetic_file(std::format("set/{} sample {:02}.wav", index % 2 ? "Kick" : "Snare", index),
                                         1000 + index, (index % 4 == 3) ? 0 : 100'000 * (index + 1));
        if (index % 4 == 3) file.metadata->duration_us.reset();
        files.push_back(std::move(file));
    }
    return files;
}

/// Pages through the full result and returns sample ids in delivered order.
std::vector<std::int64_t> collect_all(catalog::Library& library, SearchRequest request) {
    catalog::Library* scope[] = {&library};
    std::vector<std::int64_t> ids;
    SearchService service;
    for (int guard = 0; guard < 100; ++guard) {
        auto page = service.search(scope, request);
        REQUIRE(page.has_value());
        for (const auto& row : page->rows)
            ids.push_back(row.sample_id);
        if (!page->next) break;
        request.cursor = page->next;
    }
    return ids;
}

}  // namespace

TEST_SUITE("search") {
    TEST_CASE("valid queries parse into the expected tree") {
        for (const auto text :
             {"kick", "\"metallic impact\" tag:Drums/Kick", "bpm:140..155 key:F#minor duration:<2s available:true",
              "format:flac rating:>=4 favorite:true", "(tag:Drums OR tag:Percussion) AND NOT tag:Acoustic",
              "folder:\"Packs/Goa\" category:Bass analyzed:tempo", "kic*", "name:kick size:>1MB rate:44.1k"}) {
            CAPTURE(text);
            CHECK(parse_query(text).has_value());
        }
        auto query = parse_query("a b OR c");
        REQUIRE(query.has_value());
        CHECK(query->root->kind == QueryNode::Kind::Or);  // OR binds weaker than implicit AND
        CHECK(query->root->children.front()->kind == QueryNode::Kind::And);
        CHECK(parse_query("").has_value());
        CHECK(parse_query("").value().empty());
    }

    TEST_CASE("invalid queries fail with a code-point position and run nothing") {
        struct Case {
            std::string_view text;
            std::uint32_t position;
        };
        for (const auto& [text, position] :
             {Case{"bpm:fast", 4}, Case{"duration:<2parsecs", 10}, Case{"\"unterminated", 0}, Case{"(kick", 0},
              Case{"kick)", 4}, Case{"colour:red", 0}, Case{"k*", 0}, Case{"שלום bpm:x", 9}, Case{"rating:9", 7},
              Case{"a OR", 2}}) {
            CAPTURE(text);
            auto query = parse_query(text);
            REQUIRE_FALSE(query.has_value());
            CHECK(query.error().code == ErrorCode::ParseError);
            REQUIRE(query.error().position.has_value());
            CHECK(*query.error().position == position);
        }
        std::string deep;
        for (int level = 0; level < 20; ++level)
            deep += "(";
        deep += "x";
        for (int level = 0; level < 20; ++level)
            deep += ")";
        CHECK_FALSE(parse_query(deep).has_value());
        CHECK_FALSE(parse_query(std::string(5000, 'a')).has_value());
    }

    TEST_CASE("user text is never interpreted as SQL or FTS syntax") {
        ScratchDirectory scratch;
        auto populated = test::make_library(scratch / "lib", paging_fixture());
        catalog::Library* scope[] = {populated.library.get()};
        for (const auto text : {"'; DROP TABLE samples; --", "\"a\" OR 1=1", "kick NEAR(snare)",
                                "tag:\"x') OR 1=1 --\"", "name:\"*\"", "folder:\"%\""}) {
            CAPTURE(text);
            auto page = SearchService().search(scope, SearchRequest{.text = text});
            if (page) CHECK(page->rows.empty());
        }
        CHECK(populated.library->sample_count().value_or(0) == 12);
    }

    TEST_CASE("hard filters hold for every returned row") {
        ScratchDirectory scratch;
        auto populated = test::make_library(scratch / "lib", paging_fixture());
        catalog::Library* scope[] = {populated.library.get()};
        auto page = SearchService().search(scope, SearchRequest{.text = "kick duration:>=200ms", .page_size = 500});
        REQUIRE(page.has_value());
        CHECK_FALSE(page->rows.empty());
        for (const auto& row : page->rows) {
            CHECK(row.display_name.starts_with("Kick"));
            REQUIRE(row.duration_us.has_value());
            CHECK(*row.duration_us >= 200'000);
        }
        auto unknown_key = SearchService().search(scope, SearchRequest{.text = "key:unknown", .page_size = 500});
        CHECK(unknown_key->rows.size() == 12);
    }

    TEST_CASE("keyset paging visits every row exactly once in order, nulls last, both directions") {
        ScratchDirectory scratch;
        auto populated = test::make_library(scratch / "lib", paging_fixture());
        for (const bool descending : {false, true}) {
            for (const auto sort : {SortField::Duration, SortField::Name, SortField::Size, SortField::Bpm}) {
                CAPTURE(descending);
                CAPTURE(to_string(sort));
                auto ids = collect_all(*populated.library,
                                       SearchRequest{.sort = sort, .descending = descending, .page_size = 5});
                CHECK(ids.size() == 12);
                CHECK(std::set<std::int64_t>(ids.begin(), ids.end()).size() == 12);
            }
            catalog::Library* scope[] = {populated.library.get()};
            auto all = SearchService().search(
                scope, SearchRequest{.sort = SortField::Duration, .descending = descending, .page_size = 500});
            REQUIRE(all.has_value());
            for (std::size_t index = 0; index + 1 < all->rows.size(); ++index) {
                const auto& current = all->rows[index].duration_us;
                const auto& next = all->rows[index + 1].duration_us;
                if (!current)
                    CHECK_FALSE(next.has_value());  // nulls form a trailing partition
                else if (next)
                    CHECK((descending ? *current >= *next : *current <= *next));
            }
        }
    }

    TEST_CASE("a cursor from a different query is rejected as stale") {
        ScratchDirectory scratch;
        auto populated = test::make_library(scratch / "lib", paging_fixture());
        catalog::Library* scope[] = {populated.library.get()};
        auto first = SearchService().search(scope, SearchRequest{.text = "kick", .page_size = 2});
        REQUIRE(first.has_value());
        REQUIRE(first->next.has_value());
        auto other =
            SearchService().search(scope, SearchRequest{.text = "snare", .page_size = 2, .cursor = first->next});
        REQUIRE_FALSE(other.has_value());
        CHECK(other.error().code == ErrorCode::StaleCursor);
    }

    TEST_CASE("federated search merges libraries in one global order without duplicates") {
        ScratchDirectory scratch;
        auto first = test::make_library(scratch / "a", paging_fixture(), "A");
        auto second = test::make_library(scratch / "b", paging_fixture(), "B");
        catalog::Library* scope[] = {first.library.get(), second.library.get()};
        SearchService service;
        SearchRequest request{.sort = SortField::Duration, .page_size = 7};
        std::vector<SampleRow> rows;
        for (int guard = 0; guard < 20; ++guard) {
            auto page = service.search(scope, request);
            REQUIRE(page.has_value());
            CHECK(page->coverage.size() == 2);
            rows.insert(rows.end(), page->rows.begin(), page->rows.end());
            if (!page->next) break;
            request.cursor = page->next;
        }
        CHECK(rows.size() == 24);
        std::set<std::string> keys;
        for (const auto& row : rows)
            keys.insert(uuids::to_string(row.library) + std::to_string(row.sample_id));
        CHECK(keys.size() == 24);
        auto count = service.count(scope, SearchRequest{.text = "kick"});
        REQUIRE(count.has_value());
        CHECK(count->total == 12);
    }

    TEST_CASE("relevance ranks better matches first and fuses libraries") {
        ScratchDirectory scratch;
        std::vector<catalog::ScannedFile> files = {test::synthetic_file("kick kick kick.wav", 1, 1),
                                                   test::synthetic_file("kick snare.wav", 2, 1),
                                                   test::synthetic_file("snare only.wav", 3, 1)};
        auto populated = test::make_library(scratch / "lib", files);
        catalog::Library* scope[] = {populated.library.get()};
        auto page = SearchService().search(scope, SearchRequest{.text = "kick", .sort = SortField::Relevance});
        REQUIRE(page.has_value());
        REQUIRE(page->rows.size() == 2);
        CHECK(page->rows.front().display_name == "kick kick kick");
    }

    TEST_CASE("tag, category, folder and collection filters match subtrees and aliases") {
        ScratchDirectory scratch;
        auto populated = test::make_library(scratch / "lib", paging_fixture());
        auto& library = *populated.library;
        catalog::EditCommand tag;
        tag.sample_ids = {1, 2};
        tag.tags_add = {"Drums/Kick/Acoustic"};
        tag.category = std::optional<std::string>("Drums/Kicks");
        REQUIRE(library.apply_edit(tag).has_value());
        catalog::Library* scope[] = {&library};
        SearchService service;
        CHECK(service.search(scope, {.text = "tag:Drums"})->rows.size() == 2);
        CHECK(service.search(scope, {.text = "tag:Kick"})->rows.size() == 2);
        CHECK(service.search(scope, {.text = "tag:Drums/Kick/Acoustic"})->rows.size() == 2);
        CHECK(service.search(scope, {.text = "NOT tag:Drums"})->rows.size() == 10);
        CHECK(service.search(scope, {.text = "category:Drums"})->rows.size() == 2);
        CHECK(service.search(scope, {.text = "folder:set"})->rows.size() == 12);
        CHECK(service.search(scope, {.text = "folder:Samples/set", .page_size = 500})->rows.size() == 12);
        CHECK(service.search(scope, {.text = "tag:Nope/Missing"})->rows.empty());
        CHECK(service.search(scope, {.text = "acoustic"})->rows.size() == 2);  // tag paths are searchable text
    }

}  // TEST_SUITE("search")
