#include "mso/ui/root_locations.hpp"

#include "mso/ui/application_session.hpp"

namespace mso::ui {

RootLocations::RootLocations(ApplicationSession& session, QObject* parent) : QObject(parent), session_(session) {}

void RootLocations::reload() {
    if (!session_.is_ready()) return;
    const std::uint64_t generation = ++generation_;
    session_.run(
        this,
        [](app::Application& application) {
            std::map<RootKey, std::filesystem::path> roots;
            for (const auto& info : application.libraries()) {
                auto records =
                    application.with_library(info.uuid, [](catalog::Library& library) { return library.list_roots(); });
                if (!records) continue;  // closed meanwhile or unreadable: its rows are simply not draggable
                for (const auto& root : *records) {
                    if (auto base = application.root_path(info.uuid, root))
                        roots.emplace(RootKey{info.uuid, root.id}, std::move(*base));
                }
            }
            return roots;
        },
        [this, generation](std::map<RootKey, std::filesystem::path> roots) {
            if (generation != generation_) return;
            roots_ = std::move(roots);
            emit reloaded();
        });
}

std::optional<std::filesystem::path> RootLocations::path_of(const search::SampleRow& row) const {
    if (row.relative_path.empty()) return std::nullopt;
    const auto found = roots_.find(RootKey{row.library, row.root_id});
    if (found == roots_.end()) return std::nullopt;
    return found->second / path_from_utf8(row.relative_path);
}

}  // namespace mso::ui
