#include "mso/ui/file_manager.hpp"

#include <QDesktopServices>
#include <QProcess>

#include "mso/ui/display_format.hpp"

namespace mso::ui {

#ifdef _WIN32
namespace {

/// Starts `command` detached (asynchronous: nothing waits for Explorer).
bool start_detached(const FileManager::LaunchCommand& command) {
    QProcess process;
    process.setProgram(command.program);
    process.setNativeArguments(command.native_arguments);
    return process.startDetached();
}

}  // namespace

FileManager::LaunchCommand FileManager::reveal_command(const std::filesystem::path& file) {
    // explorer.exe parses "/select,<path>" itself; the path is quoted verbatim (native arguments)
    // because QProcess's own quoting would split it at the comma.
    return {QStringLiteral("explorer.exe"), QStringLiteral("/select,\"%1\"").arg(DisplayFormat::from_path(file))};
}

FileManager::LaunchCommand FileManager::open_folder_command(const std::filesystem::path& folder) {
    return {QStringLiteral("explorer.exe"), QStringLiteral("\"%1\"").arg(DisplayFormat::from_path(folder))};
}
#endif

bool FileManager::reveal_file(const std::filesystem::path& file) {
#ifdef _WIN32
    return start_detached(reveal_command(file));
#else
    return open_folder(file.parent_path());
#endif
}

bool FileManager::open_folder(const std::filesystem::path& folder) {
#ifdef _WIN32
    return start_detached(open_folder_command(folder));
#else
    return QDesktopServices::openUrl(DisplayFormat::to_file_url(folder));  // xdg-open, started detached
#endif
}

QString FileManager::reveal_action_text() {
#ifdef _WIN32
    return tr("Show in Explorer");
#else
    return tr("Show in Folder");
#endif
}

}  // namespace mso::ui
