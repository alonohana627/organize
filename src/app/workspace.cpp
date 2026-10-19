#include "mso/app/workspace.hpp"

#include <cstdlib>
#include <format>
#include <fstream>
#include <sstream>

namespace mso::app {

namespace {

constexpr std::string_view kFileName = "workspace.json";

std::filesystem::path environment_path(const char* name) {
    const char* value = std::getenv(name);
    return value && *value ? path_from_utf8(value) : std::filesystem::path();
}

}  // namespace

std::filesystem::path Workspace::default_directory() {
    if (auto overridden = environment_path("MSO_WORKSPACE_DIR"); !overridden.empty()) return overridden;
#ifdef _WIN32
    if (auto local = environment_path("LOCALAPPDATA"); !local.empty()) return local / "MusicSampleOrganizer";
    return std::filesystem::temp_directory_path() / "MusicSampleOrganizer";
#else
    if (auto data = environment_path("XDG_DATA_HOME"); !data.empty()) return data / "music-sample-organizer";
    if (auto home = environment_path("HOME"); !home.empty())
        return home / ".local" / "share" / "music-sample-organizer";
    return std::filesystem::temp_directory_path() / "music-sample-organizer";
#endif
}

Workspace::Workspace(Workspace&& other) noexcept
    : directory_(std::move(other.directory_)),
      libraries_(std::move(other.libraries_)),
      bindings_(std::move(other.bindings_)),
      preferences_(std::move(other.preferences_)) {}

Result<Workspace> Workspace::load(const std::filesystem::path& directory) {
    std::error_code error;
    std::filesystem::create_directories(directory, error);
    if (error) return fail(ErrorCode::AccessDenied, std::format("cannot create workspace folder: {}", error.message()));
    Workspace workspace(directory);
    const auto file = directory / kFileName;
    if (!std::filesystem::exists(file, error)) return workspace;

    std::ifstream in(file, std::ios::binary);
    std::stringstream contents;
    contents << in.rdbuf();
    const auto document = nlohmann::json::parse(contents.str(), nullptr, false);
    if (document.is_discarded() || !document.is_object() || document.value("version", 0) > kVersion) {
        // Never block startup on a damaged or newer workspace file: keep it aside, start fresh.
        std::filesystem::rename(file, directory / std::format("workspace.unreadable-{}.json", now_unix_ms()), error);
        return workspace;
    }
    for (const auto& entry : document.value("libraries", nlohmann::json::array())) {
        auto uuid = uuids::uuid::from_string(entry.value("uuid", std::string()));
        if (!uuid) continue;
        workspace.libraries_.push_back(KnownLibrary{*uuid, entry.value("name", std::string()),
                                                    path_from_utf8(entry.value("directory", std::string())),
                                                    entry.value("open", false)});
    }
    for (const auto& entry : document.value("root_bindings", nlohmann::json::array())) {
        auto library = uuids::uuid::from_string(entry.value("library", std::string()));
        auto root = uuids::uuid::from_string(entry.value("root", std::string()));
        if (!library || !root) continue;
        workspace.bindings_.push_back(RootBinding{*library, *root, path_from_utf8(entry.value("path", std::string()))});
    }
    if (document.contains("preferences") && document["preferences"].is_object())
        workspace.preferences_ = document["preferences"];
    return workspace;
}

Result<void> Workspace::save_locked() const {
    nlohmann::json libraries = nlohmann::json::array();
    for (const auto& library : libraries_)
        libraries.push_back({{"uuid", uuids::to_string(library.uuid)},
                             {"name", library.name},
                             {"directory", path_to_utf8(library.directory)},
                             {"open", library.open}});
    nlohmann::json bindings = nlohmann::json::array();
    for (const auto& binding : bindings_)
        bindings.push_back({{"library", uuids::to_string(binding.library)},
                            {"root", uuids::to_string(binding.root)},
                            {"path", path_to_utf8(binding.path)}});
    const nlohmann::json document = {
        {"version", kVersion}, {"libraries", libraries}, {"root_bindings", bindings}, {"preferences", preferences_}};
    const auto file = directory_ / kFileName;
    auto temporary = file;
    temporary += ".tmp";
    {
        std::ofstream out(temporary, std::ios::binary | std::ios::trunc);
        out << document.dump(2);
        out.flush();
        if (!out) return fail(ErrorCode::DiskFull, "cannot write the workspace file");
    }
    std::error_code error;
    std::filesystem::rename(temporary, file, error);
    if (error)
        return fail(ErrorCode::AccessDenied, std::format("cannot replace the workspace file: {}", error.message()));
    return {};
}

std::vector<KnownLibrary> Workspace::known_libraries() const {
    std::lock_guard lock(mutex_);
    return libraries_;
}

Result<void> Workspace::remember_library(const KnownLibrary& library) {
    std::lock_guard lock(mutex_);
    std::erase_if(libraries_, [&](const KnownLibrary& known) { return known.uuid == library.uuid; });
    libraries_.push_back(library);
    return save_locked();
}

Result<void> Workspace::set_library_open(const uuids::uuid& library, bool open) {
    std::lock_guard lock(mutex_);
    for (auto& known : libraries_)
        if (known.uuid == library) known.open = open;
    return save_locked();
}

std::optional<std::filesystem::path> Workspace::root_binding(const uuids::uuid& library,
                                                             const uuids::uuid& root) const {
    std::lock_guard lock(mutex_);
    for (const auto& binding : bindings_)
        if (binding.library == library && binding.root == root) return binding.path;
    return std::nullopt;
}

Result<void> Workspace::bind_root(const RootBinding& binding) {
    std::lock_guard lock(mutex_);
    std::erase_if(bindings_, [&](const RootBinding& existing) {
        return existing.library == binding.library && existing.root == binding.root;
    });
    bindings_.push_back(binding);
    return save_locked();
}

nlohmann::json Workspace::preference(std::string_view key) const {
    std::lock_guard lock(mutex_);
    const auto found = preferences_.find(std::string(key));
    return found == preferences_.end() ? nlohmann::json() : *found;
}

Result<void> Workspace::set_preference(std::string_view key, nlohmann::json value) {
    std::lock_guard lock(mutex_);
    preferences_[std::string(key)] = std::move(value);
    return save_locked();
}

}  // namespace mso::app
