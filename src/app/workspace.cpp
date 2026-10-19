#include "mso/app/workspace.hpp"

#include <format>
#include <fstream>
#include <sstream>

#include "mso/app/file_lock.hpp"
#include "mso/app/json_fields.hpp"
#include "mso/catalog/durable_file.hpp"

#ifdef _WIN32
#include <windows.h>  // GetEnvironmentVariableW
#else
#include <cstdlib>  // std::getenv
#endif

namespace mso::app {

namespace {

/// How long a change waits for another process to finish its workspace write.
constexpr auto kLockTimeout = std::chrono::seconds(5);

/// An environment variable as a path; empty when unset or empty. Windows reads the UTF-16
/// environment (the narrow one is in the ANSI code page); POSIX paths are native bytes.
std::filesystem::path environment_path(const char* name) {
#ifdef _WIN32
    const std::string_view narrow(name);
    const std::wstring wide(narrow.begin(), narrow.end());  // the variable names are ASCII
    const DWORD size = GetEnvironmentVariableW(wide.c_str(), nullptr, 0);
    if (size <= 1) return {};  // unset (0) or empty (just the terminator)
    std::wstring value(size, L'\0');
    const DWORD written = GetEnvironmentVariableW(wide.c_str(), value.data(), size);
    if (written == 0 || written >= size) return {};  // changed between the two calls
    value.resize(written);
    return std::filesystem::path(value);
#else
    const char* value = std::getenv(name);
    return value != nullptr && *value != '\0' ? std::filesystem::path(value) : std::filesystem::path();
#endif
}

/// The workspace.json document of a workspace state.
nlohmann::json serialize(const std::vector<KnownLibrary>& libraries, const std::vector<RootBinding>& bindings,
                         const nlohmann::json& preferences) {
    nlohmann::json library_array = nlohmann::json::array();
    for (const auto& library : libraries)
        library_array.push_back({{"uuid", uuids::to_string(library.uuid)},
                                 {"name", library.name},
                                 {"directory", path_to_utf8(library.directory)},
                                 {"open", library.open}});
    nlohmann::json binding_array = nlohmann::json::array();
    for (const auto& binding : bindings)
        binding_array.push_back({{"library", uuids::to_string(binding.library)},
                                 {"root", uuids::to_string(binding.root)},
                                 {"path", path_to_utf8(binding.path)}});
    return {{"version", Workspace::kVersion},
            {"libraries", library_array},
            {"root_bindings", binding_array},
            {"preferences", preferences}};
}

}  // namespace

std::filesystem::path Workspace::default_directory() {
    if (auto overridden = environment_path("MSO_WORKSPACE_DIR"); !overridden.empty()) return overridden;
#ifdef _WIN32
    if (auto local = environment_path("LOCALAPPDATA"); !local.empty()) return local / "MusicSampleOrganizer";
    if (auto profile = environment_path("USERPROFILE"); !profile.empty())
        return profile / "AppData" / "Local" / "MusicSampleOrganizer";
#else
    if (auto data = environment_path("XDG_DATA_HOME"); !data.empty()) return data / "music-sample-organizer";
    if (auto home = environment_path("HOME"); !home.empty())
        return home / ".local" / "share" / "music-sample-organizer";
#endif
    return {};
}

Workspace::Workspace(Workspace&& other) noexcept
    : directory_(std::move(other.directory_)), document_(std::move(other.document_)) {}

Result<Workspace::FileState> Workspace::read_file(Document& document) const {
    const auto file = directory_ / kFileName;
    std::error_code error;
    const bool present = std::filesystem::exists(file, error);
    if (error) return fail(ErrorCode::AccessDenied, std::format("cannot read the workspace file: {}", error.message()));
    if (!present) return FileState::Absent;

    std::ifstream in(file, std::ios::binary);
    if (!in) return fail(ErrorCode::AccessDenied, "cannot open the workspace file");
    std::stringstream contents;
    contents << in.rdbuf();
    const auto parsed = nlohmann::json::parse(contents.str(), nullptr, false);
    if (parsed.is_discarded() || !parsed.is_object()) return FileState::Unreadable;
    const auto version = integer_field(parsed, "version");
    if (!version) return FileState::Unreadable;
    if (*version > kVersion) return FileState::Newer;

    Document read;
    if (const auto* libraries = array_field(parsed, "libraries")) {
        for (const auto& entry : *libraries) {
            const auto uuid = uuids::uuid::from_string(string_field(entry, "uuid").value_or(""));
            const auto directory = string_field(entry, "directory");
            if (!uuid || !directory) continue;  // wrong shape: skip the entry, keep the rest
            read.libraries.push_back(KnownLibrary{*uuid, string_field(entry, "name").value_or(""),
                                                  path_from_utf8(*directory),
                                                  boolean_field(entry, "open").value_or(false)});
        }
    }
    if (const auto* bindings = array_field(parsed, "root_bindings")) {
        for (const auto& entry : *bindings) {
            const auto library = uuids::uuid::from_string(string_field(entry, "library").value_or(""));
            const auto root = uuids::uuid::from_string(string_field(entry, "root").value_or(""));
            const auto path = string_field(entry, "path");
            if (!library || !root || !path) continue;
            read.bindings.push_back(RootBinding{*library, *root, path_from_utf8(*path)});
        }
    }
    if (const auto* preferences = object_field(parsed, "preferences")) read.preferences = *preferences;
    document = std::move(read);
    return FileState::Readable;
}

Result<Workspace> Workspace::load(const std::filesystem::path& directory) {
    if (directory.empty())
        return fail(ErrorCode::InvalidArgument,
                    "no per-user data folder could be determined; set MSO_WORKSPACE_DIR or pass a workspace folder");
    std::error_code error;
    std::filesystem::create_directories(directory, error);
    if (error) return fail(ErrorCode::AccessDenied, std::format("cannot create workspace folder: {}", error.message()));
    Workspace workspace(directory);

    auto lock = FileLock::acquire(directory / kLockFileName, kLockTimeout);
    if (!lock) return std::unexpected(lock.error());
    auto state = workspace.read_file(workspace.document_);
    if (!state) return std::unexpected(state.error());
    if (*state == FileState::Unreadable || *state == FileState::Newer) {
        // Never block start-up on a damaged or newer workspace file: keep it aside, start fresh.
        // The renamed copy keeps every byte, so nothing written by a newer version is lost.
        std::filesystem::rename(directory / kFileName,
                                directory / std::format("workspace.unreadable-{}.json", now_unix_ms()), error);
        if (error)
            return fail(ErrorCode::AccessDenied,
                        std::format("cannot set aside the unreadable workspace file: {}", error.message()));
    }
    return workspace;
}

Result<void> Workspace::update(const std::function<bool(Document&)>& change) {
    std::lock_guard lock(mutex_);
    auto file_lock = FileLock::acquire(directory_ / kLockFileName, kLockTimeout);
    if (!file_lock) return std::unexpected(file_lock.error());

    // Merge: apply this one change to whatever is on disk now (another process may have written).
    Document latest = document_;
    auto state = read_file(latest);
    if (!state) return std::unexpected(state.error());
    if (*state == FileState::Newer)
        return fail(ErrorCode::SchemaTooNew, "workspace.json was rewritten by a newer version of the application");
    if (!change(latest)) {
        document_ = std::move(latest);
        return {};
    }
    const std::string text = dump_json(serialize(latest.libraries, latest.bindings, latest.preferences), 2);
    // Uniquely named temporary sibling, flushed to stable storage, renamed over the file, directory
    // entry flushed (POSIX): a crash leaves either the old or the new file, never a torn one.
    if (auto written = catalog::write_file_atomically(directory_ / kFileName, text); !written)
        return std::unexpected(written.error());
    document_ = std::move(latest);
    return {};
}

std::vector<KnownLibrary> Workspace::known_libraries() const {
    std::lock_guard lock(mutex_);
    return document_.libraries;
}

Result<void> Workspace::remember_library(const KnownLibrary& library) {
    return update([&](Document& document) {
        std::erase_if(document.libraries, [&](const KnownLibrary& known) { return known.uuid == library.uuid; });
        document.libraries.push_back(library);
        return true;
    });
}

Result<void> Workspace::set_library_open(const uuids::uuid& library, bool open) {
    return update([&](Document& document) {
        bool changed = false;
        for (auto& known : document.libraries)
            if (known.uuid == library && known.open != open) {
                known.open = open;
                changed = true;
            }
        return changed;
    });
}

std::optional<std::filesystem::path> Workspace::root_binding(const uuids::uuid& library,
                                                             const uuids::uuid& root) const {
    std::lock_guard lock(mutex_);
    for (const auto& binding : document_.bindings)
        if (binding.library == library && binding.root == root) return binding.path;
    return std::nullopt;
}

std::vector<RootBinding> Workspace::root_bindings(const uuids::uuid& library) const {
    std::lock_guard lock(mutex_);
    std::vector<RootBinding> bindings;
    for (const auto& binding : document_.bindings)
        if (binding.library == library) bindings.push_back(binding);
    return bindings;
}

Result<void> Workspace::bind_root(const RootBinding& binding) {
    return update([&](Document& document) {
        std::erase_if(document.bindings, [&](const RootBinding& existing) {
            return existing.library == binding.library && existing.root == binding.root;
        });
        document.bindings.push_back(binding);
        return true;
    });
}

nlohmann::json Workspace::preference(std::string_view key) const {
    std::lock_guard lock(mutex_);
    const auto found = document_.preferences.find(std::string(key));
    return found == document_.preferences.end() ? nlohmann::json() : *found;
}

Result<void> Workspace::set_preference(std::string_view key, nlohmann::json value) {
    return update([&](Document& document) {
        document.preferences[std::string(key)] = value;
        return true;
    });
}

}  // namespace mso::app
