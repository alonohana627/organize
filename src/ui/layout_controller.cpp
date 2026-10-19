#include "mso/ui/layout_controller.hpp"

#include <QDockWidget>
#include <QHeaderView>
#include <QInputDialog>
#include <QMainWindow>
#include <QMenu>

#include "mso/ui/application_session.hpp"
#include "mso/ui/layout_store.hpp"

namespace mso::ui {

LayoutController::LayoutController(ApplicationSession& session, QMainWindow& window, QHeaderView& header)
    : QObject(&window), session_(session), window_(window), header_(header), presets_(session) {}

void LayoutController::capture_default() {
    default_state_ = window_.saveState(LayoutStore::kSchemaVersion);
    default_header_state_ = header_.saveState();
}

void LayoutController::add_menu_entries(QMenu& menu) {
    menu.addAction(tr("&Reset Layout"), this, &LayoutController::reset);
    menu.addAction(tr("&Save Layout Preset…"), this, qOverload<>(&LayoutController::save_preset));
    presets_menu_ = menu.addMenu(tr("Layout &Presets"));
    delete_presets_menu_ = menu.addMenu(tr("&Delete Layout Preset"));
    rebuild_preset_menus();
}

void LayoutController::restore_saved() {
    const bool restored = restore(session_.preference(LayoutStore::kCurrentLayoutKey));
    static_cast<void>(restored);  // a missing or foreign saved layout keeps the default: nothing to report
}

void LayoutController::save_current() {
    session_.set_preference(LayoutStore::kCurrentLayoutKey, LayoutStore::capture(window_, header_));
}

bool LayoutController::restore(const nlohmann::json& layout) {
    const bool restored = LayoutStore::restore(window_, header_, layout);
    emit headerRestored();  // whatever part of the header was applied
    return restored;
}

void LayoutController::reset() {
    // Both halves are restored even when the first fails (|| would skip the header).
    const bool docks_restored = window_.restoreState(default_state_, LayoutStore::kSchemaVersion);
    const bool header_restored = header_.restoreState(default_header_state_);
    if (!docks_restored || !header_restored) emit statusMessage(tr("The default layout could not be fully restored."));
    emit headerRestored();
    for (QDockWidget* dock : window_.findChildren<QDockWidget*>(Qt::FindDirectChildrenOnly))
        dock->show();
    emit statusMessage(tr("Layout reset"));
}

void LayoutController::save_preset() {
    bool accepted = false;
    const QString name =
        QInputDialog::getText(&window_, tr("Save layout preset"), tr("Preset name:"), QLineEdit::Normal, {}, &accepted)
            .trimmed();
    if (!accepted || name.isEmpty()) return;
    save_preset_as(name);
}

void LayoutController::save_preset_as(const QString& name) {
    presets_.save(name, LayoutStore::capture(window_, header_));
    rebuild_preset_menus();
    emit statusMessage(tr("Saved layout preset “%1”").arg(name));
}

void LayoutController::apply_preset(const QString& name) {
    const auto layout = presets_.layout(name);
    if (!layout || !restore(*layout))
        emit statusMessage(tr("This preset was saved by an incompatible version and was not fully applied."));
}

void LayoutController::delete_preset(const QString& name) {
    presets_.remove(name);
    rebuild_preset_menus();
}

void LayoutController::rebuild_preset_menus() {
    if (!presets_menu_ || !delete_presets_menu_) return;
    presets_menu_->clear();
    delete_presets_menu_->clear();
    const QStringList names = presets_.names();
    if (names.isEmpty()) {
        presets_menu_->addAction(tr("(none saved)"))->setEnabled(false);
        delete_presets_menu_->addAction(tr("(none saved)"))->setEnabled(false);
        return;
    }
    for (const QString& name : names) {
        presets_menu_->addAction(name, this, [this, name] { apply_preset(name); });
        delete_presets_menu_->addAction(name, this, [this, name] { delete_preset(name); });
    }
}

}  // namespace mso::ui
