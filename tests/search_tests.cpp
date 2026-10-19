// Search: query language, injection safety, hard filters, keyset and relevance paging, federation,
// set-based row hydration, and the in-memory matcher checked against the SQL compiler.
#include <doctest.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <map>
#include <random>
#include <set>

#include <nlohmann/json.hpp>

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
                                         1000 + index, (index % 4 == 3) ? 0 : 100'000 * (index + 1),
                                         index % 3 == 0 ? "flac" : "wav");
        if (index % 4 == 3) file.metadata->duration_us.reset();
        file.metadata->sample_rate = index % 2 ? 44100 : 48000;
        if (index % 5 == 4) file.metadata->sample_rate.reset();
        files.push_back(std::move(file));
    }
    return files;
}

/// Pages through the full result and returns sample ids in delivered order.
std::vector<std::int64_t> collect_all(catalog::Library& library, SearchRequest request) {
    catalog::Library* scope[] = {&library};
    std::vector<std::int64_t> ids;
    SearchService service;
    for (int guard = 0; guard < 1000; ++guard) {
        auto page = service.search(scope, request);
        REQUIRE(page.has_value());
        for (const auto& row : page->rows)
            ids.push_back(row.sample_id);
        if (!page->next) break;
        request.cursor = page->next;
    }
    return ids;
}

/// Ids of one big page (the reference order).
std::vector<std::int64_t> single_page(catalog::Library& library, SearchRequest request) {
    request.page_size = SearchService::kMaxPageSize;
    catalog::Library* scope[] = {&library};
    auto page = SearchService().search(scope, request);
    REQUIRE(page.has_value());
    REQUIRE_FALSE(page->next.has_value());
    std::vector<std::int64_t> ids;
    for (const auto& row : page->rows)
        ids.push_back(row.sample_id);
    return ids;
}

/// Exact count of a query (fails the test on error).
std::int64_t count_of(catalog::Library& library, std::string text, Scope scope = {}) {
    catalog::Library* libraries[] = {&library};
    auto counted = SearchService().count(libraries, SearchRequest{.text = std::move(text), .scope = std::move(scope)});
    REQUIRE(counted.has_value());
    return counted->total;
}

/// A file with metadata for the rich fixture.
catalog::ScannedFile rich_file(std::string path, std::int64_t size, std::optional<std::int64_t> duration_us,
                               std::string format, std::string codec, std::optional<std::int64_t> rate,
                               std::optional<std::int64_t> bits, std::optional<std::int64_t> filename_bpm = {}) {
    auto file = test::synthetic_file(std::move(path), size, duration_us.value_or(0), std::move(format));
    file.metadata->codec = std::move(codec);
    file.metadata->duration_us = duration_us;
    file.metadata->sample_rate = rate;
    file.metadata->bit_depth = bits;
    file.metadata->filename_bpm_milli = filename_bpm;
    return file;
}

/// A library with three roots (case-insensitive "Samples" with a missing file, offline "Other",
/// case-sensitive "Exact"), tags, categories, collections, ratings, notes, analysis results and
/// many unknown values: every predicate has hits, misses and unknowns. 13 samples.
struct RichLibrary {
    std::unique_ptr<catalog::Library> library;
    std::int64_t samples_root = 0;
    std::int64_t other_root = 0;
    std::int64_t exact_root = 0;
};

RichLibrary make_rich_library(const std::filesystem::path& directory) {
    RichLibrary rich;
    auto created = catalog::Library::create(directory, "Rich");
    REQUIRE(created.has_value());
    rich.library = std::move(*created);
    auto& library = *rich.library;

    auto samples = library.add_root("Samples", catalog::RootMode::Referenced, false);
    REQUIRE(samples.has_value());
    rich.samples_root = samples->id;
    REQUIRE(library.set_root_availability(samples->id, catalog::Availability::Online).has_value());
    std::vector<catalog::ScannedFile> files = {
        rich_file("Drums/Kicks/Kick 01.wav", 2'000, 1'200'000, "wav", "pcm_s16", 44100, 16, 120'180),
        rich_file("Drums/Kicks/Kick 02.wav", 900, 300'000, "wav", "pcm_s24", 48000, 24),
        rich_file("Drums/Snares/Snare_02.flac", 50'000, 2'300'000, "flac", "flac", 96000, 24, 128'000),
        rich_file("Loops/House/Loop 120.wav", 1'000, 2'000'000, "wav", "pcmxs16", 44100, 16, 119'500),
        rich_file("Ünïcode/ÄBC/Pad.wav", 3'000, std::nullopt, "wav", "pcm_s16", std::nullopt, std::nullopt),
        rich_file("a_b/x.wav", 10, 99'000, "wav", "", 22050, 8),
        rich_file("axb/y.wav", 1'500, 1'500'000, "aiff", "pcm_s16be", 44100, 16),
        rich_file("top.wav", 5'000'000, 61'000'000, "wav", "pcm_f32", 192000, 32),
        rich_file("Missing/gone.wav", 700, 400'000, "wav", "pcm_s16", 44100, 16),
    };
    files.push_back(catalog::ScannedFile{.relative_path = "Unknown/nothing.bin", .byte_size = 1, .mtime_ns = 1});
    auto session = library.begin_scan(samples->id);
    REQUIRE(session.has_value());
    REQUIRE(library.commit_scan_batch(*session, files, {}, [](std::string_view) { return false; }).has_value());
    REQUIRE(library.finish_scan(*session, false).has_value());
    // A second scan without "Missing/gone.wav" marks it missing.
    files.erase(std::ranges::find(files, std::string("Missing/gone.wav"), &catalog::ScannedFile::relative_path));
    auto rescan = library.begin_scan(samples->id);
    REQUIRE(rescan.has_value());
    REQUIRE(library.commit_scan_batch(*rescan, files, {}, [](std::string_view) { return false; }).has_value());
    REQUIRE(library.finish_scan(*rescan, false).has_value());

    auto other = library.add_root("Other", catalog::RootMode::Referenced, false);
    REQUIRE(other.has_value());
    rich.other_root = other->id;
    const std::vector<catalog::ScannedFile> other_files = {
        rich_file("Drums/Ride.wav", 4'000, 900'000, "wav", "pcm_s16", 44100, 16),
        rich_file("FX/Riser.mp3", 8'000, 9'000'000, "mp3", "mp3", 44100, std::nullopt, 140'000),
    };
    auto other_session = library.begin_scan(other->id);
    REQUIRE(other_session.has_value());
    REQUIRE(
        library.commit_scan_batch(*other_session, other_files, {}, [](std::string_view) { return false; }).has_value());
    REQUIRE(library.finish_scan(*other_session, false).has_value());
    REQUIRE(library.set_root_availability(other->id, catalog::Availability::Offline).has_value());

    auto exact = library.add_root("Exact", catalog::RootMode::Referenced, true);
    REQUIRE(exact.has_value());
    rich.exact_root = exact->id;
    REQUIRE(library.set_root_availability(exact->id, catalog::Availability::Online).has_value());
    const std::vector<catalog::ScannedFile> exact_files = {
        rich_file("CaseDir/Hit.wav", 600, 500'000, "wav", "pcm_s16", 44100, 16)};
    auto exact_session = library.begin_scan(exact->id);
    REQUIRE(exact_session.has_value());
    REQUIRE(
        library.commit_scan_batch(*exact_session, exact_files, {}, [](std::string_view) { return false; }).has_value());
    REQUIRE(library.finish_scan(*exact_session, false).has_value());

    const auto id = [&](std::string_view name) { return test::sample_id_named(library, name); };
    catalog::EditCommand kicks;
    kicks.sample_ids = {id("Kick 01"), id("Kick 02")};
    kicks.tags_add = {"Drums/Kick/Acoustic"};
    kicks.category = std::optional<std::string>("Drums/Kicks");
    kicks.rating = 4;
    REQUIRE(library.apply_edit(kicks).has_value());
    catalog::EditCommand snare;
    snare.sample_ids = {id("Snare_02"), id("Ride")};
    snare.tags_add = {"Drums/Snare", "Bright"};
    snare.category = std::optional<std::string>("Drums");
    snare.favorite = true;
    snare.rating = 2;
    REQUIRE(library.apply_edit(snare).has_value());
    catalog::EditCommand loop;
    loop.sample_ids = {id("Loop 120")};
    loop.category = std::optional<std::string>("Loops/House");
    loop.notes = "he said \"boom\" twice";
    loop.rating = 5;
    REQUIRE(library.apply_edit(loop).has_value());

    catalog::Library::AnalysisFacts kick_analysis{.sample_id = id("Kick 01"),
                                                  .content_revision = 1,
                                                  .key = MusicalKey{9, KeyMode::Minor},
                                                  .key_confidence = 0.8,
                                                  .key_algorithm = "test",
                                                  .key_requested = true};
    REQUIRE(library.record_analysis(kick_analysis).has_value());
    catalog::Library::AnalysisFacts pad_analysis{.sample_id = id("Pad"),
                                                 .content_revision = 1,
                                                 .bpm_milli = 90'000,
                                                 .bpm_confidence = 0.7,
                                                 .key = MusicalKey{0, KeyMode::Major},
                                                 .key_confidence = 0.6,
                                                 .tempo_algorithm = "test",
                                                 .key_algorithm = "test",
                                                 .tempo_requested = true,
                                                 .key_requested = true};
    REQUIRE(library.record_analysis(pad_analysis).has_value());

    catalog::TaxonomyCommand favorites{.kind = catalog::TaxonomyCommand::Kind::CreateCollection,
                                       .new_name = "Übersicht"};
    REQUIRE(library.apply_taxonomy(favorites).has_value());
    catalog::TaxonomyCommand flacs{.kind = catalog::TaxonomyCommand::Kind::CreateCollection,
                                   .new_name = "Flacs",
                                   .smart_query = std::string("format:flac")};
    REQUIRE(library.apply_taxonomy(flacs).has_value());
    auto collections = library.list_collections();
    REQUIRE(collections.has_value());
    const auto manual = std::ranges::find(*collections, std::string("Übersicht"), &catalog::CollectionRecord::name);
    REQUIRE(manual != collections->end());
    catalog::EditCommand collect;
    collect.sample_ids = {id("Pad"), id("top")};
    collect.add_to_collection = manual->id;
    REQUIRE(library.apply_edit(collect).has_value());
    return rich;
}

/// Every live sample row of a library, by id.
std::map<std::int64_t, SampleRow> all_rows(catalog::Library& library) {
    std::map<std::int64_t, SampleRow> rows;
    REQUIRE(for_each_row(library, [&](SampleRow&& row) { rows.emplace(row.sample_id, std::move(row)); }).has_value());
    return rows;
}

/// Collection id by name (fails the test when absent).
std::int64_t collection_named(catalog::Library& library, std::string_view name) {
    auto collections = library.list_collections();
    REQUIRE(collections.has_value());
    const auto found = std::ranges::find(*collections, std::string(name), &catalog::CollectionRecord::name);
    REQUIRE(found != collections->end());
    return found->id;
}

}  // namespace

TEST_SUITE("search") {
    TEST_CASE("valid queries parse into the expected tree") {
        for (const auto text :
             {"kick", "\"metallic impact\" tag:Drums/Kick", "bpm:140..155 key:F#minor duration:<2s available:true",
              "format:flac rating:>=4 favorite:true", "(tag:Drums OR tag:Percussion) AND NOT tag:Acoustic",
              "folder:\"Packs/Goa\" category:Bass analyzed:tempo", "kic*", "name:kick size:>1MB rate:44.1k", "\"a*b\"",
              "analyzed:false", "analyzed:none", "key:F♯m", "duration:1.5min"}) {
            const std::string_view shown_text = text;
            CAPTURE(shown_text);
            CHECK(parse_query(text).has_value());
        }
        auto query = parse_query("a b OR c");
        REQUIRE(query.has_value());
        CHECK(query->root->kind == QueryNode::Kind::Or);  // OR binds weaker than implicit AND
        CHECK(query->root->children.front()->kind == QueryNode::Kind::And);
        CHECK(parse_query("").has_value());
        CHECK(parse_query("").value().empty());
        auto phrase = parse_query("\"a*b\"");
        CHECK(phrase->root->text.text == "a*b");  // '*' is literal inside a phrase
        CHECK_FALSE(phrase->root->text.prefix);
    }

    TEST_CASE("numeric values become inclusive bounds at the typed precision") {
        struct Case {
            std::string_view text;
            std::int64_t low;
            std::int64_t high;
        };
        for (const auto& [text, low, high] :
             {Case{"bpm:120", 119'500, 120'499}, Case{"bpm:120.0", 119'950, 120'049},
              Case{"bpm:120.18", 120'175, 120'184}, Case{"bpm:120..130", 119'500, 130'499},
              Case{"duration:2s", 1'500'000, 2'499'999}, Case{"duration:2000ms", 1'999'500, 2'000'499},
              Case{"duration:1.5min", 87'000'000, 92'999'999}, Case{"rate:44.1k", 44'050, 44'149},
              Case{"rate:48000", 48'000, 48'000}, Case{"size:1kb", 500, 1'499}, Case{"rating:3", 3, 3},
              Case{"channels:1..2", 1, 2}}) {
            const std::string_view shown_text = text;
            CAPTURE(shown_text);
            auto query = parse_query(text);
            REQUIRE(query.has_value());
            CHECK(query->root->predicate.low == low);
            CHECK(query->root->predicate.high == high);
        }
        CHECK(parse_query("bpm:>120")->root->predicate.low == 120'001);
        CHECK(parse_query("bpm:>=120.5")->root->predicate.low == 120'500);
        CHECK(parse_query("bpm:<120")->root->predicate.high == 119'999);
        CHECK(parse_query("bpm:<=120")->root->predicate.high == 120'000);
        CHECK(parse_query("duration:<0.0000015s")->root->predicate.high == 1);  // value < 1.5 µs
    }

    TEST_CASE("invalid queries fail with a code-point position and run nothing") {
        struct Case {
            std::string_view text;
            std::uint32_t position;
        };
        for (const auto& [text, position] : {Case{"bpm:fast", 4},
                                             Case{"duration:<2parsecs", 11},
                                             Case{"\"unterminated", 0},
                                             Case{"(kick", 0},
                                             Case{"kick)", 4},
                                             Case{"colour:red", 0},
                                             Case{"k*", 0},
                                             Case{"שלום bpm:x", 9},
                                             Case{"rating:9", 7},
                                             Case{"a OR", 2},
                                             Case{"kick AND AND snare", 5},
                                             Case{")", 0},
                                             Case{"()", 1},
                                             Case{"\"!!!\"", 0},
                                             Case{"!!!", 0},
                                             Case{"rating:3.5", 7},
                                             Case{"channels:2.0", 9},
                                             Case{"bits:<1.5", 6},
                                             Case{"bpm:5..2", 4},
                                             Case{"bpm:99999999999999999999", 4},
                                             Case{"format:.", 7},
                                             Case{"analyzed:maybe", 9}}) {
            const std::string_view shown_text = text;
            CAPTURE(shown_text);
            auto query = parse_query(text);
            REQUIRE_FALSE(query.has_value());
            CHECK(query.error().code == ErrorCode::ParseError);
            REQUIRE(query.error().position.has_value());
            CHECK(*query.error().position == position);
        }
        CHECK(parse_query(")").error().safe_detail == "unmatched ')'");
        CHECK(parse_query("()").error().safe_detail == "empty parentheses");
        std::string deep;
        for (int level = 0; level < 20; ++level)
            deep += "(";
        deep += "x";
        for (int level = 0; level < 20; ++level)
            deep += ")";
        CHECK_FALSE(parse_query(deep).has_value());
        std::string negations;
        for (int level = 0; level < 1000; ++level)  // 4,001 characters: inside the length limit
            negations += "NOT ";
        negations += "x";
        auto deep_not = parse_query(negations);  // bounded recursion, no stack exhaustion
        REQUIRE_FALSE(deep_not.has_value());
        CHECK(deep_not.error().safe_detail == "query is nested more than 16 levels");
        std::string sixteen_negations;
        for (int level = 0; level < 16; ++level)
            sixteen_negations += "NOT ";
        CHECK(parse_query(sixteen_negations + "x").has_value());
        CHECK_FALSE(parse_query(std::string(5000, 'a')).has_value());
    }

    TEST_CASE("user text is never interpreted as SQL or FTS syntax") {
        ScratchDirectory scratch;
        auto populated = test::make_library(scratch / "lib", paging_fixture());
        catalog::Library* scope[] = {populated.library.get()};
        struct Case {
            std::string_view text;
            bool parse_error;  ///< rejected before anything runs; otherwise runs and finds nothing
        };
        for (const auto& [text, parse_error] :
             {Case{"'; DROP TABLE samples; --", true}, Case{"\"a\" OR 1=1", false}, Case{"kick NEAR(snare)", false},
              Case{"tag:\"x') OR 1=1 --\"", false}, Case{"name:\"*\"", true}, Case{"folder:\"%\"", false},
              Case{"codec:%", false}, Case{"collection:\"x' OR '1'='1\"", false}, Case{"\"kick\" OR \"\"\"", true},
              Case{"category:\"a/b') --\"", false}}) {
            const std::string_view shown_text = text;
            CAPTURE(shown_text);
            auto page = SearchService().search(scope, SearchRequest{.text = std::string(text)});
            if (parse_error) {
                REQUIRE_FALSE(page.has_value());
                CHECK(page.error().code == ErrorCode::ParseError);
            } else {
                REQUIRE(page.has_value());
                CHECK(page->rows.empty());
            }
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

    TEST_CASE("equality matches within the typed precision and units scale values") {
        ScratchDirectory scratch;
        auto rich = make_rich_library(scratch / "lib");
        auto& library = *rich.library;
        const auto names = [&](std::string text) {
            catalog::Library* scope[] = {&library};
            auto page = SearchService().search(scope, SearchRequest{.text = std::move(text), .page_size = 500});
            REQUIRE(page.has_value());
            std::set<std::string> found;
            for (const auto& row : page->rows)
                found.insert(row.display_name);
            return found;
        };
        // Kick 01 has 120.18 BPM from its file name; Loop 120 has 119.5.
        CHECK(names("bpm:120") == std::set<std::string>{"Kick 01", "Loop 120"});
        CHECK(names("bpm:120.2") == std::set<std::string>{"Kick 01"});
        CHECK(names("bpm:120.18") == std::set<std::string>{"Kick 01"});
        CHECK(names("bpm:120.0").empty());
        CHECK(names("bpm:120bpm") == names("bpm:120"));
        CHECK(names("duration:2s") == std::set<std::string>{"Snare_02", "Loop 120", "y"});
        CHECK(names("duration:2.3s") == std::set<std::string>{"Snare_02"});
        CHECK(names("duration:2300ms") == std::set<std::string>{"Snare_02"});
        CHECK(names("duration:2.0s") == std::set<std::string>{"Loop 120"});
        CHECK(names("duration:<100ms") == std::set<std::string>{"x"});
        CHECK(names("duration:1min..2min") == std::set<std::string>{"top"});
        CHECK(names("size:>=1kb size:<=2kb") == std::set<std::string>{"Kick 01", "Loop 120", "y"});
        CHECK(names("size:1kib") == std::set<std::string>{"Kick 02", "Loop 120", "y", "gone", "Hit"});  // 512..1535 B
        CHECK(names("size:1.0kib") == std::set<std::string>{"Loop 120"});                               // 973..1075 B
        CHECK(names("size:5mb") == std::set<std::string>{"top"});
        CHECK(names("rate:44.1k") == names("rate:44100hz"));
        CHECK(names("rate:96khz") == std::set<std::string>{"Snare_02"});
        CHECK(names("bits:8") == std::set<std::string>{"x"});
    }

    TEST_CASE("LIKE wildcards in codec values and folder names match literally") {
        ScratchDirectory scratch;
        auto rich = make_rich_library(scratch / "lib");
        auto& library = *rich.library;
        CHECK(count_of(library, "codec:pcm_") == 8);  // pcm_s16, pcm_s24, pcm_s16be, pcm_f32; not pcmxs16
        CHECK(count_of(library, "codec:PCM_S16") == 6);
        CHECK(count_of(library, "codec:pcm%") == 0);
        CHECK(count_of(library, "folder:a_b") == 1);
        CHECK(count_of(library, "folder:a%") == 0);
    }

    TEST_CASE("column searches and phrases with embedded quotes") {
        ScratchDirectory scratch;
        auto rich = make_rich_library(scratch / "lib");
        auto& library = *rich.library;
        CHECK(count_of(library, "name:kick") == 2);
        CHECK(count_of(library, "path:kicks") == 2);
        CHECK(count_of(library, "path:loops") == 1);
        CHECK(count_of(library, "notes:boom") == 1);
        CHECK(count_of(library, "notes:\"said \\\"boom\\\"\"") == 1);
        CHECK(count_of(library, "notes:\"boom said\"") == 0);
        CHECK(count_of(library, "name:kic*") == 2);
        CHECK(count_of(library, "acoustic") == 2);  // tag paths are searchable text
    }

    TEST_CASE("unknown values: X and NOT X partition the library") {
        ScratchDirectory scratch;
        auto rich = make_rich_library(scratch / "lib");
        auto& library = *rich.library;
        const auto total = library.sample_count().value_or(-1);
        REQUIRE(total == 13);
        for (const auto text : {"bpm:>125",
                                "bpm:120",
                                "duration:<1s",
                                "category:Drums",
                                "category:Drums/Kicks",
                                "key:Am",
                                "key:unknown",
                                "format:wav",
                                "codec:pcm",
                                "rate:44.1k",
                                "bits:16",
                                "size:>1kb",
                                "folder:Drums",
                                "tag:Kick",
                                "tag:Drums/Kick",
                                "collection:Flacs",
                                "available:true",
                                "analyzed:tempo",
                                "analyzed:none",
                                "favorite:true",
                                "rating:>=3",
                                "bpm:>100 OR duration:<1s",
                                "kick"}) {
            const std::string_view shown_text = text;
            CAPTURE(shown_text);
            CHECK(count_of(library, text) + count_of(library, std::format("NOT ({})", text)) == total);
        }
        CHECK(count_of(library, "NOT bpm:>125") == total - count_of(library, "bpm:>125"));
        CHECK(count_of(library, "NOT category:Drums") == 9);  // uncategorized samples included
    }

    TEST_CASE("keyset paging visits every row exactly once in order, nulls last, both directions") {
        ScratchDirectory scratch;
        auto populated = test::make_library(scratch / "lib", paging_fixture());
        auto& library = *populated.library;
        // Ratings and keys give the remaining sorts real values (and unknown keys).
        for (int index = 1; index <= 12; ++index) {
            catalog::EditCommand rating;
            rating.sample_ids = {index};
            rating.rating = index % 6;
            REQUIRE(library.apply_edit(rating).has_value());
            if (index % 3 == 0) continue;
            catalog::EditCommand key;
            key.sample_ids = {index};
            key.key = catalog::KeyOverrideEdit{catalog::OverrideState::Value,
                                               MusicalKey{index % 12, index % 2 ? KeyMode::Minor : KeyMode::Major}};
            REQUIRE(library.apply_edit(key).has_value());
        }
        for (const bool descending : {false, true}) {
            for (const auto sort :
                 {SortField::Duration, SortField::Name, SortField::Size, SortField::Bpm, SortField::Key,
                  SortField::Rating, SortField::Format, SortField::Added, SortField::SampleRate}) {
                CAPTURE(descending);
                CAPTURE(to_string(sort));
                const SearchRequest request{.sort = sort, .descending = descending, .page_size = 5};
                const auto paged = collect_all(library, request);
                CHECK(paged.size() == 12);
                CHECK(std::set<std::int64_t>(paged.begin(), paged.end()).size() == 12);
                CHECK(paged == single_page(library, request));  // cross-page order equals one-page order
                const auto paged_by_three =
                    collect_all(library, SearchRequest{request.text, request.scope, sort, descending, 3, std::nullopt});
                CHECK(paged_by_three == paged);
            }
            catalog::Library* scope[] = {&library};
            for (const auto sort : {SortField::Duration, SortField::SampleRate, SortField::Rating, SortField::Key}) {
                CAPTURE(to_string(sort));
                auto all = SearchService().search(
                    scope, SearchRequest{.sort = sort, .descending = descending, .page_size = 500});
                REQUIRE(all.has_value());
                const auto value = [&](const SampleRow& row) -> std::optional<std::int64_t> {
                    switch (sort) {
                        case SortField::Duration:
                            return row.duration_us;
                        case SortField::SampleRate:
                            return row.sample_rate;
                        case SortField::Rating:
                            return row.rating;
                        default:
                            if (!row.key) return std::nullopt;
                            return row.key->pitch_class * 2 + (row.key->mode == KeyMode::Minor ? 1 : 0);
                    }
                };
                for (std::size_t index = 0; index + 1 < all->rows.size(); ++index) {
                    const auto current = value(all->rows[index]);
                    const auto next = value(all->rows[index + 1]);
                    if (!current)
                        CHECK_FALSE(next.has_value());  // nulls form a trailing partition
                    else if (next)
                        CHECK((descending ? *current >= *next : *current <= *next));
                }
            }
            auto by_format = SearchService().search(
                scope, SearchRequest{.sort = SortField::Format, .descending = descending, .page_size = 500});
            REQUIRE(by_format.has_value());
            for (std::size_t index = 0; index + 1 < by_format->rows.size(); ++index)
                CHECK((descending ? by_format->rows[index].format >= by_format->rows[index + 1].format
                                  : by_format->rows[index].format <= by_format->rows[index + 1].format));
        }
    }

    TEST_CASE("stale, foreign and malformed cursors are rejected as STALE_CURSOR, never thrown") {
        ScratchDirectory scratch;
        auto first_library = test::make_library(scratch / "a", paging_fixture(), "A");
        auto second_library = test::make_library(scratch / "b", paging_fixture(), "B");
        catalog::Library* one[] = {first_library.library.get()};
        catalog::Library* both[] = {first_library.library.get(), second_library.library.get()};
        auto first = SearchService().search(one, SearchRequest{.text = "kick", .page_size = 2});
        REQUIRE(first.has_value());
        REQUIRE(first->next.has_value());

        const auto expect_stale = [&](std::span<catalog::Library* const> libraries, SearchRequest request) {
            auto page = SearchService().search(libraries, request);
            REQUIRE_FALSE(page.has_value());
            CHECK(page.error().code == ErrorCode::StaleCursor);
        };
        expect_stale(one, SearchRequest{.text = "snare", .page_size = 2, .cursor = first->next});
        expect_stale(one, SearchRequest{.text = "kick", .descending = true, .page_size = 2, .cursor = first->next});
        expect_stale(one, SearchRequest{
                              .text = "kick", .scope = {.uncategorized = true}, .page_size = 2, .cursor = first->next});
        expect_stale(both,
                     SearchRequest{.text = "kick", .page_size = 2, .cursor = first->next});  // library set changed

        const auto valid = nlohmann::json::parse(first->next->token);
        std::vector<std::string> malformed = {"", "not json", "[]", "{}", "{\"v\":2}", "{\"v\":\"2\",\"q\":1}"};
        const auto mutate = [&](auto&& change) {
            nlohmann::json document = valid;
            change(document);
            malformed.push_back(document.dump());
        };
        mutate([](nlohmann::json& document) { document["v"] = 1; });
        mutate([](nlohmann::json& document) { document["libraries"] = "x"; });
        mutate([](nlohmann::json& document) {
            for (auto& [library, state] : document["libraries"].items())
                state["p"] = "zero";
        });
        mutate([](nlohmann::json& document) {
            for (auto& [library, state] : document["libraries"].items())
                state["p"] = 7;
        });
        mutate([](nlohmann::json& document) {
            for (auto& [library, state] : document["libraries"].items())
                state.erase("id");
        });
        mutate([](nlohmann::json& document) {
            for (auto& [library, state] : document["libraries"].items())
                state["v"] = nlohmann::json::array();
        });
        mutate([](nlohmann::json& document) { document["offset"] = -3; });
        for (const auto& token : malformed) {
            CAPTURE(token);
            expect_stale(one, SearchRequest{.text = "kick", .page_size = 2, .cursor = Cursor{token}});
        }
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
        for (std::size_t index = 0; index + 1 < rows.size(); ++index) {
            const auto& current = rows[index].duration_us;
            const auto& next = rows[index + 1].duration_us;
            if (!current)
                CHECK_FALSE(next.has_value());
            else if (next)
                CHECK(*current <= *next);
        }
        auto count = service.count(scope, SearchRequest{.text = "kick"});
        REQUIRE(count.has_value());
        CHECK(count->total == 12);
        auto invalid = service.count(scope, SearchRequest{.text = "collection:Nope", .scope = {.folder = "x"}});
        REQUIRE_FALSE(invalid.has_value());  // a request error fails the whole call
        CHECK(invalid.error().code == ErrorCode::InvalidArgument);
    }

    TEST_CASE("relevance ranks better matches first and fuses libraries") {
        ScratchDirectory scratch;
        std::vector<catalog::ScannedFile> files = {test::synthetic_file("kick kick kick.wav", 1, 1),
                                                   test::synthetic_file("kick snare.wav", 2, 1),
                                                   test::synthetic_file("snare only.wav", 3, 1)};
        auto populated = test::make_library(scratch / "lib", files);
        auto other = test::make_library(scratch / "other", files, "Other");
        catalog::Library* scope[] = {populated.library.get()};
        auto page = SearchService().search(scope, SearchRequest{.text = "kick", .sort = SortField::Relevance});
        REQUIRE(page.has_value());
        REQUIRE(page->rows.size() == 2);
        CHECK(page->rows.front().display_name == "kick kick kick");
        CHECK(page->rows.front().relevance > page->rows.back().relevance);
        CHECK_FALSE(page->relevance_window_limited);

        catalog::Library* both[] = {populated.library.get(), other.library.get()};
        auto fused = SearchService().search(both, SearchRequest{.text = "kick", .sort = SortField::Relevance});
        REQUIRE(fused.has_value());
        REQUIRE(fused->rows.size() == 4);
        CHECK(fused->rows[0].display_name == "kick kick kick");
        CHECK(fused->rows[1].display_name == "kick kick kick");
        CHECK(fused->rows[0].library != fused->rows[1].library);
    }

    TEST_CASE("relevance keeps matches of non-text OR branches, ranked after text hits") {
        ScratchDirectory scratch;
        std::vector<catalog::ScannedFile> files = {
            test::synthetic_file("kick one.wav", 1, 1), test::synthetic_file("kick kick two.wav", 2, 1),
            test::synthetic_file("snare.wav", 3, 1), test::synthetic_file("pad.wav", 4, 1)};
        auto populated = test::make_library(scratch / "lib", files);
        auto& library = *populated.library;
        catalog::EditCommand tag;
        tag.sample_ids = {test::sample_id_named(library, "snare")};
        tag.tags_add = {"Drums"};
        REQUIRE(library.apply_edit(tag).has_value());
        catalog::Library* scope[] = {&library};

        const auto relevance_names = [&](std::string text) {
            auto page =
                SearchService().search(scope, SearchRequest{.text = std::move(text), .sort = SortField::Relevance});
            REQUIRE(page.has_value());
            std::vector<std::string> names;
            for (const auto& row : page->rows)
                names.push_back(row.display_name);
            return names;
        };
        CHECK(relevance_names("kick OR tag:Drums") == std::vector<std::string>{"kick kick two", "kick one", "snare"});
        CHECK(relevance_names("kick OR NOT snare") == std::vector<std::string>{"kick kick two", "kick one", "pad"});
        CHECK(relevance_names("kick tag:Drums").empty());
        CHECK(relevance_names("kick OR tag:Drums").size() ==
              static_cast<std::size_t>(count_of(library, "kick OR tag:Drums")));
        auto page =
            SearchService().search(scope, SearchRequest{.text = "kick OR tag:Drums", .sort = SortField::Relevance});
        CHECK(page->rows.back().relevance == 0.0);  // matched only through the tag branch
    }

    TEST_CASE("relevance pages through every match once and reports a full window") {
        ScratchDirectory scratch;
        std::vector<catalog::ScannedFile> files;
        for (int index = 0; index < 1001; ++index)
            files.push_back(test::synthetic_file(std::format("hit {:04}.wav", index), 10 + index, 1000));
        auto populated = test::make_library(scratch / "lib", files);
        auto& library = *populated.library;
        catalog::Library* scope[] = {&library};

        const SearchRequest request{.text = "hit", .sort = SortField::Relevance, .page_size = 300};
        auto first = SearchService().search(scope, request);
        REQUIRE(first.has_value());
        CHECK(first->relevance_window_limited);  // 1,001 matches, window of 1,000
        const auto paged = collect_all(library, request);
        CHECK(paged.size() == SearchService::kRelevanceDepth);
        CHECK(std::set<std::int64_t>(paged.begin(), paged.end()).size() == paged.size());

        const SearchRequest smaller{
            .text = "hit 0001 OR hit 0002 OR hit 0003", .sort = SortField::Relevance, .page_size = 1};
        auto window = SearchService().search(scope, smaller);
        REQUIRE(window.has_value());
        CHECK_FALSE(window->relevance_window_limited);
        CHECK(collect_all(library, smaller).size() == 3);
        // A non-text OR branch ranks the whole filtered set, so the window applies to it too.
        auto mixed =
            SearchService().search(scope, SearchRequest{.text = "hit OR format:wav", .sort = SortField::Relevance});
        REQUIRE(mixed.has_value());
        CHECK(mixed->relevance_window_limited);
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
        CHECK(service.search(scope, {.text = "folder:set/"})->rows.size() == 12);
        CHECK(service.search(scope, {.text = "folder:Samples/set", .page_size = 500})->rows.size() == 12);
        CHECK(service.search(scope, {.text = "folder:Samples"})->rows.size() == 12);
        CHECK(service.search(scope, {.text = "folder:se"})->rows.empty());  // whole segments only
        CHECK(service.search(scope, {.text = "tag:Nope/Missing"})->rows.empty());
        CHECK(service.search(scope, {.text = "acoustic"})->rows.size() == 2);  // tag paths are searchable text
        auto invalid_path = service.search(scope, {.text = "tag:Drums//Kick"});
        REQUIRE_FALSE(invalid_path.has_value());
        CHECK(invalid_path.error().code == ErrorCode::InvalidArgument);
    }

    TEST_CASE("folder and collection names compare with Unicode case folding") {
        ScratchDirectory scratch;
        auto rich = make_rich_library(scratch / "lib");
        auto& library = *rich.library;
        CHECK(count_of(library, "folder:ünïcode") == 1);  // "Ünïcode/ÄBC" folder
        CHECK(count_of(library, "folder:ünïcode/äbc") == 1);
        CHECK(count_of(library, "folder:äbc") == 0);          // folders are matched from the root down
        CHECK(count_of(library, "folder:Drums/kicks") == 2);  // case-insensitive root
        CHECK(count_of(library, "folder:CaseDir") == 1);      // case-sensitive root: exact case only
        CHECK(count_of(library, "folder:casedir") == 0);
        CHECK(count_of(library, "folder:exact/CaseDir") == 1);        // root names always fold
        CHECK(count_of(library, "folder:\"samples/ÜNÏCODE\"") == 1);  // root name + folder
        CHECK(count_of(library, "collection:übersicht") == 2);
        CHECK(count_of(library, "collection:\"ÜBERSICHT\"") == 2);
        CHECK(count_of(library, "collection:flacs") == count_of(library, "format:flac"));
        CHECK(count_of(library, "collection:nothing") == 0);
    }

    TEST_CASE("every scope field restricts results; a folder scope needs its root") {
        ScratchDirectory scratch;
        auto rich = make_rich_library(scratch / "lib");
        auto& library = *rich.library;
        CHECK(count_of(library, "", {.root_id = rich.samples_root}) == 10);
        CHECK(count_of(library, "", {.root_id = rich.other_root}) == 2);
        CHECK(count_of(library, "", {.root_id = rich.samples_root, .folder = "Drums"}) == 3);
        CHECK(count_of(library, "", {.root_id = rich.samples_root, .folder = "drums/kicks/"}) == 2);
        CHECK(count_of(library, "", {.root_id = rich.other_root, .folder = "Drums"}) == 1);
        CHECK(count_of(library, "", {.root_id = rich.samples_root, .folder = ""}) == 10);
        CHECK(count_of(library, "", {.collection_id = collection_named(library, "Übersicht")}) == 2);
        CHECK(count_of(library, "", {.collection_id = collection_named(library, "Flacs")}) == 1);
        CHECK(count_of(library, "", {.collection_id = 9999}) == 0);
        const auto drums = library.ensure_tag("Drums");
        REQUIRE(drums.has_value());
        CHECK(count_of(library, "", {.tag_id = *drums}) == 4);
        const auto drum_category = library.ensure_category("Drums");
        REQUIRE(drum_category.has_value());
        CHECK(count_of(library, "", {.category_id = *drum_category}) == 4);
        CHECK(count_of(library, "", {.uncategorized = true}) == 8);
        CHECK(count_of(library, "", {.root_id = rich.exact_root, .folder = "CaseDir"}) == 1);
        CHECK(count_of(library, "", {.root_id = rich.exact_root, .folder = "casedir"}) == 0);
        CHECK(count_of(library, "rating:>=4", {.root_id = rich.samples_root, .folder = "Drums"}) == 2);

        catalog::Library* scope[] = {&library};
        auto without_root = SearchService().search(scope, SearchRequest{.scope = {.folder = "Drums"}});
        REQUIRE_FALSE(without_root.has_value());
        CHECK(without_root.error().code == ErrorCode::InvalidArgument);
    }

    TEST_CASE("smart collections expand, but nesting and size are bounded") {
        ScratchDirectory scratch;
        auto rich = make_rich_library(scratch / "lib");
        auto& library = *rich.library;
        const auto create = [&](std::string name, std::string query) {
            catalog::TaxonomyCommand command{.kind = catalog::TaxonomyCommand::Kind::CreateCollection,
                                             .new_name = std::move(name),
                                             .smart_query = std::move(query)};
            REQUIRE(library.apply_taxonomy(command).has_value());
        };
        create("Nested", "collection:Flacs OR tag:Kick");
        CHECK(count_of(library, "collection:Nested") == 3);
        create("Loop", "collection:Loop");
        catalog::Library* scope[] = {&library};
        auto loop = SearchService().search(scope, SearchRequest{.text = "collection:Loop"});
        REQUIRE_FALSE(loop.has_value());
        CHECK(loop.error().code == ErrorCode::InvalidArgument);

        std::string sixteen = "collection:C0";
        for (int index = 1; index < 16; ++index)
            sixteen += " OR collection:C0";
        create("C0", "format:wav");
        create("C1", sixteen);
        std::ranges::replace(sixteen, '0', '1');
        create("C2", sixteen);
        std::ranges::replace(sixteen, '1', '2');
        create("C3", sixteen);
        CHECK(count_of(library, "collection:C2") == count_of(library, "format:wav"));  // 16 × 16 terms fit
        auto exploded = SearchService().search(scope, SearchRequest{.text = "collection:C3"});
        REQUIRE_FALSE(exploded.has_value());  // 16³ terms do not
        CHECK(exploded.error().code == ErrorCode::InvalidArgument);
        auto scoped =
            SearchService().count(scope, SearchRequest{.scope = {.collection_id = collection_named(library, "C3")}});
        REQUIRE_FALSE(scoped.has_value());
        CHECK(scoped.error().code == ErrorCode::InvalidArgument);
    }

    TEST_CASE("analyzed: flags and availability follow the primary location") {
        ScratchDirectory scratch;
        auto rich = make_rich_library(scratch / "lib");
        auto& library = *rich.library;
        CHECK(count_of(library, "analyzed:key") == 2);
        CHECK(count_of(library, "analyzed:tempo") == 1);
        CHECK(count_of(library, "analyzed:any") == 2);
        CHECK(count_of(library, "analyzed:false") == 11);
        CHECK(count_of(library, "analyzed:none") == count_of(library, "NOT analyzed:any"));
        CHECK(count_of(library, "available:true") == 10);
        CHECK(count_of(library, "available:false") == 3);  // one missing, two on an offline root
        const auto rows = all_rows(library);
        std::map<RowAvailability, int> availability;
        for (const auto& [id, row] : rows)
            ++availability[row.availability];
        CHECK(availability[RowAvailability::Available] == 10);
        CHECK(availability[RowAvailability::Missing] == 1);
        CHECK(availability[RowAvailability::SourceOffline] == 2);
    }

    TEST_CASE("load_rows hydrates set-based in the requested order") {
        ScratchDirectory scratch;
        auto rich = make_rich_library(scratch / "lib");
        auto& library = *rich.library;
        const auto kick = test::sample_id_named(library, "Kick 01");
        const auto ride = test::sample_id_named(library, "Ride");
        const auto top = test::sample_id_named(library, "top");
        const std::vector<std::int64_t> wanted = {ride, 99'999, kick, ride, top};
        auto rows = load_rows(library, wanted);
        REQUIRE(rows.has_value());
        REQUIRE(rows->size() == 3);
        CHECK((*rows)[0].sample_id == ride);
        CHECK((*rows)[1].sample_id == kick);
        CHECK((*rows)[2].sample_id == top);

        const auto& kick_row = (*rows)[1];
        CHECK(kick_row.library == library.uuid());
        CHECK(kick_row.root_id == rich.samples_root);
        CHECK(kick_row.root_name == "Samples");
        CHECK(kick_row.relative_path == "Drums/Kicks/Kick 01.wav");
        CHECK(kick_row.folder_path == "Drums/Kicks");
        CHECK(kick_row.file_name == "Kick 01.wav");
        CHECK(kick_row.category == "Drums/Kicks");
        REQUIRE(kick_row.category_id.has_value());
        CHECK(kick_row.tag_paths == std::vector<std::string>{"Drums/Kick/Acoustic"});
        CHECK(kick_row.tags == "Drums/Kick/Acoustic");
        CHECK(kick_row.tag_ids == std::vector<std::int64_t>{*library.ensure_tag("Drums/Kick/Acoustic")});
        CHECK(kick_row.rating == 4);
        CHECK(kick_row.bpm_milli == 120'180);
        CHECK(kick_row.key == MusicalKey{9, KeyMode::Minor});
        CHECK(kick_row.availability == RowAvailability::Available);
        CHECK((*rows)[0].availability == RowAvailability::SourceOffline);
        CHECK((*rows)[0].root_name == "Other");
        CHECK((*rows)[0].tag_paths == std::vector<std::string>{"Bright", "Drums/Snare"});
        REQUIRE((*rows)[0].tag_ids.size() == 2);
        CHECK((*rows)[0].tag_ids[0] == *library.ensure_tag("Bright"));
        CHECK((*rows)[0].tag_ids[1] == *library.ensure_tag("Drums/Snare"));
        CHECK((*rows)[2].folder_path.empty());
        CHECK(load_rows(library, std::vector<std::int64_t>{})->empty());

        catalog::EditCommand remove;
        remove.sample_ids = {kick};
        remove.removed = true;
        REQUIRE(library.apply_edit(remove).has_value());
        CHECK(load_rows(library, std::vector<std::int64_t>{kick})->empty());  // removed samples are absent
    }

    TEST_CASE("for_each_row streams every live row, identical to load_rows") {
        ScratchDirectory scratch;
        auto rich = make_rich_library(scratch / "lib");
        auto& library = *rich.library;
        const auto rows = all_rows(library);
        CHECK(static_cast<std::int64_t>(rows.size()) == library.sample_count().value_or(-1));
        std::vector<std::int64_t> ids;
        for (const auto& [id, row] : rows)
            ids.push_back(id);
        auto loaded = load_rows(library, ids);
        REQUIRE(loaded.has_value());
        REQUIRE(loaded->size() == rows.size());
        for (const auto& row : *loaded) {
            const auto& streamed = rows.at(row.sample_id);
            CAPTURE(row.display_name);
            CHECK(streamed.relative_path == row.relative_path);
            CHECK(streamed.root_id == row.root_id);
            CHECK(streamed.tag_ids == row.tag_ids);
            CHECK(streamed.tag_paths == row.tag_paths);
            CHECK(streamed.category == row.category);
            CHECK(streamed.availability == row.availability);
            CHECK(streamed.bpm_milli == row.bpm_milli);
            CHECK(streamed.analysis_flags == row.analysis_flags);
        }
        auto page = SearchService().search(std::array{&library}, SearchRequest{.page_size = 500});
        REQUIRE(page.has_value());
        for (const auto& row : page->rows)
            CHECK(rows.at(row.sample_id).tags == row.tags);  // search pages hydrate the same way
    }

    TEST_CASE("matching_ids returns exactly the matches in ascending id order") {
        ScratchDirectory scratch;
        auto rich = make_rich_library(scratch / "lib");
        auto& library = *rich.library;
        for (const auto text : {"", "kick", "kick OR tag:Bright", "NOT format:wav", "drums folder:Drums"}) {
            const std::string_view shown_text = text;
            CAPTURE(shown_text);
            auto ids = matching_ids(library, SearchRequest{.text = text});
            REQUIRE(ids.has_value());
            CHECK(std::ranges::is_sorted(*ids));
            auto expected = single_page(library, SearchRequest{.text = text});
            std::ranges::sort(expected);
            CHECK(*ids == expected);
        }
        auto scoped = matching_ids(library, SearchRequest{.scope = {.root_id = rich.other_root}});
        REQUIRE(scoped.has_value());
        CHECK(scoped->size() == 2);
        auto invalid = matching_ids(library, SearchRequest{.text = "bpm:"});
        REQUIRE_FALSE(invalid.has_value());
        CHECK(invalid.error().code == ErrorCode::ParseError);
    }

    TEST_CASE("the in-memory matcher agrees with the SQL compiler") {
        ScratchDirectory scratch;
        auto rich = make_rich_library(scratch / "lib");
        auto& library = *rich.library;
        const auto rows = all_rows(library);
        for (const auto text : {"",
                                "tag:Kick",
                                "tag:kick",
                                "tag:Bright",
                                "NOT tag:Kick",
                                "category:Drums",
                                "category:drums/KICKS",
                                "category:Loops/House",
                                "category:Kicks",
                                "NOT category:Drums",
                                "folder:Drums",
                                "folder:samples/drums",
                                "folder:Samples",
                                "folder:Other",
                                "folder:ÜNÏCODE/äbc",
                                "folder:a_b",
                                "NOT folder:Loops",
                                "folder:Drums/Kicks/",
                                "folder:CaseDir",
                                "folder:casedir",
                                "folder:EXACT",
                                "folder:Exact/CaseDir",
                                "bpm:120",
                                "bpm:120.2",
                                "bpm:120.0",
                                "bpm:>125",
                                "NOT bpm:>125",
                                "bpm:100..130",
                                "duration:2s",
                                "duration:<1s",
                                "NOT duration:<1s",
                                "duration:100ms..3s",
                                "rating:>=3",
                                "rating:0",
                                "rating:<2",
                                "favorite:true",
                                "favorite:false",
                                "format:flac",
                                "format:wav",
                                "NOT format:wav",
                                "codec:pcm_",
                                "codec:PCM_S16",
                                "rate:44.1k",
                                "rate:48000",
                                "channels:2",
                                "bits:24",
                                "bits:>=24",
                                "size:>1kb",
                                "size:1kb",
                                "available:true",
                                "available:false",
                                "analyzed:tempo",
                                "analyzed:none",
                                "analyzed:any",
                                "key:Am",
                                "key:C",
                                "key:unknown",
                                "NOT key:Am",
                                "(tag:Kick OR format:flac) AND NOT rating:0",
                                "tag:Kick OR NOT (bpm:120 OR duration:>2s)",
                                "NOT NOT available:true",
                                "favorite:true rating:2"}) {
            const std::string_view shown_text = text;
            CAPTURE(shown_text);
            auto query = parse_query(text);
            REQUIRE(query.has_value());
            REQUIRE(is_row_evaluable(*query));
            auto sql = matching_ids(library, SearchRequest{.text = text});
            REQUIRE(sql.has_value());
            std::vector<std::int64_t> memory;
            RowMatcher matcher(*query);
            for (const auto& [id, row] : rows) {
                if (matcher(row)) memory.push_back(id);
                CHECK(matches(*query, row) == matcher(row));
            }
            CHECK(memory == *sql);
        }
        for (const auto text : {"kick", "name:kick", "collection:Flacs", "tag:Drums/Kick", "tag:Kick OR kick",
                                "category:Drums//Kick", "NOT collection:x"}) {
            const std::string_view shown_text = text;
            CAPTURE(shown_text);
            CHECK_FALSE(is_row_evaluable(*parse_query(text)));
        }
        CHECK(is_row_evaluable(*parse_query("category:Drums/Kicks")));
    }

    TEST_CASE("for_each_row and page hydration scale to 100K samples (benchmark)" * doctest::skip()) {
        // Run with --no-skip: builds a 100K synthetic library like `mso bench` and times the
        // set-based loaders.
        using Clock = std::chrono::steady_clock;
        ScratchDirectory scratch;
        auto created = catalog::Library::create(scratch / "bench", "Benchmark");
        REQUIRE(created.has_value());
        auto& library = **created;
        auto root = library.add_root("Synthetic", catalog::RootMode::Referenced);
        REQUIRE(root.has_value());
        REQUIRE(library.set_root_availability(root->id, catalog::Availability::Online).has_value());
        std::mt19937_64 random(20261019);
        static constexpr std::string_view kFolders[] = {"Drums/Kicks", "Drums/Snares", "Loops/House", "FX/Risers"};
        static constexpr std::string_view kWords[] = {"kick", "snare", "hat", "clap", "bass", "pad"};
        constexpr std::int64_t kRows = 100'000;
        auto session = library.begin_scan(root->id);
        REQUIRE(session.has_value());
        for (std::int64_t first = 0; first < kRows; first += 500) {
            std::vector<catalog::ScannedFile> batch;
            for (std::int64_t index = first; index < first + 500; ++index) {
                auto file = test::synthetic_file(
                    std::format("{}/{} {:07}.wav", kFolders[random() % 4], kWords[random() % 6], index),
                    static_cast<std::int64_t>(10'000 + random() % 50'000'000),
                    static_cast<std::int64_t>(50'000 + random() % 30'000'000));
                if (random() % 3 == 0)
                    file.metadata->filename_bpm_milli = static_cast<std::int64_t>(70 + random() % 110) * 1000;
                batch.push_back(std::move(file));
            }
            REQUIRE(library.commit_scan_batch(*session, batch, {}, [](std::string_view) { return false; }).has_value());
        }
        REQUIRE(library.finish_scan(*session, false).has_value());
        catalog::EditCommand tag;
        for (std::int64_t id = 1; id <= kRows; id += 3)
            tag.sample_ids.push_back(id);
        tag.tags_add = {"Drums/Kick", "Bench"};
        tag.category = std::optional<std::string>("Drums/Kicks");
        REQUIRE(library.apply_edit(tag).has_value());

        const auto time = [](auto&& work) {
            const auto started = Clock::now();
            work();
            return std::chrono::duration<double, std::milli>(Clock::now() - started).count();
        };
        std::int64_t streamed = 0;
        const double stream_ms =
            time([&] { REQUIRE(for_each_row(library, [&](SampleRow&&) { ++streamed; }).has_value()); });
        CHECK(streamed == kRows);
        std::vector<std::int64_t> page_ids;
        for (std::int64_t id = 1; id <= 200; ++id)
            page_ids.push_back(id * 397 % kRows + 1);
        const double load_ms = time([&] { REQUIRE(load_rows(library, page_ids)->size() == 200); });
        catalog::Library* scope[] = {&library};
        const double page_ms =
            time([&] { REQUIRE(SearchService().search(scope, SearchRequest{})->rows.size() == 200); });
        std::vector<SampleRow> rows;
        REQUIRE(for_each_row(library, [&](SampleRow&& row) { rows.push_back(std::move(row)); }).has_value());
        auto query = parse_query("tag:Kick bpm:100..140 NOT folder:FX");
        REQUIRE(query.has_value());
        std::size_t hits = 0;
        const double match_ms = time([&] {
            RowMatcher matcher(*query);
            for (const auto& row : rows)
                hits += matcher(row) ? 1 : 0;
        });
        CHECK(static_cast<std::int64_t>(hits) == count_of(library, "tag:Kick bpm:100..140 NOT folder:FX"));
        MESSAGE(
            std::format("for_each_row {} rows: {:.0f} ms; load_rows 200: {:.1f} ms; first page: {:.1f} ms; "
                        "RowMatcher over {} rows: {:.1f} ms",
                        streamed, stream_ms, load_ms, page_ms, rows.size(), match_ms));
    }

}  // TEST_SUITE("search")
