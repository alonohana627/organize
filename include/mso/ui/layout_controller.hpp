#pragma once
/// @file
/// LayoutController: the window layout commands of the View menu — reset, the layout restored at
/// start-up and saved at exit, and named layout presets.
#include <QByteArray>
#include <QObject>
#include <QString>
#include <QStringList>

#include <nlohmann/json.hpp>

#include "mso/ui/layout_preset_store.hpp"

class QHeaderView;
class QMainWindow;
class QMenu;

namespace mso::ui {

class ApplicationSession;

/// Owns the layout entries of the View menu. Layouts are captured and applied with LayoutStore;
/// presets live in the session's preferences (LayoutPresetStore). After any layout is applied,
/// headerRestored() lets the window re-query with the sort the header now shows.
class LayoutController final : public QObject {
    Q_OBJECT

public:
    /// Lays out `window` and its sample table `header`; all three must outlive the controller.
    LayoutController(ApplicationSession& session, QMainWindow& window, QHeaderView& header);

    /// Remembers the current layout as the one Reset Layout returns to (call once the window is
    /// built, before a saved layout is applied).
    void capture_default();
    /// Adds Reset Layout, Save Layout Preset, Layout Presets and Delete Layout Preset to `menu`.
    void add_menu_entries(QMenu& menu);
    /// Applies the layout saved when the application last closed.
    void restore_saved();
    /// Saves the current layout for the next start.
    void save_current();
    /// Applies a layout; false when it was missing, foreign or not fully applied.
    [[nodiscard]] bool restore(const nlohmann::json& layout);
    /// Returns to the default layout and shows every dock.
    void reset();
    /// Asks for a name and saves the current layout as a preset.
    void save_preset();
    /// Saves the current layout as the preset `name` (replacing one of that name).
    void save_preset_as(const QString& name);
    /// Applies the preset `name`.
    void apply_preset(const QString& name);
    /// Deletes the preset `name`.
    void delete_preset(const QString& name);
    /// Preset names in alphabetical order.
    QStringList preset_names() const { return presets_.names(); }

signals:
    /// A layout was applied: the header's sort indicator may show another sort.
    void headerRestored();
    /// One-line feedback for the status bar.
    void statusMessage(const QString& message);

private:
    /// Lists the presets in the two preset submenus.
    void rebuild_preset_menus();

    /// Keeps the current layout in its preferences.
    ApplicationSession& session_;
    /// The window whose docks and geometry are laid out.
    QMainWindow& window_;
    /// The sample table's header (column order, widths, visibility, sort).
    QHeaderView& header_;
    /// Named layouts.
    LayoutPresetStore presets_;
    /// Dock layout Reset Layout returns to.
    QByteArray default_state_;
    /// Table header Reset Layout returns to.
    QByteArray default_header_state_;
    /// "Layout Presets" submenu (owned by the View menu), null until add_menu_entries().
    QMenu* presets_menu_ = nullptr;
    /// "Delete Layout Preset" submenu (owned by the View menu), null until add_menu_entries().
    QMenu* delete_presets_menu_ = nullptr;
};

}  // namespace mso::ui
