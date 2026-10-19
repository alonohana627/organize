#pragma once
/// @file
/// SearchBar: query-language input with debounced submission and inline, positioned parse errors.
#include <QString>
#include <QTimer>
#include <QWidget>

class QAction;
class QLabel;
class QLineEdit;

namespace mso::ui {

/// Text box for the typed query language (docs/03_DATA_AND_SEARCH.md §6). Typing is debounced
/// (150 ms); only queries that parse are submitted, so an invalid query never runs a modified
/// search. Parse errors are shown below the box with the reported position marked.
class SearchBar final : public QWidget {
    Q_OBJECT

public:
    /// Quiet time after typing before a query runs.
    static constexpr int kDebounceMilliseconds = 150;

    /// An empty search box.
    explicit SearchBar(QWidget* parent = nullptr);

    /// Current text of the box.
    QString text() const;
    /// Replaces the text and submits it immediately (e.g. when a smart collection is opened).
    void set_text(const QString& text);
    /// Moves keyboard focus into the box and selects its contents (Ctrl+F).
    void focus_and_select();
    /// The parse error shown (position and message), or empty when the query is valid.
    QString error_text() const;

signals:
    /// A syntactically valid query was typed (after the debounce interval).
    void querySubmitted(const QString& text);

private:
    /// Parses the current text; shows the error or submits the query.
    void validate_and_submit();
    /// Shows a parse error below the box with the reported position underlined.
    void show_error(const QString& message, int character_index);
    /// Hides the error line and icon.
    void clear_error();

    /// The query box.
    QLineEdit* input_ = nullptr;
    QAction* error_action_ = nullptr;  ///< warning icon inside the box while the query is invalid
    QLabel* error_label_ = nullptr;    ///< parse error with the position marked
    /// Restarted by every keystroke.
    QTimer debounce_;
    QString last_submitted_;       ///< last text emitted, to skip duplicate submissions
    bool submitted_once_ = false;  ///< a query was submitted (last_submitted_ is meaningful)
};

}  // namespace mso::ui
