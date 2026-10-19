#pragma once
/// @file
/// FileManager: shows files and folders in the platform's file manager ("Show in Explorer").
#include <filesystem>

namespace mso::ui {

/// Thin wrapper over the platform file manager. Starting it is asynchronous (a detached process
/// or a desktop-services request), so these calls are safe on the UI thread.
class FileManager {
public:
    /// Opens the folder containing `file` with the file selected (Windows Explorer); on other
    /// platforms opens the containing folder. Returns false when nothing could be started.
    static bool reveal_file(const std::filesystem::path& file);
    /// Opens `folder` itself in the file manager.
    static bool open_folder(const std::filesystem::path& folder);
};

}  // namespace mso::ui
