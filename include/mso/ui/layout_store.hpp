#pragma once
/// @file
/// LayoutStore: captures and restores the main window layout (geometry, docks, table header) as
/// versioned JSON for the workspace preferences, including named layout presets (R13).
#include <QCoreApplication>

#include <nlohmann/json.hpp>

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
    static constexpr const char* kCurrentLayoutKey = "ui.layout.current";
    /// Preference holding {preset name: layout}.
    static constexpr const char* kPresetsKey = "ui.layout.presets";

    /// Window geometry, dock/toolbar state and the table header (column order, widths,
    /// visibility and sort indicator).
    static nlohmann::json capture(const QMainWindow& window, const QHeaderView& header);
    /// Applies a captured layout. Returns false (changing nothing) for missing or foreign data.
    /// Windows and floating docks that would land off every current screen are moved back.
    static bool restore(QMainWindow& window, QHeaderView& header, const nlohmann::json& layout);
    /// Moves a top-level window (or floating dock) fully onto the nearest available screen.
    static void clamp_to_screens(QWidget& window);
};

}  // namespace mso::ui
