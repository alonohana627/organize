#include "mso/ui/history_panel.hpp"

#include <QHBoxLayout>
#include <QLabel>
#include <QListWidget>
#include <QPushButton>
#include <QScrollBar>
#include <QShowEvent>
#include <QVBoxLayout>

#include <algorithm>
#include <vector>

#include "mso/ui/application_session.hpp"
#include "mso/ui/display_format.hpp"

namespace mso::ui {

namespace {

/// Item role holding the history entry id.
constexpr int kEntryIdRole = Qt::UserRole + 1;

QString state_label(const std::string& state) {
    if (state == "applied") return HistoryPanel::tr("applied");
    if (state == "undone") return HistoryPanel::tr("undone");
    if (state == "partial") return HistoryPanel::tr("partially applied");
    if (state == "superseded") return HistoryPanel::tr("superseded");
    if (state == "conflict") return HistoryPanel::tr("conflict — see review");
    return DisplayFormat::from_utf8(state);
}

/// True when two entry lists show the same thing.
bool same_entries(const std::vector<catalog::HistoryEntry>& left, const std::vector<catalog::HistoryEntry>& right) {
    return std::ranges::equal(left, right, [](const catalog::HistoryEntry& a, const catalog::HistoryEntry& b) {
        return a.id == b.id && a.label == b.label && a.state == b.state && a.created_at == b.created_at &&
               a.is_cursor == b.is_cursor;
    });
}

}  // namespace

HistoryPanel::HistoryPanel(ApplicationSession& session, QWidget* parent) : QWidget(parent), session_(session) {
    title_label_ = new QLabel(tr("No library selected"), this);
    title_label_->setWordWrap(true);
    title_label_->setTextFormat(Qt::PlainText);  // library names are user text
    list_ = new QListWidget(this);
    list_->setAccessibleName(tr("Edit history"));
    undo_button_ = new QPushButton(tr("Undo"), this);
    redo_button_ = new QPushButton(tr("Redo"), this);
    connect(undo_button_, &QPushButton::clicked, this, &HistoryPanel::undoRequested);
    connect(redo_button_, &QPushButton::clicked, this, &HistoryPanel::redoRequested);
    background_reload_timer_.setSingleShot(true);
    background_reload_timer_.setInterval(kBackgroundReloadMilliseconds);
    connect(&background_reload_timer_, &QTimer::timeout, this, &HistoryPanel::reload);

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
    if (changed) entries_.clear();  // another library's entries are not comparable
    if (changed || !library) reload();
}

void HistoryPanel::catalog_changed(const uuids::uuid& library) {
    if (library != library_) return;
    if (!isVisible()) {
        stale_ = true;  // read when the panel is shown (it is often a tab behind the Inspector)
        return;
    }
    if (!background_reload_timer_.isActive()) background_reload_timer_.start();
}

void HistoryPanel::showEvent(QShowEvent* event) {
    QWidget::showEvent(event);
    if (stale_) reload();
}

void HistoryPanel::reload() {
    stale_ = false;
    background_reload_timer_.stop();
    const std::uint64_t request = ++request_;
    if (!library_ || !session_.is_ready()) {
        entries_.clear();
        list_->clear();
        title_label_->setText(tr("No library selected"));
        update_buttons();
        return;
    }
    // The previous entries stay visible until the new ones arrive (no flicker on every edit).
    title_label_->setText(tr("History of %1 (kept across restarts)").arg(session_.library_name(*library_)));
    ++reads_started_;
    session_.run_with_library(
        this, *library_, [](catalog::Library& library) { return library.history(kVisibleEntries); },
        [this, request](Result<std::vector<catalog::HistoryEntry>> entries) {
            if (request != request_) return;
            if (!entries) {
                entries_.clear();
                list_->clear();
                list_->addItem(tr("History unavailable: %1").arg(DisplayFormat::error(entries.error())));
                update_buttons();
                return;
            }
            show_entries(*entries);
        });
}

void HistoryPanel::show_entries(const std::vector<catalog::HistoryEntry>& entries) {
    if (!entries_.empty() && same_entries(entries, entries_)) return;  // nothing changed: keep scroll and focus
    entries_ = entries;
    const int scroll = list_->verticalScrollBar()->value();
    const QVariant current_id = list_->currentItem() ? list_->currentItem()->data(kEntryIdRole) : QVariant();
    list_->clear();
    if (entries_.empty()) list_->addItem(tr("No edits yet"));
    for (const auto& entry : entries_) {
        QString text = tr("%1 — %2 (%3)")
                           .arg(DisplayFormat::from_utf8(entry.label), DisplayFormat::timestamp(entry.created_at),
                                state_label(entry.state));
        if (entry.is_cursor) text = tr("▶ %1   ← next Undo").arg(text);
        auto* item = new QListWidgetItem(text, list_);
        item->setData(kEntryIdRole, static_cast<qlonglong>(entry.id));
        if (entry.is_cursor) {
            QFont font = item->font();
            font.setBold(true);
            item->setFont(font);
        }
        if (current_id.isValid() && current_id.toLongLong() == entry.id) list_->setCurrentItem(item);
    }
    list_->verticalScrollBar()->setValue(scroll);
    update_buttons();
}

void HistoryPanel::update_buttons() {
    // Undo reverts the cursor entry; Redo re-applies an undone entry newer than the cursor.
    const auto cursor = std::ranges::find_if(entries_, &catalog::HistoryEntry::is_cursor);
    const bool undo_available = cursor != entries_.end();
    const bool redo_available = std::any_of(entries_.begin(), cursor,
                                            [](const catalog::HistoryEntry& entry) { return entry.state == "undone"; });
    undo_button_->setEnabled(undo_available);
    redo_button_->setEnabled(redo_available);
}

}  // namespace mso::ui
