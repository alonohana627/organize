// Metadata export (CSV/JSON) and the application layer: workspace, roots, path resolution.
#include <doctest.h>

#include <algorithm>
#include <fstream>
#include <sstream>

#include <nlohmann/json.hpp>

#include "mso/app/application.hpp"
#include "mso/app/metadata_export.hpp"
#include "test_support.hpp"

using namespace mso;
using mso::test::ScratchDirectory;

namespace {

std::string read_text(const std::filesystem::path& file) {
    std::ifstream in(file, std::ios::binary);
    std::stringstream contents;
    contents << in.rdbuf();
    return contents.str();
}

std::unique_ptr<app::Application> start_application(const std::filesystem::path& workspace) {
    app::ApplicationOptions options;
    options.workspace_directory = workspace;
    options.monitor_roots = false;
    options.run_analysis_supervisor = false;
    auto application = app::Application::start(options);
    REQUIRE(application.has_value());
    return std::move(*application);
}

}  // namespace

TEST_SUITE("export") {
    TEST_CASE("spreadsheet-safe mode neutralizes formula-like values only") {
        CHECK(app::spreadsheet_safe("plain") == "plain");
        CHECK(app::spreadsheet_safe("=HYPERLINK(1)") == "'=HYPERLINK(1)");
        CHECK(app::spreadsheet_safe("-3") == "'-3");
        CHECK(app::spreadsheet_safe("@sum") == "'@sum");
        CHECK(app::spreadsheet_safe("") == "");
    }

    TEST_CASE("CSV and JSON exports contain every sample and tags as JSON arrays") {
        ScratchDirectory scratch;
        auto populated = test::make_library(scratch / "lib", {test::synthetic_file("Kick, \"hard\".wav", 10, 1'000),
                                                              test::synthetic_file("=cmd.wav", 20, 2'000)});
        catalog::EditCommand tag;
        tag.sample_ids = {1};
        tag.tags_add = {"Drums/Kick", "Hard"};
        REQUIRE(populated.library->apply_edit(tag).has_value());
        auto root_paths = [](const uuids::uuid&) { return std::optional<std::filesystem::path>(); };

        auto csv = app::write_metadata_export(*populated.library, scratch / "out.csv", {}, root_paths, {});
        REQUIRE(csv.has_value());
        CHECK(csv->rows == 2);
        const auto text = read_text(scratch / "out.csv");
        CHECK(text.starts_with("\xEF\xBB\xBFsample_uuid,"));
        CHECK(text.find("\"Kick, \"\"hard\"\"\"") != std::string::npos);
        CHECK(text.find("'=cmd") != std::string::npos);
        CHECK(text.find("\"[\"\"Drums/Kick\"\",\"\"Hard\"\"]\"") != std::string::npos);
        const auto first_record_end = text.find("\r\n");  // RFC 4180 record terminator
        REQUIRE(first_record_end != std::string::npos);
        CHECK(text.substr(first_record_end - 10, 10) == "created_at");
        CHECK(std::ranges::count(text, '\n') == std::ranges::count(text, '\r'));  // no bare LF records
        CHECK(std::filesystem::exists(scratch / "out.csv.manifest.json"));

        app::ExportOptions json_options{.format = app::ExportFormat::Json};
        auto json = app::write_metadata_export(*populated.library, scratch / "out.json", json_options, root_paths, {});
        REQUIRE(json.has_value());
        const auto document = nlohmann::json::parse(read_text(scratch / "out.json"));
        CHECK(document["schema_version"] == 1);
        REQUIRE(document["samples"].size() == 2);
        CHECK(document["samples"][0]["tags"].size() == 2);
        CHECK(document["samples"][0].contains("location"));
        CHECK_FALSE(document["samples"][0]["location"].contains("absolute_path"));
    }

    TEST_CASE("a cancelled export leaves neither a destination nor a temporary file") {
        ScratchDirectory scratch;
        auto populated = test::make_library(scratch / "lib", {test::synthetic_file("a.wav", 1, 1)});
        std::stop_source stop;
        stop.request_stop();
        auto cancelled = app::write_metadata_export(
            *populated.library, scratch / "out.csv", {},
            [](const uuids::uuid&) { return std::optional<std::filesystem::path>(); }, stop.get_token());
        REQUIRE_FALSE(cancelled.has_value());
        CHECK(cancelled.error().code == ErrorCode::Cancelled);
        std::size_t files = 0;
        for ([[maybe_unused]] const auto& entry : std::filesystem::directory_iterator(scratch.path()))
            ++files;
        CHECK(files == 1);  // only the library folder
    }

    TEST_CASE("the application remembers libraries and root bindings across restarts") {
        ScratchDirectory scratch;
        test::write_sine_wav(scratch / "source/Kick.wav", 0.2);
        uuids::uuid library_uuid;
        {
            auto application = start_application(scratch / "workspace");
            auto created = application->create_library(scratch / "lib", "Remembered");
            REQUIRE(created.has_value());
            library_uuid = created->uuid;
            auto root = application->add_root(library_uuid, scratch / "source");
            REQUIRE(root.has_value());
            REQUIRE(application->import_now(library_uuid, root->id).has_value());
        }
        auto application = start_application(scratch / "workspace");
        REQUIRE(application->libraries().size() == 1);
        auto path = application->resolve_playable_path(library_uuid, 1);
        REQUIRE(path.has_value());
        CHECK(path->filename() == "Kick.wav");

        std::filesystem::remove(*path);
        auto missing = application->resolve_playable_path(library_uuid, 1);
        REQUIRE_FALSE(missing.has_value());
        CHECK(missing.error().code == ErrorCode::FileMissing);
        std::filesystem::rename(scratch / "source", scratch / "unplugged");
        auto offline = application->resolve_playable_path(library_uuid, 1);
        REQUIRE_FALSE(offline.has_value());
        CHECK(offline.error().code == ErrorCode::SourceOffline);
    }

    TEST_CASE("the same library cannot be opened twice, and at most eight are open") {
        ScratchDirectory scratch;
        auto application = start_application(scratch / "workspace");
        REQUIRE(application->create_library(scratch / "lib", "Once").has_value());
        std::filesystem::copy(scratch / "lib", scratch / "copy", std::filesystem::copy_options::recursive);
        auto duplicate = application->open_library(scratch / "copy");
        REQUIRE_FALSE(duplicate.has_value());
        CHECK(duplicate.error().code == ErrorCode::InvalidArgument);
        for (int index = 0; index < 7; ++index)
            REQUIRE(application->create_library(scratch / std::format("more{}", index), "More").has_value());
        auto ninth = application->create_library(scratch / "ninth", "Too many");
        REQUIRE_FALSE(ninth.has_value());
        CHECK(ninth.error().code == ErrorCode::ResourceLimit);
    }

    TEST_CASE("a corrupt workspace file never prevents startup") {
        ScratchDirectory scratch;
        std::filesystem::create_directories(scratch / "workspace");
        std::ofstream(scratch / "workspace/workspace.json") << "{ not json";
        auto application = start_application(scratch / "workspace");
        CHECK(application->libraries().empty());
    }

}  // TEST_SUITE("export")

TEST_SUITE("export") {

TEST_CASE("automatic mode creates a default library and queues analysis after an import") {
    ScratchDirectory scratch;
    test::write_sine_wav(scratch / "pack/Kick.wav", 0.2);
    test::write_sine_wav(scratch / "pack/Loop 128bpm.wav", 1.0);
    app::ApplicationOptions options;
    options.workspace_directory = scratch / "workspace";
    options.monitor_roots = false;
    options.run_analysis_supervisor = false;  // the queue is checked, not executed, here
    options.automatic_mode = true;
    auto application = app::Application::start(options);
    REQUIRE(application.has_value());
    auto& app = **application;

    REQUIRE(app.libraries().size() == 1);
    const auto library = app.libraries().front();
    CHECK(library.name == "My Samples");
    CHECK(library.directory.parent_path().filename() == "Libraries");
    CHECK(app::Application::is_library_folder(library.directory));
    CHECK_FALSE(app::Application::is_library_folder(scratch / "pack"));

    auto root = app.add_root(library.uuid, scratch / "pack");
    REQUIRE(root.has_value());
    auto operation = app.start_import(library.uuid, root->id);
    REQUIRE(operation.has_value());
    app.wait_for_operation(*operation);

    auto runs = app.library(library.uuid)->list_analysis_runs();
    REQUIRE(runs.has_value());
    REQUIRE(runs->size() == 1);
    CHECK(runs->front().profile_name == "Full");
    CHECK(runs->front().queued == 2);
    CHECK(app.libraries().front().root_count == 1);

    auto second = app.create_library_in_workspace("My Samples");
    REQUIRE(second.has_value());
    CHECK(second->directory.filename() == "My Samples 2");
}

}  // TEST_SUITE("export")
