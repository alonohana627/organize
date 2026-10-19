#pragma once
/// @file
/// Application services shared by the desktop UI and the command-line tool. Owns the workspace
/// (machine-local registry of libraries and root bindings), up to eight open libraries with their
/// in-memory sample indexes, import operations, the analysis supervisor and metadata export.
///
/// Responsiveness model: every open library has a SampleIndex (all live rows in memory). search(),
/// count() and rows() are answered from it in milliseconds; edits made through apply_edit(),
/// undo() and redo() commit to SQLite and patch the index before they return, so the caller can
/// re-read the edited rows immediately. Everything else that changes a catalog (imports, analysis,
/// root availability, work done through with_library()) reaches the index through the catalog
/// outbox on a background thread, and the UI hears about it through coalesced events.
///
/// Memory: the indexes of all open libraries together hold at most
/// ApplicationOptions::index_row_limit rows. A library that would exceed it (or whose growth
/// exceeds it) is answered from SQLite instead, with the same results, only slower; it is indexed
/// again once it fits with 10% to spare (checked when another library closes and every 30 s).
///
/// Index reloads (when the change feed cannot describe a change, e.g. a tag renamed on more than
/// 10,000 samples) are coalesced per library: at most one queued and one running, and requests
/// that arrive while one runs become a single follow-up. Background updates of one library are
/// coalesced to one per 100 ms.
///
/// Threading: every method is thread-safe and synchronous. Long work (import, analysis, export,
/// index loading) runs on background threads owned by the Application. Events are delivered on
/// whichever thread produced them: usually those background threads, but also the calling thread
/// of a method that reports while it runs (add_root(), rows() and the edit methods can emit
/// RootAvailabilityEvent or BackgroundErrorEvent before they return). A UI must marshal events to
/// its own thread and must not block inside the handler. Never call blocking methods from a UI
/// thread; run them on a worker (e.g. QtConcurrent).
#include <chrono>
#include <compare>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <memory>
#include <optional>
#include <span>
#include <stop_token>
#include <string>
#include <string_view>
#include <type_traits>
#include <variant>
#include <vector>

#include "mso/app/sample_index.hpp"
#include "mso/app/workspace.hpp"
#include "mso/catalog/library.hpp"
#include "mso/files/importer.hpp"
#include "mso/search/search_service.hpp"

namespace mso::app {

/// A library currently open in this session.
struct OpenLibraryInfo {
    uuids::uuid uuid;
    std::string name;
    std::filesystem::path directory;
    bool read_only = false;
    /// Another process (e.g. the desktop app while the CLI runs) holds the library's lock, so this
    /// session opened it read-only.
    bool in_use_elsewhere = false;
    std::int64_t root_count = 0;  ///< sample folders registered in the library
    /// Samples whose rebuilt name or path key collided with another row's when this session opened
    /// the library (an open review item lists them); 0 normally.
    std::int64_t key_conflicts = 0;
    /// The verified backup taken before keys were rebuilt at this open, if any.
    std::optional<std::filesystem::path> pre_rekey_backup;
};

/// Identifies one sample across the open libraries.
struct SampleKey {
    uuids::uuid library;
    std::int64_t sample_id = 0;
    /// Library UUID first, then sample id. (uuids::uuid offers == and < only, so the ordering is
    /// synthesized as weak_ordering rather than `auto`.)
    std::weak_ordering operator<=>(const SampleKey&) const = default;
    bool operator==(const SampleKey&) const = default;
};

/// Identifies a long-running operation (import, analysis supervision, export).
using OperationId = std::uint64_t;

// ---------------------------------------------------------------- events

/// Import progress for one root (throttled to ~10 Hz by the importer).
struct ImportProgressEvent {
    OperationId operation = 0;
    uuids::uuid library;
    std::int64_t root_id = 0;
    files::ImportProgress progress;
};

/// Committed catalog state of one library changed. Coalesced: at most one event per library every
/// kEventInterval, carrying the union of everything that changed since the previous one.
struct CatalogChangedEvent {
    uuids::uuid library;
    std::vector<std::int64_t> sample_ids;  ///< rows to refresh in place; empty with !full_reload = nothing
    bool structure_changed = false;        ///< reload navigator trees (roots, folders, tags, collections)
    bool full_reload = false;              ///< re-run the current query (rows added/removed, index reloaded)
};

/// A root became reachable or unreachable (checked every 2 seconds).
struct RootAvailabilityEvent {
    uuids::uuid library;
    std::int64_t root_id = 0;
    catalog::Availability availability = catalog::Availability::Unknown;
};

/// Analysis queue progress for one library (coalesced like CatalogChangedEvent).
struct AnalysisProgressEvent {
    uuids::uuid library;
    std::vector<catalog::Library::AnalysisRunStatus> runs;
    std::string worker_status;  ///< e.g. "idle", "analyzing kick.wav", "worker unavailable: ..."
};

/// A long operation ended (successfully, cancelled, or with an error).
struct OperationFinishedEvent {
    OperationId operation = 0;
    std::string description;
    std::optional<Error> error;  ///< nullopt = success; CANCELLED = cancelled with partial work kept
};

/// A background step failed (index synchronization, an analysis job transition, a workspace
/// write). Nothing is retried silently without being reported here.
struct BackgroundErrorEvent {
    std::optional<uuids::uuid> library;
    std::string context;  ///< what was being done, e.g. "updating the sample index"
    Error error;
};

/// Whether a library can be changed in this session changed: a library opened read-only because
/// another process (usually the CLI) held its lock became writable once that process released it.
/// The UI tells the user and re-enables editing, importing and analysis for it.
struct LibraryAccessChangedEvent {
    uuids::uuid library;
    bool read_only = false;         ///< the new state
    bool in_use_elsewhere = false;  ///< another process still holds the lock
};

/// A library being opened (open_library(), or a read-only library becoming writable) is doing
/// one-time work that can take seconds on a big catalog: a verified backup, schema migrations, a
/// rebuild of its name and path keys, an integrity check. Emitted on the opening thread at the start
/// of each step, at most every kOpenProgressInterval while a step runs, and when a step completes,
/// so the UI can show that the library is being upgraded instead of looking hung.
struct LibraryOpenProgressEvent {
    uuids::uuid library;              ///< from the folder's library.json
    std::filesystem::path directory;  ///< the library folder
    catalog::OpenStep step = catalog::OpenStep::BackingUp;
    int completed = 0;  ///< parts of `step` done (a migration, a rebuilt table, the whole backup or check)
    int total = 1;      ///< parts of `step`
};

using ApplicationEvent =
    std::variant<ImportProgressEvent, CatalogChangedEvent, RootAvailabilityEvent, AnalysisProgressEvent,
                 OperationFinishedEvent, BackgroundErrorEvent, LibraryAccessChangedEvent, LibraryOpenProgressEvent>;
using EventHandler = std::function<void(const ApplicationEvent&)>;

// ---------------------------------------------------------------- export

/// File format of a metadata export.
enum class ExportFormat { Csv, Json };

/// What a metadata export writes.
struct ExportOptions {
    ExportFormat format = ExportFormat::Csv;
    bool spreadsheet_safe = true;  ///< CSV: prefix cells starting with = + - @ tab CR with an apostrophe
    bool include_absolute_paths = false;
    bool utf8_bom = true;  ///< CSV only
    std::string query;     ///< restrict to matching samples (empty = all)
};

/// Outcome of a metadata export.
struct ExportSummary {
    std::int64_t rows = 0;
    std::filesystem::path file;
};

// ---------------------------------------------------------------- application

/// Start-up options. The defaults are the desktop app's; the CLI and tests switch parts off.
struct ApplicationOptions {
    std::optional<std::filesystem::path> workspace_directory;  ///< default: per-user data directory
    std::optional<std::filesystem::path> worker_executable;    ///< default: mso-worker next to this executable
    bool reopen_libraries = true;
    bool monitor_roots = true;
    bool run_analysis_supervisor = true;
    /// Keep every open library's rows in memory (SampleIndex). The one-shot CLI turns it off and
    /// searches SQLite directly.
    bool in_memory_index = true;
    /// Record opened/closed libraries in the workspace so the desktop app reopens them. The CLI turns
    /// it off: libraries it touches must never be auto-reopened by the desktop app.
    bool persist_open_libraries = true;
    /// Imports that run at the same time (further imports wait in a queue).
    std::size_t import_threads = 2;
    /// Most rows the in-memory indexes of all open libraries hold together (the memory budget of
    /// docs/06; see SampleIndex::kDefaultRowLimit). A library that does not fit is searched in SQLite.
    std::size_t index_row_limit = SampleIndex::kDefaultRowLimit;
    /// How long open_library() waits for a library this process is still closing (its last
    /// background work still holds the lock) before reporting DB_BUSY.
    std::chrono::milliseconds close_wait{10'000};
    /// Desktop behaviour: create "My Samples" in the workspace when no library exists, rescan every
    /// folder at start-up and whenever it reconnects, and analyze what an import adds (see
    /// automatic_analysis_profile). The CLI and tests leave this off and stay fully explicit.
    bool automatic_mode = false;
    /// Event handler installed before start() opens any library, so events of the start-up itself
    /// (LibraryOpenProgressEvent while a reopened library upgrades) are delivered. Replace it later
    /// with set_event_handler(). Empty: no handler until set_event_handler() is called.
    EventHandler event_handler;
};

/// State of the background analysis queue.
struct AnalysisQueueStatus {
    bool paused = true;               ///< nothing is leased while paused (also true without a supervisor)
    bool worker_unavailable = false;  ///< mso-worker could not be started; resuming retries it
    std::string detail;               ///< last worker status or error, for display
};

/// The application services: libraries, roots, imports, search, edits, analysis and export, shared
/// by the desktop UI and the CLI. See the file comment for the responsiveness and threading model.
class Application {
public:
    static constexpr std::size_t kMaxOpenLibraries = 8;

    /// Minimum spacing of CatalogChangedEvent and AnalysisProgressEvent per library.
    static constexpr std::chrono::milliseconds kEventInterval{150};
    /// Minimum spacing of LibraryOpenProgressEvent within one step (step starts and ends always go out).
    static constexpr std::chrono::milliseconds kOpenProgressInterval{100};

    /// Loads the workspace, starts the background services and (per options) reopens libraries.
    [[nodiscard]] static Result<std::unique_ptr<Application>> start(const ApplicationOptions& options = {});
    /// Stops background work in dependency order (root monitor, imports, analysis supervisor, index
    /// maintenance), then closes every library.
    ~Application();
    Application(const Application&) = delete;
    Application& operator=(const Application&) = delete;

    /// Replaces the event handler (may be called from any thread). When it returns, no call of the
    /// previous handler is still running, except when called from inside a handler (which would
    /// otherwise wait for itself).
    void set_event_handler(EventHandler handler);

    // ------------------------------------------------------------ libraries
    /// Creates a new library folder (empty or absent) and opens it. RESOURCE_LIMIT when eight are open.
    [[nodiscard]] Result<OpenLibraryInfo> create_library(const std::filesystem::path& directory, std::string_view name);
    /// Creates a library under the workspace's Libraries folder (a unique folder name derived
    /// from `name`), so users never have to pick or empty a folder for the catalog.
    [[nodiscard]] Result<OpenLibraryInfo> create_library_in_workspace(std::string_view name);
    /// The library new sample folders go to by default: the only/first open writable library,
    /// creating "My Samples" in the workspace when none is open.
    [[nodiscard]] Result<OpenLibraryInfo> default_library();
    /// True when the folder holds a library (library.json).
    static bool is_library_folder(const std::filesystem::path& directory);
    /// Opens a library. The identity in library.json is checked first: a second library with the
    /// same UUID (a copy must be cloned) or a ninth library is refused before anything is touched.
    /// The library's lock file is then taken; only with the lock is the catalog migrated and
    /// recovered after a crash. When another process holds the lock the library opens read-only
    /// (OpenLibraryInfo::in_use_elsewhere); the root monitor keeps retrying the lock and makes the
    /// library writable when that process lets go (LibraryAccessChangedEvent). When this process
    /// is still closing the same library, it waits up to ApplicationOptions::close_wait for that to finish, then
    /// fails with DB_BUSY (retryable) instead of opening it read-only.
    [[nodiscard]] Result<OpenLibraryInfo> open_library(const std::filesystem::path& directory);
    /// Closes a library: it leaves the open set immediately; its imports, its index load and its
    /// running analysis job are cancelled (the job goes back to the queue), and work still holding
    /// it (a with_library() call) keeps it alive until that work ends.
    [[nodiscard]] Result<void> close_library(const uuids::uuid& library);
    /// Every open library.
    std::vector<OpenLibraryInfo> libraries() const;

    /// Runs `work(catalog::Library&)` while holding the library alive (close_library() cannot
    /// destroy it meanwhile); NOT_FOUND when it is not open. The result is Result<R> for a `work`
    /// returning R, and is flattened when `work` already returns a Result. Changes made by `work`
    /// reach the index through the outbox shortly after it returns.
    template <class Work>
    [[nodiscard]] auto with_library(const uuids::uuid& library, Work&& work) const;

    // ------------------------------------------------------------ roots
    /// Registers a referenced source folder and binds it to its canonical path on this machine.
    /// INVALID_ARGUMENT when the folder is already a root of this library.
    [[nodiscard]] Result<catalog::RootRecord> add_root(const uuids::uuid& library, const std::filesystem::path& folder,
                                                       std::string_view name = {});
    /// Points an existing root at a new local path (e.g. the drive letter changed).
    [[nodiscard]] Result<void> rebind_root(const uuids::uuid& library, std::int64_t root_id,
                                           const std::filesystem::path& folder);
    /// Absolute local folder of a root (managed roots live inside the library).
    std::optional<std::filesystem::path> root_path(const uuids::uuid& library, const catalog::RootRecord& root) const;
    /// Checks every root of the open libraries now (the background monitor does this every 2 s),
    /// after re-reading root bindings another process (the CLI) may have added to the workspace.
    void refresh_root_availability();
    /// Tries again to take the lock of every library opened read-only because another process held
    /// it, and makes it writable when that succeeds (the root monitor does this every 2 s).
    void retry_read_only_libraries();

    // ------------------------------------------------------------ import
    /// Queues a background scan of one root (at most ApplicationOptions::import_threads run at
    /// once). Progress and completion arrive as events.
    [[nodiscard]] Result<OperationId> start_import(const uuids::uuid& library, std::int64_t root_id);
    /// Runs a scan on the calling thread (CLI and tests).
    [[nodiscard]] Result<files::ImportSummary> import_now(const uuids::uuid& library, std::int64_t root_id,
                                                          std::stop_token stop = {},
                                                          const files::ImportProgressCallback& progress = {});
    /// Requests cooperative cancellation of a queued or running operation.
    void cancel_operation(OperationId operation);
    /// Blocks until the operation has finished (returns at once for an unknown or finished one).
    void wait_for_operation(OperationId operation);

    // ------------------------------------------------------------ search, rows and edits
    /// Searches one library (`only_library`) or every open library (nullopt). Served from the in-memory
    /// indexes (folder/tag/category/collection scopes, sorting, paging and row-evaluable filters
    /// in memory; full-text terms via search::matching_ids then memory). Falls back to SQLite while
    /// an index is still loading and for relevance ordering of text terms.
    [[nodiscard]] Result<search::SearchPage> search(const search::SearchRequest& request,
                                                    std::optional<uuids::uuid> only_library = std::nullopt) const;
    /// Exact count of a request (same sources as search()).
    [[nodiscard]] Result<search::CountResult> count(const search::SearchRequest& request,
                                                    std::optional<uuids::uuid> only_library = std::nullopt) const;
    /// Current rows of specific samples (for in-place row refresh), each carrying its SampleKey
    /// (SampleRow::library and sample_id). Samples that no longer exist, or whose library is not
    /// open, are omitted; a real failure (the database cannot be read) is an error, never an
    /// empty result. From the index, or SQLite while it is loading.
    [[nodiscard]] Result<std::vector<search::SampleRow>> rows(std::span<const SampleKey> keys) const;
    /// Folders of a root with sample counts, from the index (empty while it is loading).
    std::vector<IndexedFolder> folders(const uuids::uuid& library, std::int64_t root_id) const;
    /// True once the library's in-memory index has loaded.
    bool index_ready(const uuids::uuid& library) const;
    /// Applies an edit and patches the in-memory index before returning (so rows() already shows
    /// it); the coalesced CatalogChangedEvent follows. An edit the change feed cannot describe waits
    /// for the index reload it causes. A receipt with `partial` set means only part of a chunked
    /// edit was applied; callers must tell the user.
    [[nodiscard]] Result<catalog::MutationReceipt> apply_edit(const uuids::uuid& library,
                                                              const catalog::EditCommand& command);
    /// Applies a taxonomy change (tags, categories, collections) and patches the index.
    [[nodiscard]] Result<catalog::MutationReceipt> apply_taxonomy(const uuids::uuid& library,
                                                                  const catalog::TaxonomyCommand& command);
    /// Undoes the action at the library's history cursor and patches the index. Retrying with the
    /// same `operation` returns the stored receipt instead of undoing a second action.
    [[nodiscard]] Result<catalog::MutationReceipt> undo(const uuids::uuid& library,
                                                        const uuids::uuid& operation = new_uuid());
    /// Redoes the next action and patches the index (idempotent per `operation`, like undo()).
    [[nodiscard]] Result<catalog::MutationReceipt> redo(const uuids::uuid& library,
                                                        const uuids::uuid& operation = new_uuid());
    /// Absolute path of a sample's playable location after re-checking it exists right now.
    /// SOURCE_OFFLINE when its root is unbound/unreachable, FILE_MISSING when the file is gone.
    [[nodiscard]] Result<std::filesystem::path> resolve_playable_path(const uuids::uuid& library,
                                                                      std::int64_t sample_id) const;

    // ------------------------------------------------------------ analysis
    /// Profile queued automatically after an import in automatic mode; empty disables it.
    /// Persisted as the workspace preference "analysis.automatic_profile" (default "Full").
    std::string automatic_analysis_profile() const;
    [[nodiscard]] Result<void> set_automatic_analysis_profile(std::string_view profile);
    /// Built-in profiles: "Waveforms", "Musical", "Duplicates", "Full" (empty for an unknown name).
    static std::vector<std::string> profile_stages(std::string_view profile);
    /// Creates an explicit analysis run over specific samples (the selection is frozen).
    [[nodiscard]] Result<catalog::Library::AnalysisRunStatus> start_analysis(
        const uuids::uuid& library, std::string_view profile, const std::vector<std::int64_t>& sample_ids,
        bool force = false);
    /// Creates an analysis run over every live sample of a root (set-based in the catalog).
    [[nodiscard]] Result<catalog::Library::AnalysisRunStatus> start_root_analysis(const uuids::uuid& library,
                                                                                  std::int64_t root_id,
                                                                                  std::string_view profile,
                                                                                  bool force = false);
    /// Pauses, resumes or cancels one run.
    [[nodiscard]] Result<void> control_analysis(const uuids::uuid& library, std::int64_t run_id,
                                                catalog::Library::RunControl control);
    /// Pauses or resumes the whole background analysis queue (all libraries). Resuming also
    /// retries a worker that could not be started.
    void set_analysis_paused(bool paused);
    bool analysis_paused() const;
    /// Paused / worker-unavailable state of the queue (the CLI uses it to never wait forever).
    AnalysisQueueStatus analysis_status() const;

    // ------------------------------------------------------------ export
    /// Streams a CSV or JSON export of one library to `destination` (atomic replace).
    [[nodiscard]] Result<ExportSummary> export_metadata(const uuids::uuid& library,
                                                        const std::filesystem::path& destination,
                                                        const ExportOptions& options, std::stop_token stop = {}) const;

    /// The machine-local workspace (root bindings, preferences).
    Workspace& workspace();

private:
    struct State;
    explicit Application(std::unique_ptr<State> state);

    /// The open library (kept alive by the returned pointer, which shares ownership of its entry).
    std::shared_ptr<catalog::Library> hold_library(const uuids::uuid& library) const;
    /// Asks the index maintenance thread to catch up with `library`'s outbox.
    void request_index_update(const uuids::uuid& library) const;
    /// Queues the automatic profile for a folder after its import (automatic mode only).
    void queue_automatic_analysis(const uuids::uuid& library, std::int64_t root_id);
    /// Commits through `mutate` and patches the index on the calling thread before returning.
    [[nodiscard]] Result<catalog::MutationReceipt> mutate_and_patch(
        const uuids::uuid& library, const std::function<Result<catalog::MutationReceipt>(catalog::Library&)>& mutate);

    std::unique_ptr<State> state_;
};

// ---------------------------------------------------------------- template implementation

namespace detail {
/// Result<T> for T, and Result<T> unchanged.
template <class Type>
struct flattened_result {
    using type = Result<Type>;
};
template <class Value>
struct flattened_result<Result<Value>> {
    using type = Result<Value>;
};
}  // namespace detail

template <class Work>
auto Application::with_library(const uuids::uuid& library, Work&& work) const {
    using Returned = std::invoke_result_t<Work, catalog::Library&>;
    using Flattened = typename detail::flattened_result<std::remove_cvref_t<Returned>>::type;
    const auto held = hold_library(library);
    if (!held) return Flattened(fail(ErrorCode::NotFound, "library is not open"));
    /// Notifies the index thread after `work`, also when it returns early.
    struct UpdateRequest {
        const Application* application;
        const uuids::uuid& library;
        ~UpdateRequest() { application->request_index_update(library); }
    } update_request{this, library};
    if constexpr (std::is_void_v<Returned>) {
        std::invoke(std::forward<Work>(work), *held);
        return Flattened();
    } else {
        return Flattened(std::invoke(std::forward<Work>(work), *held));
    }
}

}  // namespace mso::app
