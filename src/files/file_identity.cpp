#include "mso/files/file_identity.hpp"

#include <chrono>
#include <format>

#ifdef _WIN32
#include <windows.h>
#endif

namespace mso::files {

#ifdef _WIN32

namespace {

/// Owns a Win32 file handle.
class FileHandle {
public:
    explicit FileHandle(HANDLE handle) : handle_(handle) {}
    FileHandle(const FileHandle&) = delete;
    FileHandle& operator=(const FileHandle&) = delete;
    ~FileHandle() {
        if (valid()) CloseHandle(handle_);
    }
    bool valid() const { return handle_ != INVALID_HANDLE_VALUE && handle_ != nullptr; }
    HANDLE get() const { return handle_; }

private:
    HANDLE handle_;
};

/// FILETIME (100 ns since 1601) to nanoseconds since 1970.
std::int64_t filetime_to_unix_ns(const FILETIME& time) {
    constexpr std::int64_t kEpochDifference100ns = 116444736000000000LL;
    const auto ticks = (static_cast<std::int64_t>(time.dwHighDateTime) << 32) | time.dwLowDateTime;
    return (ticks - kEpochDifference100ns) * 100;
}

Error error_from_last_error(const std::filesystem::path& file) {
    const DWORD code = GetLastError();
    if (code == ERROR_FILE_NOT_FOUND || code == ERROR_PATH_NOT_FOUND || code == ERROR_NOT_READY)
        return Error{ErrorCode::FileMissing, false, std::format("not found: {}", file.filename().string()), {}, {}};
    if (code == ERROR_ACCESS_DENIED || code == ERROR_SHARING_VIOLATION)
        return Error{ErrorCode::AccessDenied, code == ERROR_SHARING_VIOLATION, "access denied", {}, {}};
    return Error{ErrorCode::Internal, true, std::format("Win32 error {}", code), {}, {}};
}

}  // namespace

Result<FileIdentity> read_file_identity(const std::filesystem::path& file) {
    // Attribute-only access: never reads content and never blocks writers.
    FileHandle handle(CreateFileW(file.c_str(), FILE_READ_ATTRIBUTES,
                                  FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING,
                                  FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OPEN_REPARSE_POINT, nullptr));
    if (!handle.valid()) return std::unexpected(error_from_last_error(file));

    BY_HANDLE_FILE_INFORMATION information{};
    if (!GetFileInformationByHandle(handle.get(), &information)) return std::unexpected(error_from_last_error(file));
    FileIdentity identity;
    identity.byte_size = (static_cast<std::int64_t>(information.nFileSizeHigh) << 32) | information.nFileSizeLow;
    identity.mtime_ns = filetime_to_unix_ns(information.ftLastWriteTime);

    FILE_ID_INFO id_information{};
    if (GetFileInformationByHandleEx(handle.get(), FileIdInfo, &id_information, sizeof(id_information))) {
        identity.volume_id = std::format("{:016x}", id_information.VolumeSerialNumber);
        identity.file_id = std::string(reinterpret_cast<const char*>(id_information.FileId.Identifier),
                                       sizeof(id_information.FileId.Identifier));
    } else {
        // Filesystems without 128-bit IDs (some FAT variants) fall back to the 64-bit index.
        identity.volume_id = std::format("{:08x}", information.dwVolumeSerialNumber);
        const std::uint64_t index =
            (static_cast<std::uint64_t>(information.nFileIndexHigh) << 32) | information.nFileIndexLow;
        identity.file_id = std::string(reinterpret_cast<const char*>(&index), sizeof(index));
    }
    return identity;
}

bool is_reparse_point(const std::filesystem::path& path) {
    const DWORD attributes = GetFileAttributesW(path.c_str());
    return attributes != INVALID_FILE_ATTRIBUTES && (attributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0;
}

#else

Result<FileIdentity> read_file_identity(const std::filesystem::path& file) {
    std::error_code error;
    const auto size = std::filesystem::file_size(file, error);
    if (error) return fail(ErrorCode::FileMissing, error.message());
    const auto time = std::filesystem::last_write_time(file, error);
    FileIdentity identity;
    identity.byte_size = static_cast<std::int64_t>(size);
    identity.mtime_ns = std::chrono::duration_cast<std::chrono::nanoseconds>(time.time_since_epoch()).count();
    return identity;
}

bool is_reparse_point(const std::filesystem::path& path) {
    return std::filesystem::is_symlink(path);
}

#endif

}  // namespace mso::files
