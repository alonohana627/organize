// Metadata export (CSV/JSON): spreadsheet safety, raw mode, absolute paths, filters, paging past
// one SearchService page, atomic replacement and the CSV manifest. Application tests live in
// app_tests.cpp.
#include <doctest.h>

#include <algorithm>
#include <format>
#include <set>

#include <nlohmann/json.hpp>

#include "mso/app/metadata_export.hpp"
#include "test_support.hpp"

using namespace mso;
using mso::test::ScratchDirectory;

namespace {

/// A resolver for libraries whose roots are not bound on this machine.
std::optional<std::filesystem::path> unbound(const uuids::uuid&, catalog::RootMode) {
    return std::nullopt;
}

/// Number of CSV records (CRLF-terminated) after the header.
std::size_t csv_data_records(const std::string& text) {
    std::size_t records = 0;
    for (std::size_t position = text.find("\r\n"); position != std::string::npos;
         position = text.find("\r\n", position + 2))
        ++records;
    return records - 1;
}

/// Files in a directory (to check that no temporary file is left behind).
std::vector<std::string> file_names(const std::filesystem::path& directory) {
    std::vector<std::string> names;
    for (const auto& entry : std::filesystem::directory_iterator(directory))
        names.push_back(path_to_utf8(entry.path().filename()));
    std::ranges::sort(names);
    return names;
}

}  // namespace

TEST_SUITE("export") {
    TEST_CASE("spreadsheet-safe mode neutralizes formula-like values only") {
        CHECK(app::spreadsheet_safe("plain") == "plain");
        CHECK(app::spreadsheet_safe("=HYPERLINK(1)") == "'=HYPERLINK(1)");
        CHECK(app::spreadsheet_safe("-3") == "'-3");
        CHECK(app::spreadsheet_safe("@sum") == "'@sum");
        CHECK(app::spreadsheet_safe("+1") == "'+1");
        CHECK(app::spreadsheet_safe("\tx") == "'\tx");
        CHECK(app::spreadsheet_safe("") == "");
    }

    TEST_CASE("CSV and JSON exports contain every sample and tags as JSON arrays") {
        ScratchDirectory scratch;
        auto populated = test::make_library(scratch / "lib", {test::synthetic_file("Kick, hard.wav", 10, 1'000),
                                                              test::synthetic_file("=cmd.wav", 20, 2'000)});
        catalog::EditCommand tag;
        tag.sample_ids = {1};
        tag.tags_add = {"Drums/Kick", "Hard"};
        tag.display_name = "Kick, \"hard\"";  // quotes are legal in names, not in Windows file names
        REQUIRE(populated.library->apply_edit(tag).has_value());

        auto csv = app::write_metadata_export(*populated.library, scratch / "out.csv", {}, unbound, {});
        REQUIRE(csv.has_value());
        CHECK(csv->rows == 2);
        const auto text = test::read_file(scratch / "out.csv");
        CHECK(text.starts_with("\xEF\xBB\xBFsample_uuid,"));
        CHECK(text.find("\"Kick, \"\"hard\"\"\"") != std::string::npos);
        CHECK(text.find("'=cmd") != std::string::npos);
        CHECK(text.find("\"[\"\"Drums/Kick\"\",\"\"Hard\"\"]\"") != std::string::npos);
        const auto first_record_end = text.find("\r\n");  // RFC 4180 record terminator
        REQUIRE(first_record_end != std::string::npos);
        CHECK(text.substr(first_record_end - 10, 10) == "created_at");
        CHECK(std::ranges::count(text, '\n') == std::ranges::count(text, '\r'));  // no bare LF records

        const auto manifest = nlohmann::json::parse(test::read_file(scratch / "out.csv.manifest.json"));
        CHECK(manifest["rows"] == 2);
        CHECK(manifest["spreadsheet_safe"] == true);
        CHECK(manifest["consistent"] == true);

        app::ExportOptions json_options{.format = app::ExportFormat::Json};
        auto json = app::write_metadata_export(*populated.library, scratch / "out.json", json_options, unbound, {});
        REQUIRE(json.has_value());
        const auto document = nlohmann::json::parse(test::read_file(scratch / "out.json"));
        CHECK(document["schema_version"] == 1);
        CHECK(document["consistent"] == true);
        CHECK(document["library"]["catalog_revision"] == document["catalog_revision_at_end"]);
        REQUIRE(document["samples"].size() == 2);
        CHECK(document["samples"][0]["tags"].size() == 2);
        CHECK(document["samples"][0].contains("location"));
        CHECK_FALSE(document["samples"][0]["location"].contains("absolute_path"));
        CHECK_FALSE(std::filesystem::exists(scratch / "out.json.manifest.json"));
    }

    TEST_CASE("raw mode keeps formula-like cells exactly") {
        ScratchDirectory scratch;
        auto populated = test::make_library(scratch / "lib", {test::synthetic_file("=cmd.wav", 20, 2'000)});
        app::ExportOptions raw{.spreadsheet_safe = false, .utf8_bom = false};
        REQUIRE(app::write_metadata_export(*populated.library, scratch / "raw.csv", raw, unbound, {}).has_value());
        const auto text = test::read_file(scratch / "raw.csv");
        CHECK(text.starts_with("sample_uuid,"));
        CHECK(text.find(",=cmd,") != std::string::npos);
        CHECK(text.find("'=cmd") == std::string::npos);
        CHECK(nlohmann::json::parse(test::read_file(scratch / "raw.csv.manifest.json"))["spreadsheet_safe"] == false);
    }

    TEST_CASE("absolute paths come from the resolver, by root mode") {
        ScratchDirectory scratch;
        auto populated = test::make_library(scratch / "lib", {test::synthetic_file("Drums/kick.wav", 1, 1'000)});
        const auto base = scratch / "bound";
        std::optional<catalog::RootMode> asked_mode;
        auto resolver = [&](const uuids::uuid&, catalog::RootMode mode) -> std::optional<std::filesystem::path> {
            asked_mode = mode;
            return base;
        };
        app::ExportOptions csv_options{.include_absolute_paths = true};
        REQUIRE(
            app::write_metadata_export(*populated.library, scratch / "abs.csv", csv_options, resolver, {}).has_value());
        CHECK(asked_mode == catalog::RootMode::Referenced);
        const auto expected = path_to_utf8(base / path_from_utf8("Drums/kick.wav"));
        CHECK(test::read_file(scratch / "abs.csv").find(expected) != std::string::npos);

        app::ExportOptions json_options{.format = app::ExportFormat::Json, .include_absolute_paths = true};
        REQUIRE(app::write_metadata_export(*populated.library, scratch / "abs.json", json_options, resolver, {})
                    .has_value());
        const auto document = nlohmann::json::parse(test::read_file(scratch / "abs.json"));
        CHECK(document["absolute_paths"] == true);
        CHECK(document["samples"][0]["location"]["absolute_path"] == expected);

        auto unresolved =
            app::write_metadata_export(*populated.library, scratch / "none.json", json_options, unbound, {});
        REQUIRE(unresolved.has_value());
        CHECK(nlohmann::json::parse(
                  test::read_file(scratch / "none.json"))["samples"][0]["location"]["absolute_path"] == "");
    }

    TEST_CASE("a query filter restricts the exported rows") {
        ScratchDirectory scratch;
        auto populated = test::make_library(scratch / "lib", {test::synthetic_file("kick one.wav", 1, 1'000),
                                                              test::synthetic_file("snare.wav", 1, 1'000),
                                                              test::synthetic_file("kick two.wav", 1, 1'000, "flac")});
        app::ExportOptions options{.format = app::ExportFormat::Json, .query = "kick"};
        auto summary = app::write_metadata_export(*populated.library, scratch / "kicks.json", options, unbound, {});
        REQUIRE(summary.has_value());
        CHECK(summary->rows == 2);
        options.query = "kick format:flac";
        summary = app::write_metadata_export(*populated.library, scratch / "flac.json", options, unbound, {});
        REQUIRE(summary.has_value());
        CHECK(summary->rows == 1);
        options.query = "bpm:((";
        auto invalid = app::write_metadata_export(*populated.library, scratch / "bad.json", options, unbound, {});
        REQUIRE_FALSE(invalid.has_value());
        CHECK(invalid.error().code == ErrorCode::ParseError);
        CHECK_FALSE(std::filesystem::exists(scratch / "bad.json"));
    }

    TEST_CASE("exports page past one search page and replace an existing destination atomically") {
        ScratchDirectory scratch;
        std::vector<catalog::ScannedFile> files;
        for (int index = 0; index < 1234; ++index)
            files.push_back(
                test::synthetic_file(std::format("Pack/sample {:05}.wav", index), 100 + index, 1'000 + index));
        auto populated = test::make_library(scratch / "lib", files);
        test::write_file(scratch / "out/all.csv", "old content");
        auto summary = app::write_metadata_export(*populated.library, scratch / "out/all.csv", {}, unbound, {});
        REQUIRE(summary.has_value());
        CHECK(summary->rows == 1234);
        const auto text = test::read_file(scratch / "out/all.csv");
        CHECK(csv_data_records(text) == 1234);
        CHECK(text.find("old content") == std::string::npos);
        CHECK(text.find("sample 00000") != std::string::npos);
        CHECK(text.find("sample 01233") != std::string::npos);
        CHECK(file_names(scratch / "out") == std::vector<std::string>{"all.csv", "all.csv.manifest.json"});

        app::ExportOptions json_options{.format = app::ExportFormat::Json};
        REQUIRE(app::write_metadata_export(*populated.library, scratch / "out/all.json", json_options, unbound, {})
                    .has_value());
        const auto document = nlohmann::json::parse(test::read_file(scratch / "out/all.json"));
        REQUIRE(document["samples"].size() == 1234);
        std::set<std::string> uuids_seen;
        for (const auto& sample : document["samples"])
            uuids_seen.insert(sample["uuid"].get<std::string>());
        CHECK(uuids_seen.size() == 1234);  // no row repeated or skipped across pages
    }

    TEST_CASE("invalid UTF-8 in metadata is replaced, never thrown") {
        ScratchDirectory scratch;
        auto populated = test::make_library(scratch / "lib", {test::synthetic_file("ok.wav", 1, 1'000)});
        catalog::EditCommand notes;
        notes.sample_ids = {1};
        notes.display_name = "bad \xFF name";
        // The catalog may refuse invalid UTF-8 itself; either way the export must not throw.
        (void)populated.library->apply_edit(notes);  // outcome irrelevant: only the export is under test
        app::ExportOptions json_options{.format = app::ExportFormat::Json};
        auto json = app::write_metadata_export(*populated.library, scratch / "out.json", json_options, unbound, {});
        REQUIRE(json.has_value());
        CHECK_FALSE(nlohmann::json::parse(test::read_file(scratch / "out.json"), nullptr, false).is_discarded());
    }

    TEST_CASE("a cancelled export leaves neither a destination nor a temporary file") {
        ScratchDirectory scratch;
        auto populated = test::make_library(scratch / "lib", {test::synthetic_file("a.wav", 1, 1)});
        std::stop_source stop;
        stop.request_stop();
        auto cancelled =
            app::write_metadata_export(*populated.library, scratch / "out.csv", {}, unbound, stop.get_token());
        REQUIRE_FALSE(cancelled.has_value());
        CHECK(cancelled.error().code == ErrorCode::Cancelled);
        CHECK(file_names(scratch.path()) == std::vector<std::string>{"lib"});
    }

    TEST_CASE("an unwritable destination reports an error and cleans up") {
        ScratchDirectory scratch;
        auto populated = test::make_library(scratch / "lib", {test::synthetic_file("a.wav", 1, 1)});
        auto missing_folder =
            app::write_metadata_export(*populated.library, scratch / "no/such/dir/out.csv", {}, unbound, {});
        REQUIRE_FALSE(missing_folder.has_value());
        CHECK(missing_folder.error().code == ErrorCode::AccessDenied);
        std::filesystem::create_directories(scratch / "taken.csv");  // a directory where the file should go
        auto blocked = app::write_metadata_export(*populated.library, scratch / "taken.csv", {}, unbound, {});
        REQUIRE_FALSE(blocked.has_value());
        CHECK(file_names(scratch.path()) == std::vector<std::string>{"lib", "taken.csv"});
    }

}  // TEST_SUITE("export")
