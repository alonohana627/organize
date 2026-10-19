// File identity (size, last write time, volume and file id) and link classification.
#include "mso/files/detail/file_identity.hpp"

#include <cstring>
#include <format>
#include <limits>
#include <system_error>

#include "mso/media/native_file.hpp"

#ifdef _WIN32
#include <windows.h>
#else
#include <sys/stat.h>
#include <cerrno>
#endif

namespace mso::files::detail {

namespace {

/// Saturating conversion of a 64-bit count of `unit_ns` nanosecond ticks since the Unix epoch.
std::int64_t ticks_to_nanoseconds(std::int64_t ticks, std::int64_t unit_ns) noexcept {
    std::int64_t nanoseconds = 0;
    if (__builtin_mul_overflow(ticks, unit_ns, &nanoseconds)) {
        return ticks < 0 ? std::numeric_limits<std::int64_t>::min() : std::numeric_limits<std::int64_t>::max();
    }
    return nanoseconds;
}

/// A FileId holding `index` (little-endian) in its low bytes and zeros above.
FileId file_id_from_index(std::uint64_t index) noexcept {
    FileId id{};
    for (std::size_t byte = 0; byte < sizeof(index); ++byte)
        id[byte] = static_cast<std::uint8_t>(index >> (8 * byte));
    return id;
}

// Windows reparse tags and attributes, spelled out because older MinGW headers lack some of them
// and because classify_reparse_tag() is plain arithmetic on every platform.
constexpr std::uint32_t kReparseTagSymbolicLink = 0xA000000C;       // IO_REPARSE_TAG_SYMLINK
constexpr std::uint32_t kReparseTagMountPoint = 0xA0000003;         // IO_REPARSE_TAG_MOUNT_POINT
constexpr std::uint32_t kReparseTagDeduplication = 0x80000013;      // IO_REPARSE_TAG_DEDUP
constexpr std::uint32_t kReparseTagOverlayFilter = 0x80000017;      // IO_REPARSE_TAG_WOF (compact /EXE)
constexpr std::uint32_t kReparseTagCloud = 0x9000001A;              // IO_REPARSE_TAG_CLOUD (OneDrive, Cloud Files API)
constexpr std::uint32_t kReparseTagCloudMask = 0x0000F000;          // IO_REPARSE_TAG_CLOUD_1 .. _F
constexpr std::uint32_t kReparseTagFilePlaceholder = 0x80000015;    // IO_REPARSE_TAG_FILE_PLACEHOLDER (Windows 8.1)
constexpr std::uint32_t kAttributeOffline = 0x00001000;             // FILE_ATTRIBUTE_OFFLINE
constexpr std::uint32_t kAttributeRecallOnOpen = 0x00040000;        // FILE_ATTRIBUTE_RECALL_ON_OPEN
constexpr std::uint32_t kAttributeRecallOnDataAccess = 0x00400000;  // FILE_ATTRIBUTE_RECALL_ON_DATA_ACCESS

/// True for the attributes of an entry whose data is not on this device.
bool data_not_local(std::uint32_t attributes, bool is_directory) noexcept {
    if (is_directory) return (attributes & kAttributeRecallOnOpen) != 0;  // listing would be fetched
    return (attributes & (kAttributeOffline | kAttributeRecallOnOpen | kAttributeRecallOnDataAccess)) != 0;
}

}  // namespace

LinkKind classify_reparse_tag(std::uint32_t tag, std::uint32_t attributes, bool is_directory) noexcept {
    if (tag == kReparseTagSymbolicLink) return LinkKind::SymbolicLink;
    if (tag == kReparseTagMountPoint) return LinkKind::Junction;
    if (tag == kReparseTagDeduplication) return LinkKind::Deduplicated;
    // Usually invisible: the WOF driver hides its own tag while it is attached to the volume. Where
    // it shows (the driver is not attached), the data is still on this volume, just compressed.
    if (tag == kReparseTagOverlayFilter) return LinkKind::Compressed;
    if ((tag & ~kReparseTagCloudMask) == kReparseTagCloud || tag == kReparseTagFilePlaceholder) {
        return data_not_local(attributes, is_directory) ? LinkKind::CloudOnly : LinkKind::CloudPlaceholder;
    }
    return LinkKind::OtherReparsePoint;
}

#ifdef _WIN32

namespace {

constexpr std::int64_t kUnixEpochIn100ns = 116444736000000000LL;  // 1970-01-01 in FILETIME units

/// FILETIME (100 ns since 1601) to nanoseconds since 1970, saturating instead of overflowing.
std::int64_t filetime_to_unix_ns(const FILETIME& time) noexcept {
    const std::uint64_t ticks = (static_cast<std::uint64_t>(time.dwHighDateTime) << 32) | time.dwLowDateTime;
    if (ticks > static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max())) {
        return std::numeric_limits<std::int64_t>::max();
    }
    return ticks_to_nanoseconds(static_cast<std::int64_t>(ticks) - kUnixEpochIn100ns, 100);
}

}  // namespace

Result<FileIdentity> read_file_identity(const std::filesystem::path& file) {
    // Attribute-only access: never reads content and never blocks writers. Without
    // FILE_FLAG_OPEN_REPARSE_POINT the handle (and so the identity) is the link target's.
    media::UniqueWin32Handle handle(CreateFileW(media::native_path(file).c_str(), FILE_READ_ATTRIBUTES,
                                                FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
                                                OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS, nullptr));
    if (!handle.valid()) return std::unexpected(media::error_from_win32(GetLastError(), "reading the file identity"));

    BY_HANDLE_FILE_INFORMATION information{};
    if (!GetFileInformationByHandle(handle.get(), &information)) {
        return std::unexpected(media::error_from_win32(GetLastError(), "reading the file identity"));
    }
    FileIdentity identity;
    identity.byte_size = static_cast<std::int64_t>((static_cast<std::uint64_t>(information.nFileSizeHigh) << 32) |
                                                   information.nFileSizeLow);
    identity.last_write_time_ns = filetime_to_unix_ns(information.ftLastWriteTime);

    FILE_ID_INFO id_information{};
    if (GetFileInformationByHandleEx(handle.get(), FileIdInfo, &id_information, sizeof(id_information))) {
        identity.volume_id = std::format("{:016x}", id_information.VolumeSerialNumber);
        FileId id{};
        static_assert(sizeof(id_information.FileId.Identifier) == sizeof(FileId));
        std::memcpy(id.data(), id_information.FileId.Identifier, id.size());
        identity.file_id = id;
    } else {
        // Filesystems without 128-bit ids (FAT, older systems) fall back to the 64-bit index,
        // zero-extended exactly as NTFS reports it through FileIdInfo.
        identity.volume_id = std::format("{:016x}", information.dwVolumeSerialNumber);
        identity.file_id = file_id_from_index((static_cast<std::uint64_t>(information.nFileIndexHigh) << 32) |
                                              information.nFileIndexLow);
    }
    return identity;
}

EntryClassification classify_entry(const std::filesystem::path& path) noexcept {
    try {
        const std::filesystem::path native = media::native_path(path);
        const DWORD attributes = GetFileAttributesW(native.c_str());
        if (attributes == INVALID_FILE_ATTRIBUTES) return {};
        const bool is_directory = (attributes & FILE_ATTRIBUTE_DIRECTORY) != 0;
        if ((attributes & FILE_ATTRIBUTE_REPARSE_POINT) == 0) {
            return {data_not_local(attributes, is_directory) ? LinkKind::CloudOnly : LinkKind::None, is_directory};
        }
        // The reparse tag comes with the directory listing data; nothing is opened.
        WIN32_FIND_DATAW data{};
        HANDLE search = FindFirstFileExW(native.c_str(), FindExInfoBasic, &data, FindExSearchNameMatch, nullptr, 0);
        if (search == INVALID_HANDLE_VALUE) return {LinkKind::OtherReparsePoint, is_directory};
        FindClose(search);
        return {classify_reparse_tag(data.dwReserved0, attributes, is_directory), is_directory};
    } catch (const std::exception&) {
        return {};  // path conversion ran out of memory: let the open report it
    }
}

#else

namespace {

/// Last modification time of a stat result in nanoseconds since the Unix epoch.
std::int64_t modification_time_ns(const struct stat& status) noexcept {
#ifdef __APPLE__
    const struct timespec& time = status.st_mtimespec;
#else
    const struct timespec& time = status.st_mtim;
#endif
    const std::int64_t seconds_ns = ticks_to_nanoseconds(static_cast<std::int64_t>(time.tv_sec), 1'000'000'000);
    std::int64_t total = 0;
    if (__builtin_add_overflow(seconds_ns, static_cast<std::int64_t>(time.tv_nsec), &total)) return seconds_ns;
    return total;
}

}  // namespace

Result<FileIdentity> read_file_identity(const std::filesystem::path& file) {
    struct stat status{};
    if (::stat(file.c_str(), &status) != 0) {
        return std::unexpected(media::error_from_errno(errno, "reading the file identity"));
    }
    FileIdentity identity;
    identity.byte_size = static_cast<std::int64_t>(status.st_size);
    identity.last_write_time_ns = modification_time_ns(status);
    identity.volume_id = std::format("{:016x}", static_cast<std::uint64_t>(status.st_dev));
    identity.file_id = file_id_from_index(static_cast<std::uint64_t>(status.st_ino));
    return identity;
}

EntryClassification classify_entry(const std::filesystem::path& path) noexcept {
    try {
        std::error_code error;
        const auto status = std::filesystem::symlink_status(path, error);
        if (error) return {};
        if (!std::filesystem::is_symlink(status)) return {LinkKind::None, std::filesystem::is_directory(status)};
        std::error_code target_error;  // a dangling link is simply not a directory
        return {LinkKind::SymbolicLink, std::filesystem::is_directory(std::filesystem::status(path, target_error))};
    } catch (const std::exception&) {
        return {};  // out of memory: let the open report it
    }
}

#endif

LinkKind classify_link(const std::filesystem::path& path) noexcept {
    return classify_entry(path).kind;
}

}  // namespace mso::files::detail
