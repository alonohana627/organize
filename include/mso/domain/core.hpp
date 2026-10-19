#pragma once
/// @file
/// Identity, time and text primitives shared by every module.
/// No Qt, SQLite or Windows headers; third-party work is delegated to vendored libraries in deps/.
#include <array>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <utility>

#include <uuid.h>  // deps/stduuid

namespace mso {

/// New RFC 4122 version-4 UUID (deps/stduuid) for libraries, samples, roots, tags, operations and
/// history actions, drawn from the operating system's secure random source. UUIDs are stored as
/// canonical lowercase text (uuids::to_string) and parsed with uuids::uuid::from_string.
uuids::uuid new_uuid();

/// Current UTC time in Unix milliseconds, the unit of every persisted timestamp.
std::int64_t now_unix_ms() noexcept;

// ---------------------------------------------------------------- text

/// Version tag of the key normalization below. Bump it whenever fold_key() output can change,
/// because persisted name/sort/sibling keys must be rebuilt when it does.
inline constexpr std::string_view kFoldVersion = "utf8proc-2.12-nfkc-casefold-1";

/// Normalized comparison key: Unicode NFKC + case folding (deps/utf8proc) with whitespace runs
/// collapsed to one space and trimmed. Used for sort keys, sibling uniqueness and lookups;
/// original text is always kept separately for display.
std::string fold_key(std::string_view utf8);

/// Key for comparing relative paths on case-insensitive volumes: Unicode NFC + case folding,
/// without compatibility mapping (distinct file names must stay distinct). Separators become '/'.
std::string fold_path_key(std::string_view relative_path_utf8);

/// True when the bytes are well-formed UTF-8.
bool is_valid_utf8(std::string_view text) noexcept;
/// Number of Unicode code points; malformed bytes count as one each.
std::size_t count_code_points(std::string_view utf8) noexcept;

/// Converts a filesystem path to UTF-8 with '/' separators (SQLite storage form).
std::string path_to_utf8(const std::filesystem::path& path);
/// Converts stored UTF-8 back to a native (UTF-16 on Windows) path.
std::filesystem::path path_from_utf8(std::string_view utf8);
/// Accepts only a non-empty relative path with no drive, root, NUL, "." or ".." segment.
bool is_safe_relative_path(std::string_view relative) noexcept;

std::string trim(std::string_view text);
std::string to_lower_ascii(std::string_view text);

// ---------------------------------------------------------------- music and display formatting

/// Musical mode stored with a pitch class. Spelling (C# vs Db) is display-only.
enum class KeyMode { Major, Minor, Unknown };

std::string_view to_string(KeyMode mode) noexcept;  ///< "major" / "minor" / "unknown"
std::optional<KeyMode> parse_key_mode(std::string_view text) noexcept;

/// A key estimate or override: pitch class 0 = C ... 11 = B.
struct MusicalKey {
    int pitch_class = 0;
    KeyMode mode = KeyMode::Unknown;
    bool operator==(const MusicalKey&) const = default;
};

/// "F#m", "C", "A?" (unknown mode).
std::string format_key(const MusicalKey& key);
/// Parses user or tag text such as "F#minor", "Am", "Bb maj", "C sharp minor".
std::optional<MusicalKey> parse_key(std::string_view text);

/// "1.25s" below ten seconds, "m:ss" above.
std::string format_duration(std::int64_t microseconds);
/// "512 B", "3.4 MiB" ...
std::string format_byte_size(std::int64_t bytes);
/// "128.00" from milli-BPM.
std::string format_bpm(std::int64_t milli_bpm);

}  // namespace mso
