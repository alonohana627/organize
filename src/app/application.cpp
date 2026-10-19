#include "mso/app/application.hpp"

#include <format>
#include <map>
#include <mutex>
#include <set>
#include <thread>

#include "mso/analysis/worker_process.hpp"
#include "mso/app/analysis_supervisor.hpp"
#include "mso/app/metadata_export.hpp"

namespace mso::app {

namespace {
constexpr auto kRootPollInterval = std::chrono::seconds(2);
}

/// A background operation and the means to cancel and await it.
struct RunningOperation {
    std::string description;
    std::stop_source stop;
    std::jthread thread;
};

struct Application::State {
    ApplicationOptions options;
    Workspace workspace;

    mutable std::mutex libraries_mutex;
    std::map<std::string, std::shared_ptr<catalog::Library>> libraries;  ///< keyed by UUID text

    std::mutex handler_mutex;
    EventHandler handler;

    std::mutex operations_mutex;
    std::map<OperationId, std::unique_ptr<RunningOperation>> operations;
    OperationId next_operation = 1;

    std::unique_ptr<AnalysisSupervisor> supervisor;
    std::jthread root_monitor;
    Application* owner = nullptr;  ///< for automatic rescans started by the root monitor

    /// Roots with an import in flight ("<library uuid>/<root id>"), so rescans never overlap.
    std::mutex imports_mutex;
    std::set<std::string> imports_in_flight;

    explicit State(ApplicationOptions application_options, Workspace loaded)
        : options(std::move(application_options)), workspace(std::move(loaded)) {}

    void emit(const ApplicationEvent& event) {
        EventHandler current;
        {
            std::lock_guard lock(handler_mutex);
            current = handler;
        }
        if (current) current(event);
    }

    std::vector<std::shared_ptr<catalog::Library>> library_snapshot() const {
        std::lock_guard lock(libraries_mutex);
        std::vector<std::shared_ptr<catalog::Library>> snapshot;
        for (const auto& [key, library] : libraries)
            snapshot.push_back(library);
        return snapshot;
    }

    std::shared_ptr<catalog::Library> find(const uuids::uuid& uuid) const {
        std::lock_guard lock(libraries_mutex);
        const auto found = libraries.find(uuids::to_string(uuid));
        return found == libraries.end() ? nullptr : found->second;
    }

    /// Absolute folder of a root: managed roots live inside the library, referenced roots use
    /// the machine-local binding.
    std::optional<std::filesystem::path> root_folder(const catalog::Library& library, const uuids::uuid& root,
                                                     catalog::RootMode mode) {
        if (mode == catalog::RootMode::Managed) return library.directory() / "media" / uuids::to_string(root);
        return workspace.root_binding(library.uuid(), root);
    }

    Result<std::filesystem::path> resolve(catalog::Library& library, std::int64_t sample_id) {
        auto detail = library.sample_detail(sample_id);
        if (!detail) return std::unexpected(detail.error());
        if (detail->deleted) return fail(ErrorCode::NotFound, "the sample was removed from the library");
        bool any_offline = false;
        for (const auto& location : detail->locations) {
            if (location.state == catalog::LocationState::Retired) continue;
            auto base = root_folder(library, location.root_uuid, location.root_mode);
            std::error_code error;
            if (!base || !std::filesystem::is_directory(*base, error)) {
                any_offline = true;
                continue;
            }
            auto file = *base / path_from_utf8(location.relative_path);
            if (std::filesystem::is_regular_file(file, error)) return file;
        }
        if (any_offline)
            return fail(ErrorCode::SourceOffline, "the drive or folder holding this sample is not connected");
        return fail(ErrorCode::FileMissing, "the file is no longer at its recorded location");
    }

    /// Polls every bound root; availability flips are committed and announced.
    void monitor_roots(std::stop_token stop) {
        std::mutex sleep_mutex;
        std::condition_variable_any sleeper;
        while (!stop.stop_requested()) {
            check_roots_once();
            std::unique_lock lock(sleep_mutex);
            sleeper.wait_for(lock, stop, kRootPollInterval, [] { return false; });
        }
    }

    /// Compares each root's reachability with its catalog projection and commits changes.
    void check_roots_once() {
        for (const auto& library : library_snapshot()) {
            if (library->read_only()) continue;
            auto roots = library->list_roots();
            if (!roots) continue;
            const auto library_uuid = library->uuid();
            for (const auto& root : *roots) {
                auto folder = root_folder(*library, root.uuid, root.mode);
                std::error_code error;
                const bool reachable = folder && std::filesystem::is_directory(*folder, error);
                const auto now = reachable ? catalog::Availability::Online : catalog::Availability::Offline;
                if (now == root.availability || (root.availability == catalog::Availability::Unknown && reachable))
                    continue;
                if (library->set_root_availability(root.id, now)) {
                    emit(RootAvailabilityEvent{library_uuid, root.id, now});
                    emit(CatalogChangedEvent{library_uuid});
                }
            }
        }
    }
};

Application::Application(std::unique_ptr<State> state) : state_(std::move(state)) {}

Application::~Application() {
    // Stop admission first, then cancel and join background work, then close libraries.
    state_->root_monitor = {};
    state_->supervisor.reset();
    std::vector<std::unique_ptr<RunningOperation>> finishing;
    {
        std::lock_guard lock(state_->operations_mutex);
        for (auto& [id, operation] : state_->operations) {
            operation->stop.request_stop();
            finishing.push_back(std::move(operation));
        }
        state_->operations.clear();
    }
    finishing.clear();  // joins the jthreads
    std::lock_guard lock(state_->libraries_mutex);
    state_->libraries.clear();
}

Result<std::unique_ptr<Application>> Application::start(const ApplicationOptions& options) {
    auto workspace = Workspace::load(options.workspace_directory.value_or(Workspace::default_directory()));
    if (!workspace) return std::unexpected(workspace.error());
    auto state = std::make_unique<State>(options, std::move(*workspace));
    std::unique_ptr<Application> application(new Application(std::move(state)));
    auto& shared = *application->state_;

    if (options.reopen_libraries) {
        for (const auto& known : shared.workspace.known_libraries()) {
            if (!known.open) continue;
            if (!application->open_library(known.directory)) (void)shared.workspace.set_library_open(known.uuid, false);
        }
    }
    if (options.run_analysis_supervisor) {
        AnalysisSupervisor::Hooks hooks{
            [&shared] { return shared.library_snapshot(); },
            [&shared](catalog::Library& library, std::int64_t sample_id) { return shared.resolve(library, sample_id); },
            [&shared](catalog::Library& library, const std::string& status) {
                AnalysisProgressEvent event{library.identity().uuid, {}, status};
                if (auto runs = library.list_analysis_runs(20)) event.runs = std::move(*runs);
                shared.emit(event);
                shared.emit(CatalogChangedEvent{event.library});
            }};
        shared.supervisor = std::make_unique<AnalysisSupervisor>(
            options.worker_executable.value_or(worker::default_worker_executable()), std::move(hooks));
        // After an unclean shutdown, analysis stays paused until the user resumes it.
        for (const auto& library : shared.library_snapshot())
            if (library->recovery_report().interrupted_jobs > 0) shared.supervisor->set_paused(true);
    }
    shared.owner = application.get();
    if (options.automatic_mode) {
        if (shared.library_snapshot().empty()) (void)application->default_library();
        for (const auto& library : shared.library_snapshot()) {
            if (library->read_only()) continue;
            if (auto roots = library->list_roots())
                for (const auto& root : *roots)
                    (void)application->start_import(library->uuid(), root.id);
        }
    }
    if (options.monitor_roots)
        shared.root_monitor = std::jthread([&shared](std::stop_token stop) { shared.monitor_roots(stop); });
    return application;
}

void Application::set_event_handler(EventHandler handler) {
    std::lock_guard lock(state_->handler_mutex);
    state_->handler = std::move(handler);
}

Workspace& Application::workspace() {
    return state_->workspace;
}

// ---------------------------------------------------------------- libraries

namespace {
OpenLibraryInfo describe(catalog::Library& library) {
    const auto identity = library.identity();
    const auto roots = library.list_roots();
    return OpenLibraryInfo{identity.uuid, identity.name, library.directory(), library.read_only(),
                           roots ? static_cast<std::int64_t>(roots->size()) : 0};
}
}  // namespace

Result<OpenLibraryInfo> Application::create_library(const std::filesystem::path& directory, std::string_view name) {
    auto created = catalog::Library::create(directory, name);
    if (!created) return std::unexpected(created.error());
    created->reset();  // close; open_library() registers it like any other library
    return open_library(directory);
}

bool Application::is_library_folder(const std::filesystem::path& directory) {
    std::error_code error;
    return std::filesystem::is_regular_file(directory / catalog::Library::kManifestFileName, error);
}

Result<OpenLibraryInfo> Application::create_library_in_workspace(std::string_view name) {
    const std::string trimmed = trim(name);
    if (trimmed.empty()) return fail(ErrorCode::InvalidArgument, "a library needs a name");
    // Folder name: the library name without characters Windows forbids in file names.
    std::string folder_name;
    for (const char character : trimmed)
        folder_name.push_back(std::string_view("<>:\"/\\|?*").find(character) == std::string_view::npos &&
                                      static_cast<unsigned char>(character) >= 0x20
                                  ? character
                                  : '_');
    const auto base = state_->workspace.directory() / "Libraries";
    std::error_code error;
    auto directory = base / path_from_utf8(folder_name);
    for (int suffix = 2; std::filesystem::exists(directory, error); ++suffix)
        directory = base / path_from_utf8(std::format("{} {}", folder_name, suffix));
    return create_library(directory, trimmed);
}

Result<OpenLibraryInfo> Application::default_library() {
    for (const auto& info : libraries())
        if (!info.read_only) return info;
    return create_library_in_workspace("My Samples");
}

Result<OpenLibraryInfo> Application::open_library(const std::filesystem::path& directory) {
    std::error_code error;
    const auto canonical = std::filesystem::weakly_canonical(directory, error);
    auto opened = catalog::Library::open(error ? directory : canonical);
    if (!opened) return std::unexpected(opened.error());
    std::shared_ptr<catalog::Library> library = std::move(*opened);
    const auto info = describe(*library);
    {
        std::lock_guard lock(state_->libraries_mutex);
        if (state_->libraries.contains(uuids::to_string(info.uuid)))
            return fail(ErrorCode::InvalidArgument,
                        "a library with the same identity is already open (a copied library must be cloned first)");
        if (state_->libraries.size() >= kMaxOpenLibraries)
            return fail(ErrorCode::ResourceLimit, "at most 8 libraries can be open at the same time");
        state_->libraries.emplace(uuids::to_string(info.uuid), std::move(library));
    }
    (void)state_->workspace.remember_library(KnownLibrary{info.uuid, info.name, info.directory, true});
    state_->emit(CatalogChangedEvent{info.uuid});
    if (state_->supervisor) state_->supervisor->notify();
    return info;
}

Result<void> Application::close_library(const uuids::uuid& uuid) {
    {
        std::lock_guard lock(state_->libraries_mutex);
        if (state_->libraries.erase(uuids::to_string(uuid)) == 0)
            return fail(ErrorCode::NotFound, "library is not open");
    }
    return state_->workspace.set_library_open(uuid, false);
}

std::vector<OpenLibraryInfo> Application::libraries() const {
    std::vector<OpenLibraryInfo> infos;
    for (const auto& library : state_->library_snapshot())
        infos.push_back(describe(*library));
    return infos;
}

catalog::Library* Application::library(const uuids::uuid& uuid) const {
    return state_->find(uuid).get();
}

std::vector<catalog::Library*> Application::open_libraries() const {
    std::vector<catalog::Library*> pointers;
    for (const auto& library : state_->library_snapshot())
        pointers.push_back(library.get());
    return pointers;
}

// ---------------------------------------------------------------- roots

Result<catalog::RootRecord> Application::add_root(const uuids::uuid& library_uuid, const std::filesystem::path& folder,
                                                  std::string_view name) {
    auto library = state_->find(library_uuid);
    if (!library) return fail(ErrorCode::NotFound, "library is not open");
    std::error_code error;
    if (!std::filesystem::is_directory(folder, error)) return fail(ErrorCode::FileMissing, "the folder does not exist");
    const auto absolute = std::filesystem::absolute(folder, error);
    std::string root_name = trim(name);
    if (root_name.empty())
        root_name = path_to_utf8(absolute.filename().empty() ? absolute.parent_path().filename() : absolute.filename());
    if (root_name.empty()) root_name = path_to_utf8(absolute);
    auto root = library->add_root(root_name, catalog::RootMode::Referenced);
    if (!root) return std::unexpected(root.error());
    if (auto bound = state_->workspace.bind_root(RootBinding{library_uuid, root->uuid, absolute}); !bound)
        return std::unexpected(bound.error());
    (void)library->set_root_availability(root->id, catalog::Availability::Online);
    state_->emit(CatalogChangedEvent{library_uuid});
    return root;
}

Result<void> Application::rebind_root(const uuids::uuid& library_uuid, std::int64_t root_id,
                                      const std::filesystem::path& folder) {
    auto library = state_->find(library_uuid);
    if (!library) return fail(ErrorCode::NotFound, "library is not open");
    auto roots = library->list_roots();
    if (!roots) return std::unexpected(roots.error());
    const auto root = std::ranges::find(*roots, root_id, &catalog::RootRecord::id);
    if (root == roots->end()) return fail(ErrorCode::NotFound, "no such root");
    std::error_code error;
    if (!std::filesystem::is_directory(folder, error)) return fail(ErrorCode::FileMissing, "the folder does not exist");
    return state_->workspace.bind_root(RootBinding{library_uuid, root->uuid, std::filesystem::absolute(folder, error)});
}

std::optional<std::filesystem::path> Application::root_path(const uuids::uuid& library_uuid,
                                                            const catalog::RootRecord& root) const {
    auto library = state_->find(library_uuid);
    if (!library) return std::nullopt;
    return state_->root_folder(*library, root.uuid, root.mode);
}

void Application::refresh_root_availability() {
    state_->check_roots_once();
}

// ---------------------------------------------------------------- import

Result<files::ImportSummary> Application::import_now(const uuids::uuid& library_uuid, std::int64_t root_id,
                                                     std::stop_token stop,
                                                     const files::ImportProgressCallback& progress) {
    auto library = state_->find(library_uuid);
    if (!library) return fail(ErrorCode::NotFound, "library is not open");
    auto roots = library->list_roots();
    if (!roots) return std::unexpected(roots.error());
    const auto root = std::ranges::find(*roots, root_id, &catalog::RootRecord::id);
    if (root == roots->end()) return fail(ErrorCode::NotFound, "no such root");
    auto folder = state_->root_folder(*library, root->uuid, root->mode);
    if (!folder) return fail(ErrorCode::SourceOffline, "this root is not bound to a folder on this computer");
    auto summary = files::import_root(*library, root_id, *folder, stop, progress);
    state_->emit(CatalogChangedEvent{library_uuid});
    return summary;
}

Result<OperationId> Application::start_import(const uuids::uuid& library_uuid, std::int64_t root_id) {
    if (!state_->find(library_uuid)) return fail(ErrorCode::NotFound, "library is not open");
    {
        std::lock_guard lock(state_->imports_mutex);
        if (!state_->imports_in_flight.insert(std::format("{}/{}", uuids::to_string(library_uuid), root_id)).second)
            return fail(ErrorCode::DbBusy, "this folder is already being scanned", /*retryable=*/true);
    }
    auto operation = std::make_unique<RunningOperation>();
    operation->description = "Import";
    OperationId id = 0;
    {
        std::lock_guard lock(state_->operations_mutex);
        id = state_->next_operation++;
    }
    auto* raw = operation.get();
    auto stop = raw->stop.get_token();
    auto* shared = state_.get();
    auto* application = this;
    raw->thread = std::jthread([=](std::stop_token) {
        auto result = application->import_now(library_uuid, root_id, stop, [=](const files::ImportProgress& progress) {
            shared->emit(ImportProgressEvent{id, library_uuid, root_id, progress});
            shared->emit(CatalogChangedEvent{library_uuid});
        });
        OperationFinishedEvent finished{id, "Import", std::nullopt};
        if (!result)
            finished.error = result.error();
        else if (shared->options.automatic_mode)
            application->queue_automatic_analysis(library_uuid, root_id);
        {
            std::lock_guard lock(shared->imports_mutex);
            shared->imports_in_flight.erase(std::format("{}/{}", uuids::to_string(library_uuid), root_id));
        }
        shared->emit(finished);
    });
    std::lock_guard lock(state_->operations_mutex);
    state_->operations.emplace(id, std::move(operation));
    return id;
}

void Application::cancel_operation(OperationId operation) {
    std::lock_guard lock(state_->operations_mutex);
    if (auto found = state_->operations.find(operation); found != state_->operations.end())
        found->second->stop.request_stop();
}

void Application::wait_for_operation(OperationId operation) {
    std::unique_ptr<RunningOperation> finished;
    {
        std::lock_guard lock(state_->operations_mutex);
        auto found = state_->operations.find(operation);
        if (found == state_->operations.end()) return;
        finished = std::move(found->second);
        state_->operations.erase(found);
    }
    if (finished->thread.joinable()) finished->thread.join();
}

// ---------------------------------------------------------------- search and resolution

namespace {
std::vector<catalog::Library*> scoped_libraries(const std::vector<std::shared_ptr<catalog::Library>>& all,
                                                const std::optional<uuids::uuid>& scope) {
    std::vector<catalog::Library*> selected;
    for (const auto& library : all)
        if (!scope || library->identity().uuid == *scope) selected.push_back(library.get());
    return selected;
}
}  // namespace

Result<search::SearchPage> Application::search(const search::SearchRequest& request,
                                               std::optional<uuids::uuid> scope) const {
    const auto snapshot = state_->library_snapshot();  // keeps libraries alive during the call
    const auto selected = scoped_libraries(snapshot, scope);
    return search::SearchService().search(selected, request);
}

Result<search::CountResult> Application::count(const search::SearchRequest& request,
                                               std::optional<uuids::uuid> scope) const {
    const auto snapshot = state_->library_snapshot();
    const auto selected = scoped_libraries(snapshot, scope);
    return search::SearchService().count(selected, request);
}

Result<std::filesystem::path> Application::resolve_playable_path(const uuids::uuid& library_uuid,
                                                                 std::int64_t sample_id) const {
    auto library = state_->find(library_uuid);
    if (!library) return fail(ErrorCode::NotFound, "library is not open");
    return state_->resolve(*library, sample_id);
}

// ---------------------------------------------------------------- analysis

std::string Application::automatic_analysis_profile() const {
    const auto stored = state_->workspace.preference("analysis.automatic_profile");
    return stored.is_string() ? stored.get<std::string>() : std::string("Full");
}

void Application::set_automatic_analysis_profile(std::string_view profile) {
    (void)state_->workspace.set_preference("analysis.automatic_profile", std::string(profile));
}

void Application::queue_automatic_analysis(const uuids::uuid& library_uuid, std::int64_t root_id) {
    const std::string profile = automatic_analysis_profile();
    if (profile.empty()) return;
    // Every sample of the folder: the run skips samples whose results are already current, so
    // only what this import added or changed is actually analyzed.
    std::vector<std::int64_t> samples;
    search::SearchRequest request{.scope = {.root_id = root_id},
                                  .sort = search::SortField::Added,
                                  .page_size = search::SearchService::kMaxPageSize};
    while (true) {
        auto page = search(request, library_uuid);
        if (!page) return;
        for (const auto& row : page->rows)
            samples.push_back(row.sample_id);
        if (!page->next) break;
        request.cursor = page->next;
    }
    if (!samples.empty()) (void)start_analysis(library_uuid, profile, samples, /*force=*/false);
}

std::vector<std::string> Application::profile_stages(std::string_view profile) {
    if (profile == "Waveforms") return {"waveform"};
    if (profile == "Musical") return {"waveform", "loudness", "tempo", "key"};
    if (profile == "Duplicates") return {"byte_hash", "pcm_hash"};
    if (profile == "Full") return {"waveform", "loudness", "tempo", "key", "byte_hash", "pcm_hash"};
    return {};
}

Result<catalog::Library::AnalysisRunStatus> Application::start_analysis(const uuids::uuid& library_uuid,
                                                                        std::string_view profile,
                                                                        const std::vector<std::int64_t>& sample_ids,
                                                                        bool force) {
    auto library = state_->find(library_uuid);
    if (!library) return fail(ErrorCode::NotFound, "library is not open");
    auto stages = profile_stages(profile);
    if (stages.empty()) return fail(ErrorCode::InvalidArgument, std::format("unknown analysis profile '{}'", profile));
    auto run = library->create_analysis_run({std::string(profile), std::move(stages), sample_ids, force});
    if (run && state_->supervisor) state_->supervisor->notify();
    return run;
}

Result<void> Application::control_analysis(const uuids::uuid& library_uuid, std::int64_t run_id,
                                           catalog::Library::RunControl control) {
    auto library = state_->find(library_uuid);
    if (!library) return fail(ErrorCode::NotFound, "library is not open");
    auto controlled = library->control_run(run_id, control);
    if (controlled && state_->supervisor) state_->supervisor->notify();
    return controlled;
}

void Application::set_analysis_paused(bool paused) {
    if (!state_->supervisor) return;
    state_->supervisor->set_paused(paused);
    state_->supervisor->notify();
}

bool Application::analysis_paused() const {
    return state_->supervisor ? state_->supervisor->paused() : true;
}

// ---------------------------------------------------------------- export

Result<ExportSummary> Application::export_metadata(const uuids::uuid& library_uuid,
                                                   const std::filesystem::path& destination,
                                                   const ExportOptions& options, std::stop_token stop) const {
    auto library = state_->find(library_uuid);
    if (!library) return fail(ErrorCode::NotFound, "library is not open");
    auto* shared = state_.get();
    return write_metadata_export(
        *library, destination, options,
        [shared, library_uuid](const uuids::uuid& root) { return shared->workspace.root_binding(library_uuid, root); },
        stop);
}

}  // namespace mso::app
