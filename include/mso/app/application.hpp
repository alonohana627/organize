#pragma once
/// @file
/// Application services shared by the desktop UI and the command-line tool. Owns the workspace
/// (machine-local registry of libraries and root bindings), up to eight open libraries, import
/// operations, the analysis supervisor and metadata export.
///
/// Threading: every method is thread-safe and synchronous. Long work (import, analysis, export)
/// runs on background threads owned by the Application and reports through the event handler,
/// which is invoked on those background threads — a UI must marshal events to its own thread.
/// Never call blocking methods from a UI thread; run them on a worker (e.g. QtConcurrent).
#include <cstdint>
#include <filesystem>
#include <functional>
#include <memory>
#include <optional>
#include <stop_token>
#include <string>
#include <variant>
#include <vector>

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
    std::int64_t root_count = 0;  ///< sample folders registered in the library
};

/// Identifies a long-running operation (import, analysis supervision, export).
using OperationId = std::uint64_t;

// ---------------------------------------------------------------- events

/// Import progress for one root (throttled to ~10 Hz).
struct ImportProgressEvent {
    OperationId operation = 0;
    uuids::uuid library;
    std::int64_t root_id = 0;
    files::ImportProgress progress;
};

/// Committed catalog state changed; visible pages of that library should refresh.
struct CatalogChangedEvent {
    uuids::uuid library;
};

/// A root became reachable or unreachable (checked every 2 seconds).
struct RootAvailabilityEvent {
    uuids::uuid library;
    std::int64_t root_id = 0;
    catalog::Availability availability = catalog::Availability::Unknown;
};

/// Analysis queue progress for one library.
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

using ApplicationEvent = std::variant<ImportProgressEvent, CatalogChangedEvent, RootAvailabilityEvent,
                                      AnalysisProgressEvent, OperationFinishedEvent>;
using EventHandler = std::function<void(const ApplicationEvent&)>;

// ---------------------------------------------------------------- export

enum class ExportFormat { Csv, Json };

struct ExportOptions {
    ExportFormat format = ExportFormat::Csv;
    bool spreadsheet_safe = true;  ///< CSV: prefix cells starting with = + - @ with an apostrophe
    bool include_absolute_paths = false;
    bool utf8_bom = true;  ///< CSV only
    std::string query;     ///< restrict to matching samples (empty = all)
};

struct ExportSummary {
    std::int64_t rows = 0;
    std::filesystem::path file;
};

// ---------------------------------------------------------------- application

struct ApplicationOptions {
    std::optional<std::filesystem::path> workspace_directory;  ///< default: per-user data directory
    std::optional<std::filesystem::path> worker_executable;    ///< default: mso-worker next to this executable
    bool reopen_libraries = true;
    bool monitor_roots = true;
    bool run_analysis_supervisor = true;
    /// Desktop behaviour: create "My Samples" in the workspace when no library exists, rescan every
    /// folder at start-up and whenever it reconnects, and analyze what an import adds (see
    /// automatic_analysis_profile). The CLI and tests leave this off and stay fully explicit.
    bool automatic_mode = false;
};

class Application {
public:
    static constexpr std::size_t kMaxOpenLibraries = 8;

    static Result<std::unique_ptr<Application>> start(const ApplicationOptions& options = {});
    ~Application();
    Application(const Application&) = delete;
    Application& operator=(const Application&) = delete;

    /// Replaces the event handler (may be called from any thread).
    void set_event_handler(EventHandler handler);

    // ------------------------------------------------------------ libraries
    Result<OpenLibraryInfo> create_library(const std::filesystem::path& directory, std::string_view name);
    /// Creates a library under the workspace's Libraries folder (a unique folder name derived
    /// from `name`), so users never have to pick or empty a folder for the catalog.
    Result<OpenLibraryInfo> create_library_in_workspace(std::string_view name);
    /// The library new sample folders go to by default: the only/first open writable library,
    /// creating "My Samples" in the workspace when none is open.
    Result<OpenLibraryInfo> default_library();
    /// True when the folder holds a library (library.json).
    static bool is_library_folder(const std::filesystem::path& directory);
    /// Opens a library; refuses a second library with the same UUID (a copy must be cloned).
    Result<OpenLibraryInfo> open_library(const std::filesystem::path& directory);
    Result<void> close_library(const uuids::uuid& library);
    std::vector<OpenLibraryInfo> libraries() const;
    /// Borrowed pointer, valid until close_library() or destruction; nullptr when not open.
    catalog::Library* library(const uuids::uuid& library) const;
    std::vector<catalog::Library*> open_libraries() const;

    // ------------------------------------------------------------ roots
    /// Registers a referenced source folder and binds it to its absolute path on this machine.
    Result<catalog::RootRecord> add_root(const uuids::uuid& library, const std::filesystem::path& folder,
                                         std::string_view name = {});
    /// Points an existing root at a new local path (e.g. the drive letter changed).
    Result<void> rebind_root(const uuids::uuid& library, std::int64_t root_id, const std::filesystem::path& folder);
    std::optional<std::filesystem::path> root_path(const uuids::uuid& library, const catalog::RootRecord& root) const;
    /// Checks every root of the open libraries now (the background monitor does this every 2 s).
    void refresh_root_availability();

    // ------------------------------------------------------------ import
    /// Starts a background scan of one root. Progress and completion arrive as events.
    Result<OperationId> start_import(const uuids::uuid& library, std::int64_t root_id);
    /// Runs a scan on the calling thread (CLI and tests).
    Result<files::ImportSummary> import_now(const uuids::uuid& library, std::int64_t root_id, std::stop_token stop = {},
                                            const files::ImportProgressCallback& progress = {});
    void cancel_operation(OperationId operation);
    /// Blocks until the operation has finished (tests, CLI and clean shutdown).
    void wait_for_operation(OperationId operation);

    // ------------------------------------------------------------ search and resolution
    /// Searches one library (scope) or every open library (nullopt).
    Result<search::SearchPage> search(const search::SearchRequest& request,
                                      std::optional<uuids::uuid> scope = std::nullopt) const;
    Result<search::CountResult> count(const search::SearchRequest& request,
                                      std::optional<uuids::uuid> scope = std::nullopt) const;
    /// Absolute path of a sample's playable location after re-checking it exists right now.
    /// SOURCE_OFFLINE when its root is unbound/unreachable, FILE_MISSING when the file is gone.
    Result<std::filesystem::path> resolve_playable_path(const uuids::uuid& library, std::int64_t sample_id) const;

    // ------------------------------------------------------------ analysis
    /// Profile queued automatically after an import in automatic mode; empty disables it.
    /// Persisted as the workspace preference "analysis.automatic_profile" (default "Full").
    std::string automatic_analysis_profile() const;
    void set_automatic_analysis_profile(std::string_view profile);
    /// Built-in profiles: "Waveforms", "Musical", "Duplicates", "Full".
    static std::vector<std::string> profile_stages(std::string_view profile);
    Result<catalog::Library::AnalysisRunStatus> start_analysis(const uuids::uuid& library, std::string_view profile,
                                                               const std::vector<std::int64_t>& sample_ids,
                                                               bool force = false);
    Result<void> control_analysis(const uuids::uuid& library, std::int64_t run_id,
                                  catalog::Library::RunControl control);
    /// Pauses or resumes the whole background analysis queue (all libraries).
    void set_analysis_paused(bool paused);
    bool analysis_paused() const;

    // ------------------------------------------------------------ export
    Result<ExportSummary> export_metadata(const uuids::uuid& library, const std::filesystem::path& destination,
                                          const ExportOptions& options, std::stop_token stop = {}) const;

    Workspace& workspace();

private:
    /// Queues the automatic profile for a folder after its import (automatic mode only).
    void queue_automatic_analysis(const uuids::uuid& library, std::int64_t root_id);

    struct State;
    explicit Application(std::unique_ptr<State> state);
    std::unique_ptr<State> state_;
};

}  // namespace mso::app
