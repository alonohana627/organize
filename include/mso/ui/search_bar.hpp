#pragma once
/// @file
/// SearchBar: query-language input with debounced submission and inline, positioned parse errors.
#include <QString>
#include <QTimer>
#include <QWidget>

class QLabel;
class QLineEdit;

namespace mso::ui {

/// Text box for the typed query language (docs/03_DATA_AND_SEARCH.md §6). Typing is debounced
/// (150 ms); only queries that parse are submitted, so an invalid query never runs a modified
/// search. Parse errors are shown below the box with the reported position marked.
class SearchBar final : public QWidget {
    Q_OBJECT

public:
    static constexpr int kDebounceMilliseconds = 150;

    explicit SearchBar(QWidget* parent = nullptr);

    QString text() const;
    /// Replaces the text and submits it immediately (e.g. when a smart collection is opened).
    void set_text(const QString& text);
    /// Moves keyboard focus into the box and selects its contents (Ctrl+F).
    void focus_and_select();

signals:
    /// A syntactically valid query was typed (after the debounce interval).
    void querySubmitted(const QString& text);

private:
    /// Parses the current text; shows the error or submits the query.
    void validate_and_submit();
    void show_error(const QString& message, int character_index);
    void clear_error();

    QLineEdit* input_ = nullptr;
    QLabel* error_label_ = nullptr;
    QTimer debounce_;
    QString last_submitted_;
    bool submitted_once_ = false;
};

}  // namespace mso::ui
