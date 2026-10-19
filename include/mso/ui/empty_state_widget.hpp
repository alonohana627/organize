#pragma once
/// @file
/// EmptyStateWidget: the friendly first-run page shown while no library is open.
#include <QWidget>

class QLabel;
class QPushButton;

namespace mso::ui {

/// Central page with "Create library" and "Open library" actions, also used while the workspace
/// is opening and when it failed to open.
class EmptyStateWidget final : public QWidget {
    Q_OBJECT

public:
    explicit EmptyStateWidget(QWidget* parent = nullptr);

    /// Shows a progress line instead of the buttons (workspace opening).
    void show_busy(const QString& message);
    /// Shows the welcome text and both buttons.
    void show_welcome();
    /// Shows an error with the buttons still available.
    void show_error(const QString& message);

signals:
    void addFolderRequested();
    void openLibraryRequested();

private:
    QLabel* title_label_ = nullptr;
    QLabel* message_label_ = nullptr;
    QPushButton* add_folder_button_ = nullptr;
    QPushButton* open_button_ = nullptr;
};

}  // namespace mso::ui
