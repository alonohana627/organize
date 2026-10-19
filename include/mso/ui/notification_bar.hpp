#pragma once
/// @file
/// NotificationBar: a dismissible message strip above the sample table for failures that must not
/// vanish after a few seconds (a failed save, undo or import).
#include <QFrame>
#include <QString>

class QLabel;
class QToolButton;

namespace mso::ui {

/// Shows the most recent error until the user closes it. Uses the style's warning icon and the
/// palette's tool-tip colors, so it is distinct yet follows light and dark themes.
class NotificationBar final : public QFrame {
    Q_OBJECT

public:
    explicit NotificationBar(QWidget* parent = nullptr);

    /// Shows `title` in bold and `details` below it, replacing any earlier message.
    void show_error(const QString& title, const QString& details);
    /// Hides the bar.
    void dismiss();
    /// Plain text of the message shown (empty when hidden); used by tests and accessibility.
    QString message() const;

private:
    /// Widgets (owned through Qt's parent tree).
    QLabel* icon_label_ = nullptr;
    QLabel* text_label_ = nullptr;
    QToolButton* close_button_ = nullptr;
};

}  // namespace mso::ui
