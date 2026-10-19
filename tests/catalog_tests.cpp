// Catalog: identity, migrations, edits, durable undo/redo, conflicts, idempotency, taxonomy,
// crash recovery and the analysis queue's fencing.
#include <doctest.h>

#include <algorithm>
#include <numeric>

#include "mso/catalog/library.hpp"
#include "test_support.hpp"

using namespace mso;
using namespace mso::catalog;
using mso::test::ScratchDirectory;

namespace {

std::vector<ScannedFile> three_samples() {
    return {test::synthetic_file("Drums/Kick 01.wav", 1000, 400'000),
            test::synthetic_file("Drums/Snare.wav", 2000, 300'000),
            test::synthetic_file("Loops/Groove 128.wav", 3000, 8'000'000)};
}

EditCommand rate(std::vector<std::int64_t> samples, int rating) {
    EditCommand command;
    command.sample_ids = std::move(samples);
    command.rating = rating;
    return command;
}

}  // namespace

TEST_SUITE("catalog") {
    TEST_CASE("a new library has identity, all migrations and a healthy database") {
        ScratchDirectory scratch;
        auto library = Library::create(scratch / "lib", "Mine");
        REQUIRE(library.has_value());
        const auto identity = (*library)->identity();
        CHECK(identity.name == "Mine");
        CHECK_FALSE(identity.uuid.is_nil());
        CHECK(identity.schema_version >= 3);
        CHECK(std::filesystem::exists(scratch / "lib/library.json"));
        auto report = (*library)->integrity_report();
        REQUIRE(report.has_value());
        CHECK(report->find("integrity: ok; foreign keys: 0") == 0);
    }

    TEST_CASE("creating a library refuses a non-empty folder") {
        ScratchDirectory scratch;
        std::filesystem::create_directories(scratch / "lib");
        test::write_sine_wav(scratch / "lib/existing.wav", 0.1);
        auto library = Library::create(scratch / "lib", "Mine");
        REQUIRE_FALSE(library.has_value());
        CHECK(library.error().code == ErrorCode::InvalidArgument);
    }

    TEST_CASE("opening rejects a database that belongs to another library") {
        ScratchDirectory scratch;
        {
            REQUIRE(Library::create(scratch / "a", "A").has_value());
        }
        {
            REQUIRE(Library::create(scratch / "b", "B").has_value());
        }
        std::filesystem::copy_file(scratch / "b/library.json", scratch / "a/library.json",
                                   std::filesystem::copy_options::overwrite_existing);
        auto opened = Library::open(scratch / "a");
        REQUIRE_FALSE(opened.has_value());
        CHECK(opened.error().code == ErrorCode::DbCorrupt);
    }

    TEST_CASE("a tampered released migration is detected on open") {
        ScratchDirectory scratch;
        {
            auto library = Library::create(scratch / "lib", "Mine");
            REQUIRE(library.has_value());
            auto& connection = (*library)->writer_connection();
            REQUIRE(
                connection.execute_script("UPDATE schema_migrations SET checksum='bogus' WHERE version=1").has_value());
        }
        auto opened = Library::open(scratch / "lib");
        REQUIRE_FALSE(opened.has_value());
        CHECK(opened.error().code == ErrorCode::DbCorrupt);
    }

    TEST_CASE("edits, undo and redo survive closing and reopening the library") {
        ScratchDirectory scratch;
        std::int64_t kick = 0;
        {
            auto populated = test::make_library(scratch / "lib", three_samples());
            kick = test::sample_id_named(*populated.library, "Kick 01");
            EditCommand edit = rate({kick}, 4);
            edit.favorite = true;
            edit.tags_add = {"Drums/Kick"};
            edit.notes = "punchy";
            REQUIRE(populated.library->apply_edit(edit).has_value());
        }
        {
            auto library = Library::open(scratch / "lib");
            REQUIRE(library.has_value());
            auto detail = (*library)->sample_detail(kick);
            REQUIRE(detail.has_value());
            CHECK(detail->rating == 4);
            CHECK(detail->favorite);
            CHECK(detail->notes == "punchy");
            REQUIRE(detail->tags.size() == 1);
            CHECK(detail->tags.front().path == "Drums/Kick");
            REQUIRE((*library)->undo().has_value());
        }
        {
            auto library = Library::open(scratch / "lib");
            REQUIRE(library.has_value());
            auto detail = (*library)->sample_detail(kick);
            REQUIRE(detail.has_value());
            CHECK(detail->rating == 0);
            CHECK_FALSE(detail->favorite);
            CHECK(detail->tags.empty());
            REQUIRE((*library)->redo().has_value());
            CHECK((*library)->sample_detail(kick)->rating == 4);
            auto nothing = (*library)->redo();
            REQUIRE_FALSE(nothing.has_value());
            CHECK(nothing.error().code == ErrorCode::NotFound);
        }
    }

    TEST_CASE("undo never clobbers a later edit of the same field") {
        ScratchDirectory scratch;
        auto populated = test::make_library(scratch / "lib", three_samples());
        auto& library = *populated.library;
        const auto snare = test::sample_id_named(library, "Snare");
        REQUIRE(library.apply_edit(rate({snare}, 3)).has_value());
        // A later change outside the history cursor (e.g. another tool) changes the same field.
        REQUIRE(library.writer_connection()
                    .execute_script(std::format("UPDATE samples SET rating=5 WHERE id={}", snare))
                    .has_value());
        auto undone = library.undo();
        REQUIRE_FALSE(undone.has_value());
        CHECK(undone.error().code == ErrorCode::HistoryConflict);
        CHECK(library.sample_detail(snare)->rating == 5);
        auto review = library.open_review_items();
        REQUIRE(review.has_value());
        CHECK(std::ranges::any_of(*review, [](const ReviewItem& item) { return item.kind == "history_conflict"; }));
    }

    TEST_CASE("a new edit after undo supersedes the redo tail") {
        ScratchDirectory scratch;
        auto populated = test::make_library(scratch / "lib", three_samples());
        auto& library = *populated.library;
        const auto kick = test::sample_id_named(library, "Kick 01");
        REQUIRE(library.apply_edit(rate({kick}, 1)).has_value());
        REQUIRE(library.undo().has_value());
        REQUIRE(library.apply_edit(rate({kick}, 2)).has_value());
        auto redo = library.redo();
        REQUIRE_FALSE(redo.has_value());
        CHECK(library.sample_detail(kick)->rating == 2);
    }

    TEST_CASE("replaying an operation id returns the stored receipt; reusing it differently is rejected") {
        ScratchDirectory scratch;
        auto populated = test::make_library(scratch / "lib", three_samples());
        auto& library = *populated.library;
        const auto kick = test::sample_id_named(library, "Kick 01");
        EditCommand edit = rate({kick}, 5);
        auto first = library.apply_edit(edit);
        auto second = library.apply_edit(edit);
        REQUIRE(first.has_value());
        REQUIRE(second.has_value());
        CHECK(second->replayed);
        CHECK(second->committed_revision == first->committed_revision);
        edit.rating = 1;
        auto mismatch = library.apply_edit(edit);
        REQUIRE_FALSE(mismatch.has_value());
        CHECK(mismatch.error().code == ErrorCode::IdempotencyMismatch);
        CHECK(library.sample_detail(kick)->rating == 5);
    }

    TEST_CASE("invalid edits are rejected before anything is written") {
        ScratchDirectory scratch;
        auto populated = test::make_library(scratch / "lib", three_samples());
        auto& library = *populated.library;
        const auto kick = test::sample_id_named(library, "Kick 01");
        const auto revision = library.identity().catalog_revision;
        CHECK(library.apply_edit(rate({kick}, 6)).error().code == ErrorCode::InvalidArgument);
        EditCommand both;
        both.sample_ids = {kick};
        both.tags_add = {"A"};
        both.tags_remove = {"a"};
        CHECK(library.apply_edit(both).error().code == ErrorCode::InvalidArgument);
        EditCommand deep;
        deep.sample_ids = {kick};
        deep.tags_add = {"1/2/3/4/5/6/7/8/9/10/11/12/13/14/15/16/17"};
        CHECK_FALSE(library.apply_edit(deep).has_value());
        CHECK(library.identity().catalog_revision == revision);
    }

    TEST_CASE("a bulk edit larger than one chunk is one undoable action") {
        ScratchDirectory scratch;
        std::vector<ScannedFile> files;
        for (int index = 0; index < 1203; ++index)
            files.push_back(test::synthetic_file(std::format("bulk/{:04}.wav", index), 100 + index, 1'000'000));
        auto populated = test::make_library(scratch / "lib", files);
        auto& library = *populated.library;
        std::vector<std::int64_t> all(1203);
        std::iota(all.begin(), all.end(), 1);
        EditCommand edit;
        edit.sample_ids = all;
        edit.tags_add = {"Bulk"};
        auto receipt = library.apply_edit(edit);
        REQUIRE(receipt.has_value());
        CHECK(receipt->applied == 1203);
        auto history = library.history();
        REQUIRE(history.has_value());
        CHECK(history->size() == 1);
        REQUIRE(library.undo().has_value());
        auto tags = library.list_tags();
        REQUIRE(tags.has_value());
        REQUIRE(tags->size() == 1);
        CHECK(tags->front().sample_count == 0);
    }

    TEST_CASE("merging tags is undoable and restores the exact original memberships") {
        ScratchDirectory scratch;
        auto populated = test::make_library(scratch / "lib", three_samples());
        auto& library = *populated.library;
        const auto kick = test::sample_id_named(library, "Kick 01");
        const auto snare = test::sample_id_named(library, "Snare");
        EditCommand tag_both;
        tag_both.sample_ids = {kick, snare};
        tag_both.tags_add = {"Percussion"};
        REQUIRE(library.apply_edit(tag_both).has_value());
        EditCommand tag_kick;
        tag_kick.sample_ids = {kick};
        tag_kick.tags_add = {"Drums"};
        REQUIRE(library.apply_edit(tag_kick).has_value());
        auto tags = library.list_tags();
        REQUIRE(tags.has_value());
        const auto percussion = std::ranges::find(*tags, std::string("Percussion"), &TaxonomyNode::name)->id;
        const auto drums = std::ranges::find(*tags, std::string("Drums"), &TaxonomyNode::name)->id;

        TaxonomyCommand merge{.kind = TaxonomyCommand::Kind::MergeTag, .target_id = percussion, .other_id = drums};
        REQUIRE(library.apply_taxonomy(merge).has_value());
        auto merged = library.list_tags();
        REQUIRE(merged->size() == 1);
        CHECK(merged->front().sample_count == 2);
        EditCommand alias_use;  // the old name now resolves to the canonical tag
        alias_use.sample_ids = {test::sample_id_named(library, "Groove 128")};
        alias_use.tags_add = {"Percussion"};
        REQUIRE(library.apply_edit(alias_use).has_value());
        CHECK(library.sample_detail(alias_use.sample_ids.front())->tags.front().name == "Drums");
        REQUIRE(library.undo().has_value());

        REQUIRE(library.undo().has_value());  // undo the merge
        auto restored = library.list_tags();
        REQUIRE(restored->size() == 2);
        auto kick_tags = library.sample_detail(kick)->tags;
        auto snare_tags = library.sample_detail(snare)->tags;
        CHECK(kick_tags.size() == 2);  // kept its pre-existing Drums link
        REQUIRE(snare_tags.size() == 1);
        CHECK(snare_tags.front().name == "Percussion");
    }

    TEST_CASE("tag moves reject cycles") {
        ScratchDirectory scratch;
        auto populated = test::make_library(scratch / "lib", three_samples());
        auto& library = *populated.library;
        auto parent = library.ensure_tag("Drums");
        auto child = library.ensure_tag("Drums/Kick");
        REQUIRE(parent.has_value());
        REQUIRE(child.has_value());
        TaxonomyCommand move{.kind = TaxonomyCommand::Kind::MoveTag, .target_id = *parent, .new_parent_id = *child};
        auto moved = library.apply_taxonomy(move);
        REQUIRE_FALSE(moved.has_value());
        CHECK(moved.error().code == ErrorCode::InvalidArgument);
    }

    TEST_CASE("manual collections keep order and smart collections store their query") {
        ScratchDirectory scratch;
        auto populated = test::make_library(scratch / "lib", three_samples());
        auto& library = *populated.library;
        TaxonomyCommand create{.kind = TaxonomyCommand::Kind::CreateCollection, .new_name = "Crate"};
        REQUIRE(library.apply_taxonomy(create).has_value());
        TaxonomyCommand smart{
            .kind = TaxonomyCommand::Kind::CreateCollection, .new_name = "Long", .smart_query = "duration:>5s"};
        REQUIRE(library.apply_taxonomy(smart).has_value());
        auto collections = library.list_collections();
        REQUIRE(collections->size() == 2);
        const auto crate = std::ranges::find(*collections, std::string("Crate"), &CollectionRecord::name)->id;
        EditCommand add;
        add.sample_ids = {test::sample_id_named(library, "Snare"), test::sample_id_named(library, "Kick 01")};
        add.add_to_collection = crate;
        REQUIRE(library.apply_edit(add).has_value());
        const auto refreshed = library.list_collections();
        REQUIRE(refreshed.has_value());
        CHECK(std::ranges::find(*refreshed, crate, &CollectionRecord::id)->sample_count == 2);
        EditCommand to_smart = add;
        to_smart.operation = new_uuid();
        to_smart.add_to_collection = std::ranges::find(*collections, std::string("Long"), &CollectionRecord::name)->id;
        CHECK(library.apply_edit(to_smart).error().code == ErrorCode::InvalidArgument);
    }

    TEST_CASE("BPM and key follow user value → user unknown → container → analysis → filename") {
        ScratchDirectory scratch;
        auto file = test::synthetic_file("Loop.wav", 10, 1'000'000);
        file.metadata->filename_bpm_milli = 128'000;
        auto populated = test::make_library(scratch / "lib", {file});
        auto& library = *populated.library;
        const auto loop = test::sample_id_named(library, "Loop");
        CHECK(library.sample_detail(loop)->bpm_milli == 128'000);
        CHECK(library.sample_detail(loop)->bpm_source == "filename");

        Library::AnalysisFacts facts{.sample_id = loop,
                                     .content_revision = 1,
                                     .bpm_milli = 127'500,
                                     .bpm_confidence = 0.9,
                                     .tempo_algorithm = "test",
                                     .tempo_requested = true};
        REQUIRE(library.record_analysis(facts).has_value());
        CHECK(library.sample_detail(loop)->bpm_milli == 127'500);

        EditCommand unknown;
        unknown.sample_ids = {loop};
        unknown.bpm = BpmOverrideEdit{OverrideState::Unknown, std::nullopt};
        REQUIRE(library.apply_edit(unknown).has_value());
        CHECK_FALSE(library.sample_detail(loop)->bpm_milli.has_value());

        EditCommand pinned;
        pinned.sample_ids = {loop};
        pinned.bpm = BpmOverrideEdit{OverrideState::Value, 130'000};
        REQUIRE(library.apply_edit(pinned).has_value());
        REQUIRE(library.record_analysis(facts).has_value());  // re-analysis never overrides the user
        CHECK(library.sample_detail(loop)->bpm_milli == 130'000);
        CHECK(library.sample_detail(loop)->bpm_source == "user");
    }

    TEST_CASE("crash recovery turns half-applied actions into partial ones and pauses scans") {
        ScratchDirectory scratch;
        {
            auto populated = test::make_library(scratch / "lib", three_samples());
            auto& connection = populated.library->writer_connection();
            REQUIRE(connection
                        .execute_script(
                            "INSERT INTO history_actions(uuid,label,command_version,state,created_at) "
                            "VALUES('x','crashed',1,'applying',0);"
                            "INSERT INTO scan_sessions(root_id,generation,state,started_at) VALUES(1,99,'running',0);")
                        .has_value());
        }
        auto library = Library::open(scratch / "lib");
        REQUIRE(library.has_value());
        CHECK((*library)->recovery_report().partial_history_actions == 1);
        CHECK((*library)->recovery_report().interrupted_scans == 1);
    }

    TEST_CASE("verified backups are standalone copies with the same identity") {
        ScratchDirectory scratch;
        auto populated = test::make_library(scratch / "lib", three_samples());
        auto backup = populated.library->create_backup("test");
        REQUIRE(backup.has_value());
        auto copy = sqlite::Connection::open(*backup, {.read_only = true, .write_ahead_log = false});
        REQUIRE(copy.has_value());
        CHECK(copy->query_int64("SELECT count(*) FROM samples").value_or(0) == 3);
        CHECK(copy->integrity_check().value_or("") == "ok");
    }

    TEST_CASE("analysis jobs are fenced: cancelled runs and changed content never publish results") {
        ScratchDirectory scratch;
        auto populated = test::make_library(scratch / "lib", three_samples());
        auto& library = *populated.library;
        const auto kick = test::sample_id_named(library, "Kick 01");
        REQUIRE(library.set_root_availability(populated.root_id, Availability::Online).has_value());

        auto run = library.create_analysis_run({"Musical", {"tempo", "key"}, {kick}, false});
        REQUIRE(run.has_value());
        CHECK(run->queued == 1);
        auto lease = library.lease_next_job(std::chrono::seconds(15));
        REQUIRE(lease.has_value());
        REQUIRE(lease->has_value());
        REQUIRE(library.control_run(run->id, Library::RunControl::Cancel).has_value());
        Library::AnalysisFacts late{.sample_id = kick,
                                    .content_revision = (*lease)->content_revision,
                                    .bpm_milli = 100'000,
                                    .tempo_algorithm = "test",
                                    .tempo_requested = true};
        auto published = library.complete_job(**lease, late);
        REQUIRE_FALSE(published.has_value());
        CHECK(published.error().code == ErrorCode::Cancelled);
        CHECK_FALSE(library.sample_detail(kick)->bpm_milli.has_value());

        auto second = library.create_analysis_run({"Musical", {"tempo"}, {kick}, false});
        REQUIRE(second.has_value());
        auto second_lease = library.lease_next_job(std::chrono::seconds(15));
        REQUIRE(second_lease.has_value());
        REQUIRE(second_lease->has_value());
        REQUIRE(
            library.writer_connection()
                .execute_script(std::format("UPDATE samples SET content_revision=content_revision+1 WHERE id={}", kick))
                .has_value());
        auto stale = library.complete_job(**second_lease, late);
        REQUIRE_FALSE(stale.has_value());
        CHECK(stale.error().code == ErrorCode::InputChanged);
        auto runs = library.list_analysis_runs();
        REQUIRE(runs.has_value());
        CHECK(runs->front().state == "completed");
    }

    TEST_CASE("analysis skips samples whose results are current unless forced") {
        ScratchDirectory scratch;
        auto populated = test::make_library(scratch / "lib", three_samples());
        auto& library = *populated.library;
        const auto kick = test::sample_id_named(library, "Kick 01");
        Library::AnalysisFacts facts{
            .sample_id = kick, .content_revision = 1, .tempo_algorithm = "test", .tempo_requested = true};
        REQUIRE(library.record_analysis(facts).has_value());
        auto run = library.create_analysis_run({"Tempo", {"tempo"}, {kick}, false});
        REQUIRE(run.has_value());
        CHECK(run->queued == 0);
        CHECK(run->skipped == 1);
        auto forced = library.create_analysis_run({"Tempo", {"tempo"}, {kick}, true});
        CHECK(forced->queued == 1);
    }

}  // TEST_SUITE("catalog")
