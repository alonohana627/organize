#include "mso/ui/shortcut_catalog.hpp"

#include <string>

#include "mso/domain/json_fields.hpp"
#include "mso/ui/display_format.hpp"

namespace mso::ui {

namespace {

/// The platform's binding for a standard action (Ctrl+Y for Redo on Windows, Ctrl+Shift+Z
/// elsewhere …), or `fallback` where the platform defines none (Preferences, Quit on Windows).
QKeySequence standard_or(QKeySequence::StandardKey key, const QKeySequence& fallback) {
    const QKeySequence standard(key);
    return standard.isEmpty() ? fallback : standard;
}

}  // namespace

std::vector<ShortcutDefinition> ShortcutCatalog::definitions() {
    using enum ShortcutScope;
    return {
        {QStringLiteral("play_pause"), tr("Play / pause preview"), QKeySequence(Qt::Key_Space), SampleTable},
        {QStringLiteral("stop"), tr("Stop preview"), QKeySequence(Qt::Key_Escape), SampleTable},
        {QStringLiteral("undo"), tr("Undo"), QKeySequence(QKeySequence::Undo), Window},
        {QStringLiteral("redo"), tr("Redo"), QKeySequence(QKeySequence::Redo), Window},
        {QStringLiteral("focus_search"), tr("Focus search"), QKeySequence(QKeySequence::Find), Window},
        {QStringLiteral("remove_from_library"), tr("Remove from library"), QKeySequence(QKeySequence::Delete),
         SampleTable},
        {QStringLiteral("rating_0"), tr("Clear rating"), QKeySequence(Qt::Key_0), SampleTable},
        {QStringLiteral("rating_1"), tr("Rate 1 star"), QKeySequence(Qt::Key_1), SampleTable},
        {QStringLiteral("rating_2"), tr("Rate 2 stars"), QKeySequence(Qt::Key_2), SampleTable},
        {QStringLiteral("rating_3"), tr("Rate 3 stars"), QKeySequence(Qt::Key_3), SampleTable},
        {QStringLiteral("rating_4"), tr("Rate 4 stars"), QKeySequence(Qt::Key_4), SampleTable},
        {QStringLiteral("rating_5"), tr("Rate 5 stars"), QKeySequence(Qt::Key_5), SampleTable},
        {QStringLiteral("toggle_favorite"), tr("Toggle favorite"), QKeySequence(Qt::Key_F), SampleTable},
        {QStringLiteral("new_library"), tr("New library"), QKeySequence(Qt::CTRL | Qt::SHIFT | Qt::Key_N), Window},
        {QStringLiteral("open_library"), tr("Open library"), QKeySequence(QKeySequence::Open), Window},
        {QStringLiteral("add_folder"), tr("Add folder"), QKeySequence(Qt::CTRL | Qt::SHIFT | Qt::Key_O), Window},
        {QStringLiteral("preferences"), tr("Preferences"),
         standard_or(QKeySequence::Preferences, QKeySequence(Qt::CTRL | Qt::Key_Comma)), Window},
        {QStringLiteral("quit"), tr("Quit"), standard_or(QKeySequence::Quit, QKeySequence(Qt::CTRL | Qt::Key_Q)),
         Window},
    };
}

ShortcutBindings ShortcutCatalog::defaults() {
    ShortcutBindings bindings;
    for (const auto& definition : definitions())
        bindings[definition.id] = definition.default_sequence;
    return bindings;
}

ShortcutBindings ShortcutCatalog::from_json(const nlohmann::json& stored) {
    ShortcutBindings bindings = defaults();
    if (json_fields::integer_field(stored, "version") != kSchemaVersion) return bindings;
    const nlohmann::json* entries = json_fields::object_field(stored, "bindings");
    if (!entries) return bindings;
    for (const auto& [id, value] : entries->items()) {
        const QString action = DisplayFormat::from_utf8(id);
        if (!bindings.contains(action) || !value.is_string()) continue;
        const QString text = DisplayFormat::from_utf8(value.get<std::string>());
        const QKeySequence sequence = QKeySequence::fromString(text, QKeySequence::PortableText);
        // An unparsable non-empty string is damage, not an intentional "unbound".
        if (!text.isEmpty() && sequence.isEmpty()) continue;
        bindings[action] = sequence;
    }
    return bindings;
}

nlohmann::json ShortcutCatalog::to_json(const ShortcutBindings& bindings) {
    nlohmann::json entries = nlohmann::json::object();
    for (const auto& [id, sequence] : bindings)
        entries[DisplayFormat::to_utf8(id)] = DisplayFormat::to_utf8(sequence.toString(QKeySequence::PortableText));
    return nlohmann::json{{"version", kSchemaVersion}, {"bindings", std::move(entries)}};
}

std::vector<std::vector<QString>> ShortcutCatalog::conflicts(const ShortcutBindings& bindings) {
    std::map<QString, std::vector<QString>> actions_by_sequence;
    for (const auto& [id, sequence] : bindings)
        if (!sequence.isEmpty()) actions_by_sequence[sequence.toString(QKeySequence::PortableText)].push_back(id);
    std::vector<std::vector<QString>> groups;
    for (auto& [sequence, actions] : actions_by_sequence)
        if (actions.size() > 1) groups.push_back(std::move(actions));
    return groups;
}

ShortcutScope ShortcutCatalog::scope(const QString& id) {
    for (const auto& definition : definitions())
        if (definition.id == id) return definition.scope;
    return ShortcutScope::Window;
}

bool ShortcutCatalog::is_single_key(const QKeySequence& sequence) {
    if (sequence.count() != 1) return false;
    return (sequence[0].keyboardModifiers() & (Qt::ControlModifier | Qt::AltModifier | Qt::MetaModifier)) == 0;
}

QString ShortcutCatalog::label(const QString& id) {
    for (const auto& definition : definitions())
        if (definition.id == id) return definition.label;
    return id;
}

}  // namespace mso::ui
