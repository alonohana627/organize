#pragma once
/// @file
/// FileManager: shows files and folders in the platform's file manager ("Show in Explorer").
#include <QCoreApplication>
#include <QString>

#include <filesystem>

namespace mso::ui {

/// Thin wrapper over the platform file manager. Nothing here waits for it: on Windows Explorer is
/// started as a detached process (QDesktopServices::openUrl would call ShellExecute synchronously
/// on the UI thread); elsewhere the desktop's opener (xdg-open) is started detached by Qt.
class FileManager {
    Q_DECLARE_TR_FUNCTIONS(FileManager)

public:
    /// A detached process start: the program and its argument string, passed verbatim.
    struct LaunchCommand {
        QString program;           ///< e.g. explorer.exe
        QString native_arguments;  ///< already quoted for the program's own parser
    };

    /// Opens the folder containing `file` with the file selected (Windows Explorer); on other
    /// platforms opens the containing folder. Returns false when nothing could be started.
    [[nodiscard]] static bool reveal_file(const std::filesystem::path& file);
    /// Opens `folder` itself in the file manager. Returns false when nothing could be started.
    [[nodiscard]] static bool open_folder(const std::filesystem::path& folder);
    /// Menu text for revealing an item: "Show in Explorer" on Windows, "Show in Folder" elsewhere.
    static QString reveal_action_text();
#ifdef _WIN32
    /// The Explorer command that selects `file` in its folder.
    static LaunchCommand reveal_command(const std::filesystem::path& file);
    /// The Explorer command that opens `folder`.
    static LaunchCommand open_folder_command(const std::filesystem::path& folder);
#endif
};

}  // namespace mso::ui
