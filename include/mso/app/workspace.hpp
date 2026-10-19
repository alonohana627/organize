#pragma once
/// @file
/// Machine-local workspace state (docs/02_ARCHITECTURE.md §4): which libraries this computer
/// knows, which are open, root bindings (library UUID + root UUID → absolute local path) and
/// user preferences. Never stored inside a portable library.
///
/// The desktop app and the CLI share one workspace.json. Every change is therefore a
/// read-merge-write under an inter-process lock (`workspace.lock`): the latest file on disk is
/// re-read, the single change is applied to it, and the result is written to a temporary file,
/// flushed to stable storage and renamed into place. Concurrent processes never lose each other's
/// updates, and a crash never leaves a truncated file.
#include <cstdint>
#include <filesystem>
#include <functional>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

#include "mso/domain/core.hpp"
#include "mso/domain/error.hpp"

namespace mso::app {

/// A library remembered by this workstation.
struct KnownLibrary {
    uuids::uuid uuid;
    std::string name;
    std::filesystem::path directory;
    bool open = false;  ///< reopened by the desktop app at start-up
};

/// Machine-local path of one root of one library.
struct RootBinding {
    uuids::uuid library;
    uuids::uuid root;
    std::filesystem::path path;
};

/// Persistent, versioned, thread- and process-safe workspace file (workspace.json).
class Workspace {
public:
    static constexpr int kVersion = 1;
    static constexpr std::string_view kFileName = "workspace.json";
    static constexpr std::string_view kLockFileName = "workspace.lock";

    /// Per-user data directory: MSO_WORKSPACE_DIR when set, else %LOCALAPPDATA%\MusicSampleOrganizer
    /// on Windows and $XDG_DATA_HOME/music-sample-organizer (default ~/.local/share) elsewhere.
    /// Environment values are read as UTF-16 on Windows, so non-ASCII profile names work. Empty when
    /// no per-user directory can be determined (load() then reports it instead of falling back to a
    /// temporary directory that would silently lose state).
    static std::filesystem::path default_directory();

    /// Loads the workspace (or starts empty when no file exists yet). A corrupt or newer-version
    /// file is renamed aside and replaced with defaults instead of preventing start-up. Fails only
    /// when the folder cannot be determined, created or read.
    static Result<Workspace> load(const std::filesystem::path& directory);

    Workspace(Workspace&& other) noexcept;
    Workspace& operator=(Workspace&&) = delete;

    const std::filesystem::path& directory() const { return directory_; }

    /// Libraries this workstation knows (as of the last load or change made by this process).
    std::vector<KnownLibrary> known_libraries() const;
    /// Adds or replaces the entry for `library.uuid`.
    Result<void> remember_library(const KnownLibrary& library);
    /// Marks a known library open or closed (no-op for an unknown library).
    Result<void> set_library_open(const uuids::uuid& library, bool open);

    /// The local folder bound to a root, if any.
    std::optional<std::filesystem::path> root_binding(const uuids::uuid& library, const uuids::uuid& root) const;
    /// Every binding of one library.
    std::vector<RootBinding> root_bindings(const uuids::uuid& library) const;
    /// Adds or replaces the binding of (binding.library, binding.root).
    Result<void> bind_root(const RootBinding& binding);

    /// Free-form preferences (volume, audition on select, shortcuts, layout presets...); null when unset.
    nlohmann::json preference(std::string_view key) const;
    /// Stores one preference (merged into the latest file, other keys untouched).
    Result<void> set_preference(std::string_view key, nlohmann::json value);

private:
    /// Everything stored in workspace.json.
    struct Document {
        std::vector<KnownLibrary> libraries;
        std::vector<RootBinding> bindings;
        nlohmann::json preferences = nlohmann::json::object();
    };
    /// Outcome of reading workspace.json from disk.
    enum class FileState { Absent, Readable, Unreadable, Newer };

    explicit Workspace(std::filesystem::path directory) : directory_(std::move(directory)) {}
    /// Reads and parses workspace.json into `document`; entries of the wrong shape are skipped.
    Result<FileState> read_file(Document& document) const;
    /// Applies `change` to the latest on-disk document under the inter-process lock and writes it
    /// atomically when `change` reports a modification (returns true).
    Result<void> update(const std::function<bool(Document&)>& change);

    std::filesystem::path directory_;
    mutable std::mutex mutex_;  ///< guards document_ and serializes updates inside this process
    Document document_;
};

}  // namespace mso::app
