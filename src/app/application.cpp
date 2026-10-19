#include "mso/app/application.hpp"

#include <BS_thread_pool.hpp>

#include <algorithm>
#include <condition_variable>
#include <format>
#include <fstream>
#include <iterator>
#include <map>
#include <mutex>
#include <set>
#include <sstream>
#include <thread>

#include <nlohmann/json.hpp>

#include "mso/app/analysis_supervisor.hpp"
#include "mso/app/detail/index_maintainer.hpp"
#include "mso/app/detail/open_progress_relay.hpp"
#include "mso/app/file_lock.hpp"
#include "mso/app/metadata_export.hpp"
#include "mso/catalog/root_paths.hpp"
#include "mso/domain/json_fields.hpp"
#include "mso/worker/worker_process.hpp"

namespace mso::app {

namespace {

constexpr auto kRootPollInterval = std::chrono::seconds(2);
constexpr std::string_view kAutomaticProfilePreference = "analysis.automatic_profile";
constexpr std::string_view kDefaultAutomaticProfile = "Full";

/// Depth of event-handler calls on this thread (set_event_handler must not wait for itself).
thread_local int event_handler_depth = 0;

/// The library UUID recorded in a folder's library.json, read before anything else is touched.
Result<uuids::uuid> peek_library_uuid(const std::filesystem::path& directory) {
    std::ifstream in(directory / catalog::Library::kManifestFileName, std::ios::binary);
    if (!in) return fail(ErrorCode::NotFound, std::format("no library at {}", path_to_utf8(directory)));
    std::stringstream contents;
    contents << in.rdbuf();
    const auto document = nlohmann::json::parse(contents.str(), nullptr, false);
    const auto uuid = uuids::uuid::from_string(json_fields::string_field(document, "library_uuid").value_or(""));
    if (!uuid) return fail(ErrorCode::DbCorrupt, "library.json has no valid library_uuid");
    return *uuid;
}

/// A folder name Windows accepts for `name`: forbidden characters replaced, no trailing dots or
/// spaces, and never a reserved device name (CON, NUL, COM1...).
std::string safe_folder_name(std::string_view name) {
    std::string folder;
    for (const char character : name)
        folder.push_back(std::string_view("<>:\"/\\|?*").find(character) == std::string_view::npos &&
                                 static_cast<unsigned char>(character) >= 0x20
                             ? character
                             : '_');
    while (!folder.empty() && (folder.back() == '.' || folder.back() == ' '))
        folder.pop_back();
    if (folder.empty()) folder = "Library";
    const std::string stem = to_lower_ascii(folder.substr(0, folder.find('.')));
    static constexpr std::string_view kReserved[] = {"con", "prn", "aux", "nul"};
    const bool numbered_device =
        stem.size() == 4 && (stem.starts_with("com") || stem.starts_with("lpt")) && stem[3] >= '0' && stem[3] <= '9';
    if (numbered_device || std::ranges::find(kReserved, std::string_view(stem)) != std::end(kReserved))
        folder.insert(0, "_");
    return folder;
}

}  // namespace

using detail::LibraryEntry;

struct Application::State {
    /// A queued or running background operation.
    struct Operation {
        std::string description;
        uuids::uuid library;
        std::stop_source stop;
        std::mutex mutex;
        std::condition_variable finished_signal;
        bool finished = false;
    };

    ApplicationOptions options;
    Workspace workspace;
    std::mutex workspace_error_mutex;
    std::string last_workspace_error;  ///< the last periodic re-read error reported

    mutable std::mutex libraries_mutex;
    std::map<std::string, std::shared_ptr<LibraryEntry>> libraries;  ///< keyed by UUID text
    std::set<std::string> opening;  ///< UUIDs being opened (reserve a slot against races)
    /// Libraries closed while work of this process still held them (and their lock), by UUID
    /// text: reopening one waits for that work instead of opening it read-only.
    std::map<std::string, std::weak_ptr<LibraryEntry>> closing;

    std::mutex handler_mutex;
    std::condition_variable handler_idle;
    std::shared_ptr<const EventHandler> handler;
    std::size_t emissions_in_flight = 0;

    std::mutex operations_mutex;
    std::map<OperationId, std::shared_ptr<Operation>> operations;  ///< finished ones are erased
    OperationId next_operation = 1;
    std::unique_ptr<BS::thread_pool<>> import_pool;
    /// Roots with an import queued or running ("<library uuid>/<root id>"), so rescans never overlap.
    std::mutex imports_mutex;
    std::set<std::string> imports_in_flight;

    std::unique_ptr<AnalysisSupervisor> supervisor;

    /// Index maintenance and loading threads, with the coalesced announcements. Started by
    /// Application::start() and stopped explicitly by ~Application().
    detail::IndexMaintainer indexes;

    // Threads last: they start after everything above exists and are joined explicitly.
    std::jthread root_monitor;

    State(ApplicationOptions application_options, Workspace loaded)
        : options(std::move(application_options)),
          workspace(std::move(loaded)),
          indexes(detail::IndexMaintainer::Services{[this] { return entry_snapshot(); },
                                                    [this](const ApplicationEvent& event) { emit(event); }},
                  options.index_row_limit) {}

    // ------------------------------------------------------------ events

    void emit(const ApplicationEvent& event) {
        std::shared_ptr<const EventHandler> current;
        {
            std::lock_guard lock(handler_mutex);
            current = handler;
            ++emissions_in_flight;
        }
        ++event_handler_depth;
        try {
            if (current && *current) (*current)(event);
        } catch (const std::exception&) {
            // A throwing handler must not take a background thread down, and there is no one left
            // to report to: the handler is the reporting channel.
        }
        --event_handler_depth;
        {
            std::lock_guard lock(handler_mutex);
            --emissions_in_flight;
        }
        handler_idle.notify_all();
    }

    void report(std::optional<uuids::uuid> library, std::string context, Error error) {
        emit(BackgroundErrorEvent{library, std::move(context), std::move(error)});
    }

    /// Reports an error of the periodic workspace re-read once per distinct message.
    void report_workspace_once(const Error& error) {
        {
            std::lock_guard lock(workspace_error_mutex);
            if (last_workspace_error == error.message()) return;
            last_workspace_error = error.message();
        }
        report(std::nullopt, "re-reading the workspace", error);
    }

    // ------------------------------------------------------------ libraries

    /// The open libraries a request covers (one library, or all of them).
    std::vector<std::shared_ptr<LibraryEntry>> entries_for(const std::optional<uuids::uuid>& scope) const {
        auto entries = entry_snapshot();
        if (scope) std::erase_if(entries, [&](const auto& entry) { return entry->uuid != *scope; });
        return entries;
    }

    std::vector<std::shared_ptr<LibraryEntry>> entry_snapshot() const {
        std::lock_guard lock(libraries_mutex);
        std::vector<std::shared_ptr<LibraryEntry>> snapshot;
        snapshot.reserve(libraries.size());
        for (const auto& [key, entry] : libraries)
            snapshot.push_back(entry);
        return snapshot;
    }

    std::shared_ptr<LibraryEntry> find(const uuids::uuid& uuid) const {
        std::lock_guard lock(libraries_mutex);
        const auto found = libraries.find(uuids::to_string(uuid));
        return found == libraries.end() ? nullptr : found->second;
    }

    /// True while work of this process still holds a closed library, and with it the library's
    /// lock (keyed by UUID text).
    bool still_closing(const std::string& key) {
        std::lock_guard lock(libraries_mutex);
        std::erase_if(closing, [](const auto& item) { return item.second.expired(); });
        const auto found = closing.find(key);
        if (found == closing.end()) return false;
        const auto entry = found->second.lock();
        return entry && entry->lock.held();
    }

    /// Absolute folder of a root: managed roots live inside the library, referenced roots use
    /// the machine-local binding.
    std::optional<std::filesystem::path> root_folder(const catalog::Library& library, const uuids::uuid& root,
                                                     catalog::RootMode mode) const {
        if (mode == catalog::RootMode::Managed) return library.directory() / "media" / uuids::to_string(root);
        return workspace.root_binding(library.uuid(), root);
    }

    /// The playable file of a sample: its first location that exists right now.
    Result<std::filesystem::path> resolve(catalog::Library& library, std::int64_t sample_id) const {
        auto detail = library.sample_detail(sample_id);
        if (!detail) return std::unexpected(detail.error());
        return resolve(library, *detail);
    }

    /// The same for a sample whose detail was already read.
    Result<std::filesystem::path> resolve(const catalog::Library& library, const catalog::SampleDetail& detail) const {
        if (detail.deleted) return fail(ErrorCode::NotFound, "the sample was removed from the library");
        bool any_offline = false;
        for (const auto& location : detail.locations) {
            if (location.state == catalog::LocationState::Retired) continue;
            // Never join a catalog path that could escape its root ("..", absolute, drive letters).
            if (!is_safe_relative_path(location.relative_path)) continue;
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
};

// ---------------------------------------------------------------- lifetime

Application::Application(std::unique_ptr<State> state) : state_(std::move(state)) {}

Application::~Application() {
    auto& state = *state_;
    // 1. No new automatic work.
    state.root_monitor = {};
    // 2. Cancel and drain imports: they call into the supervisor and the indexes.
    {
        std::lock_guard lock(state.operations_mutex);
        for (auto& [id, operation] : state.operations)
            operation->stop.request_stop();
    }
    if (state.import_pool) {
        state.import_pool->wait();
        state.import_pool.reset();
    }
    // 3. Analysis (uses libraries and emits events).
    state.supervisor.reset();
    // 4. Index maintenance; abort loads in progress.
    for (const auto& entry : state.entry_snapshot())
        entry->closing.request_stop();
    state.indexes.stop();
    // 5. Libraries (each closes its catalog, then releases its lock).
    std::lock_guard lock(state.libraries_mutex);
    state.libraries.clear();
}

Result<std::unique_ptr<Application>> Application::start(const ApplicationOptions& options) {
    auto workspace = Workspace::load(options.workspace_directory.value_or(Workspace::default_directory()));
    if (!workspace) return std::unexpected(workspace.error());
    auto state = std::make_unique<State>(options, std::move(*workspace));
    std::unique_ptr<Application> application(new Application(std::move(state)));
    auto& shared = *application->state_;
    if (options.event_handler) application->set_event_handler(options.event_handler);

    shared.import_pool = std::make_unique<BS::thread_pool<>>(std::max<std::size_t>(1, options.import_threads));
    shared.indexes.start();

    // The supervisor exists before any library opens, so the paused-after-crash check in
    // open_library() applies to every library, including the ones reopened here.
    if (options.run_analysis_supervisor) {
        AnalysisSupervisor::Hooks hooks{
            [&shared] {
                std::vector<std::shared_ptr<catalog::Library>> libraries;
                for (const auto& entry : shared.entry_snapshot())
                    libraries.emplace_back(entry, entry->library.get());  // shares the entry's lifetime
                return libraries;
            },
            [&shared](catalog::Library& library, const catalog::SampleDetail& sample) {
                return shared.resolve(library, sample);
            },
            [&shared](catalog::Library& library, const std::string& status) {
                if (auto entry = shared.find(library.uuid())) {
                    shared.indexes.note_analysis_status(*entry, status);
                    shared.indexes.request_update(*entry);
                }
            },
            [&shared](catalog::Library* library, const std::string& context, const Error& error) {
                shared.report(library ? std::optional(library->uuid()) : std::nullopt, context, error);
            }};
        shared.supervisor = std::make_unique<AnalysisSupervisor>(
            options.worker_executable.value_or(worker::default_worker_executable()), std::move(hooks));
    }

    if (options.reopen_libraries) {
        for (const auto& known : shared.workspace.known_libraries()) {
            if (!known.open) continue;
            if (auto opened = application->open_library(known.directory); !opened) {
                shared.report(known.uuid, std::format("reopening '{}'", known.name), opened.error());
                if (options.persist_open_libraries)
                    if (auto closed = shared.workspace.set_library_open(known.uuid, false); !closed)
                        shared.report(known.uuid, "updating the workspace", closed.error());
            }
        }
    }
    if (options.automatic_mode) {
        if (shared.entry_snapshot().empty())
            if (auto created = application->default_library(); !created)
                shared.report(std::nullopt, "creating the default library", created.error());
        for (const auto& entry : shared.entry_snapshot()) {
            if (entry->library->read_only()) continue;
            auto roots = entry->library->list_roots(catalog::RootListing::WithoutSampleCounts);
            if (!roots) {
                shared.report(entry->uuid, "listing sample folders", roots.error());
                continue;
            }
            for (const auto& root : *roots)
                if (auto started = application->start_import(entry->uuid, root.id); !started)
                    shared.report(entry->uuid, std::format("rescanning '{}'", root.name), started.error());
        }
    }
    if (options.monitor_roots) {
        auto* owner = application.get();
        shared.root_monitor = std::jthread([owner](std::stop_token stop) {
            std::mutex sleep_mutex;
            std::condition_variable_any sleeper;
            while (!stop.stop_requested()) {
                try {
                    owner->retry_read_only_libraries();
                    owner->refresh_root_availability();
                } catch (const std::exception& exception) {
                    owner->state_->report(std::nullopt, "checking sample folders",
                                          detail::internal_error("root monitor", exception));
                }
                std::unique_lock lock(sleep_mutex);
                sleeper.wait_for(lock, stop, kRootPollInterval, [] { return false; });
            }
        });
    }
    return application;
}

void Application::set_event_handler(EventHandler handler) {
    std::unique_lock lock(state_->handler_mutex);
    state_->handler = std::make_shared<const EventHandler>(std::move(handler));
    // Quiescence: once this returns, no call of the previous handler is still running.
    if (event_handler_depth == 0) state_->handler_idle.wait(lock, [&] { return state_->emissions_in_flight == 0; });
}

Workspace& Application::workspace() {
    return state_->workspace;
}

// ---------------------------------------------------------------- libraries

namespace {
/// The OpenLibraryInfo of an open library.
OpenLibraryInfo describe(const catalog::Library& library, const uuids::uuid& uuid, const std::string& name,
                         bool in_use_elsewhere, std::int64_t root_count) {
    const auto& recovery = library.recovery_report();
    return OpenLibraryInfo{uuid,
                           name,
                           library.directory(),
                           library.read_only(),
                           in_use_elsewhere,
                           root_count,
                           recovery.key_conflicts,
                           recovery.pre_rekey_backup};
}

}  // namespace

std::shared_ptr<catalog::Library> Application::hold_library(const uuids::uuid& library) const {
    auto entry = state_->find(library);
    if (!entry) return nullptr;
    auto* pointer = entry->library.get();
    return std::shared_ptr<catalog::Library>(std::move(entry), pointer);
}

void Application::request_index_update(const uuids::uuid& library) const {
    if (auto entry = state_->find(library)) state_->indexes.request_update(*entry);
}

Result<OpenLibraryInfo> Application::create_library(const std::filesystem::path& directory, std::string_view name) {
    {
        std::lock_guard lock(state_->libraries_mutex);
        if (state_->libraries.size() + state_->opening.size() >= kMaxOpenLibraries)
            return fail(ErrorCode::ResourceLimit, "at most 8 libraries can be open at the same time");
    }
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
    const std::string folder_name = safe_folder_name(trimmed);
    const auto base = state_->workspace.directory() / "Libraries";
    std::error_code error;
    auto directory = base / path_from_utf8(folder_name);
    for (int suffix = 2; std::filesystem::exists(directory, error); ++suffix)
        directory = base / path_from_utf8(std::format("{} {}", folder_name, suffix));
    if (error)
        return fail(ErrorCode::AccessDenied, std::format("cannot inspect {}: {}", path_to_utf8(base), error.message()));
    return create_library(directory, trimmed);
}

Result<OpenLibraryInfo> Application::default_library() {
    for (const auto& info : libraries())
        if (!info.read_only) return info;
    return create_library_in_workspace("My Samples");
}

Result<OpenLibraryInfo> Application::open_library(const std::filesystem::path& directory) {
    auto& state = *state_;
    std::error_code error;
    const auto canonical = std::filesystem::weakly_canonical(directory, error);
    const auto folder = error ? directory : canonical;

    // 1. Identity first: refuse a duplicate or a ninth library before touching the catalog.
    auto manifest_uuid = peek_library_uuid(folder);
    if (!manifest_uuid) return std::unexpected(manifest_uuid.error());
    const auto key = uuids::to_string(*manifest_uuid);
    {
        std::lock_guard lock(state.libraries_mutex);
        if (state.libraries.contains(key) || state.opening.contains(key))
            return fail(ErrorCode::InvalidArgument,
                        "a library with the same identity is already open (a copied library must be cloned first)");
        if (state.libraries.size() + state.opening.size() >= kMaxOpenLibraries)
            return fail(ErrorCode::ResourceLimit, "at most 8 libraries can be open at the same time");
        state.opening.insert(key);
    }
    /// Releases the reserved slot on every early return.
    struct OpeningSlot {
        State& state;
        const std::string& key;
        ~OpeningSlot() {
            std::lock_guard lock(state.libraries_mutex);
            state.opening.erase(key);
        }
    } slot{state, key};

    // 2. The lock: only its holder may migrate, recover after a crash, or write.
    auto entry = std::make_shared<LibraryEntry>();
    entry->uuid = *manifest_uuid;
    catalog::OpenOptions open_options;
    open_options.on_progress = detail::open_progress_relay(
        [&state](const ApplicationEvent& event) { state.emit(event); }, *manifest_uuid, folder);
    const auto lock_file = folder / FileLock::kLibraryLockFileName;
    std::error_code exists_error;
    const bool lock_file_existed = std::filesystem::exists(lock_file, exists_error);
    auto lock = FileLock::try_acquire(lock_file);
    if (!lock && lock.error().code == ErrorCode::DbBusy && state.still_closing(key)) {
        // This process is still closing the library (an import or a job is finishing): wait for it
        // rather than opening the library read-only as if another program held it.
        lock = FileLock::acquire(lock_file, state.options.close_wait);
        if (!lock && lock.error().code == ErrorCode::DbBusy)
            return fail(ErrorCode::DbBusy, "the library is still closing; try again in a moment", true);
    }
    if (lock) {
        entry->lock = std::move(*lock);
    } else if (lock.error().code == ErrorCode::DbBusy) {
        open_options.read_only = true;  // another process writes it: browse only, no recovery
        entry->in_use_elsewhere = true;
    } else if (lock.error().code == ErrorCode::AccessDenied) {
        open_options.read_only = true;  // read-only media: the lock file cannot even be created
    } else {
        return std::unexpected(lock.error());
    }
    /// A folder that turns out not to open must not keep a library.lock this call created.
    auto discard_created_lock = [&] {
        if (!entry->lock.held() || lock_file_existed) return;
        entry->lock = FileLock();
        std::error_code ignored;  // best effort: a leftover lock file is harmless
        std::filesystem::remove(lock_file, ignored);
    };

    // 3. Open (migration and crash recovery run only on a writable open, i.e. with the lock).
    auto opened = catalog::Library::open(folder, open_options);
    if (!opened) {
        discard_created_lock();
        return std::unexpected(opened.error());
    }
    if ((*opened)->uuid() != *manifest_uuid) {
        opened->reset();
        discard_created_lock();
        return fail(ErrorCode::DbCorrupt, "library.json changed while the library was being opened");
    }
    entry->library = std::move(*opened);
    entry->name = entry->library->identity().name;
    if (state.options.in_memory_index) entry->index = std::make_unique<SampleIndex>(entry->uuid);
    auto roots = entry->library->list_roots(catalog::RootListing::WithoutSampleCounts);
    const auto info = describe(*entry->library, entry->uuid, entry->name, entry->in_use_elsewhere,
                               roots ? static_cast<std::int64_t>(roots->size()) : 0);

    // 4. After an unclean shutdown analysis stays paused until the user resumes it; applied before
    // the library becomes visible to the supervisor.
    const bool interrupted_analysis =
        !entry->library->read_only() && entry->library->recovery_report().interrupted_jobs > 0;
    if (interrupted_analysis && state.supervisor) state.supervisor->set_paused(true);

    {
        std::lock_guard libraries_lock(state.libraries_mutex);
        state.libraries.emplace(key, entry);
        state.closing.erase(key);
    }
    if (state.supervisor) state.supervisor->serve_library(entry->uuid);
    if (state.options.persist_open_libraries)
        if (auto remembered =
                state.workspace.remember_library(KnownLibrary{info.uuid, info.name, info.directory, true});
            !remembered)
            state.report(info.uuid, "remembering the library in the workspace", remembered.error());
    if (entry->index) state.indexes.queue_load(entry);
    state.indexes.note_catalog_change(*entry, {}, true, true);
    if (interrupted_analysis) state.indexes.note_analysis_status(*entry, "paused after an unclean shutdown");
    if (state.supervisor) state.supervisor->notify();
    return info;
}

Result<void> Application::close_library(const uuids::uuid& uuid) {
    std::shared_ptr<LibraryEntry> entry;
    {
        std::lock_guard lock(state_->libraries_mutex);
        const auto found = state_->libraries.find(uuids::to_string(uuid));
        if (found == state_->libraries.end()) return fail(ErrorCode::NotFound, "library is not open");
        entry = std::move(found->second);
        state_->libraries.erase(found);
        state_->closing[uuids::to_string(uuid)] = entry;
    }
    entry->closing.request_stop();
    if (state_->supervisor) state_->supervisor->abandon_library(uuid);
    {
        std::lock_guard lock(state_->operations_mutex);
        for (auto& [id, operation] : state_->operations)
            if (operation->library == uuid) operation->stop.request_stop();
    }
    entry.reset();  // destroyed here unless running work still holds it
    // Its rows no longer count against the index budget: a library answered from SQLite may fit now.
    state_->indexes.request_budget_recheck();
    if (!state_->options.persist_open_libraries) return {};
    return state_->workspace.set_library_open(uuid, false);
}

std::vector<OpenLibraryInfo> Application::libraries() const {
    std::vector<OpenLibraryInfo> infos;
    for (const auto& entry : state_->entry_snapshot()) {
        auto roots = entry->library->list_roots(catalog::RootListing::WithoutSampleCounts);
        infos.push_back(describe(*entry->library, entry->uuid, entry->name, entry->in_use_elsewhere,
                                 roots ? static_cast<std::int64_t>(roots->size()) : 0));
    }
    return infos;
}

// ---------------------------------------------------------------- roots

Result<catalog::RootRecord> Application::add_root(const uuids::uuid& library_uuid, const std::filesystem::path& folder,
                                                  std::string_view name) {
    auto entry = state_->find(library_uuid);
    if (!entry) return fail(ErrorCode::NotFound, "library is not open");
    std::error_code error;
    if (!std::filesystem::is_directory(folder, error)) return fail(ErrorCode::FileMissing, "the folder does not exist");
    const auto canonical = std::filesystem::canonical(folder, error);
    if (error) return fail(ErrorCode::AccessDenied, std::format("cannot resolve the folder: {}", error.message()));
    for (const auto& binding : state_->workspace.root_bindings(library_uuid)) {
        std::error_code ignored;  // an unreachable binding simply cannot be the same folder
        if (std::filesystem::equivalent(binding.path, canonical, ignored))
            return fail(ErrorCode::InvalidArgument, "this folder is already a sample folder of the library");
    }
    std::string root_name = trim(name);
    if (root_name.empty())
        root_name =
            path_to_utf8(canonical.filename().empty() ? canonical.parent_path().filename() : canonical.filename());
    if (root_name.empty()) root_name = path_to_utf8(canonical);
    // Location keys are exact on case-sensitive volumes, so the root records what its volume does.
    auto case_sensitive = catalog::detect_case_sensitivity(canonical);
    if (!case_sensitive) return std::unexpected(case_sensitive.error());
    auto root = entry->library->add_root(root_name, catalog::RootMode::Referenced, *case_sensitive);
    if (!root) return std::unexpected(root.error());
    // The catalog has no root removal; a root whose binding failed stays and can be rebound.
    if (auto bound = state_->workspace.bind_root(RootBinding{library_uuid, root->uuid, canonical}); !bound)
        return fail(bound.error().code,
                    std::format("the folder was added as root {} but could not be bound on this computer ({}); "
                                "rebind it to use it",
                                root->id, bound.error().message()));
    if (auto online = entry->library->set_root_availability(root->id, catalog::Availability::Online); !online) {
        state_->report(library_uuid, "marking a new sample folder online", online.error());
    } else {
        root->availability = catalog::Availability::Online;
        state_->emit(RootAvailabilityEvent{library_uuid, root->id, catalog::Availability::Online});
    }
    state_->indexes.request_update(*entry);
    return root;
}

void Application::retry_read_only_libraries() {
    auto& state = *state_;
    for (const auto& entry : state.entry_snapshot()) {
        if (!entry->in_use_elsewhere) continue;
        const auto folder = entry->library->directory();
        auto lock = FileLock::try_acquire(folder / FileLock::kLibraryLockFileName);
        if (!lock) continue;  // still held by the other process: try again on the next tick
        // A fresh writable entry replaces the read-only one; work still holding the old entry keeps
        // it until that work ends (it only ever read).
        auto upgraded = std::make_shared<LibraryEntry>();
        upgraded->uuid = entry->uuid;
        upgraded->lock = std::move(*lock);
        catalog::OpenOptions open_options;
        open_options.on_progress = detail::open_progress_relay(
            [&state](const ApplicationEvent& event) { state.emit(event); }, entry->uuid, folder);
        auto opened = catalog::Library::open(folder, open_options);
        if (!opened) {
            state.indexes.report_once(*entry, "opening the library for changes", opened.error());
            continue;
        }
        if ((*opened)->uuid() != entry->uuid) continue;  // the folder now holds another library
        upgraded->library = std::move(*opened);
        upgraded->name = upgraded->library->identity().name;
        if (state.options.in_memory_index) upgraded->index = std::make_unique<SampleIndex>(upgraded->uuid);
        {
            std::lock_guard libraries_lock(state.libraries_mutex);
            const auto found = state.libraries.find(uuids::to_string(entry->uuid));
            if (found == state.libraries.end() || found->second != entry) continue;  // closed meanwhile
            found->second = upgraded;
        }
        entry->closing.request_stop();  // its index load, if any, is no longer needed
        const bool interrupted_analysis = upgraded->library->recovery_report().interrupted_jobs > 0;
        if (interrupted_analysis && state.supervisor) state.supervisor->set_paused(true);
        if (upgraded->index) state.indexes.queue_load(upgraded);
        state.indexes.note_catalog_change(*upgraded, {}, true, true);
        if (interrupted_analysis) state.indexes.note_analysis_status(*upgraded, "paused after an unclean shutdown");
        state.emit(LibraryAccessChangedEvent{upgraded->uuid, false, false});
        if (state.supervisor) state.supervisor->notify();
        // Desktop: what changed while the other process held the library is picked up now.
        if (state.options.automatic_mode)
            if (auto roots = upgraded->library->list_roots(catalog::RootListing::WithoutSampleCounts))
                for (const auto& root : *roots)
                    if (auto started = start_import(upgraded->uuid, root.id);
                        !started && started.error().code != ErrorCode::DbBusy)
                        state.report(upgraded->uuid, std::format("rescanning '{}'", root.name), started.error());
    }
}

Result<void> Application::rebind_root(const uuids::uuid& library_uuid, std::int64_t root_id,
                                      const std::filesystem::path& folder) {
    auto entry = state_->find(library_uuid);
    if (!entry) return fail(ErrorCode::NotFound, "library is not open");
    auto roots = entry->library->list_roots(catalog::RootListing::WithoutSampleCounts);
    if (!roots) return std::unexpected(roots.error());
    const auto root = std::ranges::find(*roots, root_id, &catalog::RootRecord::id);
    if (root == roots->end()) return fail(ErrorCode::NotFound, "no such root");
    std::error_code error;
    if (!std::filesystem::is_directory(folder, error)) return fail(ErrorCode::FileMissing, "the folder does not exist");
    const auto canonical = std::filesystem::canonical(folder, error);
    if (error) return fail(ErrorCode::AccessDenied, std::format("cannot resolve the folder: {}", error.message()));
    for (const auto& binding : state_->workspace.root_bindings(library_uuid)) {
        std::error_code ignored;  // an unreachable binding simply cannot be the same folder
        if (binding.root != root->uuid && std::filesystem::equivalent(binding.path, canonical, ignored))
            return fail(ErrorCode::InvalidArgument, "this folder is already another sample folder of the library");
    }
    if (auto bound = state_->workspace.bind_root(RootBinding{library_uuid, root->uuid, canonical}); !bound)
        return bound;
    refresh_root_availability();
    return {};
}

std::optional<std::filesystem::path> Application::root_path(const uuids::uuid& library_uuid,
                                                            const catalog::RootRecord& root) const {
    auto entry = state_->find(library_uuid);
    if (!entry) return std::nullopt;
    return state_->root_folder(*entry->library, root.uuid, root.mode);
}

void Application::refresh_root_availability() {
    auto& state = *state_;
    // Root bindings another process (the CLI's add-root) wrote must be known before roots are
    // judged, or a root added there would be marked offline here.
    if (auto reloaded = state.workspace.reload_if_changed(); !reloaded) state.report_workspace_once(reloaded.error());
    for (const auto& entry : state.entry_snapshot()) {
        if (entry->library->read_only()) continue;
        auto roots = entry->library->list_roots(catalog::RootListing::WithoutSampleCounts);
        if (!roots) {
            state.indexes.report_once(*entry, "listing sample folders", roots.error());
            continue;
        }
        for (const auto& root : *roots) {
            auto folder = state.root_folder(*entry->library, root.uuid, root.mode);
            std::error_code error;
            const bool reachable = folder && std::filesystem::is_directory(*folder, error);
            const auto now = reachable ? catalog::Availability::Online : catalog::Availability::Offline;
            if (now == root.availability || (root.availability == catalog::Availability::Unknown && reachable))
                continue;
            if (auto changed = entry->library->set_root_availability(root.id, now); !changed) {
                state.indexes.report_once(*entry, std::format("updating the availability of '{}'", root.name),
                                          changed.error());
                continue;
            }
            state.emit(RootAvailabilityEvent{entry->uuid, root.id, now});
            state.indexes.request_update(*entry);
            // Desktop: a folder that comes back is rescanned (files may have changed while away).
            if (state.options.automatic_mode && now == catalog::Availability::Online)
                if (auto started = start_import(entry->uuid, root.id);
                    !started && started.error().code != ErrorCode::DbBusy)
                    state.report(entry->uuid, std::format("rescanning '{}'", root.name), started.error());
        }
    }
}

// ---------------------------------------------------------------- import

Result<files::ImportSummary> Application::import_now(const uuids::uuid& library_uuid, std::int64_t root_id,
                                                     std::stop_token stop,
                                                     const files::ImportProgressCallback& progress) {
    auto entry = state_->find(library_uuid);
    if (!entry) return fail(ErrorCode::NotFound, "library is not open");
    auto roots = entry->library->list_roots(catalog::RootListing::WithoutSampleCounts);
    if (!roots) return std::unexpected(roots.error());
    const auto root = std::ranges::find(*roots, root_id, &catalog::RootRecord::id);
    if (root == roots->end()) return fail(ErrorCode::NotFound, std::format("the library has no root {}", root_id));
    auto folder = state_->root_folder(*entry->library, root->uuid, root->mode);
    if (!folder) return fail(ErrorCode::SourceOffline, "this root is not bound to a folder on this computer");
    auto* state = state_.get();
    auto summary =
        files::import_root(*entry->library, root_id, *folder, stop, [&](const files::ImportProgress& current) {
            if (progress) progress(current);
            state->indexes.request_update(*entry);  // committed batches reach the index
        });
    state->indexes.request_update(*entry);
    return summary;
}

Result<OperationId> Application::start_import(const uuids::uuid& library_uuid, std::int64_t root_id) {
    auto& state = *state_;
    if (!state.find(library_uuid)) return fail(ErrorCode::NotFound, "library is not open");
    const auto import_key = std::format("{}/{}", uuids::to_string(library_uuid), root_id);
    {
        std::lock_guard lock(state.imports_mutex);
        if (!state.imports_in_flight.insert(import_key).second)
            return fail(ErrorCode::DbBusy, "this folder is already being scanned", /*retryable=*/true);
    }
    auto operation = std::make_shared<State::Operation>();
    operation->description = "Import";
    operation->library = library_uuid;
    OperationId id = 0;
    {
        // Registered before it is queued, so a cancel right after this returns is never lost.
        std::lock_guard lock(state.operations_mutex);
        id = state.next_operation++;
        state.operations.emplace(id, operation);
    }
    state.import_pool->detach_task([this, operation, id, library_uuid, root_id, import_key] {
        auto& shared = *state_;
        OperationFinishedEvent finished{id, operation->description, std::nullopt};
        // Exception barrier: a throw is reported as the operation's error.
        try {
            const auto stop = operation->stop.get_token();
            if (stop.stop_requested()) {
                finished.error = Error{ErrorCode::Cancelled, false, "import cancelled", {}, {}};
            } else {
                auto result = import_now(library_uuid, root_id, stop, [&](const files::ImportProgress& progress) {
                    shared.emit(ImportProgressEvent{id, library_uuid, root_id, progress});
                });
                if (!result)
                    finished.error = result.error();
                else if (shared.options.automatic_mode && !stop.stop_requested())
                    queue_automatic_analysis(library_uuid, root_id);
            }
        } catch (const std::exception& exception) {
            finished.error = detail::internal_error("import", exception);
        }
        {
            std::lock_guard lock(shared.imports_mutex);
            shared.imports_in_flight.erase(import_key);
        }
        shared.emit(finished);
        {
            std::lock_guard lock(shared.operations_mutex);
            shared.operations.erase(id);  // pruned as soon as it ends
        }
        {
            std::lock_guard lock(operation->mutex);
            operation->finished = true;
        }
        operation->finished_signal.notify_all();
    });
    return id;
}

void Application::cancel_operation(OperationId operation) {
    std::lock_guard lock(state_->operations_mutex);
    if (auto found = state_->operations.find(operation); found != state_->operations.end())
        found->second->stop.request_stop();
}

void Application::wait_for_operation(OperationId operation) {
    std::shared_ptr<State::Operation> running;
    {
        std::lock_guard lock(state_->operations_mutex);
        auto found = state_->operations.find(operation);
        if (found == state_->operations.end()) return;  // unknown or already finished
        running = found->second;
    }
    std::unique_lock lock(running->mutex);
    running->finished_signal.wait(lock, [&] { return running->finished; });
}

// ---------------------------------------------------------------- search, rows and edits

namespace {

/// The libraries of a request as index sources, or nullopt when one of them has no usable index.
template <class Entries>
std::optional<std::vector<IndexedLibrary>> index_sources(const Entries& entries) {
    if (entries.empty() || !std::ranges::all_of(entries, [](const auto& entry) { return entry->index_usable(); }))
        return std::nullopt;
    std::vector<IndexedLibrary> sources;
    for (const auto& entry : entries)
        sources.push_back(IndexedLibrary{entry->index.get(), entry->library.get(), entry->name});
    return sources;
}

/// The catalogs of a request, for SearchService.
template <class Entries>
std::vector<catalog::Library*> catalogs_of(const Entries& entries) {
    std::vector<catalog::Library*> selected;
    for (const auto& entry : entries)
        selected.push_back(entry->library.get());
    return selected;
}

}  // namespace

Result<search::SearchPage> Application::search(const search::SearchRequest& request,
                                               std::optional<uuids::uuid> only_library) const {
    const auto entries = state_->entries_for(only_library);  // keeps them alive
    auto query = search::parse_query(request.text);          // parsed once for both decisions
    if (!query) return std::unexpected(query.error());
    if (SampleIndex::serves(*query, request.sort))
        if (const auto sources = index_sources(entries)) {
            auto page = search_indexes(*sources, request, *query);
            // An index cleared between the check and the search: SQLite answers instead.
            if (page || page.error().code != ErrorCode::IndexUnavailable) return page;
        }
    return search::SearchService().search(catalogs_of(entries), request);
}

Result<search::CountResult> Application::count(const search::SearchRequest& request,
                                               std::optional<uuids::uuid> only_library) const {
    const auto entries = state_->entries_for(only_library);
    auto query = search::parse_query(request.text);
    if (!query) return std::unexpected(query.error());
    if (const auto sources = index_sources(entries)) {
        auto counted = count_indexes(*sources, request, *query);
        if (counted || counted.error().code != ErrorCode::IndexUnavailable) return counted;
    }
    return search::SearchService().count(catalogs_of(entries), request);
}

Result<std::vector<search::SampleRow>> Application::rows(std::span<const SampleKey> keys) const {
    std::map<uuids::uuid, std::vector<std::int64_t>> by_library;
    for (const auto& key : keys)
        by_library[key.library].push_back(key.sample_id);
    std::vector<search::SampleRow> found;
    for (const auto& [library, sample_ids] : by_library) {
        auto entry = state_->find(library);
        if (!entry) continue;  // closed: its samples no longer exist for this session
        if (entry->index_usable()) {
            auto rows = entry->index->rows(sample_ids);
            std::ranges::move(rows, std::back_inserter(found));
            continue;
        }
        auto rows = search::load_rows(*entry->library, sample_ids);
        if (!rows) return std::unexpected(rows.error());
        std::ranges::move(*rows, std::back_inserter(found));
    }
    return found;
}

std::vector<IndexedFolder> Application::folders(const uuids::uuid& library, std::int64_t root_id) const {
    auto entry = state_->find(library);
    if (!entry || !entry->index_usable()) return {};
    return entry->index->folders(root_id);
}

bool Application::index_ready(const uuids::uuid& library) const {
    auto entry = state_->find(library);
    return entry && entry->index_usable();
}

Result<catalog::MutationReceipt> Application::mutate_and_patch(
    const uuids::uuid& library, const std::function<Result<catalog::MutationReceipt>(catalog::Library&)>& mutate) {
    auto entry = state_->find(library);
    if (!entry) return fail(ErrorCode::NotFound, "library is not open");
    auto receipt = mutate(*entry->library);
    // Patch the index on this thread right after the commit: rows() reflects the edit when this
    // returns, before the coalesced event goes out. An edit too large for the outbox waits for
    // the reload it causes.
    if (receipt) {
        state_->indexes.update_index(entry);
        state_->indexes.bring_index_to(entry, receipt->committed_revision);
    }
    return receipt;
}

Result<catalog::MutationReceipt> Application::apply_edit(const uuids::uuid& library,
                                                         const catalog::EditCommand& command) {
    return mutate_and_patch(library, [&](catalog::Library& opened) { return opened.apply_edit(command); });
}

Result<catalog::MutationReceipt> Application::apply_taxonomy(const uuids::uuid& library,
                                                             const catalog::TaxonomyCommand& command) {
    return mutate_and_patch(library, [&](catalog::Library& opened) { return opened.apply_taxonomy(command); });
}

Result<catalog::MutationReceipt> Application::undo(const uuids::uuid& library, const uuids::uuid& operation) {
    return mutate_and_patch(library, [&](catalog::Library& opened) { return opened.undo(operation); });
}

Result<catalog::MutationReceipt> Application::redo(const uuids::uuid& library, const uuids::uuid& operation) {
    return mutate_and_patch(library, [&](catalog::Library& opened) { return opened.redo(operation); });
}

Result<std::filesystem::path> Application::resolve_playable_path(const uuids::uuid& library_uuid,
                                                                 std::int64_t sample_id) const {
    auto entry = state_->find(library_uuid);
    if (!entry) return fail(ErrorCode::NotFound, "library is not open");
    return state_->resolve(*entry->library, sample_id);
}

// ---------------------------------------------------------------- analysis

std::string Application::automatic_analysis_profile() const {
    const auto stored = state_->workspace.preference(kAutomaticProfilePreference);
    return stored.is_string() ? stored.get<std::string>() : std::string(kDefaultAutomaticProfile);
}

Result<void> Application::set_automatic_analysis_profile(std::string_view profile) {
    if (!profile.empty() && profile_stages(profile).empty())
        return fail(ErrorCode::InvalidArgument, std::format("unknown analysis profile '{}'", profile));
    return state_->workspace.set_preference(kAutomaticProfilePreference, std::string(profile));
}

void Application::queue_automatic_analysis(const uuids::uuid& library_uuid, std::int64_t root_id) {
    const std::string profile = automatic_analysis_profile();
    if (profile.empty()) return;
    // Every sample of the folder, set-based in the catalog: the run skips samples whose results are
    // already current, so only what this import added or changed is actually analyzed.
    if (auto run = start_root_analysis(library_uuid, root_id, profile, /*force=*/false); !run)
        state_->report(library_uuid, "queueing automatic analysis", run.error());
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
    auto entry = state_->find(library_uuid);
    if (!entry) return fail(ErrorCode::NotFound, "library is not open");
    auto stages = profile_stages(profile);
    if (stages.empty()) return fail(ErrorCode::InvalidArgument, std::format("unknown analysis profile '{}'", profile));
    auto run = entry->library->create_analysis_run({std::string(profile), std::move(stages), sample_ids, force});
    if (run && state_->supervisor) state_->supervisor->notify();
    return run;
}

Result<catalog::Library::AnalysisRunStatus> Application::start_root_analysis(const uuids::uuid& library_uuid,
                                                                             std::int64_t root_id,
                                                                             std::string_view profile, bool force) {
    auto entry = state_->find(library_uuid);
    if (!entry) return fail(ErrorCode::NotFound, "library is not open");
    const auto stages = profile_stages(profile);
    if (stages.empty()) return fail(ErrorCode::InvalidArgument, std::format("unknown analysis profile '{}'", profile));
    auto run = entry->library->create_analysis_run_for_root(root_id, profile, stages, force);
    if (run && state_->supervisor) state_->supervisor->notify();
    return run;
}

Result<void> Application::control_analysis(const uuids::uuid& library_uuid, std::int64_t run_id,
                                           catalog::Library::RunControl control) {
    auto entry = state_->find(library_uuid);
    if (!entry) return fail(ErrorCode::NotFound, "library is not open");
    auto controlled = entry->library->control_run(run_id, control);
    if (controlled && state_->supervisor) state_->supervisor->notify();
    return controlled;
}

void Application::set_analysis_paused(bool paused) {
    if (state_->supervisor) state_->supervisor->set_paused(paused);
}

bool Application::analysis_paused() const {
    return state_->supervisor ? state_->supervisor->paused() : true;
}

AnalysisQueueStatus Application::analysis_status() const {
    if (!state_->supervisor) return AnalysisQueueStatus{true, false, "analysis is not running in this session"};
    const auto status = state_->supervisor->status();
    return AnalysisQueueStatus{status.paused, status.worker_unavailable, status.detail};
}

// ---------------------------------------------------------------- export

Result<ExportSummary> Application::export_metadata(const uuids::uuid& library_uuid,
                                                   const std::filesystem::path& destination,
                                                   const ExportOptions& options, std::stop_token stop) const {
    auto entry = state_->find(library_uuid);
    if (!entry) return fail(ErrorCode::NotFound, "library is not open");
    const auto* shared = state_.get();
    const auto* library = entry->library.get();
    return write_metadata_export(
        *entry->library, destination, options,
        [shared, library](const uuids::uuid& root, catalog::RootMode mode) {
            return shared->root_folder(*library, root, mode);
        },
        stop);
}

}  // namespace mso::app
