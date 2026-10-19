#pragma once
/// @file
/// LayoutStore: captures and restores the main window layout (geometry, docks, table header) as
/// versioned JSON for the workspace preferences, including named layout presets (R13).
#include <QCoreApplication>

#include <string_view>

#include <nlohmann/json.hpp>

#include "mso/ui/ui_preferences.hpp"

class QHeaderView;
class QMainWindow;

namespace mso::ui {

/// Stateless conversions between widgets and persisted layout JSON.
class LayoutStore {
    Q_DECLARE_TR_FUNCTIONS(LayoutStore)

public:
    /// Version of persisted layout objects; other versions are ignored (safe defaults).
    static constexpr int kSchemaVersion = 1;
    /// Preference holding the layout that was active when the application last closed.
    static constexpr std::string_view kCurrentLayoutKey = UiPreferenceKeys::kCurrentLayout;
    /// Preference holding {preset name: layout}.
    static constexpr std::string_view kPresetsKey = UiPreferenceKeys::kLayoutPresets;

    /// Window geometry, dock/toolbar state and the table header (column order, widths,
    /// visibility and sort indicator).
    static nlohmann::json capture(const QMainWindow& window, const QHeaderView& header);
    /// Applies a captured layout. Returns false for missing or foreign data, or when Qt rejects a
    /// part of it (what could be applied stays applied). Windows and floating docks that would land
    /// off every current screen are moved back.
    [[nodiscard]] static bool restore(QMainWindow& window, QHeaderView& header, const nlohmann::json& layout);
    /// Moves a top-level window (or floating dock) fully onto the nearest available screen.
    static void clamp_to_screens(QWidget& window);
};

}  // namespace mso::ui
