#pragma once
/// @file
/// ShortcutCatalog: the customizable keyboard commands, their defaults, persistence and conflict
/// detection (requirement R17). Bindings are stored in the workspace preference "ui.shortcuts".
#include <QCoreApplication>
#include <QKeySequence>
#include <QString>

#include <map>
#include <string_view>
#include <vector>

#include <nlohmann/json.hpp>

#include "mso/ui/ui_preferences.hpp"

namespace mso::ui {

/// Where a shortcut is active.
enum class ShortcutScope {
    /// Only while the sample table (or a child) has focus: single keys like Space, Delete, F and
    /// 0–5 must not act on the table's selection while the user types or works in another panel.
    SampleTable,
    /// Anywhere in the main window: commands with modifiers (Ctrl+Z, Ctrl+F, …).
    Window,
};

/// One customizable command.
struct ShortcutDefinition {
    QString id;                                   ///< stable identifier stored in preferences, e.g. "play_pause"
    QString label;                                ///< translated description shown in Preferences
    QKeySequence default_sequence;                ///< factory binding (may be empty)
    ShortcutScope scope = ShortcutScope::Window;  ///< where the command listens
};

/// Action id → key sequence. An empty sequence means "unbound".
using ShortcutBindings = std::map<QString, QKeySequence>;

/// Registry of shortcut definitions plus conversion to and from the persisted JSON form.
class ShortcutCatalog {
    Q_DECLARE_TR_FUNCTIONS(ShortcutCatalog)

public:
    /// Version of the persisted object; other versions fall back to defaults.
    static constexpr int kSchemaVersion = 1;
    /// Workspace preference key.
    static constexpr std::string_view kPreferenceKey = UiPreferenceKeys::kShortcuts;

    /// Every customizable command, in the order Preferences lists them. Built on each call, so the
    /// labels follow the translation installed now (a static table would freeze the first one).
    static std::vector<ShortcutDefinition> definitions();
    /// The factory binding of every command.
    static ShortcutBindings defaults();
    /// Reads persisted bindings; unknown ids are ignored and missing or malformed entries keep
    /// their defaults, so a damaged preference never leaves a command unreachable by accident.
    static ShortcutBindings from_json(const nlohmann::json& stored);
    /// Persisted form of `bindings` (versioned, portable key text).
    static nlohmann::json to_json(const ShortcutBindings& bindings);
    /// Groups of two or more action ids bound to the same non-empty sequence.
    static std::vector<std::vector<QString>> conflicts(const ShortcutBindings& bindings);
    /// Translated label of an action id (the id itself when unknown).
    static QString label(const QString& id);
    /// Scope of an action id (Window when unknown).
    static ShortcutScope scope(const QString& id);
    /// True for one key pressed without Ctrl, Alt or Meta (Shift and keypad keys count as single
    /// keys): such a shortcut would fire while the user types in a list or tree, so it must be
    /// scoped to the sample table whatever its command.
    static bool is_single_key(const QKeySequence& sequence);
};

}  // namespace mso::ui
