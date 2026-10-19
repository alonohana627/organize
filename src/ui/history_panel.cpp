#include "mso/ui/history_panel.hpp"

#include <QHBoxLayout>
#include <QLabel>
#include <QListWidget>
#include <QPushButton>
#include <QVBoxLayout>

#include <vector>

#include "mso/ui/application_session.hpp"
#include "mso/ui/display_format.hpp"

namespace mso::ui {

namespace {

QString state_label(const std::string& state) {
    if (state == "applied") return HistoryPanel::tr("applied");
    if (state == "undone") return HistoryPanel::tr("undone");
    if (state == "partial") return HistoryPanel::tr("partially applied");
    if (state == "superseded") return HistoryPanel::tr("superseded");
    if (state == "conflict") return HistoryPanel::tr("conflict — see review");
    return DisplayFormat::from_utf8(state);
}

}  // namespace

HistoryPanel::HistoryPanel(ApplicationSession& session, QWidget* parent) : QWidget(parent), session_(session) {
    title_label_ = new QLabel(tr("No library selected"), this);
    title_label_->setWordWrap(true);
    list_ = new QListWidget(this);
    list_->setAccessibleName(tr("Edit history"));
    undo_button_ = new QPushButton(tr("Undo"), this);
    redo_button_ = new QPushButton(tr("Redo"), this);
    connect(undo_button_, &QPushButton::clicked, this, &HistoryPanel::undoRequested);
    connect(redo_button_, &QPushButton::clicked, this, &HistoryPanel::redoRequested);

    auto* buttons = new QHBoxLayout;
    buttons->addWidget(undo_button_);
    buttons->addWidget(redo_button_);
    buttons->addStretch(1);
    auto* layout = new QVBoxLayout(this);
    layout->addWidget(title_label_);
    layout->addWidget(list_, 1);
    layout->addLayout(buttons);
    show_library(std::nullopt);
}

void HistoryPanel::show_library(const std::optional<uuids::uuid>& library) {
    const bool changed = library != library_;
    library_ = library;
    if (changed || !library) reload();
}

void HistoryPanel::reload() {
    const std::uint64_t request = ++request_;
    const bool has_library = library_ && session_.is_ready();
    undo_button_->setEnabled(has_library);
    redo_button_->setEnabled(has_library);
    if (!has_library) {
        list_->clear();
        title_label_->setText(tr("No library selected"));
        return;
    }
    // The previous entries stay visible until the new ones arrive (no flicker on every edit).
    title_label_->setText(tr("History of %1 (kept across restarts)").arg(session_.library_name(*library_)));
    session_.run_with_library(
        this, *library_, [](catalog::Library& library) { return library.history(kVisibleEntries); },
        [this, request](Result<std::vector<catalog::HistoryEntry>> entries) {
            if (request != request_) return;
            list_->clear();
            if (!entries) {
                list_->addItem(tr("History unavailable: %1").arg(DisplayFormat::error(entries.error())));
                return;
            }
            if (entries->empty()) list_->addItem(tr("No edits yet"));
            for (const auto& entry : *entries) {
                QString text = tr("%1 — %2 (%3)")
                                   .arg(DisplayFormat::from_utf8(entry.label),
                                        DisplayFormat::timestamp(entry.created_at), state_label(entry.state));
                if (entry.is_cursor) text = tr("▶ %1   ← next Undo").arg(text);
                auto* item = new QListWidgetItem(text, list_);
                if (entry.is_cursor) {
                    QFont font = item->font();
                    font.setBold(true);
                    item->setFont(font);
                }
            }
        });
}

}  // namespace mso::ui
