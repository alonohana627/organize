#pragma once
/// @file
/// Machine-local workspace state (docs/02_ARCHITECTURE.md §4): which libraries this computer
/// knows, which are open, root bindings (library UUID + root UUID → absolute local path) and
/// user preferences. Never stored inside a portable library.
#include <cstdint>
#include <filesystem>
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
    bool open = false;
};

/// Machine-local path of one root of one library.
struct RootBinding {
    uuids::uuid library;
    uuids::uuid root;
    std::filesystem::path path;
};

/// Persistent, versioned, thread-safe workspace file (workspace.json, atomically replaced).
class Workspace {
public:
    static constexpr int kVersion = 1;

    /// Per-user data directory: %LOCALAPPDATA%\MusicSampleOrganizer on Windows,
    /// $XDG_DATA_HOME/music-sample-organizer (default ~/.local/share) elsewhere.
    static std::filesystem::path default_directory();

    /// Loads (or starts empty when absent). A corrupt file is renamed aside and replaced with
    /// defaults instead of preventing startup.
    static Result<Workspace> load(const std::filesystem::path& directory);

    Workspace(Workspace&& other) noexcept;
    Workspace& operator=(Workspace&&) = delete;

    const std::filesystem::path& directory() const { return directory_; }

    std::vector<KnownLibrary> known_libraries() const;
    Result<void> remember_library(const KnownLibrary& library);
    Result<void> set_library_open(const uuids::uuid& library, bool open);

    std::optional<std::filesystem::path> root_binding(const uuids::uuid& library, const uuids::uuid& root) const;
    Result<void> bind_root(const RootBinding& binding);

    /// Free-form preferences (volume, audition on select, shortcuts, layout presets...).
    nlohmann::json preference(std::string_view key) const;
    Result<void> set_preference(std::string_view key, nlohmann::json value);

private:
    explicit Workspace(std::filesystem::path directory) : directory_(std::move(directory)) {}
    Result<void> save_locked() const;

    std::filesystem::path directory_;
    mutable std::mutex mutex_;
    std::vector<KnownLibrary> libraries_;
    std::vector<RootBinding> bindings_;
    nlohmann::json preferences_ = nlohmann::json::object();
};

}  // namespace mso::app
