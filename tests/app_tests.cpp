// The application layer: library lifetime and locking, workspace persistence and merging, the
// in-memory SampleIndex (checked against SearchService on the same data), incremental index
// updates, coalesced events and automatic analysis.
#include <doctest.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <format>
#include <mutex>
#include <numeric>
#include <thread>

#include <nlohmann/json.hpp>

#include "mso/app/application.hpp"
#include "mso/app/file_lock.hpp"
#include "mso/app/sample_index.hpp"
#include "mso/app/workspace.hpp"
#include "test_support.hpp"

using namespace mso;
using mso::test::ScratchDirectory;
using namespace std::chrono_literals;

namespace {

/// Generous bound for background work (index loads, coalesced events) on slow CI machines.
constexpr auto kBackgroundTimeout = std::chrono::milliseconds(20'000);

app::ApplicationOptions quiet_options(const std::filesystem::path& workspace) {
    app::ApplicationOptions options;
    options.workspace_directory = workspace;
    options.monitor_roots = false;
    options.run_analysis_supervisor = false;
    return options;
}

std::unique_ptr<app::Application> start_application(const std::filesystem::path& workspace) {
    auto application = app::Application::start(quiet_options(workspace));
    REQUIRE(application.has_value());
    return std::move(*application);
}

/// Applies one edit to a set of samples (skipped when the set is empty).
void edit_samples(catalog::Library& library, std::vector<std::int64_t> sample_ids,
                  const std::function<void(catalog::EditCommand&)>& fill) {
    if (sample_ids.empty()) return;
    catalog::EditCommand command;
    command.sample_ids = std::move(sample_ids);
    fill(command);
    auto receipt = library.apply_edit(command);
    REQUIRE(receipt.has_value());
}

/// Commits extra files into a root in a scan that does not mark anything missing.
void add_files(catalog::Library& library, std::int64_t root_id, const std::vector<catalog::ScannedFile>& files) {
    auto session = library.begin_scan(root_id);
    REQUIRE(session.has_value());
    REQUIRE(library.commit_scan_batch(*session, files, {}, [](std::string_view) { return false; }).has_value());
    REQUIRE(library.finish_scan(*session, /*cancelled=*/true).has_value());
}

/// A library with varied metadata: several folders and two roots, null durations and sample
/// rates, BPM, keys, ratings, favorites, a tag tree, a category tree and a manual collection.
struct VariedLibrary {
    test::PopulatedLibrary populated;
    std::int64_t second_root = 0;
    std::int64_t drums_tag = 0;
    std::int64_t bass_category = 0;
    std::int64_t collection = 0;
};

VariedLibrary make_varied_library(const std::filesystem::path& directory, int count, std::string_view name = "Varied") {
    static constexpr std::string_view kFolders[] = {"Drums/Kicks", "Drums/Snares", "Loops", "", "FX/Risers", "drums"};
    static constexpr std::string_view kWords[] = {"kick", "snare", "loop", "riser", "Pad", "Ärger"};
    std::vector<catalog::ScannedFile> files;
    for (int index = 0; index < count; ++index) {
        const auto folder = kFolders[static_cast<std::size_t>(index) % std::size(kFolders)];
        const auto word = kWords[static_cast<std::size_t>(index * 7) % std::size(kWords)];
        auto file = test::synthetic_file(
            std::format("{}{}{} {:04}.wav", folder, folder.empty() ? "" : "/", word, index),
            1000 + (index * 7919) % 5000, 10'000 + (index * 104'729) % 3'000'000, index % 4 == 0 ? "flac" : "wav");
        if (index % 9 == 0) file.metadata->duration_us.reset();
        if (index % 11 == 0) file.metadata->sample_rate.reset();
        if (index % 3 == 0) file.metadata->filename_bpm_milli = (80 + index % 90) * 1000;
        if (index % 5 == 0)
            file.metadata->filename_key = MusicalKey{index % 12, index % 2 ? KeyMode::Minor : KeyMode::Major};
        files.push_back(std::move(file));
    }
    VariedLibrary varied;
    varied.populated = test::make_library(directory, files, name);
    auto& library = *varied.populated.library;
    auto second = library.add_root("Other", catalog::RootMode::Referenced);
    REQUIRE(second.has_value());
    varied.second_root = second->id;
    add_files(library, second->id,
              {test::synthetic_file("Drums/Kicks/other kick.wav", 10, 20'000),
               test::synthetic_file("Loops/other loop.wav", 20, 30'000), test::synthetic_file("top.wav", 30, 40'000)});

    std::vector<std::int64_t> ids(static_cast<std::size_t>(count) + 3);
    std::iota(ids.begin(), ids.end(), std::int64_t{1});
    auto where = [&](auto predicate) {
        std::vector<std::int64_t> selected;
        std::ranges::copy_if(ids, std::back_inserter(selected), predicate);
        return selected;
    };
    for (int rating = 1; rating <= 5; ++rating)
        edit_samples(library, where([&](std::int64_t id) { return id % 6 == rating; }),
                     [&](catalog::EditCommand& command) { command.rating = rating; });
    edit_samples(library, where([](std::int64_t id) { return id % 7 == 0; }),
                 [](catalog::EditCommand& command) { command.favorite = true; });
    edit_samples(library, where([](std::int64_t id) { return id % 4 == 0; }),
                 [](catalog::EditCommand& command) { command.tags_add = {"Drums/Kick"}; });
    edit_samples(library, where([](std::int64_t id) { return id % 10 == 1; }),
                 [](catalog::EditCommand& command) { command.tags_add = {"Drums"}; });
    edit_samples(library, where([](std::int64_t id) { return id % 13 == 2; }),
                 [](catalog::EditCommand& command) { command.tags_add = {"Mood/Dark", "Hard"}; });
    edit_samples(library, where([](std::int64_t id) { return id % 8 == 3; }),
                 [](catalog::EditCommand& command) { command.category = std::optional<std::string>("Bass/Sub"); });
    edit_samples(library, where([](std::int64_t id) { return id % 8 == 5; }),
                 [](catalog::EditCommand& command) { command.category = std::optional<std::string>("Bass"); });
    edit_samples(library, where([](std::int64_t id) { return id % 8 == 6; }),
                 [](catalog::EditCommand& command) { command.category = std::optional<std::string>("Keys"); });

    catalog::TaxonomyCommand create{.kind = catalog::TaxonomyCommand::Kind::CreateCollection, .new_name = "Picks"};
    REQUIRE(library.apply_taxonomy(create).has_value());
    auto collections = library.list_collections();
    REQUIRE(collections.has_value());
    REQUIRE(collections->size() == 1);
    varied.collection = collections->front().id;
    edit_samples(library, where([](std::int64_t id) { return id % 17 == 0; }),
                 [&](catalog::EditCommand& command) { command.add_to_collection = varied.collection; });

    auto tags = library.list_tags();
    REQUIRE(tags.has_value());
    for (const auto& tag : *tags)
        if (tag.path == "Drums") varied.drums_tag = tag.id;
    auto categories = library.list_categories();
    REQUIRE(categories.has_value());
    for (const auto& category : *categories)
        if (category.path == "Bass") varied.bass_category = category.id;
    REQUIRE(varied.drums_tag != 0);
    REQUIRE(varied.bass_category != 0);
    return varied;
}

/// Every (library, id) of a request, following cursors with a small page size.
using SearchFunction = std::function<Result<search::SearchPage>(const search::SearchRequest&)>;
std::vector<app::SampleKey> collect(const SearchFunction& run, search::SearchRequest request) {
    request.page_size = 37;
    std::vector<app::SampleKey> keys;
    for (int page_number = 0; page_number < 1000; ++page_number) {
        auto page = run(request);
        REQUIRE_MESSAGE(page.has_value(), page.error().message());
        for (const auto& row : page->rows)
            keys.push_back(app::SampleKey{row.library, row.sample_id});
        if (!page->next) return keys;
        request.cursor = page->next;
    }
    FAIL("paging did not terminate");
    return keys;
}

/// Checks that the index and SearchService return the same rows in the same order, and the same
/// count, for one request.
void check_same_results(std::span<const app::IndexedLibrary> sources, std::span<catalog::Library* const> libraries,
                        const search::SearchRequest& request) {
    CAPTURE(request.text);
    CAPTURE(search::to_string(request.sort));
    CAPTURE(request.descending);
    CAPTURE(request.scope.root_id.value_or(-1));
    CAPTURE(request.scope.folder.value_or("<none>"));
    CAPTURE(request.scope.tag_id.value_or(-1));
    CAPTURE(request.scope.category_id.value_or(-1));
    CAPTURE(request.scope.collection_id.value_or(-1));
    CAPTURE(request.scope.uncategorized);
    search::SearchService service;
    const auto from_index =
        collect([&](const search::SearchRequest& paged) { return app::search_indexes(sources, paged); }, request);
    const auto from_sql =
        collect([&](const search::SearchRequest& paged) { return service.search(libraries, paged); }, request);
    CHECK(from_index == from_sql);
    auto index_count = app::count_indexes(sources, request);
    auto sql_count = service.count(libraries, request);
    REQUIRE(index_count.has_value());
    REQUIRE(sql_count.has_value());
    CHECK(index_count->total == sql_count->total);
    CHECK(index_count->total == static_cast<std::int64_t>(from_index.size()));
}

/// Requests covering every sort in both directions, row-evaluable and full-text queries, and every scope.
std::vector<search::SearchRequest> comparison_requests(const VariedLibrary& varied) {
    std::vector<search::SearchRequest> requests;
    for (const auto sort :
         {search::SortField::Name, search::SortField::Added, search::SortField::Duration, search::SortField::Size,
          search::SortField::SampleRate, search::SortField::Bpm, search::SortField::Key, search::SortField::Rating,
          search::SortField::Format, search::SortField::Relevance})
        for (const bool descending : {false, true})
            requests.push_back({.sort = sort, .descending = descending});
    for (const auto* text : {"kick", "kick OR snare", "NOT loop", "kic*", "bpm:120..150", "rating:>=3", "favorite:true",
                             "format:flac", "duration:<1s", "key:Am", "tag:Drums", "tag:Drums/Kick", "category:Bass",
                             "folder:Drums", "analyzed:tempo", "riser rating:>=2", "\"Pad 0\""})
        for (const auto sort : {search::SortField::Name, search::SortField::Duration})
            requests.push_back({.text = text, .sort = sort, .descending = sort == search::SortField::Duration});
    const std::int64_t root = varied.populated.root_id;
    const std::vector<search::Scope> scopes = {
        {.root_id = root},
        {.root_id = varied.second_root},
        {.root_id = root, .folder = "Drums"},
        {.root_id = root, .folder = "drums/kicks/"},
        {.root_id = varied.second_root, .folder = "Drums"},
        {.root_id = root, .folder = "Nowhere"},
        {.collection_id = varied.collection},
        {.tag_id = varied.drums_tag},
        {.category_id = varied.bass_category},
        {.category_id = 99'999},
        {.uncategorized = true},
        {.root_id = root, .folder = "Loops", .tag_id = varied.drums_tag},
    };
    for (const auto& scope : scopes)
        for (const auto sort : {search::SortField::Name, search::SortField::Bpm}) {
            requests.push_back({.scope = scope, .sort = sort, .descending = sort == search::SortField::Bpm});
            requests.push_back({.text = "kick", .scope = scope, .sort = sort});
            requests.push_back({.text = "rating:>=1", .scope = scope, .sort = sort});
        }
    return requests;
}

/// Loads an index for `library` and fails the test on error.
std::unique_ptr<app::SampleIndex> loaded_index(catalog::Library& library) {
    auto index = std::make_unique<app::SampleIndex>(library.uuid());
    auto loaded = index->load(library);
    REQUIRE_MESSAGE(loaded.has_value(), loaded.error().message());
    auto caught_up = index->synchronize(library);
    REQUIRE_MESSAGE(caught_up.has_value(), caught_up.error().message());
    return index;
}

/// Commits synthetic files through an open Application library.
void populate_through_application(app::Application& application, const uuids::uuid& library,
                                  const std::vector<catalog::ScannedFile>& files) {
    auto populated = application.with_library(library, [&](catalog::Library& opened) -> Result<void> {
        auto root = opened.add_root("Synthetic", catalog::RootMode::Referenced);
        if (!root) return std::unexpected(root.error());
        add_files(opened, root->id, files);
        return {};
    });
    REQUIRE(populated.has_value());
}

/// Collects application events from background threads.
class EventLog {
public:
    void record(const app::ApplicationEvent& event) {
        std::lock_guard lock(mutex_);
        events_.push_back({std::chrono::steady_clock::now(), event});
    }
    /// Messages of every BackgroundErrorEvent received.
    std::vector<std::string> errors() const {
        std::lock_guard lock(mutex_);
        std::vector<std::string> messages;
        for (const auto& [time, event] : events_)
            if (const auto* failure = std::get_if<app::BackgroundErrorEvent>(&event))
                messages.push_back(failure->context + ": " + failure->error.message());
        return messages;
    }
    /// Catalog events of one library with their arrival times.
    std::vector<std::pair<std::chrono::steady_clock::time_point, app::CatalogChangedEvent>> catalog_events(
        const uuids::uuid& library) const {
        std::lock_guard lock(mutex_);
        std::vector<std::pair<std::chrono::steady_clock::time_point, app::CatalogChangedEvent>> found;
        for (const auto& [time, event] : events_)
            if (const auto* changed = std::get_if<app::CatalogChangedEvent>(&event);
                changed && changed->library == library)
                found.emplace_back(time, *changed);
        return found;
    }

private:
    mutable std::mutex mutex_;
    std::vector<std::pair<std::chrono::steady_clock::time_point, app::ApplicationEvent>> events_;
};

}  // namespace

TEST_SUITE("app") {
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
            CHECK(root->availability == catalog::Availability::Online);
            auto duplicate = application->add_root(library_uuid, scratch / "source/.");
            REQUIRE_FALSE(duplicate.has_value());
            CHECK(duplicate.error().code == ErrorCode::InvalidArgument);
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

    TEST_CASE("a second copy or a ninth library is refused before the catalog is touched") {
        ScratchDirectory scratch;
        auto application = start_application(scratch / "workspace");
        auto first = application->create_library(scratch / "lib", "Once");
        REQUIRE(first.has_value());
        // A live analysis run must survive a refused re-open (no crash recovery of a live session).
        populate_through_application(*application, first->uuid, {test::synthetic_file("a.wav", 1, 1'000)});
        auto run = application->start_analysis(first->uuid, "Waveforms", {1});
        REQUIRE(run.has_value());
        auto again = application->open_library(scratch / "lib");
        REQUIRE_FALSE(again.has_value());
        CHECK(again.error().code == ErrorCode::InvalidArgument);
        auto runs = application->with_library(first->uuid,
                                              [](catalog::Library& opened) { return opened.list_analysis_runs(); });
        REQUIRE(runs.has_value());
        REQUIRE(runs->size() == 1);
        CHECK(runs->front().state != "paused");
        CHECK(runs->front().queued == 1);

        std::filesystem::copy(scratch / "lib", scratch / "copy", std::filesystem::copy_options::recursive);
        auto duplicate = application->open_library(scratch / "copy");
        REQUIRE_FALSE(duplicate.has_value());
        CHECK(duplicate.error().code == ErrorCode::InvalidArgument);
        for (int index = 0; index < 7; ++index)
            REQUIRE(application->create_library(scratch / std::format("more{}", index), "More").has_value());
        auto ninth = application->create_library(scratch / "ninth", "Too many");
        REQUIRE_FALSE(ninth.has_value());
        CHECK(ninth.error().code == ErrorCode::ResourceLimit);
        CHECK_FALSE(std::filesystem::exists(scratch / "ninth"));
    }

    TEST_CASE("a library locked by another process opens read-only, and recovery waits for the lock") {
        ScratchDirectory scratch;
        auto owner = start_application(scratch / "owner");
        auto created = owner->create_library(scratch / "lib", "Shared");
        REQUIRE(created.has_value());
        CHECK_FALSE(created->read_only);
        populate_through_application(*owner, created->uuid, {test::synthetic_file("a.wav", 1, 1'000)});

        // The lock is per handle, so a second Application in this process behaves like another process.
        auto competing = app::FileLock::try_acquire(scratch / "lib" / app::FileLock::kLibraryLockFileName);
        REQUIRE_FALSE(competing.has_value());
        CHECK(competing.error().code == ErrorCode::DbBusy);
        auto visitor = start_application(scratch / "visitor");
        auto visiting = visitor->open_library(scratch / "lib");
        REQUIRE(visiting.has_value());
        CHECK(visiting->read_only);
        CHECK(visiting->in_use_elsewhere);
        auto page = visitor->search({});
        REQUIRE(page.has_value());
        CHECK(page->rows.size() == 1);
        catalog::EditCommand edit;
        edit.sample_ids = {1};
        edit.rating = 4;
        CHECK_FALSE(visitor->apply_edit(visiting->uuid, edit).has_value());

        REQUIRE(owner->close_library(created->uuid).has_value());
        auto writer = start_application(scratch / "writer");
        auto writable = writer->open_library(scratch / "lib");
        REQUIRE(writable.has_value());
        CHECK_FALSE(writable->read_only);
        CHECK(writer->apply_edit(writable->uuid, edit).has_value());
    }

    TEST_CASE("with_library keeps a library alive across close_library") {
        ScratchDirectory scratch;
        auto application = start_application(scratch / "workspace");
        auto created = application->create_library(scratch / "lib", "Held");
        REQUIRE(created.has_value());
        auto roots = application->with_library(created->uuid, [&](catalog::Library& library) {
            // Closing from inside the work: the library must stay usable until the work returns.
            REQUIRE(application->close_library(created->uuid).has_value());
            CHECK(application->libraries().empty());
            return library.list_roots();
        });
        REQUIRE(roots.has_value());
        CHECK(roots->empty());
        auto after = application->with_library(created->uuid, [](catalog::Library&) { return 1; });
        REQUIRE_FALSE(after.has_value());
        CHECK(after.error().code == ErrorCode::NotFound);
        auto void_work = application->with_library(created->uuid, [](catalog::Library&) {});
        CHECK_FALSE(void_work.has_value());
        // Reopening works once the held entry is gone (the lock was released with it).
        CHECK(application->open_library(scratch / "lib").has_value());
    }

    TEST_CASE("a corrupt workspace file never prevents startup") {
        ScratchDirectory scratch;
        test::write_file(scratch / "workspace/workspace.json", "{ not json");
        auto application = start_application(scratch / "workspace");
        CHECK(application->libraries().empty());
        bool set_aside = false;
        for (const auto& entry : std::filesystem::directory_iterator(scratch / "workspace"))
            set_aside = set_aside || entry.path().filename().string().starts_with("workspace.unreadable-");
        CHECK(set_aside);
    }

    TEST_CASE("workspace entries of the wrong type are skipped, not thrown") {
        ScratchDirectory scratch;
        const auto good = uuids::to_string(new_uuid());
        test::write_file(scratch / "workspace.json",
                         nlohmann::json{{"version", 1},
                                        {"libraries",
                                         {{{"uuid", 5}, {"directory", "x"}},
                                          {{"uuid", good}, {"name", 7}, {"directory", "lib"}, {"open", "yes"}},
                                          "not an object"}},
                                        {"root_bindings", {{{"library", good}, {"root", good}, {"path", 3}}}},
                                        {"preferences", {{"volume", -6}}}}
                             .dump());
        auto workspace = app::Workspace::load(scratch.path());
        REQUIRE(workspace.has_value());
        const auto libraries = workspace->known_libraries();
        REQUIRE(libraries.size() == 1);
        CHECK(uuids::to_string(libraries.front().uuid) == good);
        CHECK(libraries.front().name.empty());
        CHECK_FALSE(libraries.front().open);
        CHECK(workspace->root_bindings(libraries.front().uuid).empty());
        CHECK(workspace->preference("volume") == -6);

        test::write_file(scratch / "newer/workspace.json", R"({"version": 99})");
        auto newer = app::Workspace::load(scratch / "newer");
        REQUIRE(newer.has_value());
        CHECK(newer->known_libraries().empty());
        CHECK_FALSE(std::filesystem::exists(scratch / "newer/workspace.json"));  // set aside, not overwritten
    }

    TEST_CASE("concurrent workspace writers merge instead of losing updates") {
        ScratchDirectory scratch;
        auto desktop = app::Workspace::load(scratch.path());
        auto cli = app::Workspace::load(scratch.path());
        REQUIRE(desktop.has_value());
        REQUIRE(cli.has_value());
        const auto library = new_uuid();
        REQUIRE(desktop->remember_library(app::KnownLibrary{library, "Mine", scratch / "lib", true}).has_value());
        REQUIRE(cli->bind_root(app::RootBinding{library, new_uuid(), scratch / "a"}).has_value());
        REQUIRE(desktop->bind_root(app::RootBinding{library, new_uuid(), scratch / "b"}).has_value());
        REQUIRE(cli->set_preference("cli", true).has_value());
        REQUIRE(desktop->set_preference("desktop", 1).has_value());

        auto reread = app::Workspace::load(scratch.path());
        REQUIRE(reread.has_value());
        CHECK(reread->known_libraries().size() == 1);
        CHECK(reread->root_bindings(library).size() == 2);
        CHECK(reread->preference("cli") == true);
        CHECK(reread->preference("desktop") == 1);

        // Threads of two "processes" writing at once: every update lands.
        std::vector<std::jthread> writers;
        for (int writer = 0; writer < 4; ++writer)
            writers.emplace_back([&, writer] {
                auto& target = writer % 2 ? *desktop : *cli;
                for (int index = 0; index < 10; ++index)
                    CHECK(target.set_preference(std::format("key{}-{}", writer, index), index).has_value());
            });
        writers.clear();
        auto final_state = app::Workspace::load(scratch.path());
        REQUIRE(final_state.has_value());
        for (int writer = 0; writer < 4; ++writer)
            for (int index = 0; index < 10; ++index)
                CHECK(final_state->preference(std::format("key{}-{}", writer, index)) == index);
    }

    TEST_CASE("the in-memory index answers exactly like SearchService") {
        ScratchDirectory scratch;
        auto varied = make_varied_library(scratch / "lib", 640);
        auto& library = *varied.populated.library;
        auto index = loaded_index(library);
        CHECK(index->size() == 643);
        const app::IndexedLibrary sources[] = {{index.get(), &library, "Varied"}};
        catalog::Library* libraries[] = {&library};
        for (const auto& request : comparison_requests(varied))
            if (app::SampleIndex::serves(request)) check_same_results(sources, libraries, request);

        SUBCASE("rows carry the same data") {
            search::SearchRequest request{.sort = search::SortField::Added, .page_size = 500};
            auto from_index = app::search_indexes(sources, request);
            auto from_sql = search::SearchService().search(libraries, request);
            REQUIRE(from_index.has_value());
            REQUIRE(from_sql.has_value());
            REQUIRE(from_index->rows.size() == from_sql->rows.size());
            for (std::size_t row = 0; row < from_sql->rows.size(); ++row) {
                const auto& left = from_index->rows[row];
                const auto& right = from_sql->rows[row];
                CHECK(left.display_name == right.display_name);
                CHECK(left.relative_path == right.relative_path);
                CHECK(left.rating == right.rating);
                CHECK(left.favorite == right.favorite);
                CHECK(left.tags == right.tags);
                CHECK(left.category == right.category);
                CHECK(left.bpm_milli == right.bpm_milli);
                CHECK(left.availability == right.availability);
            }
        }
        SUBCASE("a folder scope without its root is a request error") {
            auto page = app::search_indexes(sources, {.scope = {.folder = "Drums"}});
            REQUIRE_FALSE(page.has_value());
            CHECK(page.error().code == ErrorCode::InvalidArgument);
        }
        SUBCASE("relevance ordering of text terms is left to SQLite") {
            CHECK_FALSE(app::SampleIndex::serves({.text = "kick", .sort = search::SortField::Relevance}));
            CHECK(app::SampleIndex::serves({.text = "bpm:>100", .sort = search::SortField::Relevance}));
        }
        SUBCASE("folders are derived from the rows") {
            const auto folders = index->folders(varied.populated.root_id);
            REQUIRE_FALSE(folders.empty());
            CHECK(folders.front().relative_path.empty());
            CHECK(folders.front().total_samples == 640);
            const auto drums = std::ranges::find(folders, std::string("Drums"), &app::IndexedFolder::relative_path);
            REQUIRE(drums != folders.end());
            CHECK(drums->direct_samples == 0);
            CHECK(drums->total_samples > 0);
        }
    }

    TEST_CASE("federated index search orders across libraries like SearchService") {
        ScratchDirectory scratch;
        auto first = make_varied_library(scratch / "one", 130, "One");
        auto second = make_varied_library(scratch / "two", 90, "Two");
        auto first_index = loaded_index(*first.populated.library);
        auto second_index = loaded_index(*second.populated.library);
        const app::IndexedLibrary sources[] = {{first_index.get(), first.populated.library.get(), "One"},
                                               {second_index.get(), second.populated.library.get(), "Two"}};
        catalog::Library* libraries[] = {first.populated.library.get(), second.populated.library.get()};
        for (const auto sort : {search::SortField::Name, search::SortField::Duration, search::SortField::Bpm,
                                search::SortField::Key, search::SortField::Format})
            for (const bool descending : {false, true}) {
                check_same_results(sources, libraries, {.sort = sort, .descending = descending});
                check_same_results(sources, libraries, {.text = "kick", .sort = sort, .descending = descending});
                check_same_results(sources, libraries, {.text = "rating:>=2", .sort = sort, .descending = descending});
            }
    }

    TEST_CASE("the index follows edits, removals, new files and taxonomy changes incrementally") {
        ScratchDirectory scratch;
        auto varied = make_varied_library(scratch / "lib", 200);
        auto& library = *varied.populated.library;
        auto index = loaded_index(library);
        const app::IndexedLibrary sources[] = {{index.get(), &library, "Varied"}};
        catalog::Library* libraries[] = {&library};
        const auto before = index->generations();

        edit_samples(library, {5, 6, 7}, [](catalog::EditCommand& command) { command.rating = 3; });  // none had 3
        auto change = index->synchronize(library);
        REQUIRE(change.has_value());
        CHECK(change->sample_ids == std::vector<std::int64_t>{5, 6, 7});
        CHECK_FALSE(change->rows_added_or_removed);
        CHECK(index->row(6)->rating == 3);
        CHECK(index->generations().structure == before.structure);
        CHECK(index->generations().data > before.data);

        edit_samples(library, {8}, [](catalog::EditCommand& command) { command.removed = true; });
        change = index->synchronize(library);
        REQUIRE(change.has_value());
        CHECK(change->rows_added_or_removed);
        CHECK_FALSE(index->row(8).has_value());
        CHECK(index->generations().structure > before.structure);

        edit_samples(library, {8}, [](catalog::EditCommand& command) { command.removed = false; });
        add_files(
            library, varied.populated.root_id,
            {test::synthetic_file("Drums/new kick.wav", 5, 5'000), test::synthetic_file("brand new.wav", 6, 6'000)});
        edit_samples(library, {9, 10}, [](catalog::EditCommand& command) { command.tags_add = {"Fresh/Tag"}; });
        catalog::TaxonomyCommand rename{
            .kind = catalog::TaxonomyCommand::Kind::RenameTag, .target_id = varied.drums_tag, .new_name = "Percussion"};
        REQUIRE(library.apply_taxonomy(rename).has_value());
        change = index->synchronize(library);
        REQUIRE(change.has_value());
        CHECK(change->rows_added_or_removed);
        CHECK(change->structure_changed);
        CHECK(index->row(8).has_value());
        CHECK(index->size() == 205);
        const auto renamed = index->row(4);  // tagged Drums/Kick, now Percussion/Kick
        REQUIRE(renamed.has_value());
        CHECK(std::ranges::find(renamed->tag_paths, std::string("Percussion/Kick")) != renamed->tag_paths.end());

        for (const auto& request : comparison_requests(varied))
            if (app::SampleIndex::serves(request)) check_same_results(sources, libraries, request);
    }

    TEST_CASE("index cursors survive value edits and go stale when rows are added") {
        ScratchDirectory scratch;
        auto varied = make_varied_library(scratch / "lib", 120);
        auto& library = *varied.populated.library;
        auto index = loaded_index(library);
        const app::IndexedLibrary sources[] = {{index.get(), &library, "Varied"}};
        search::SearchRequest request{.sort = search::SortField::Name, .page_size = 50};
        auto first = app::search_indexes(sources, request);
        REQUIRE(first.has_value());
        REQUIRE(first->next.has_value());

        edit_samples(library, {first->rows.back().sample_id},
                     [](catalog::EditCommand& command) { command.rating = 1; });
        REQUIRE(index->synchronize(library).has_value());
        request.cursor = first->next;
        auto second = app::search_indexes(sources, request);
        REQUIRE(second.has_value());
        CHECK(second->rows.size() == 50);

        add_files(library, varied.populated.root_id, {test::synthetic_file("aaa first.wav", 1, 1)});
        REQUIRE(index->synchronize(library).has_value());
        auto stale = app::search_indexes(sources, request);
        REQUIRE_FALSE(stale.has_value());
        CHECK(stale.error().code == ErrorCode::StaleCursor);

        search::SearchRequest other{.sort = search::SortField::Bpm, .cursor = first->next};
        auto wrong_query = app::search_indexes(sources, other);
        REQUIRE_FALSE(wrong_query.has_value());
        CHECK(wrong_query.error().code == ErrorCode::StaleCursor);
    }

    TEST_CASE("edits through the application patch the index before returning, events are coalesced") {
        ScratchDirectory scratch;
        auto options = quiet_options(scratch / "workspace");
        auto started = app::Application::start(options);
        REQUIRE(started.has_value());
        auto& application = **started;
        EventLog log;
        application.set_event_handler([&](const app::ApplicationEvent& event) { log.record(event); });
        auto created = application.create_library(scratch / "lib", "Live");
        REQUIRE(created.has_value());
        std::vector<catalog::ScannedFile> files;
        for (int index = 0; index < 50; ++index)
            files.push_back(test::synthetic_file(std::format("Drums/hit {:02}.wav", index), 10, 10'000));
        populate_through_application(application, created->uuid, files);
        REQUIRE(test::wait_until([&] { return application.index_ready(created->uuid); }, kBackgroundTimeout));
        REQUIRE(test::wait_until(
            [&] {
                auto page = application.search({}, created->uuid);
                return page && page->rows.size() == 50;
            },
            kBackgroundTimeout));

        auto rate = [](std::int64_t sample_id, int rating) {
            catalog::EditCommand edit;  // a fresh command (and idempotency key) per edit
            edit.sample_ids = {sample_id};
            edit.rating = rating;
            return edit;
        };
        REQUIRE(application.apply_edit(created->uuid, rate(3, 4)).has_value());
        const auto rows = application.rows({app::SampleKey{created->uuid, 3}, app::SampleKey{created->uuid, 999}});
        REQUIRE(rows.size() == 1);
        CHECK(rows.front().rating == 4);  // no waiting: the index was patched by apply_edit
        auto rated = application.count({.text = "rating:4"}, created->uuid);
        REQUIRE(rated.has_value());
        CHECK(rated->total == 1);

        REQUIRE(application.undo(created->uuid).has_value());
        CHECK(application.rows({app::SampleKey{created->uuid, 3}}).front().rating == 0);
        REQUIRE(application.redo(created->uuid).has_value());
        CHECK(application.rows({app::SampleKey{created->uuid, 3}}).front().rating == 4);

        // A burst of edits is announced together (a full reload still pending absorbs them).
        for (int round = 0; round < 12; ++round)
            REQUIRE(application.apply_edit(created->uuid, rate(10 + round, 1 + round % 5)).has_value());
        const auto burst_end = std::chrono::steady_clock::now();
        auto announced_after = [&](const std::function<bool(const app::CatalogChangedEvent&)>& predicate) {
            return test::wait_until(
                [&] {
                    for (const auto& [time, event] : log.catalog_events(created->uuid))
                        if (time >= burst_end && predicate(event)) return true;
                    return false;
                },
                kBackgroundTimeout);
        };
        auto lists = [](std::int64_t sample_id) {
            return [sample_id](const app::CatalogChangedEvent& event) {
                return std::ranges::find(event.sample_ids, sample_id) != event.sample_ids.end();
            };
        };
        CHECK(announced_after(
            [&](const app::CatalogChangedEvent& event) { return event.full_reload || lists(21)(event); }));
        // With nothing else pending, an edit is announced with its sample id for an in-place refresh.
        REQUIRE(application.apply_edit(created->uuid, rate(30, 5)).has_value());
        CHECK(announced_after(lists(30)));
        const auto events = log.catalog_events(created->uuid);
        for (const auto& message : log.errors())
            FAIL_CHECK(message);
        for (std::size_t index = 1; index < events.size(); ++index)
            CHECK(events[index].first - events[index - 1].first >= app::Application::kEventInterval - 20ms);
        application.set_event_handler({});
    }

    TEST_CASE("automatic mode creates a default library and queues set-based analysis after an import") {
        ScratchDirectory scratch;
        test::write_sine_wav(scratch / "pack/Kick.wav", 0.2);
        test::write_sine_wav(scratch / "pack/Loop 128bpm.wav", 1.0);
        auto options = quiet_options(scratch / "workspace");
        options.automatic_mode = true;  // the queue is checked, not executed, here
        auto application = app::Application::start(options);
        REQUIRE(application.has_value());
        auto& app = **application;
        std::mutex errors_mutex;
        std::vector<std::string> background_errors;
        app.set_event_handler([&](const app::ApplicationEvent& event) {
            std::lock_guard lock(errors_mutex);
            if (const auto* failure = std::get_if<app::BackgroundErrorEvent>(&event))
                background_errors.push_back(failure->context + ": " + failure->error.message());
            if (const auto* finished = std::get_if<app::OperationFinishedEvent>(&event); finished && finished->error)
                background_errors.push_back(finished->description + ": " + finished->error->message());
        });

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
        app.wait_for_operation(*operation);  // finished operations are pruned; waiting again returns at once

        {
            std::lock_guard lock(errors_mutex);
            for (const auto& error : background_errors)
                FAIL_CHECK(error);
        }
        auto runs =
            app.with_library(library.uuid, [](catalog::Library& opened) { return opened.list_analysis_runs(); });
        REQUIRE(runs.has_value());
        REQUIRE(runs->size() == 1);
        CHECK(runs->front().profile_name == "Full");
        CHECK(runs->front().queued == 2);
        CHECK(app.libraries().front().root_count == 1);

        auto second = app.create_library_in_workspace("My Samples");
        REQUIRE(second.has_value());
        CHECK(second->directory.filename() == "My Samples 2");
        auto reserved = app.create_library_in_workspace("CON");
        REQUIRE(reserved.has_value());
        CHECK(reserved->directory.filename() == "_CON");
    }

    TEST_CASE("cancelling an import right after starting it is never lost") {
        ScratchDirectory scratch;
        for (int index = 0; index < 20; ++index)
            test::write_sine_wav(scratch / std::format("pack/tone {}.wav", index), 0.05);
        auto application = start_application(scratch / "workspace");
        auto created = application->create_library(scratch / "lib", "Cancel");
        REQUIRE(created.has_value());
        auto root = application->add_root(created->uuid, scratch / "pack");
        REQUIRE(root.has_value());
        std::optional<app::OperationFinishedEvent> finished;
        std::mutex finished_mutex;
        application->set_event_handler([&](const app::ApplicationEvent& event) {
            if (const auto* done = std::get_if<app::OperationFinishedEvent>(&event)) {
                std::lock_guard lock(finished_mutex);
                finished = *done;
            }
        });
        auto operation = application->start_import(created->uuid, root->id);
        REQUIRE(operation.has_value());
        application->cancel_operation(*operation);
        application->wait_for_operation(*operation);
        application->set_event_handler({});
        std::lock_guard lock(finished_mutex);
        REQUIRE(finished.has_value());
        REQUIRE(finished->error.has_value());
        CHECK(finished->error->code == ErrorCode::Cancelled);
    }

    TEST_CASE("a missing worker pauses analysis and reports it instead of spinning") {
        ScratchDirectory scratch;
        auto options = quiet_options(scratch / "workspace");
        options.run_analysis_supervisor = true;
        options.worker_executable = scratch / "no-such-worker.exe";
        auto application = app::Application::start(options);
        REQUIRE(application.has_value());
        auto& app = **application;
        auto created = app.create_library(scratch / "lib", "Workerless");
        REQUIRE(created.has_value());
        test::write_sine_wav(scratch / "pack/tone.wav", 0.1);
        auto root = app.add_root(created->uuid, scratch / "pack");
        REQUIRE(root.has_value());
        REQUIRE(app.import_now(created->uuid, root->id).has_value());
        REQUIRE(app.start_analysis(created->uuid, "Waveforms", {1}).has_value());
        app.set_analysis_paused(false);
        REQUIRE(test::wait_until([&] { return app.analysis_status().worker_unavailable; }, kBackgroundTimeout));
        CHECK(app.analysis_status().paused);
        CHECK_FALSE(app.analysis_status().detail.empty());
        auto runs =
            app.with_library(created->uuid, [](catalog::Library& opened) { return opened.list_analysis_runs(); });
        REQUIRE(runs.has_value());
        CHECK(runs->front().queued == 1);  // released, not failed
    }

    TEST_CASE("rebinding a root makes its samples playable again and refuses another root's folder") {
        ScratchDirectory scratch;
        test::write_sine_wav(scratch / "drive E/pack/Kick.wav", 0.1);
        test::write_sine_wav(scratch / "other/Snare.wav", 0.1);
        auto application = start_application(scratch / "workspace");
        auto created = application->create_library(scratch / "lib", "Rebind");
        REQUIRE(created.has_value());
        auto root = application->add_root(created->uuid, scratch / "drive E/pack");
        REQUIRE(root.has_value());
        auto other = application->add_root(created->uuid, scratch / "other");
        REQUIRE(other.has_value());
        REQUIRE(application->import_now(created->uuid, root->id).has_value());
        std::filesystem::rename(scratch / "drive E", scratch / "drive F");
        auto offline = application->resolve_playable_path(created->uuid, 1);
        REQUIRE_FALSE(offline.has_value());
        CHECK(offline.error().code == ErrorCode::SourceOffline);
        auto taken = application->rebind_root(created->uuid, root->id, scratch / "other");
        REQUIRE_FALSE(taken.has_value());
        CHECK(taken.error().code == ErrorCode::InvalidArgument);
        REQUIRE(application->rebind_root(created->uuid, root->id, scratch / "drive F/pack").has_value());
        auto path = application->resolve_playable_path(created->uuid, 1);
        REQUIRE(path.has_value());
        CHECK(path->filename() == "Kick.wav");
        CHECK(application->workspace().root_binding(created->uuid, root->uuid) ==
              std::filesystem::canonical(scratch / "drive F/pack"));
    }

    TEST_CASE("the file lock excludes a second holder and is released with its owner") {
        ScratchDirectory scratch;
        const auto file = scratch / "test.lock";
        {
            auto held = app::FileLock::try_acquire(file);
            REQUIRE(held.has_value());
            CHECK(held->held());
            auto second = app::FileLock::try_acquire(file);
            REQUIRE_FALSE(second.has_value());
            CHECK(second.error().code == ErrorCode::DbBusy);
            CHECK(second.error().retryable);
            auto waited = app::FileLock::acquire(file, 30ms);
            CHECK_FALSE(waited.has_value());
        }
        CHECK(app::FileLock::try_acquire(file).has_value());
        auto missing = app::FileLock::try_acquire(scratch / "missing/dir/test.lock");
        REQUIRE_FALSE(missing.has_value());
        CHECK(missing.error().code == ErrorCode::FileMissing);
    }

}  // TEST_SUITE("app")
