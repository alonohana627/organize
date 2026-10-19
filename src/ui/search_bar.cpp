#include "mso/ui/search_bar.hpp"

#include <QAction>
#include <QLabel>
#include <QLineEdit>
#include <QStyle>
#include <QVBoxLayout>

#include <algorithm>
#include <cstdint>

#include "mso/search/query.hpp"
#include "mso/ui/display_format.hpp"

namespace mso::ui {

namespace {

/// Converts a code-point offset (what the parser reports) into a UTF-16 QString index.
int character_index_of_code_point(const QString& text, std::uint32_t code_point) {
    int index = 0;
    for (std::uint32_t seen = 0; seen < code_point && index < text.size(); ++seen)
        index += text.at(index).isHighSurrogate() && index + 1 < text.size() ? 2 : 1;
    return index;
}

}  // namespace

SearchBar::SearchBar(QWidget* parent) : QWidget(parent) {
    input_ = new QLineEdit(this);
    input_->setPlaceholderText(tr("Search: kick bpm:120..130 key:Am tag:Drums rating:>=3 available:true"));
    input_->setClearButtonEnabled(true);
    input_->setAccessibleName(tr("Search samples"));
    input_->setToolTip(
        tr("Words search names, paths, tags and notes. Fields: tag, category, folder, collection, "
           "bpm, key, duration, rating, favorite, format, codec, rate, channels, bits, size, "
           "available, analyzed, name, path, notes. Combine with AND, OR, NOT and parentheses."));

    error_action_ = input_->addAction(style()->standardIcon(QStyle::SP_MessageBoxWarning), QLineEdit::TrailingPosition);
    error_action_->setVisible(false);

    error_label_ = new QLabel(this);
    error_label_->setTextFormat(Qt::RichText);
    error_label_->setWordWrap(true);
    error_label_->setAccessibleName(tr("Search error"));
    error_label_->hide();

    auto* layout = new QVBoxLayout(this);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(2);
    layout->addWidget(input_);
    layout->addWidget(error_label_);

    debounce_.setSingleShot(true);
    debounce_.setInterval(kDebounceMilliseconds);
    connect(&debounce_, &QTimer::timeout, this, &SearchBar::validate_and_submit);
    connect(input_, &QLineEdit::textEdited, this, [this] { debounce_.start(); });
    connect(input_, &QLineEdit::returnPressed, this, [this] {
        debounce_.stop();
        validate_and_submit();
    });
    // The clear button changes the text without textEdited.
    connect(input_, &QLineEdit::textChanged, this, [this](const QString& text) {
        if (text.isEmpty()) debounce_.start();
    });
}

QString SearchBar::text() const {
    return input_->text();
}

void SearchBar::set_text(const QString& text) {
    input_->setText(text);
    debounce_.stop();
    validate_and_submit();
}

void SearchBar::focus_and_select() {
    input_->setFocus(Qt::ShortcutFocusReason);
    input_->selectAll();
}

void SearchBar::validate_and_submit() {
    const QString text = input_->text();
    // Parsing is pure, bounded CPU work (≤4,096 code points), so it runs here; searching does not.
    auto parsed = search::parse_query(DisplayFormat::to_utf8(text));
    if (!parsed) {
        const int index = parsed.error().position ? character_index_of_code_point(text, *parsed.error().position)
                                                  : static_cast<int>(text.size());
        show_error(DisplayFormat::from_utf8(parsed.error().safe_detail), index);
        return;
    }
    clear_error();
    if (submitted_once_ && text == last_submitted_) return;
    submitted_once_ = true;
    last_submitted_ = text;
    emit querySubmitted(text);
}

void SearchBar::show_error(const QString& message, int character_index) {
    const QString text = input_->text();
    const int clamped = std::clamp(character_index, 0, static_cast<int>(text.size()));
    const QString before = text.left(clamped).toHtmlEscaped();
    const QString at = clamped < text.size() ? text.mid(clamped, 1).toHtmlEscaped() : QStringLiteral("&nbsp;");
    const QString after = text.mid(clamped + 1).toHtmlEscaped();
    error_label_->setText(tr("Query not run, error at position %1: %2<br><code>%3<u><b>%4</b></u>%5</code>")
                              .arg(clamped + 1)
                              .arg(message.toHtmlEscaped(), before, at, after));
    error_label_->setAccessibleDescription(tr("Error at position %1: %2").arg(clamped + 1).arg(message));
    error_label_->show();
    // The style's warning icon inside the box marks the error in every theme, without colors.
    error_action_->setToolTip(error_label_->accessibleDescription());
    error_action_->setVisible(true);
}

void SearchBar::clear_error() {
    error_label_->hide();
    error_label_->clear();
    error_action_->setVisible(false);
}

QString SearchBar::error_text() const {
    return error_label_->isHidden() ? QString() : error_label_->accessibleDescription();
}

}  // namespace mso::ui
