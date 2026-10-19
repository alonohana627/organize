#include "mso/ui/layout_store.hpp"

#include <QByteArray>
#include <QDockWidget>
#include <QGuiApplication>
#include <QHeaderView>
#include <QMainWindow>
#include <QScreen>

#include <string>

namespace mso::ui {

namespace {

std::string encode(const QByteArray& bytes) {
    return bytes.toBase64().toStdString();
}

QByteArray decode(const nlohmann::json& value) {
    if (!value.is_string()) return {};
    const std::string text = value.get<std::string>();
    return QByteArray::fromBase64(QByteArray(text.data(), static_cast<qsizetype>(text.size())));
}

}  // namespace

nlohmann::json LayoutStore::capture(const QMainWindow& window, const QHeaderView& header) {
    return nlohmann::json{{"version", kSchemaVersion},
                          {"geometry", encode(window.saveGeometry())},
                          {"state", encode(window.saveState(kSchemaVersion))},
                          {"header", encode(header.saveState())}};
}

bool LayoutStore::restore(QMainWindow& window, QHeaderView& header, const nlohmann::json& layout) {
    if (!layout.is_object() || layout.value("version", 0) != kSchemaVersion) return false;
    const QByteArray geometry = decode(layout.value("geometry", nlohmann::json()));
    const QByteArray state = decode(layout.value("state", nlohmann::json()));
    const QByteArray header_state = decode(layout.value("header", nlohmann::json()));
    if (!geometry.isEmpty()) window.restoreGeometry(geometry);
    if (!state.isEmpty()) window.restoreState(state, kSchemaVersion);
    if (!header_state.isEmpty()) header.restoreState(header_state);
    clamp_to_screens(window);
    for (QDockWidget* dock : window.findChildren<QDockWidget*>())
        if (dock->isFloating()) clamp_to_screens(*dock);
    return true;
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
