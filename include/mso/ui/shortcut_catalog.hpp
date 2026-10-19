#pragma once
/// @file
/// ShortcutCatalog: the customizable keyboard commands, their defaults, persistence and conflict
/// detection (requirement R17). Bindings are stored in the workspace preference "ui.shortcuts".
#include <QCoreApplication>
#include <QKeySequence>
#include <QString>

#include <map>
#include <vector>

#include <nlohmann/json.hpp>

namespace mso::ui {

/// One customizable command.
struct ShortcutDefinition {
    QString id;                     ///< stable identifier stored in preferences, e.g. "play_pause"
    QString label;                  ///< translated description shown in Preferences
    QKeySequence default_sequence;  ///< factory binding (may be empty)
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
    static constexpr const char* kPreferenceKey = "ui.shortcuts";

    /// Every customizable command, in the order Preferences lists them.
    static const std::vector<ShortcutDefinition>& definitions();
    static ShortcutBindings defaults();
    /// Reads persisted bindings; unknown ids are ignored and missing or malformed entries keep
    /// their defaults, so a damaged preference never leaves a command unreachable by accident.
    static ShortcutBindings from_json(const nlohmann::json& stored);
    static nlohmann::json to_json(const ShortcutBindings& bindings);
    /// Groups of two or more action ids bound to the same non-empty sequence.
    static std::vector<std::vector<QString>> conflicts(const ShortcutBindings& bindings);
    /// Translated label of an action id (the id itself when unknown).
    static QString label(const QString& id);
};

}  // namespace mso::ui
