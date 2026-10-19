#pragma once
/// @file
/// EmptyStateWidget: the friendly first-run page shown while no library is open.
#include <QWidget>

class QLabel;
class QPushButton;

namespace mso::ui {

/// Central page with "Add sample folder" and "Open existing library" actions, also used while the
/// workspace is opening and when it failed to open.
///
/// The message wraps to the page's width and its whole text is always shown: the label takes the
/// height its wrapped text needs at the current width (height-for-width through every layout),
/// and when the window is too small even for that, the page scrolls instead of cutting it off.
class EmptyStateWidget final : public QWidget {
    Q_OBJECT

public:
    /// Widest the message grows before it wraps (it reads best as a short column).
    static constexpr int kMaximumMessageWidth = 560;

    /// The onboarding page, showing "Opening workspace…" until show_welcome() or show_error().
    explicit EmptyStateWidget(QWidget* parent = nullptr);

    /// Shows a progress line instead of the buttons (workspace opening).
    void show_busy(const QString& message);
    /// Shows the welcome text and both buttons.
    void show_welcome();
    /// Shows an error with the buttons still available.
    void show_error(const QString& message);
    /// The message label (tests check that its whole text fits).
    const QLabel* message_label() const { return message_label_; }

signals:
    /// The user wants to add a sample folder (creating a library if needed).
    void addFolderRequested();
    /// The user wants to open an existing library.
    void openLibraryRequested();

private:
    // Widgets (owned through Qt's parent tree).
    QLabel* title_label_ = nullptr;             ///< welcome or busy title
    QLabel* message_label_ = nullptr;           ///< welcome text, progress line or error
    QPushButton* add_folder_button_ = nullptr;  ///< Add sample folder…
    QPushButton* open_button_ = nullptr;        ///< Open existing library…
};

}  // namespace mso::ui
