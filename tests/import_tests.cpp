// Import and reconciliation against a real folder: probing, renames, changes, missing files,
// cancellation with resume, and no re-probing of unchanged files.
#include <doctest.h>

#include <fstream>

#include "mso/files/importer.hpp"
#include "mso/search/search_service.hpp"
#include "test_support.hpp"

using namespace mso;
using mso::test::ScratchDirectory;

namespace {

struct ImportFixture {
    ScratchDirectory scratch;
    std::filesystem::path source;
    std::unique_ptr<catalog::Library> library;
    std::int64_t root_id = 0;

    ImportFixture() : source(scratch / "samples") {
        test::write_sine_wav(source / "Drums/Kick 01.wav", 0.3, 60);
        test::write_sine_wav(source / "Loops/Groove_124bpm_Am.wav", 1.0, 220);
        test::write_sine_wav(source / path_from_utf8("FX/שלום.wav"), 0.2,
                             880);  // narrow literals are not UTF-8 paths on Windows
        std::ofstream(source / "FX/readme.txt") << "not audio";
        auto created = catalog::Library::create(scratch / "lib", "Imports");
        REQUIRE(created.has_value());
        library = std::move(*created);
        auto root = library->add_root("Samples", catalog::RootMode::Referenced);
        REQUIRE(root.has_value());
        root_id = root->id;
    }

    files::ImportSummary import(std::stop_token stop = {}) {
        auto summary = files::import_root(*library, root_id, source, stop);
        REQUIRE(summary.has_value());
        return *summary;
    }
};

}  // namespace

TEST_SUITE("import") {
    TEST_CASE("import catalogs audio files with probed metadata and filename hints, skipping non-audio") {
        ImportFixture fixture;
        const auto summary = fixture.import();
        CHECK(summary.progress.added == 3);
        CHECK(summary.finish.complete_coverage);
        auto groove = fixture.library->sample_detail(test::sample_id_named(*fixture.library, "Groove_124bpm_Am"));
        REQUIRE(groove.has_value());
        CHECK(groove->format == "wav");
        CHECK(groove->sample_rate == 44100);
        CHECK(groove->duration_exact);
        CHECK(groove->bpm_milli == 124'000);
        CHECK(groove->bpm_source == "filename");
        REQUIRE(groove->key.has_value());
        CHECK(format_key(*groove->key) == "Am");
        CHECK(test::sample_id_named(*fixture.library, "שלום") > 0);
    }

    TEST_CASE("a rescan keeps annotations through renames and reports changed and missing files") {
        ImportFixture fixture;
        fixture.import();
        auto& library = *fixture.library;
        const auto kick = test::sample_id_named(library, "Kick 01");
        catalog::EditCommand favorite;
        favorite.sample_ids = {kick};
        favorite.favorite = true;
        favorite.tags_add = {"Mine"};
        REQUIRE(library.apply_edit(favorite).has_value());

        std::filesystem::rename(fixture.source / "Drums/Kick 01.wav", fixture.source / "Drums/Kick Renamed.wav");
        std::filesystem::remove(fixture.source / path_from_utf8("FX/שלום.wav"));
        test::write_sine_wav(fixture.source / "Loops/Groove_124bpm_Am.wav", 2.0, 110);  // content changed in place

        const auto summary = fixture.import();
        CHECK(summary.progress.relinked == 1);
        CHECK(summary.progress.updated == 1);
        CHECK(summary.progress.added == 0);
        CHECK(summary.finish.marked_missing == 1);

        auto detail = library.sample_detail(kick);
        REQUIRE(detail.has_value());
        CHECK(detail->favorite);
        CHECK(detail->locations.front().relative_path == "Drums/Kick Renamed.wav");
        auto groove = library.sample_detail(test::sample_id_named(library, "Groove_124bpm_Am"));
        CHECK(groove->content_revision == 2);
        CHECK(groove->duration_us == 2'000'000);

        catalog::Library* scope[] = {&library};
        auto missing = search::SearchService().search(scope, {.text = "available:false"});
        REQUIRE(missing->rows.size() == 1);
        CHECK(missing->rows.front().availability == search::RowAvailability::Missing);
    }

    TEST_CASE("unchanged files are not probed again") {
        ImportFixture fixture;
        fixture.import();
        const auto second = fixture.import();
        CHECK(second.progress.files_committed == 3);
        CHECK(second.progress.files_probed == 0);
        CHECK(second.progress.added == 0);
    }

    TEST_CASE("a cancelled import commits nothing new and the next scan completes") {
        ImportFixture fixture;
        std::stop_source stop;
        stop.request_stop();
        auto cancelled = files::import_root(*fixture.library, fixture.root_id, fixture.source, stop.get_token());
        REQUIRE_FALSE(cancelled.has_value());
        CHECK(cancelled.error().code == ErrorCode::Cancelled);
        CHECK(fixture.library->sample_count().value_or(-1) == 0);
        const auto resumed = fixture.import();
        CHECK(resumed.progress.added == 3);
    }

    TEST_CASE("an unreachable root is reported offline and keeps its samples") {
        ImportFixture fixture;
        fixture.import();
        auto offline = files::import_root(*fixture.library, fixture.root_id, fixture.scratch / "gone", {});
        REQUIRE_FALSE(offline.has_value());
        CHECK(offline.error().code == ErrorCode::SourceOffline);
        CHECK(fixture.library->sample_count().value_or(0) == 3);
        auto roots = fixture.library->list_roots();
        CHECK(roots->front().availability == catalog::Availability::Offline);
    }

}  // TEST_SUITE("import")
