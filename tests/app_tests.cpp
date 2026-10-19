// The application layer: library lifetime and locking (closing, reopening, read-only upgrades),
// workspace persistence and merging, the in-memory SampleIndex (checked against SearchService on the
// same data, keyset paging while values change, its row budget, sort orders patched in place),
// incremental index updates, coalesced reloads and events, automatic analysis, and the analysis
// supervisor (deadlines, abandoned libraries, worker start backoff) against a --self-test worker.
#include <doctest.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <format>
#include <mutex>
#include <numeric>
#include <set>
#include <thread>

#include <nlohmann/json.hpp>

#include "mso/app/analysis_supervisor.hpp"
#include "mso/app/application.hpp"
#include "mso/app/detail/index_load_schedule.hpp"
#include "mso/app/detail/open_progress_relay.hpp"
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

/// Pages through `request` with `run`, calling `between` after every page (to change values while
/// paging), and returns every (library, id) in the order returned.
std::vector<app::SampleKey> collect_while_editing(const SearchFunction& run, search::SearchRequest request,
                                                  const std::function<void(int page_number)>& between) {
    std::vector<app::SampleKey> keys;
    for (int page_number = 0; page_number < 1000; ++page_number) {
        auto page = run(request);
        REQUIRE_MESSAGE(page.has_value(), page.error().message());
        for (const auto& row : page->rows)
            keys.push_back(app::SampleKey{row.library, row.sample_id});
        if (!page->next) return keys;
        between(page_number);
        request.cursor = page->next;
    }
    FAIL("paging did not terminate");
    return keys;
}

/// Checks keyset paging under edits: every row that was never edited appears exactly once, and in
/// the order of a fresh query after all edits.
void check_unedited_rows_once(const std::vector<app::SampleKey>& paged, const std::vector<app::SampleKey>& fresh,
                              const std::set<app::SampleKey>& edited) {
    std::vector<app::SampleKey> paged_unedited;
    std::ranges::copy_if(paged, std::back_inserter(paged_unedited),
                         [&](const app::SampleKey& key) { return !edited.contains(key); });
    std::vector<app::SampleKey> fresh_unedited;
    std::ranges::copy_if(fresh, std::back_inserter(fresh_unedited),
                         [&](const app::SampleKey& key) { return !edited.contains(key); });
    CHECK(paged_unedited == fresh_unedited);
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
    /// Number of LibraryAccessChangedEvents of one library.
    int access_changes(const uuids::uuid& library) const {
        std::lock_guard lock(mutex_);
        return static_cast<int>(std::ranges::count_if(events_, [&](const auto& entry) {
            const auto* changed = std::get_if<app::LibraryAccessChangedEvent>(&entry.second);
            return changed != nullptr && changed->library == library;
        }));
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

/// Supervisor time limits for tests: fast polling, production-sized leases and deadlines.
app::AnalysisSupervisor::Timing fast_timing() {
    app::AnalysisSupervisor::Timing timing;
    timing.idle_wait = 20ms;
    timing.event_poll = 20ms;
    return timing;
}

/// A --self-test worker of this build: it never opens the input and simulates two minutes of work
/// per task (it honors cancellation), so a job stays running until the supervisor ends it.
worker::WorkerOptions self_test_worker() {
    return worker::WorkerOptions{.executable = test::worker_executable(), .self_test = true};
}

/// A library with one sample and one queued analysis job, plus the hooks an AnalysisSupervisor
/// needs to serve it. Records the worker statuses and errors the supervisor reports. Declare the
/// supervisor after it, so the supervisor stops first.
class SupervisedLibrary {
public:
    SupervisedLibrary() {
        auto populated = test::make_library(scratch_ / "lib", {test::synthetic_file("tone.wav", 100, 100'000)});
        library_ = std::shared_ptr<catalog::Library>(std::move(populated.library));
        REQUIRE(library_->set_root_availability(populated.root_id, catalog::Availability::Online).has_value());
        test::write_sine_wav(audio_file(), 0.1);
        auto run = library_->create_analysis_run({"Musical", {"loudness"}, {sample_id()}, false});
        REQUIRE(run.has_value());
        REQUIRE(run->queued == 1);
    }

    /// Hooks serving this library; the sample always resolves to a real file.
    app::AnalysisSupervisor::Hooks hooks() {
        return app::AnalysisSupervisor::Hooks{
            [this] { return std::vector<std::shared_ptr<catalog::Library>>{library_}; },
            [this](catalog::Library&, const catalog::SampleDetail&) -> Result<std::filesystem::path> {
                return audio_file();
            },
            [this](catalog::Library&, const std::string& status) {
                std::lock_guard lock(mutex_);
                statuses_.push_back(status);
            },
            [this](catalog::Library*, const std::string& context, const Error& error) {
                std::lock_guard lock(mutex_);
                errors_.push_back(context + ": " + error.message());
            }};
    }

    /// One column of the job, as text ("" for NULL).
    std::string job_column(std::string_view column) {
        auto reader = library_->acquire_reader();
        REQUIRE(reader.has_value());
        auto statement = reader->connection().prepare(std::format("SELECT {} FROM jobs ORDER BY id LIMIT 1", column));
        REQUIRE(statement.has_value());
        auto has_row = statement->next_row();
        REQUIRE(has_row.has_value());
        REQUIRE(*has_row);
        return statement->column_is_null(0) ? std::string() : statement->column_text(0);
    }

    /// How often the supervisor reported the worker as unavailable.
    int unavailable_reports() const {
        std::lock_guard lock(mutex_);
        return static_cast<int>(std::ranges::count_if(
            statuses_, [](const std::string& status) { return status.starts_with("worker unavailable"); }));
    }
    /// Every error the supervisor reported.
    std::vector<std::string> errors() const {
        std::lock_guard lock(mutex_);
        return errors_;
    }
    /// Options of a "worker" that exists but cannot be started: a transient start failure.
    worker::WorkerOptions unstartable_worker() const {
        const auto file = scratch_ / "not-a-program.exe";
        test::write_file(file, "this is not an executable");
        return worker::WorkerOptions{.executable = file};
    }

    catalog::Library& library() { return *library_; }
    const std::filesystem::path& scratch() const { return scratch_.path(); }

private:
    std::filesystem::path audio_file() const { return scratch_ / "pack/tone.wav"; }
    std::int64_t sample_id() {
        auto reader = library_->acquire_reader();
        REQUIRE(reader.has_value());
        auto id = reader->connection().query_int64("SELECT min(id) FROM samples");
        REQUIRE(id.has_value());
        return *id;
    }

    ScratchDirectory scratch_{"supervisor"};
    std::shared_ptr<catalog::Library> library_;
    mutable std::mutex mutex_;
    std::vector<std::string> statuses_;
    std::vector<std::string> errors_;
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
                CHECK(left.tag_paths == right.tag_paths);
                CHECK(left.tag_ids == right.tag_ids);
                CHECK(left.path_key == right.path_key);
                CHECK(left.file_name == right.file_name);
                CHECK(left.folder_path == right.folder_path);
                CHECK(left.root_name == right.root_name);
                CHECK(left.duration_us == right.duration_us);
                CHECK(left.key == right.key);
                CHECK(left.analysis_flags == right.analysis_flags);
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

        // A merged tag's old name keeps matching its new home, in memory exactly as in SQL.
        auto tags = library.list_tags();
        REQUIRE(tags.has_value());
        std::int64_t hard = 0;
        std::int64_t dark = 0;
        for (const auto& tag : *tags) {
            if (tag.path == "Hard") hard = tag.id;
            if (tag.path == "Mood/Dark") dark = tag.id;
        }
        REQUIRE(hard != 0);
        REQUIRE(dark != 0);
        catalog::TaxonomyCommand merge{
            .kind = catalog::TaxonomyCommand::Kind::MergeTag, .target_id = hard, .other_id = dark};
        REQUIRE(library.apply_taxonomy(merge).has_value());
        change = index->synchronize(library);
        REQUIRE(change.has_value());
        CHECK(change->structure_changed);
        for (const auto* text : {"tag:Hard", "tag:Dark", "NOT tag:Hard", "tag:Mood"})
            check_same_results(sources, libraries, {.text = text});
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

    TEST_CASE("keyset cursors never skip or repeat an unedited row while ratings and BPMs change") {
        ScratchDirectory scratch;
        auto varied = make_varied_library(scratch / "lib", 300);
        auto& library = *varied.populated.library;
        auto index = loaded_index(library);
        const app::IndexedLibrary sources[] = {{index.get(), &library, "Varied"}};
        auto run = [&](const search::SearchRequest& request) { return app::search_indexes(sources, request); };

        const std::vector<search::SearchRequest> requests = {
            {.sort = search::SortField::Rating, .descending = true, .page_size = 40},
            {.sort = search::SortField::Rating, .page_size = 40},
            {.sort = search::SortField::Bpm, .page_size = 40},
            {.sort = search::SortField::Bpm, .descending = true, .page_size = 40},
            {.text = "rating:>=1", .sort = search::SortField::Name, .page_size = 40},
            {.text = "bpm:80..200", .sort = search::SortField::Bpm, .page_size = 40},
        };
        int round = 0;
        for (const auto& request : requests) {
            CAPTURE(request.text);
            CAPTURE(search::to_string(request.sort));
            CAPTURE(request.descending);
            std::set<app::SampleKey> edited;
            const auto paged = collect_while_editing(run, request, [&](int page_number) {
                // Move rows across the cursor in both directions: ids before and after it change
                // rating, BPM and filter membership, like analysis results and ratings arriving.
                ++round;
                const std::vector<std::int64_t> ids = {1 + (page_number * 7) % 300, 150 + (page_number * 13) % 150,
                                                       2 + (page_number * 29) % 290};
                for (const auto id : ids)
                    edited.insert(app::SampleKey{library.uuid(), id});
                edit_samples(library, ids, [&](catalog::EditCommand& command) {
                    command.rating = (round + page_number) % 6;
                    command.bpm = catalog::BpmOverrideEdit{catalog::OverrideState::Value,
                                                           std::int64_t{60'000 + (round * 37'000) % 150'000}};
                });
                auto change = index->synchronize(library);
                REQUIRE(change.has_value());
                CHECK_FALSE(change->rows_added_or_removed);  // values only: cursors stay valid
            });
            search::SearchRequest fresh_request = request;
            fresh_request.cursor.reset();
            check_unedited_rows_once(paged, collect(run, fresh_request), edited);
            // Nothing is returned twice unless it was edited across the cursor.
            std::map<app::SampleKey, int> seen;
            for (const auto& key : paged)
                if (++seen[key] > 1) CHECK(edited.contains(key));
        }
    }

    TEST_CASE("federated keyset cursors keep each library's position while values change") {
        ScratchDirectory scratch;
        auto first = make_varied_library(scratch / "one", 120, "One");
        auto second = make_varied_library(scratch / "two", 80, "Two");
        auto first_index = loaded_index(*first.populated.library);
        auto second_index = loaded_index(*second.populated.library);
        const app::IndexedLibrary sources[] = {{first_index.get(), first.populated.library.get(), "One"},
                                               {second_index.get(), second.populated.library.get(), "Two"}};
        auto run = [&](const search::SearchRequest& request) { return app::search_indexes(sources, request); };
        std::set<app::SampleKey> edited;
        const search::SearchRequest request{.sort = search::SortField::Rating, .descending = true, .page_size = 25};
        const auto paged = collect_while_editing(run, request, [&](int page_number) {
            auto& library = page_number % 2 ? *second.populated.library : *first.populated.library;
            auto& index = page_number % 2 ? *second_index : *first_index;
            const std::int64_t id = 1 + page_number * 5;
            edited.insert(app::SampleKey{library.uuid(), id});
            edit_samples(library, {id}, [&](catalog::EditCommand& command) { command.rating = 5 - page_number % 6; });
            REQUIRE(index.synchronize(library).has_value());
        });
        check_unedited_rows_once(paged, collect(run, request), edited);
        CHECK(paged.size() >= 203 - edited.size());
    }

    TEST_CASE("SampleIndex::load stops when its token fires and keeps its previous state") {
        ScratchDirectory scratch;
        auto varied = make_varied_library(scratch / "lib", 60);
        auto& library = *varied.populated.library;
        app::SampleIndex index(library.uuid());
        std::stop_source stop;
        stop.request_stop();
        auto loaded = index.load(library, stop.get_token());
        REQUIRE_FALSE(loaded.has_value());
        CHECK(loaded.error().code == ErrorCode::Cancelled);
        CHECK_FALSE(index.ready());
        // The cancelled stream gave its reader back: the next load reads every row.
        REQUIRE(index.load(library).has_value());
        CHECK(index.size() == 63);
    }

    TEST_CASE("a library over the index budget is answered from SQLite with the same results") {
        ScratchDirectory scratch;
        SUBCASE("SampleIndex::load refuses more rows than its limit and keeps its previous state") {
            auto varied = make_varied_library(scratch / "lib", 60);
            auto& library = *varied.populated.library;
            app::SampleIndex index(library.uuid());
            auto loaded = index.load(library, {}, 10);
            REQUIRE_FALSE(loaded.has_value());
            CHECK(loaded.error().code == ErrorCode::ResourceLimit);
            CHECK_FALSE(index.ready());
            REQUIRE(index.load(library, {}, 1000).has_value());
            CHECK(index.size() == 63);
            index.clear();
            CHECK_FALSE(index.ready());
            CHECK(index.size() == 0);
            const app::IndexedLibrary sources[] = {{&index, &library, "Varied"}};
            auto page = app::search_indexes(sources, {});
            REQUIRE_FALSE(page.has_value());
            CHECK(page.error().code == ErrorCode::IndexUnavailable);
        }
        SUBCASE("the Application falls back to SQLite and says so") {
            auto options = quiet_options(scratch / "workspace");
            options.index_row_limit = 20;
            auto started = app::Application::start(options);
            REQUIRE(started.has_value());
            auto& application = **started;
            EventLog log;
            application.set_event_handler([&](const app::ApplicationEvent& event) { log.record(event); });
            auto created = application.create_library(scratch / "big", "Big");
            REQUIRE(created.has_value());
            std::vector<catalog::ScannedFile> files;
            for (int index = 0; index < 50; ++index)
                files.push_back(test::synthetic_file(std::format("Loops/loop {:02}.wav", index), 10, 10'000 + index));
            populate_through_application(application, created->uuid, files);
            REQUIRE(test::wait_until(
                [&] {
                    for (const auto& message : log.errors())
                        if (message.find("in-memory index limit") != std::string::npos) return true;
                    return false;
                },
                kBackgroundTimeout));
            CHECK_FALSE(application.index_ready(created->uuid));
            auto page = application.search(
                {.text = "rating:0", .sort = search::SortField::Duration, .descending = true}, created->uuid);
            REQUIRE(page.has_value());
            CHECK(page->rows.size() == 50);
            CHECK(page->rows.front().duration_us == 10'049);
            catalog::EditCommand edit;
            edit.sample_ids = {1};
            edit.rating = 3;
            REQUIRE(application.apply_edit(created->uuid, edit).has_value());
            const std::vector<app::SampleKey> key = {{created->uuid, 1}};
            auto rows = application.rows(key);
            REQUIRE(rows.has_value());
            REQUIRE(rows->size() == 1);
            CHECK(rows->front().rating == 3);
            application.set_event_handler({});
        }
    }

    TEST_CASE("reopening a library that is still closing waits for it instead of opening it read-only") {
        ScratchDirectory scratch;
        auto options = quiet_options(scratch / "workspace");
        options.close_wait = 100ms;
        auto started = app::Application::start(options);
        REQUIRE(started.has_value());
        auto& application = **started;
        auto created = application.create_library(scratch / "lib", "Closing");
        REQUIRE(created.has_value());
        std::atomic<bool> holding{false};
        std::atomic<bool> release{false};
        std::jthread holder([&] {
            // Long work of this process (like an import) keeps the closed library and its lock.
            auto held = application.with_library(created->uuid, [&](catalog::Library&) {
                holding = true;
                CHECK(test::wait_until([&] { return release.load(); }, kBackgroundTimeout));
            });
            CHECK(held.has_value());
        });
        REQUIRE(test::wait_until([&] { return holding.load(); }, kBackgroundTimeout));
        REQUIRE(application.close_library(created->uuid).has_value());
        auto too_early = application.open_library(scratch / "lib");
        REQUIRE_FALSE(too_early.has_value());
        CHECK(too_early.error().code == ErrorCode::DbBusy);
        CHECK(too_early.error().retryable);
        release = true;
        holder.join();
        auto reopened = application.open_library(scratch / "lib");
        REQUIRE(reopened.has_value());
        CHECK_FALSE(reopened->read_only);
        CHECK_FALSE(reopened->in_use_elsewhere);
    }

    TEST_CASE("a library opened read-only while another process held it becomes writable when it is released") {
        ScratchDirectory scratch;
        auto cli = start_application(scratch / "cli");
        auto created = cli->create_library(scratch / "lib", "Shared");
        REQUIRE(created.has_value());
        populate_through_application(*cli, created->uuid, {test::synthetic_file("a.wav", 1, 1'000)});

        auto desktop = start_application(scratch / "desktop");
        EventLog log;
        desktop->set_event_handler([&](const app::ApplicationEvent& event) { log.record(event); });
        auto visiting = desktop->open_library(scratch / "lib");
        REQUIRE(visiting.has_value());
        CHECK(visiting->read_only);
        desktop->retry_read_only_libraries();  // still held: nothing changes
        CHECK(desktop->libraries().front().read_only);

        REQUIRE(cli->close_library(created->uuid).has_value());
        desktop->retry_read_only_libraries();
        const auto upgraded = desktop->libraries();
        REQUIRE(upgraded.size() == 1);
        CHECK_FALSE(upgraded.front().read_only);
        CHECK_FALSE(upgraded.front().in_use_elsewhere);
        catalog::EditCommand edit;
        edit.sample_ids = {1};
        edit.rating = 2;
        CHECK(desktop->apply_edit(created->uuid, edit).has_value());
        desktop->set_event_handler({});
        CHECK(log.access_changes(created->uuid) == 1);
    }

    TEST_CASE("a folder that fails to open keeps no library.lock, and the CLI's root bindings reach the desktop") {
        ScratchDirectory scratch;
        auto application = start_application(scratch / "workspace");
        test::write_file(scratch / "broken/library.json",
                         nlohmann::json{{"library_uuid", uuids::to_string(new_uuid())}}.dump());
        test::write_file(scratch / "broken/catalog.sqlite", "this is not a database");
        CHECK_FALSE(application->open_library(scratch / "broken").has_value());
        CHECK_FALSE(std::filesystem::exists(scratch / "broken" / app::FileLock::kLibraryLockFileName));

        auto created = application->create_library(scratch / "lib", "Bound elsewhere");
        REQUIRE(created.has_value());
        auto other_process = app::Workspace::load(scratch / "workspace");
        REQUIRE(other_process.has_value());
        const auto root = new_uuid();
        REQUIRE(other_process->bind_root(app::RootBinding{created->uuid, root, scratch / "pack"}).has_value());
        CHECK_FALSE(application->workspace().root_binding(created->uuid, root).has_value());
        application->refresh_root_availability();  // re-reads the workspace first
        CHECK(application->workspace().root_binding(created->uuid, root) == scratch / "pack");
        auto unchanged = application->workspace().reload_if_changed();
        REQUIRE(unchanged.has_value());
        CHECK_FALSE(*unchanged);
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
        const std::vector<app::SampleKey> asked = {{created->uuid, 3}, {created->uuid, 999}, {new_uuid(), 1}};
        const auto rows = application.rows(asked);
        REQUIRE(rows.has_value());
        REQUIRE(rows->size() == 1);        // unknown samples and libraries that are not open are omitted
        CHECK(rows->front().rating == 4);  // no waiting: the index was patched by apply_edit
        CHECK(rows->front().library == created->uuid);
        auto rating_of = [&](std::int64_t sample_id) {
            const std::vector<app::SampleKey> key = {{created->uuid, sample_id}};
            auto found = application.rows(key);
            REQUIRE(found.has_value());
            REQUIRE(found->size() == 1);
            return found->front().rating;
        };
        auto rated = application.count({.text = "rating:4"}, created->uuid);
        REQUIRE(rated.has_value());
        CHECK(rated->total == 1);

        REQUIRE(application.undo(created->uuid).has_value());
        CHECK(rating_of(3) == 0);
        REQUIRE(application.redo(created->uuid).has_value());
        CHECK(rating_of(3) == 4);

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

    TEST_CASE("index reloads are coalesced: a burst of requests during a load becomes exactly one follow-up load") {
        app::detail::IndexLoadSchedule schedule;
        CHECK_FALSE(schedule.load_pending());
        CHECK_FALSE(schedule.pending_load().has_value());

        // Idle: the first request enqueues; further requests while queued are covered by it.
        const auto first = schedule.request();
        CHECK(first.enqueue);
        CHECK(first.load_number == 1);
        for (int burst = 0; burst < 10; ++burst) {
            const auto covered = schedule.request();
            CHECK_FALSE(covered.enqueue);
            CHECK(covered.load_number == 1);
        }
        CHECK(schedule.pending_load() == 1u);

        // Running: a burst of requests becomes one follow-up, numbered after the running load.
        CHECK(schedule.begin_load() == 1);
        CHECK(schedule.load_pending());
        for (int burst = 0; burst < 50; ++burst) {
            const auto follow_up = schedule.request();
            CHECK_FALSE(follow_up.enqueue);  // the loader re-queues it when the running load ends
            CHECK(follow_up.load_number == 2);
        }
        CHECK(schedule.pending_load() == 1u);
        CHECK(schedule.finish_load());  // exactly one follow-up is due
        CHECK(schedule.pending_load() == 2u);
        CHECK(schedule.wait_for_load(1, {}));

        // The follow-up runs; nothing arrives meanwhile, so no further load is scheduled.
        CHECK(schedule.begin_load() == 2);
        CHECK_FALSE(schedule.finish_load());
        CHECK_FALSE(schedule.load_pending());
        CHECK(schedule.loads_begun() == 2);
        CHECK(schedule.wait_for_load(2, {}));

        // A waiter for a load that will never run (nothing pending) returns at once.
        CHECK_FALSE(schedule.wait_for_load(3, {}));

        // A waiter blocked on a running load wakes when it finishes.
        CHECK(schedule.request().enqueue);
        CHECK(schedule.begin_load() == 3);
        std::atomic<bool> woke{false};
        std::jthread waiter([&] { woke = schedule.wait_for_load(3, {}); });
        CHECK_FALSE(schedule.finish_load());
        waiter.join();
        CHECK(woke.load());
    }

    TEST_CASE("cached sort orders are patched in place and stay exact through edits, removals and new rows") {
        ScratchDirectory scratch;
        auto varied = make_varied_library(scratch / "lib", 240);
        auto& library = *varied.populated.library;
        auto index = loaded_index(library);
        const app::IndexedLibrary sources[] = {{index.get(), &library, "Varied"}};
        catalog::Library* libraries[] = {&library};
        constexpr search::SortField kSorts[] = {
            search::SortField::Name, search::SortField::Added,      search::SortField::Duration,
            search::SortField::Size, search::SortField::SampleRate, search::SortField::Bpm,
            search::SortField::Key,  search::SortField::Rating,     search::SortField::Format};
        auto check_every_order = [&](std::string_view step) {
            CAPTURE(step);
            for (const auto sort : kSorts)
                for (const bool descending : {false, true})
                    check_same_results(sources, libraries, {.sort = sort, .descending = descending});
            check_same_results(sources, libraries, {.text = "rating:>=2", .sort = search::SortField::Rating});
        };
        auto synchronize = [&] {
            auto change = index->synchronize(library);
            REQUIRE_MESSAGE(change.has_value(), change.error().message());
            CHECK_FALSE(change->reload_required);
        };
        check_every_order("every order built before any change");

        edit_samples(library, {7}, [](catalog::EditCommand& command) { command.rating = 5; });
        synchronize();
        check_every_order("one rating moved");

        edit_samples(library, {7}, [](catalog::EditCommand& command) { command.favorite = true; });
        synchronize();
        check_every_order("a change that moves no order");

        std::vector<std::int64_t> many;
        for (std::int64_t id = 3; id <= 240; id += 3)
            many.push_back(id);
        edit_samples(library, many, [](catalog::EditCommand& command) {
            command.bpm = catalog::BpmOverrideEdit{catalog::OverrideState::Value, 99'000};
        });
        synchronize();
        check_every_order("many BPMs moved at once, with ties");

        edit_samples(library, {11}, [](catalog::EditCommand& command) { command.display_name = "AAAA first by name"; });
        edit_samples(library, {12}, [](catalog::EditCommand& command) { command.display_name = "zzzz last by name"; });
        synchronize();
        check_every_order("renamed rows");

        // Removed rows free their slots, and rows added in the same change reuse them.
        edit_samples(library, {20, 21, 22}, [](catalog::EditCommand& command) { command.removed = true; });
        add_files(library, varied.populated.root_id,
                  {test::synthetic_file("Drums/fresh 1.wav", 77, 1'000), test::synthetic_file("fresh 2.wav", 3, 9),
                   test::synthetic_file("Loops/fresh 3.wav", 5'000'000, 50'000'000, "flac")});
        synchronize();
        CHECK(index->size() == 243);
        check_every_order("rows removed and added in one change");

        edit_samples(library, {20, 21, 22}, [](catalog::EditCommand& command) { command.removed = false; });
        synchronize();
        CHECK(index->size() == 246);
        check_every_order("removed rows restored");
    }

    TEST_CASE("a library that is missing from one page and answers on the next makes the cursor stale") {
        ScratchDirectory scratch;
        auto first = make_varied_library(scratch / "first", 40, "First");
        auto second = make_varied_library(scratch / "second", 40, "Second");
        auto first_index = loaded_index(*first.populated.library);
        auto second_index = loaded_index(*second.populated.library);
        // The second library's catalog with a single reader: holding it makes the full-text id set of
        // that library fail (RESOURCE_LIMIT after the reader wait), so it is missing from the page.
        const auto second_directory = second.populated.library->directory();
        second.populated.library.reset();
        auto narrow = catalog::Library::open(second_directory, {.max_readers = 1});
        REQUIRE(narrow.has_value());
        auto& second_library = **narrow;
        const app::IndexedLibrary sources[] = {{first_index.get(), first.populated.library.get(), "First"},
                                               {second_index.get(), &second_library, "Second"}};
        search::SearchRequest request{.text = "kick", .sort = search::SortField::Name, .page_size = 5};
        search::SearchPage page;
        {
            auto held = second_library.acquire_reader();
            REQUIRE(held.has_value());
            auto partial = app::search_indexes(sources, request);
            REQUIRE(partial.has_value());
            page = std::move(*partial);
        }
        REQUIRE(page.coverage.size() == 2);
        CHECK(page.coverage[0].complete != page.coverage[1].complete);
        REQUIRE(page.next.has_value());
        // The library answers again, with no position stored for it: resuming would restart it at
        // its first row, so the view must re-query instead.
        request.cursor = page.next;
        auto resumed = app::search_indexes(sources, request);
        REQUIRE_FALSE(resumed.has_value());
        CHECK(resumed.error().code == ErrorCode::StaleCursor);
    }

    TEST_CASE("an edit too large for the outbox returns only after the index reflects it") {
        ScratchDirectory scratch;
        auto application = start_application(scratch / "workspace");
        auto created = application->create_library(scratch / "lib", "Large");
        REQUIRE(created.has_value());
        // More samples than one change envelope lists (10,000): renaming their tag commits one
        // envelope that only says "everything changed", and the index has to reload.
        constexpr int kSamples = 10'001;
        std::vector<catalog::ScannedFile> files;
        files.reserve(kSamples);
        for (int index = 0; index < kSamples; ++index)
            files.push_back(test::synthetic_file(std::format("Pack/hit {:05}.wav", index), 10, 10'000));
        auto populated = application->with_library(created->uuid, [&](catalog::Library& opened) -> Result<void> {
            auto root = opened.add_root("Synthetic", catalog::RootMode::Referenced);
            if (!root) return std::unexpected(root.error());
            for (std::size_t offset = 0; offset < files.size(); offset += catalog::Library::kMaximumScanBatchFiles) {
                const auto end = std::min(files.size(), offset + catalog::Library::kMaximumScanBatchFiles);
                add_files(opened, root->id,
                          {files.begin() + static_cast<std::ptrdiff_t>(offset),
                           files.begin() + static_cast<std::ptrdiff_t>(end)});
            }
            return {};
        });
        REQUIRE(populated.has_value());
        catalog::EditCommand tag_all;
        tag_all.sample_ids.resize(kSamples);
        std::iota(tag_all.sample_ids.begin(), tag_all.sample_ids.end(), std::int64_t{1});
        tag_all.tags_add = {"Drums"};
        REQUIRE(application->apply_edit(created->uuid, tag_all).has_value());
        REQUIRE(test::wait_until([&] { return application->index_ready(created->uuid); }, kBackgroundTimeout));
        auto tags =
            application->with_library(created->uuid, [](catalog::Library& opened) { return opened.list_tags(); });
        REQUIRE(tags.has_value());
        REQUIRE(tags->size() == 1);

        catalog::TaxonomyCommand rename{
            .kind = catalog::TaxonomyCommand::Kind::RenameTag, .target_id = tags->front().id, .new_name = "Percussion"};
        REQUIRE(application->apply_taxonomy(created->uuid, rename).has_value());
        // No waiting here: apply_taxonomy returned after the reload it caused.
        CHECK(application->index_ready(created->uuid));
        const std::vector<app::SampleKey> keys = {{created->uuid, 1}, {created->uuid, kSamples}};
        auto rows = application->rows(keys);
        REQUIRE(rows.has_value());
        REQUIRE(rows->size() == 2);
        for (const auto& row : *rows)
            CHECK(row.tag_paths == std::vector<std::string>{"Percussion"});
        auto counted = application->count({.text = "tag:Percussion"}, created->uuid);
        REQUIRE(counted.has_value());
        CHECK(counted->total == kSamples);
    }

    TEST_CASE("a library over the index budget is indexed again once another library closes") {
        ScratchDirectory scratch;
        auto options = quiet_options(scratch / "workspace");
        options.index_row_limit = 100;
        auto started = app::Application::start(options);
        REQUIRE(started.has_value());
        auto& application = **started;
        auto make_files = [](int count) {
            std::vector<catalog::ScannedFile> files;
            for (int index = 0; index < count; ++index)
                files.push_back(test::synthetic_file(std::format("hit {:03}.wav", index), 10, 10'000));
            return files;
        };
        auto big = application.create_library(scratch / "big", "Big");
        REQUIRE(big.has_value());
        populate_through_application(application, big->uuid, make_files(80));
        REQUIRE(test::wait_until(
            [&] {
                auto counted = application.count({}, big->uuid);
                return application.index_ready(big->uuid) && counted && counted->total == 80;
            },
            kBackgroundTimeout));
        auto small = application.create_library(scratch / "small", "Small");
        REQUIRE(small.has_value());
        populate_through_application(application, small->uuid, make_files(50));
        // 80 + 50 rows exceed the budget of 100: the second library is answered from SQLite.
        REQUIRE(test::wait_until(
            [&] {
                auto counted = application.count({}, small->uuid);
                return !application.index_ready(small->uuid) && counted && counted->total == 50;
            },
            kBackgroundTimeout));
        REQUIRE(application.close_library(big->uuid).has_value());
        CHECK(test::wait_until([&] { return application.index_ready(small->uuid); }, kBackgroundTimeout));
        auto counted = application.count({}, small->uuid);
        REQUIRE(counted.has_value());
        CHECK(counted->total == 50);
    }

    TEST_CASE("analysis supervisor: job deadlines and retry delays follow the timing") {
        app::AnalysisSupervisor::Timing timing;
        timing.minimum_job_deadline = 60s;
        timing.deadline_per_audio_second = 4.0;
        timing.maximum_job_deadline = 30min;
        CHECK(app::AnalysisSupervisor::job_deadline(timing, std::nullopt) == 60s);
        CHECK(app::AnalysisSupervisor::job_deadline(timing, 0) == 60s);
        CHECK(app::AnalysisSupervisor::job_deadline(timing, -5'000'000) == 60s);   // a damaged duration adds nothing
        CHECK(app::AnalysisSupervisor::job_deadline(timing, 10'000'000) == 100s);  // 60 s + 4 x 10 s
        CHECK(app::AnalysisSupervisor::job_deadline(timing, std::int64_t{24} * 3600 * 1'000'000) == 30min);

        timing.first_retry_delay = 1s;
        timing.maximum_retry_delay = 60s;
        CHECK(app::AnalysisSupervisor::retry_delay(timing, 1) == 1s);
        CHECK(app::AnalysisSupervisor::retry_delay(timing, 2) == 2s);
        CHECK(app::AnalysisSupervisor::retry_delay(timing, 4) == 8s);
        CHECK(app::AnalysisSupervisor::retry_delay(timing, 7) == 60s);  // 64 s capped
        CHECK(app::AnalysisSupervisor::retry_delay(timing, 1000) == 60s);
    }

    TEST_CASE("analysis supervisor: a job past its deadline is cancelled and failed as retryable") {
        SupervisedLibrary supervised;
        auto timing = fast_timing();
        timing.minimum_job_deadline = 300ms;  // the self-test task simulates two minutes of work
        timing.deadline_per_audio_second = 0;
        timing.maximum_job_deadline = 300ms;
        const auto started = std::chrono::steady_clock::now();
        app::AnalysisSupervisor supervisor(self_test_worker(), supervised.hooks(), timing);
        REQUIRE(test::wait_until([&] { return supervised.job_column("error_code") == "WORKER_CRASH"; },
                                 kBackgroundTimeout));
        CHECK(std::chrono::steady_clock::now() - started < 60s);  // the deadline ended it, not the task
        CHECK(supervised.errors().empty());
    }

    TEST_CASE("analysis supervisor: abandoning a library returns its running job to the queue") {
        SupervisedLibrary supervised;
        app::AnalysisSupervisor supervisor(self_test_worker(), supervised.hooks(), fast_timing());
        REQUIRE(test::wait_until([&] { return supervised.job_column("state") == "running"; }, kBackgroundTimeout));
        supervisor.abandon_library(supervised.library().uuid());
        REQUIRE(test::wait_until([&] { return supervised.job_column("state") == "queued"; }, kBackgroundTimeout));
        CHECK(supervised.job_column("error_code").empty());  // given back, not failed
        supervisor.serve_library(supervised.library().uuid());
        CHECK(test::wait_until([&] { return supervised.job_column("state") == "running"; }, kBackgroundTimeout));
        CHECK(supervised.errors().empty());
    }

    TEST_CASE("analysis supervisor: failed worker starts back off, then pause the queue; resuming retries at once") {
        SUBCASE("repeated failures pause the queue and the job stays queued") {
            SupervisedLibrary supervised;
            auto timing = fast_timing();
            timing.first_retry_delay = 5ms;
            timing.maximum_retry_delay = 20ms;
            timing.failures_before_pause = 3;
            app::AnalysisSupervisor supervisor(supervised.unstartable_worker(), supervised.hooks(), timing);
            REQUIRE(test::wait_until([&] { return supervisor.paused(); }, kBackgroundTimeout));
            CHECK(supervisor.status().worker_unavailable);
            CHECK(supervised.unavailable_reports() == 3);
            CHECK(supervised.job_column("state") == "queued");
        }
        SUBCASE("a missing executable pauses at the first failure") {
            SupervisedLibrary supervised;
            app::AnalysisSupervisor supervisor(
                worker::WorkerOptions{.executable = supervised.scratch() / "missing.exe"}, supervised.hooks(),
                fast_timing());
            REQUIRE(test::wait_until([&] { return supervisor.paused(); }, kBackgroundTimeout));
            CHECK(supervised.unavailable_reports() == 1);
        }
        SUBCASE("resuming ends a long backoff wait") {
            SupervisedLibrary supervised;
            auto timing = fast_timing();
            timing.first_retry_delay = 1h;  // the test would time out if only the backoff ended the wait
            timing.maximum_retry_delay = 1h;
            app::AnalysisSupervisor supervisor(supervised.unstartable_worker(), supervised.hooks(), timing);
            REQUIRE(test::wait_until([&] { return supervised.unavailable_reports() == 1; }, kBackgroundTimeout));
            supervisor.notify();           // new work does not cut a backoff short...
            supervisor.set_paused(false);  // ...but resuming does
            CHECK(test::wait_until([&] { return supervised.unavailable_reports() >= 2; }, kBackgroundTimeout));
        }
    }

    TEST_CASE("open progress reaches the event handler: every step start and end, throttled in between") {
        std::vector<app::LibraryOpenProgressEvent> events;
        const auto library = new_uuid();
        const auto relay = app::detail::open_progress_relay(
            [&](const app::ApplicationEvent& event) {
                const auto* progress = std::get_if<app::LibraryOpenProgressEvent>(&event);
                REQUIRE(progress != nullptr);
                events.push_back(*progress);
            },
            library, "lib");
        using catalog::OpenStep;
        relay({OpenStep::BackingUp, 0, 1});
        relay({OpenStep::BackingUp, 1, 1});
        relay({OpenStep::RebuildingKeys, 0, 4});
        relay({OpenStep::RebuildingKeys, 1, 4});  // within the interval of the step start: dropped
        relay({OpenStep::RebuildingKeys, 2, 4});
        relay({OpenStep::RebuildingKeys, 4, 4});  // a step's end always goes out
        REQUIRE(events.size() == 4);
        CHECK(events[0].step == OpenStep::BackingUp);
        CHECK(events[1].completed == 1);
        CHECK(events[2].step == OpenStep::RebuildingKeys);
        CHECK(events[2].completed == 0);
        CHECK(events[3].completed == 4);
        CHECK(events[3].total == 4);
        CHECK(events[3].library == library);
        CHECK(events[3].directory == std::filesystem::path("lib"));
    }

}  // TEST_SUITE("app")
