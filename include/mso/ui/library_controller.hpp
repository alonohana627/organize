#pragma once
/// @file
/// LibraryController: creating, opening and closing libraries, adding and rescanning sample folders.
#include <QObject>
#include <QString>

#include <cstdint>
#include <optional>

#include "mso/app/application.hpp"
#include "mso/domain/core.hpp"

class QWidget;

namespace mso::ui {

class ApplicationSession;

/// The library-level commands of the File menu, the navigator and the onboarding page. Every
/// blocking step (including checking whether a folder is a library) runs off the UI thread.
class LibraryController final : public QObject {
    Q_OBJECT

public:
    /// Dialogs open over `dialog_parent`; `session` must outlive the controller.
    LibraryController(ApplicationSession& session, QWidget* dialog_parent);

    /// Asks for a name, creates a library in the workspace, then asks for its first folder.
    void new_library();
    /// Asks for a folder; opens it as a library, or offers to add it as a sample folder.
    void open_library();
    /// Closes `library`.
    void close_library(const uuids::uuid& library);
    /// Adds a sample folder (asking for one when `folder` is empty) to `library`, or to the only or
    /// default library (`suggested` preselects one when several are open), then scans it.
    void add_folder(const std::optional<uuids::uuid>& library, const QString& folder = {},
                    const std::optional<uuids::uuid>& suggested = std::nullopt);
    /// Rescans one root.
    void rescan(const uuids::uuid& library, std::int64_t root_id);
    /// Asks for the new location of a moved root, rebinds it and rescans it.
    void relocate(const uuids::uuid& library, std::int64_t root_id);

    /// Lets the user pick one open library by position (names may repeat: duplicates show their
    /// folder). Returns the only candidate without asking; nullopt when cancelled or none is open.
    [[nodiscard]] static std::optional<uuids::uuid> choose_library(QWidget* parent,
                                                                   const std::vector<app::OpenLibraryInfo>& libraries,
                                                                   const QString& purpose, bool writable_only,
                                                                   const std::optional<uuids::uuid>& preferred);

signals:
    /// An import started (for the Jobs panel).
    void importStarted(mso::app::OperationId operation, const QString& description);
    /// `library` is about to close (stop previews of its samples).
    void libraryClosing(const uuids::uuid& library);
    /// One-line feedback for the status bar.
    void statusMessage(const QString& message);
    /// A command failed; shown prominently.
    void failed(const QString& title, const QString& details);

private:
    /// Starts the scan of a root and announces it.
    void start_import(const uuids::uuid& library, std::int64_t root_id, const QString& description);

    /// Runs the library commands.
    ApplicationSession& session_;
    /// Parent of the file and choice dialogs.
    QWidget* dialog_parent_;
};

}  // namespace mso::ui
