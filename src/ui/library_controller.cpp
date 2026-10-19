#include "mso/ui/library_controller.hpp"

#include <QComboBox>
#include <QDialog>
#include <QDialogButtonBox>
#include <QDir>
#include <QFileDialog>
#include <QFileInfo>
#include <QInputDialog>
#include <QLabel>
#include <QMessageBox>
#include <QVBoxLayout>
#include <QWidget>

#include <map>

#include "mso/ui/add_folder_dialog.hpp"
#include "mso/ui/application_session.hpp"
#include "mso/ui/display_format.hpp"

namespace mso::ui {

LibraryController::LibraryController(ApplicationSession& session, QWidget* dialog_parent)
    : QObject(dialog_parent), session_(session), dialog_parent_(dialog_parent) {}

std::optional<uuids::uuid> LibraryController::choose_library(QWidget* parent,
                                                             const std::vector<app::OpenLibraryInfo>& libraries,
                                                             const QString& purpose, bool writable_only,
                                                             const std::optional<uuids::uuid>& preferred) {
    // Copies, not pointers into `libraries`: the dialog's event loop may deliver a new library list
    // (ApplicationSession::refresh_libraries) and replace the vector the caller passed.
    std::vector<app::OpenLibraryInfo> candidates;
    for (const auto& info : libraries)
        if (!writable_only || !info.read_only) candidates.push_back(info);
    if (candidates.empty()) return std::nullopt;
    if (candidates.size() == 1) return candidates.front().uuid;

    std::map<std::string, int> name_uses;
    for (const auto& info : candidates)
        ++name_uses[info.name];
    QDialog dialog(parent);
    dialog.setWindowTitle(tr("Choose library"));
    auto* combo = new QComboBox(&dialog);
    combo->setAccessibleName(tr("Library"));
    for (std::size_t index = 0; index < candidates.size(); ++index) {
        const app::OpenLibraryInfo& info = candidates[index];
        const QString name = DisplayFormat::from_utf8(info.name);
        // Two libraries may share a name; their folders tell them apart.
        combo->addItem(name_uses[info.name] > 1 ? tr("%1 (%2)").arg(name, DisplayFormat::from_path(info.directory))
                                                : name);
        if (preferred && info.uuid == *preferred) combo->setCurrentIndex(static_cast<int>(index));
    }
    auto* buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, &dialog);
    QObject::connect(buttons, &QDialogButtonBox::accepted, &dialog, &QDialog::accept);
    QObject::connect(buttons, &QDialogButtonBox::rejected, &dialog, &QDialog::reject);
    auto* layout = new QVBoxLayout(&dialog);
    layout->addWidget(new QLabel(purpose, &dialog));
    layout->addWidget(combo);
    layout->addWidget(buttons);
    if (dialog.exec() != QDialog::Accepted || combo->currentIndex() < 0) return std::nullopt;
    return candidates[static_cast<std::size_t>(combo->currentIndex())].uuid;  // by position, never by name
}

void LibraryController::new_library() {
    if (!session_.is_ready()) return;
    bool accepted = false;
    const QString name = QInputDialog::getText(dialog_parent_, tr("New library"), tr("Library name:"),
                                               QLineEdit::Normal, tr("My Samples"), &accepted)
                             .trimmed();
    if (!accepted || name.isEmpty()) return;
    emit statusMessage(tr("Creating library…"));
    session_.run(
        this,
        [name = DisplayFormat::to_utf8(name)](app::Application& application) {
            return application.create_library_in_workspace(name);
        },
        [this](Result<app::OpenLibraryInfo> created) {
            if (!created) {
                emit failed(tr("Could not create the library"), DisplayFormat::error(created.error()));
                return;
            }
            session_.refresh_libraries();
            add_folder(created->uuid);  // a library is only useful with samples in it
        });
}

void LibraryController::open_library() {
    if (!session_.is_ready()) return;
    const QString directory =
        QFileDialog::getExistingDirectory(dialog_parent_, tr("Open library folder"), {}, QFileDialog::ShowDirsOnly);
    if (directory.isEmpty()) return;
    const std::filesystem::path path = DisplayFormat::to_path(directory);
    // Even "is this a library?" reads the disk: ask on the pool.
    session_.run(
        this, [path](app::Application&) { return app::Application::is_library_folder(path); },
        [this, directory, path](bool is_library) {
            if (!is_library) {
                // Most people pick their sample folder here: offer to add it instead of failing.
                const auto answer = QMessageBox::question(
                    dialog_parent_, tr("Not a library"),
                    tr("%1 is a folder of samples, not a library.\n\nAdd it as a sample folder? It will be scanned "
                       "and analyzed automatically.")
                        .arg(QDir::toNativeSeparators(directory)));
                if (answer == QMessageBox::Yes) add_folder(std::nullopt, directory);
                return;
            }
            emit statusMessage(tr("Opening library…"));
            session_.run(
                this, [path](app::Application& application) { return application.open_library(path); },
                [this](Result<app::OpenLibraryInfo> opened) {
                    if (!opened) {
                        emit failed(tr("Could not open the library"), DisplayFormat::error(opened.error()));
                        return;
                    }
                    const QString name = DisplayFormat::from_utf8(opened->name);
                    emit statusMessage(opened->read_only ? tr("Opened %1 read-only").arg(name)
                                                         : tr("Opened %1").arg(name));
                    session_.refresh_libraries();
                });
        });
}

void LibraryController::close_library(const uuids::uuid& library) {
    if (!session_.is_ready()) return;
    emit libraryClosing(library);
    session_.run(
        this, [library](app::Application& application) { return application.close_library(library); },
        [this](Result<void> closed) {
            if (!closed) emit failed(tr("Could not close the library"), DisplayFormat::error(closed.error()));
            session_.refresh_libraries();
        });
}

void LibraryController::add_folder(const std::optional<uuids::uuid>& library, const QString& folder,
                                   const std::optional<uuids::uuid>& suggested) {
    if (!session_.is_ready()) return;
    QString chosen = folder;
    if (chosen.isEmpty())
        chosen = QFileDialog::getExistingDirectory(dialog_parent_, tr("Choose a folder of samples"), {},
                                                   QFileDialog::ShowDirsOnly);
    if (chosen.isEmpty()) return;

    std::vector<std::pair<QString, uuids::uuid>> writable;
    for (const auto& info : session_.libraries())
        if (!info.read_only) writable.emplace_back(DisplayFormat::from_utf8(info.name), info.uuid);
    if (writable.empty() && !library) {
        // No library yet: create the default one in the workspace, then continue with this folder.
        session_.run(
            this, [](app::Application& application) { return application.default_library(); },
            [this, chosen](Result<app::OpenLibraryInfo> created) {
                if (!created) {
                    emit failed(tr("Could not create a library"), DisplayFormat::error(created.error()));
                    return;
                }
                session_.refresh_libraries();
                add_folder(created->uuid, chosen);
            });
        return;
    }

    std::optional<uuids::uuid> target = library;
    if (!target && writable.size() == 1) target = writable.front().second;
    if (!target) {
        AddFolderDialog dialog(QDir::toNativeSeparators(chosen), writable, suggested, dialog_parent_);
        if (dialog.exec() != QDialog::Accepted) return;
        target = dialog.library();
        if (!target) return;
    }
    session_.run(
        this,
        [library = *target, path = DisplayFormat::to_path(chosen),
         name = DisplayFormat::to_utf8(QFileInfo(chosen).fileName())](app::Application& application) {
            return application.add_root(library, path, name);
        },
        [this, library = *target, chosen](Result<catalog::RootRecord> root) {
            if (!root) {
                emit failed(tr("Could not add the folder"), DisplayFormat::error(root.error()));
                return;
            }
            session_.refresh_libraries();  // switches from the onboarding page to the browser
            start_import(library, root->id, tr("Import %1").arg(QDir::toNativeSeparators(chosen)));
        });
}

void LibraryController::rescan(const uuids::uuid& library, std::int64_t root_id) {
    start_import(library, root_id, tr("Rescan"));
}

void LibraryController::relocate(const uuids::uuid& library, std::int64_t root_id) {
    if (!session_.is_ready()) return;
    const QString folder =
        QFileDialog::getExistingDirectory(dialog_parent_, tr("Locate the moved folder"), {}, QFileDialog::ShowDirsOnly);
    if (folder.isEmpty()) return;
    session_.run(
        this,
        [library, root_id, path = DisplayFormat::to_path(folder)](app::Application& application) {
            return application.rebind_root(library, root_id, path);
        },
        [this, library, root_id](Result<void> rebound) {
            if (!rebound) {
                emit failed(tr("Could not locate the folder"), DisplayFormat::error(rebound.error()));
                return;
            }
            start_import(library, root_id, tr("Rescan after relocating"));
        });
}

void LibraryController::start_import(const uuids::uuid& library, std::int64_t root_id, const QString& description) {
    if (!session_.is_ready()) return;
    session_.run(
        this, [library, root_id](app::Application& application) { return application.start_import(library, root_id); },
        [this, description](Result<app::OperationId> operation) {
            if (!operation) {
                emit failed(tr("Could not start the import"), DisplayFormat::error(operation.error()));
                return;
            }
            emit importStarted(*operation, description);
            emit statusMessage(tr("%1 started; results appear while it runs.").arg(description));
        });
}

}  // namespace mso::ui
