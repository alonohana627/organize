#include "mso/ui/file_manager.hpp"

#include <QDesktopServices>
#include <QProcess>

#include "mso/ui/display_format.hpp"

namespace mso::ui {

bool FileManager::reveal_file(const std::filesystem::path& file) {
#ifdef _WIN32
    // explorer.exe parses "/select,<path>" itself; the path is quoted verbatim (native
    // arguments) because QProcess's own quoting would split it at the comma.
    QProcess explorer;
    explorer.setProgram(QStringLiteral("explorer.exe"));
    explorer.setNativeArguments(QStringLiteral("/select,\"%1\"").arg(DisplayFormat::from_path(file)));
    return explorer.startDetached();
#else
    return open_folder(file.parent_path());
#endif
}

bool FileManager::open_folder(const std::filesystem::path& folder) {
    return QDesktopServices::openUrl(DisplayFormat::to_file_url(folder));
}

QString FileManager::reveal_action_text() {
#ifdef _WIN32
    return tr("Show in Explorer");
#else
    return tr("Show in Folder");
#endif
}

}  // namespace mso::ui
