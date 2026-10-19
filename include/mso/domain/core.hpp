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
[[nodiscard]] uuids::uuid new_uuid();

/// Current UTC time in Unix milliseconds, the unit of every persisted timestamp.
[[nodiscard]] std::int64_t now_unix_ms() noexcept;

// ---------------------------------------------------------------- text

/// Version tag of the key normalizations below (fold_key and path_key). Bump it whenever either
/// output can change: a library stores it (library_state.normalization_version), and opening a
/// library written with another version rebuilds every stored name and path key.
/// Version 2: fold_key folds the valid parts of malformed UTF-8; case-insensitive path keys use the
/// simple uppercase mapping without normalization (version 1: NFC plus lowercase).
inline constexpr std::string_view kFoldVersion = "utf8proc-2.12-nfkc-casefold-2;path-simple-uppercase-1";

/// Normalized comparison key: Unicode NFKC + case folding (deps/utf8proc) with whitespace runs
/// collapsed to one space and trimmed. Used for sort keys, sibling uniqueness and lookups;
/// original text is always kept separately for display.
[[nodiscard]] std::string fold_key(std::string_view utf8);

/// Location identity key of a root-relative path (`locations.path_key`, `folders.path_key`).
/// On a case-insensitive root (`case_sensitive == false`) each code point is mapped with the simple
/// 1:1 Unicode uppercase mapping and nothing is normalized, which is how NTFS compares names (its
/// $UpCase table): composed and decomposed spellings, "İ" and "i", or the Kelvin sign and "k" are
/// different files and keep different keys, while "Kick.WAV" and "kick.wav" share one. APFS also
/// ignores normalization, but a file whose name changes normalization between scans is relinked
/// by its file identity, so one scheme serves every platform and a library stays portable. On a
/// case-sensitive root the path is kept exactly. On Windows `\` also becomes `/` (it is a separator
/// there); on POSIX it is an ordinary file-name character and is kept. Malformed UTF-8 bytes are
/// kept verbatim so the key stays deterministic.
[[nodiscard]] std::string path_key(std::string_view relative_path, bool case_sensitive);

/// The case-insensitive `path_key` (`path_key(relative_path, false)`), used for user-typed folder
/// filters and roots stored as case-insensitive.
[[nodiscard]] std::string fold_path_key(std::string_view relative_path_utf8);

/// True when the bytes are well-formed UTF-8.
[[nodiscard]] bool is_valid_utf8(std::string_view text) noexcept;
/// Number of Unicode code points; malformed bytes count as one each.
[[nodiscard]] std::size_t count_code_points(std::string_view utf8) noexcept;
/// True when the text contains at least one Unicode letter or digit (general category L* or N*).
/// Text without one (e.g. "!!!") produces no full-text tokens.
[[nodiscard]] bool has_letter_or_digit(std::string_view utf8) noexcept;

/// Converts a filesystem path to UTF-8 with '/' separators (SQLite storage form).
[[nodiscard]] std::string path_to_utf8(const std::filesystem::path& path);
/// Converts stored UTF-8 back to a native (UTF-16 on Windows) path.
[[nodiscard]] std::filesystem::path path_from_utf8(std::string_view utf8);
/// Accepts only a non-empty relative path that stays inside its root: no NUL, no leading
/// separator, no drive prefix ("C:"), no empty, "." or ".." segment. Windows additionally
/// rejects what Win32 would reinterpret: ':' anywhere (alternate data streams), reserved device
/// names (CON, PRN, AUX, NUL, COM1-9, LPT1-9, with or without an extension), segments ending in
/// '.' or ' ', control characters and `<>"|?*`. Separators are '/' everywhere and also '\' on Windows.
[[nodiscard]] bool is_safe_relative_path(std::string_view relative) noexcept;

/// Removes leading and trailing ASCII whitespace (space, tab, CR, LF).
[[nodiscard]] std::string trim(std::string_view text);
/// Lowercases ASCII letters only; every other byte (including UTF-8 sequences) is unchanged.
[[nodiscard]] std::string to_lower_ascii(std::string_view text);

// ---------------------------------------------------------------- music and display formatting

/// Musical mode stored with a pitch class. Spelling (C# vs Db) is display-only.
enum class KeyMode { Major, Minor, Unknown };

/// Storage name of a mode: "major", "minor" or "unknown".
[[nodiscard]] std::string_view to_string(KeyMode mode) noexcept;
/// Inverse of `to_string(KeyMode)`: accepts exactly "major", "minor" or "unknown", else nullopt.
[[nodiscard]] std::optional<KeyMode> parse_key_mode(std::string_view text) noexcept;

/// A key estimate or override: pitch class 0 = C ... 11 = B.
struct MusicalKey {
    int pitch_class = 0;              ///< 0 = C ... 11 = B
    KeyMode mode = KeyMode::Unknown;  ///< major, minor or not known
    bool operator==(const MusicalKey&) const = default;
};

/// "F#m", "C", "A?" (unknown mode).
[[nodiscard]] std::string format_key(const MusicalKey& key);
/// Parses user or tag text such as "F#minor", "Am", "Bb maj", "C sharp minor", "F♯m", "B♭".
[[nodiscard]] std::optional<MusicalKey> parse_key(std::string_view text);

/// "1.25s" below ten seconds, "m:ss" above.
[[nodiscard]] std::string format_duration(std::int64_t microseconds);
/// "512 B", "3.4 MiB" ...
[[nodiscard]] std::string format_byte_size(std::int64_t bytes);
/// "128.00" from milli-BPM.
[[nodiscard]] std::string format_bpm(std::int64_t milli_bpm);

}  // namespace mso
