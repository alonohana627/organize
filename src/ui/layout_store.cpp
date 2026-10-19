#include "mso/ui/layout_store.hpp"

#include <QByteArray>
#include <QDockWidget>
#include <QGuiApplication>
#include <QHeaderView>
#include <QMainWindow>
#include <QScreen>

#include <string>
#include <string_view>

#include "mso/app/json_fields.hpp"

namespace mso::ui {

namespace {

std::string encode(const QByteArray& bytes) {
    return bytes.toBase64().toStdString();
}

/// Base64 field `key` of `layout`, or empty when absent or not a string.
QByteArray decode(const nlohmann::json& layout, std::string_view key) {
    const auto text = app::string_field(layout, key);
    if (!text) return {};
    return QByteArray::fromBase64(QByteArray(text->data(), static_cast<qsizetype>(text->size())));
}

}  // namespace

nlohmann::json LayoutStore::capture(const QMainWindow& window, const QHeaderView& header) {
    return nlohmann::json{{"version", kSchemaVersion},
                          {"geometry", encode(window.saveGeometry())},
                          {"state", encode(window.saveState(kSchemaVersion))},
                          {"header", encode(header.saveState())}};
}

bool LayoutStore::restore(QMainWindow& window, QHeaderView& header, const nlohmann::json& layout) {
    if (app::integer_field(layout, "version") != kSchemaVersion) return false;
    const QByteArray geometry = decode(layout, "geometry");
    const QByteArray state = decode(layout, "state");
    const QByteArray header_state = decode(layout, "header");
    bool restored = true;
    if (!geometry.isEmpty()) restored = window.restoreGeometry(geometry) && restored;
    if (!state.isEmpty()) restored = window.restoreState(state, kSchemaVersion) && restored;
    if (!header_state.isEmpty()) restored = header.restoreState(header_state) && restored;
    clamp_to_screens(window);
    for (QDockWidget* dock : window.findChildren<QDockWidget*>())
        if (dock->isFloating()) clamp_to_screens(*dock);
    return restored;
}

void LayoutStore::clamp_to_screens(QWidget& window) {
    const QRect frame = window.frameGeometry();
    for (const QScreen* screen : QGuiApplication::screens())
        if (screen->availableGeometry().intersects(frame)) return;  // at least partly visible
    QScreen* target = QGuiApplication::screenAt(frame.center());
    if (!target) target = QGuiApplication::primaryScreen();
    if (!target) return;
    const QRect available = target->availableGeometry();
    const QSize size = frame.size().boundedTo(available.size());
    window.resize(size - (frame.size() - window.size()));
    window.move(available.center() - QPoint(size.width() / 2, size.height() / 2));
}

}  // namespace mso::ui
