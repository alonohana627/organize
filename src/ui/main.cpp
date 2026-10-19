/// @file
/// Entry point of the Music Sample Organizer desktop application.
///
///   MusicSampleOrganizer [--workspace <dir>] [--exit-when-ready] [--screenshot <file.png>]
///
/// --workspace (or the MSO_WORKSPACE_DIR environment variable) selects the machine-local
/// workspace directory, so tests and separate profiles never touch the user's real workspace.
/// --exit-when-ready quits as soon as the workspace has opened (exit code 0) or failed (1):
/// a startup smoke test that also runs headless with QT_QPA_PLATFORM=offscreen.
/// --screenshot saves the main window to a PNG once the first results have loaded, then quits
/// (documentation and visual review; also works offscreen).
#include <QApplication>
#include <QCommandLineParser>
#include <QIcon>
#include <QPixmap>
#include <QSize>
#include <QTimer>

#include "mso/app/application.hpp"
#include "mso/ui/application_session.hpp"
#include "mso/ui/display_format.hpp"
#include "mso/ui/main_window.hpp"

#ifndef MSO_VERSION_STRING
#define MSO_VERSION_STRING "0.0.0"
#endif

int main(int argc, char* argv[]) {
    QApplication application(argc, argv);
    QApplication::setApplicationName(QStringLiteral("Music Sample Organizer"));
    QApplication::setApplicationVersion(QStringLiteral(MSO_VERSION_STRING));
    // Every rendered size from the Qt resources (cmake/ApplicationIcon.cmake), so title bars, the
    // taskbar and Alt+Tab each get a crisp image instead of a scaled one.
    QIcon application_icon;
    for (const int size : {16, 24, 32, 48, 64, 128, 256})
        application_icon.addFile(QStringLiteral(":/icons/music_sample_organizer_%1.png").arg(size), QSize(size, size));
    QApplication::setWindowIcon(application_icon);

    QCommandLineParser parser;
    parser.setApplicationDescription(QApplication::translate("main", "Offline sample library browser"));
    parser.addHelpOption();
    parser.addVersionOption();
    const QCommandLineOption workspace_option(
        QStringLiteral("workspace"),
        QApplication::translate("main", "Workspace directory (default: the per-user data directory)."),
        QApplication::translate("main", "directory"));
    const QCommandLineOption exit_when_ready_option(
        QStringLiteral("exit-when-ready"),
        QApplication::translate("main", "Quit once the workspace has opened (smoke test)."));
    const QCommandLineOption screenshot_option(
        QStringLiteral("screenshot"),
        QApplication::translate("main", "Save the main window to a PNG after start-up, then quit."),
        QApplication::translate("main", "file"));
    parser.addOption(workspace_option);
    parser.addOption(exit_when_ready_option);
    parser.addOption(screenshot_option);
    parser.process(application);

    mso::app::ApplicationOptions options;
    // The desktop app manages libraries and analysis for the user: a default library, rescans at
    // start-up and on reconnect, and automatic analysis of newly imported samples.
    options.automatic_mode = true;
    QString workspace = parser.value(workspace_option);
    if (workspace.isEmpty()) workspace = qEnvironmentVariable("MSO_WORKSPACE_DIR");
    if (!workspace.isEmpty()) options.workspace_directory = mso::ui::DisplayFormat::to_path(workspace);

    int exit_code = 0;
    {
        // The session outlives the window: panels hold references to it until they are destroyed.
        mso::ui::ApplicationSession session(options);
        mso::ui::MainWindow window(session);
        if (parser.isSet(exit_when_ready_option)) {
            // Close after the first library refresh so the whole startup path has run.
            QObject::connect(&session, &mso::ui::ApplicationSession::librariesChanged, &window,
                             [&window] { QTimer::singleShot(0, &window, &QWidget::close); });
            QObject::connect(&session, &mso::ui::ApplicationSession::startFailed, &window, [](const QString& message) {
                qCritical("Workspace failed to open: %s", qUtf8Printable(message));
                QApplication::exit(1);
            });
        }
        if (parser.isSet(screenshot_option)) {
            constexpr int kSettleMilliseconds = 3000;  // first result page, counts and docks populated
            const QString file = parser.value(screenshot_option);
            QObject::connect(&session, &mso::ui::ApplicationSession::librariesChanged, &window, [&window, file] {
                QTimer::singleShot(kSettleMilliseconds, &window, [&window, file] {
                    if (!window.grab().save(file))
                        qCritical("Could not save the screenshot to %s", qUtf8Printable(file));
                    window.close();
                });
            });
        }
        // Saved layout and shortcuts apply before the first paint, so the window never jumps.
        session.preload_preferences();
        window.restore_saved_state();
        if (parser.isSet(screenshot_option)) window.resize(1600, 950);  // reproducible documentation shots
        window.show();
        session.start();
        exit_code = QApplication::exec();
    }
    return exit_code;
}
