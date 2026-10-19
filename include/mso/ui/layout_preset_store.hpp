#pragma once
/// @file
/// LayoutPresetStore: named window layouts kept in the workspace preference "ui.layout.presets".
#include <QString>
#include <QStringList>

#include <optional>

#include <nlohmann/json.hpp>

namespace mso::ui {

class ApplicationSession;

/// Saves, lists, reads and deletes named layouts. Reads and writes go through the session's
/// preference cache, so a menu rebuilt right after a save already shows the new preset.
class LayoutPresetStore {
public:
    /// Presets of `session`, which must outlive the store.
    explicit LayoutPresetStore(ApplicationSession& session);

    /// Preset names in alphabetical order.
    QStringList names() const;
    /// The stored layout of `name`, or nullopt when there is no such preset.
    std::optional<nlohmann::json> layout(const QString& name) const;
    /// Stores (or replaces) a preset.
    void save(const QString& name, nlohmann::json layout);
    /// Deletes a preset; unknown names are ignored.
    void remove(const QString& name);

private:
    /// The presets object (empty object when unset or damaged).
    nlohmann::json presets() const;

    /// Holds the presets in its preference cache.
    ApplicationSession& session_;
};

}  // namespace mso::ui
