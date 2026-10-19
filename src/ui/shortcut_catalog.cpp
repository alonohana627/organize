#include "mso/ui/shortcut_catalog.hpp"

#include <string>

#include "mso/ui/display_format.hpp"

namespace mso::ui {

const std::vector<ShortcutDefinition>& ShortcutCatalog::definitions() {
    static const std::vector<ShortcutDefinition> kDefinitions = {
        {QStringLiteral("play_pause"), tr("Play / pause preview"), QKeySequence(Qt::Key_Space)},
        {QStringLiteral("stop"), tr("Stop preview"), QKeySequence(Qt::Key_Escape)},
        {QStringLiteral("undo"), tr("Undo"), QKeySequence(Qt::CTRL | Qt::Key_Z)},
        {QStringLiteral("redo"), tr("Redo"), QKeySequence(Qt::CTRL | Qt::SHIFT | Qt::Key_Z)},
        {QStringLiteral("focus_search"), tr("Focus search"), QKeySequence(Qt::CTRL | Qt::Key_F)},
        {QStringLiteral("remove_from_library"), tr("Remove from library"), QKeySequence(Qt::Key_Delete)},
        {QStringLiteral("rating_0"), tr("Clear rating"), QKeySequence(Qt::Key_0)},
        {QStringLiteral("rating_1"), tr("Rate 1 star"), QKeySequence(Qt::Key_1)},
        {QStringLiteral("rating_2"), tr("Rate 2 stars"), QKeySequence(Qt::Key_2)},
        {QStringLiteral("rating_3"), tr("Rate 3 stars"), QKeySequence(Qt::Key_3)},
        {QStringLiteral("rating_4"), tr("Rate 4 stars"), QKeySequence(Qt::Key_4)},
        {QStringLiteral("rating_5"), tr("Rate 5 stars"), QKeySequence(Qt::Key_5)},
        {QStringLiteral("toggle_favorite"), tr("Toggle favorite"), QKeySequence(Qt::Key_F)},
        {QStringLiteral("new_library"), tr("New library"), QKeySequence(Qt::CTRL | Qt::SHIFT | Qt::Key_N)},
        {QStringLiteral("open_library"), tr("Open library"), QKeySequence(Qt::CTRL | Qt::Key_O)},
        {QStringLiteral("add_folder"), tr("Add folder"), QKeySequence(Qt::CTRL | Qt::SHIFT | Qt::Key_O)},
        {QStringLiteral("preferences"), tr("Preferences"), QKeySequence(Qt::CTRL | Qt::Key_Comma)},
        {QStringLiteral("quit"), tr("Quit"), QKeySequence(Qt::CTRL | Qt::Key_Q)},
    };
    return kDefinitions;
}

ShortcutBindings ShortcutCatalog::defaults() {
    ShortcutBindings bindings;
    for (const auto& definition : definitions())
        bindings[definition.id] = definition.default_sequence;
    return bindings;
}

ShortcutBindings ShortcutCatalog::from_json(const nlohmann::json& stored) {
    ShortcutBindings bindings = defaults();
    if (!stored.is_object() || stored.value("version", 0) != kSchemaVersion) return bindings;
    const auto entries = stored.find("bindings");
    if (entries == stored.end() || !entries->is_object()) return bindings;
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

QString ShortcutCatalog::label(const QString& id) {
    for (const auto& definition : definitions())
        if (definition.id == id) return definition.label;
    return id;
}

}  // namespace mso::ui
