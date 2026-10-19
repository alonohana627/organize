#include "mso/ui/layout_preset_store.hpp"

#include "mso/ui/application_session.hpp"
#include "mso/ui/display_format.hpp"
#include "mso/ui/ui_preferences.hpp"

namespace mso::ui {

LayoutPresetStore::LayoutPresetStore(ApplicationSession& session) : session_(session) {}

nlohmann::json LayoutPresetStore::presets() const {
    nlohmann::json stored = session_.preference(UiPreferenceKeys::kLayoutPresets);
    return stored.is_object() ? stored : nlohmann::json::object();
}

QStringList LayoutPresetStore::names() const {
    QStringList result;
    for (const auto& [name, layout] : presets().items())
        result << DisplayFormat::from_utf8(name);
    result.sort(Qt::CaseInsensitive);
    return result;
}

std::optional<nlohmann::json> LayoutPresetStore::layout(const QString& name) const {
    const nlohmann::json all = presets();
    const auto found = all.find(DisplayFormat::to_utf8(name));
    if (found == all.end()) return std::nullopt;
    return *found;
}

void LayoutPresetStore::save(const QString& name, nlohmann::json layout) {
    nlohmann::json all = presets();
    all[DisplayFormat::to_utf8(name)] = std::move(layout);
    session_.set_preference(UiPreferenceKeys::kLayoutPresets, std::move(all));
}

void LayoutPresetStore::remove(const QString& name) {
    nlohmann::json all = presets();
    all.erase(DisplayFormat::to_utf8(name));
    session_.set_preference(UiPreferenceKeys::kLayoutPresets, std::move(all));
}

}  // namespace mso::ui
