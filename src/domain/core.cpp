#include "mso/domain/core.hpp"

#include <utf8proc.h>

#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <format>
#include <random>
#include <utility>

namespace mso {

// ---------------------------------------------------------------- UUIDs

uuids::uuid new_uuid() {
    // libc++ implements std::random_device with rand_s (RtlGenRandom) on Windows and the kernel
    // entropy source elsewhere: a CSPRNG, as identifiers must not be predictable.
    static thread_local std::random_device entropy;
    static thread_local uuids::basic_uuid_random_generator<std::random_device> generator(entropy);
    return generator();
}

std::int64_t now_unix_ms() noexcept {
    using namespace std::chrono;
    return duration_cast<milliseconds>(system_clock::now().time_since_epoch()).count();
}

// ---------------------------------------------------------------- text

namespace {

/// Applies a utf8proc normalization to well-formed UTF-8; returns the input unchanged if utf8proc
/// rejects it (it cannot for valid input, but the key must stay deterministic regardless).
std::string map_valid_utf8(std::string_view utf8, utf8proc_option_t options) {
    if (utf8.empty()) return {};
    utf8proc_uint8_t* mapped = nullptr;
    const utf8proc_ssize_t length = utf8proc_map(reinterpret_cast<const utf8proc_uint8_t*>(utf8.data()),
                                                 static_cast<utf8proc_ssize_t>(utf8.size()), &mapped, options);
    std::string result = (length >= 0 && mapped)
                             ? std::string(reinterpret_cast<const char*>(mapped), static_cast<std::size_t>(length))
                             : std::string(utf8);
    std::free(mapped);
    return result;
}

/// Normalizes every maximal well-formed run of `utf8` and copies malformed bytes verbatim between
/// them, so a stray invalid byte never disables folding of the rest of the text ("KICK\xFF" and
/// "kick\xFF" fold alike) while distinct invalid bytes stay distinct.
std::string map_utf8_runs(std::string_view utf8, utf8proc_option_t options) {
    const auto* bytes = reinterpret_cast<const utf8proc_uint8_t*>(utf8.data());
    const auto size = static_cast<utf8proc_ssize_t>(utf8.size());
    std::string result;
    result.reserve(utf8.size());
    utf8proc_ssize_t run_start = 0;
    utf8proc_ssize_t offset = 0;
    while (offset < size) {
        utf8proc_int32_t code_point = 0;
        const utf8proc_ssize_t consumed = utf8proc_iterate(bytes + offset, size - offset, &code_point);
        if (consumed > 0) {
            offset += consumed;
            continue;
        }
        result += map_valid_utf8(
            utf8.substr(static_cast<std::size_t>(run_start), static_cast<std::size_t>(offset - run_start)), options);
        result.push_back(static_cast<char>(bytes[offset]));
        run_start = ++offset;
    }
    result += map_valid_utf8(utf8.substr(static_cast<std::size_t>(run_start)), options);
    return result;
}

/// Separator conversion of `path_key`: '\' is a path separator only on Windows.
constexpr utf8proc_int32_t native_separator_to_slash(utf8proc_int32_t code_point) noexcept {
#ifdef _WIN32
    return code_point == U'\\' ? U'/' : code_point;
#else
    return code_point;
#endif
}

/// The case-insensitive key of one code point, as close to NTFS's $UpCase table as utf8proc allows.
/// That table covers only the Basic Multilingual Plane, and maps a letter to its uppercase only when
/// the pair round-trips: "ſ" (long s), dotless "ı", the micro sign "µ", final "ς" and title-case
/// digraphs such as "ǅ" keep their own keys, because NTFS keeps such files apart (checked on
/// Windows 11 with real files, tests/domain_tests.cpp). Letters whose case pair is newer than the
/// volume's table (Georgian Mtavruli, Cherokee small letters, some Latin Extended-D letters) are
/// still mapped: two files that differ only in such a letter, side by side, share one key.
utf8proc_int32_t case_insensitive_key(utf8proc_int32_t code_point) noexcept {
    constexpr utf8proc_int32_t kLastBasicMultilingualPlaneCodePoint = 0xFFFF;
    const utf8proc_int32_t upper = utf8proc_toupper(code_point);
    if (upper == code_point || code_point > kLastBasicMultilingualPlaneCodePoint ||
        upper > kLastBasicMultilingualPlaneCodePoint || utf8proc_tolower(upper) != code_point)
        return code_point;
    return upper;
}

}  // namespace

std::string fold_key(std::string_view utf8) {
    const auto options = static_cast<utf8proc_option_t>(UTF8PROC_STABLE | UTF8PROC_COMPOSE | UTF8PROC_COMPAT |
                                                        UTF8PROC_CASEFOLD | UTF8PROC_IGNORE);
    const std::string normalized = map_utf8_runs(utf8, options);

    // Collapse whitespace runs (NFKC already mapped exotic spaces to U+0020) and trim.
    std::string key;
    key.reserve(normalized.size());
    bool pending_space = false;
    for (const char character : normalized) {
        if (character == ' ' || character == '\t' || character == '\n' || character == '\r') {
            pending_space = !key.empty();
            continue;
        }
        if (pending_space) {
            key.push_back(' ');
            pending_space = false;
        }
        key.push_back(character);
    }
    return key;
}

std::string path_key(std::string_view relative_path, bool case_sensitive) {
    if (case_sensitive) {
        std::string key(relative_path);
#ifdef _WIN32
        std::ranges::replace(key, '\\', '/');
#endif
        return key;
    }
    // Simple (1:1) uppercase mapping per code point (case_insensitive_key), no normalization, like NTFS's $UpCase
    // comparison: "Cafe\u0301" and "Caf\u00e9", "İ" and "i", or the Kelvin sign and "k" are
    // different files there, so they keep different keys.
    std::string key;
    key.reserve(relative_path.size());
    const auto* bytes = reinterpret_cast<const utf8proc_uint8_t*>(relative_path.data());
    const auto size = static_cast<utf8proc_ssize_t>(relative_path.size());
    for (utf8proc_ssize_t offset = 0; offset < size;) {
        utf8proc_int32_t code_point = 0;
        const utf8proc_ssize_t consumed = utf8proc_iterate(bytes + offset, size - offset, &code_point);
        if (consumed <= 0) {  // keep malformed bytes verbatim so the key stays deterministic
            key.push_back(static_cast<char>(bytes[offset]));
            ++offset;
            continue;
        }
        offset += consumed;
        utf8proc_uint8_t encoded[4];
        const auto written = utf8proc_encode_char(case_insensitive_key(native_separator_to_slash(code_point)), encoded);
        key.append(reinterpret_cast<const char*>(encoded), static_cast<std::size_t>(written));
    }
    return key;
}

std::string fold_path_key(std::string_view relative_path_utf8) {
    return path_key(relative_path_utf8, false);
}

bool is_valid_utf8(std::string_view text) noexcept {
    const auto* bytes = reinterpret_cast<const utf8proc_uint8_t*>(text.data());
    utf8proc_ssize_t offset = 0;
    const auto size = static_cast<utf8proc_ssize_t>(text.size());
    while (offset < size) {
        utf8proc_int32_t code_point = 0;
        const utf8proc_ssize_t consumed = utf8proc_iterate(bytes + offset, size - offset, &code_point);
        if (consumed <= 0) return false;
        offset += consumed;
    }
    return true;
}

std::string_view utf8_prefix(std::string_view utf8, std::size_t maximum_bytes) noexcept {
    if (utf8.size() <= maximum_bytes) return utf8;
    std::size_t end = maximum_bytes;
    const auto is_continuation_byte = [&](std::size_t index) {
        return (static_cast<unsigned char>(utf8[index]) & 0xC0U) == 0x80U;
    };
    // Back up over the continuation bytes of the sequence that the cut would split (at most three).
    for (int step = 0; step < 3 && end > 0 && is_continuation_byte(end); ++step)
        --end;
    return utf8.substr(0, end);
}

std::size_t count_code_points(std::string_view utf8) noexcept {
    const auto* bytes = reinterpret_cast<const utf8proc_uint8_t*>(utf8.data());
    const auto size = static_cast<utf8proc_ssize_t>(utf8.size());
    std::size_t count = 0;
    for (utf8proc_ssize_t offset = 0; offset < size; ++count) {
        utf8proc_int32_t code_point = 0;
        const utf8proc_ssize_t consumed = utf8proc_iterate(bytes + offset, size - offset, &code_point);
        offset += consumed > 0 ? consumed : 1;
    }
    return count;
}

bool has_letter_or_digit(std::string_view utf8) noexcept {
    const auto* bytes = reinterpret_cast<const utf8proc_uint8_t*>(utf8.data());
    const auto size = static_cast<utf8proc_ssize_t>(utf8.size());
    for (utf8proc_ssize_t offset = 0; offset < size;) {
        utf8proc_int32_t code_point = 0;
        const utf8proc_ssize_t consumed = utf8proc_iterate(bytes + offset, size - offset, &code_point);
        if (consumed <= 0) {
            ++offset;
            continue;
        }
        offset += consumed;
        switch (utf8proc_category(code_point)) {
            case UTF8PROC_CATEGORY_LU:
            case UTF8PROC_CATEGORY_LL:
            case UTF8PROC_CATEGORY_LT:
            case UTF8PROC_CATEGORY_LM:
            case UTF8PROC_CATEGORY_LO:
            case UTF8PROC_CATEGORY_ND:
            case UTF8PROC_CATEGORY_NL:
            case UTF8PROC_CATEGORY_NO:
                return true;
            default:
                break;
        }
    }
    return false;
}

std::string path_to_utf8(const std::filesystem::path& path) {
    const auto text = path.generic_u8string();
    return std::string(reinterpret_cast<const char*>(text.data()), text.size());
}

std::filesystem::path path_from_utf8(std::string_view utf8) {
    return std::filesystem::path(std::u8string(reinterpret_cast<const char8_t*>(utf8.data()), utf8.size()));
}

namespace {

#ifdef _WIN32
constexpr std::string_view kPathSeparators = "/\\";

/// True for Win32 device names, which open a device in every directory: CON, PRN, AUX, NUL,
/// CONIN$, CONOUT$, COM0-9 and LPT0-9 (also with the superscript digits 1-3), with any extension
/// and trailing spaces ("nul.wav", "COM1 .txt").
bool is_reserved_windows_name(std::string_view segment) noexcept {
    std::string_view base = segment.substr(0, segment.find('.'));
    while (!base.empty() && base.back() == ' ')
        base.remove_suffix(1);
    if (base.size() < 3 || base.size() > 7) return false;
    std::string lowered(base);
    for (char& character : lowered)
        if (character >= 'A' && character <= 'Z') character = static_cast<char>(character + ('a' - 'A'));
    if (lowered == "con" || lowered == "prn" || lowered == "aux" || lowered == "nul" || lowered == "conin$" ||
        lowered == "conout$")
        return true;
    if (!lowered.starts_with("com") && !lowered.starts_with("lpt")) return false;
    const std::string_view digit = std::string_view(lowered).substr(3);
    return (digit.size() == 1 && digit[0] >= '0' && digit[0] <= '9') || digit == "\u00b9" || digit == "\u00b2" ||
           digit == "\u00b3";
}

/// True for a segment Win32 would reinterpret or reject: an alternate data stream (':'), a
/// trailing dot or space (Win32 strips them, so "a." aliases "a"), a reserved device name, a
/// control character or one of <>"|?*.
bool is_unsafe_windows_segment(std::string_view segment) noexcept {
    if (segment.back() == '.' || segment.back() == ' ') return true;
    for (const char character : segment) {
        if (static_cast<unsigned char>(character) < 0x20) return true;
        if (std::string_view(":<>\"|?*").find(character) != std::string_view::npos) return true;
    }
    return is_reserved_windows_name(segment);
}
#else
constexpr std::string_view kPathSeparators = "/";
#endif

}  // namespace

bool is_safe_relative_path(std::string_view relative) noexcept {
    constexpr std::size_t kMaxWindowsPathUtf16 = 32767;
    if (relative.empty() || relative.size() > kMaxWindowsPathUtf16 * 3) return false;
    if (relative.find('\0') != std::string_view::npos) return false;
    if (kPathSeparators.find(relative.front()) != std::string_view::npos) return false;
    // A drive prefix is rejected on every platform so a catalog never stores a path that Windows
    // would resolve against a drive instead of the root.
    if (relative.size() >= 2 && relative[1] == ':') return false;
    std::size_t segment_start = 0;
    while (segment_start <= relative.size()) {
        auto segment_end = relative.find_first_of(kPathSeparators, segment_start);
        if (segment_end == std::string_view::npos) segment_end = relative.size();
        const auto segment = relative.substr(segment_start, segment_end - segment_start);
        if (segment.empty() || segment == "." || segment == "..") return false;
#ifdef _WIN32
        if (is_unsafe_windows_segment(segment)) return false;
#endif
        segment_start = segment_end + 1;
    }
    return true;
}

std::string trim(std::string_view text) {
    const auto first = text.find_first_not_of(" \t\r\n");
    if (first == std::string_view::npos) return {};
    const auto last = text.find_last_not_of(" \t\r\n");
    return std::string(text.substr(first, last - first + 1));
}

std::string to_lower_ascii(std::string_view text) {
    std::string lowered(text);
    for (char& character : lowered)
        if (character >= 'A' && character <= 'Z') character = static_cast<char>(character + ('a' - 'A'));
    return lowered;
}

// ---------------------------------------------------------------- music and display formatting

std::string_view to_string(KeyMode mode) noexcept {
    switch (mode) {
        case KeyMode::Major:
            return "major";
        case KeyMode::Minor:
            return "minor";
        case KeyMode::Unknown:
            return "unknown";
    }
    return "unknown";
}

std::optional<KeyMode> parse_key_mode(std::string_view text) noexcept {
    if (text == "major") return KeyMode::Major;
    if (text == "minor") return KeyMode::Minor;
    if (text == "unknown") return KeyMode::Unknown;
    return std::nullopt;
}

std::string format_key(const MusicalKey& key) {
    static constexpr std::string_view kSharpNames[12] = {"C",  "C#", "D",  "D#", "E",  "F",
                                                         "F#", "G",  "G#", "A",  "A#", "B"};
    if (key.pitch_class < 0 || key.pitch_class > 11) return {};
    std::string text(kSharpNames[key.pitch_class]);
    if (key.mode == KeyMode::Minor) text += "m";
    if (key.mode == KeyMode::Unknown) text += "?";
    return text;
}

std::optional<MusicalKey> parse_key(std::string_view text) {
    std::string compact = to_lower_ascii(trim(text));
    // Unicode accidentals: U+266F MUSIC SHARP SIGN and U+266D MUSIC FLAT SIGN.
    constexpr std::pair<std::string_view, std::string_view> kAccidentals[] = {{"\u266f", "#"}, {"\u266d", "b"}};
    for (const auto& [symbol, ascii] : kAccidentals)
        for (auto found = compact.find(symbol); found != std::string::npos; found = compact.find(symbol, found))
            compact.replace(found, symbol.size(), ascii);
    std::erase_if(compact, [](char character) { return character == ' ' || character == '_' || character == '-'; });
    if (compact.empty() || compact[0] < 'a' || compact[0] > 'g') return std::nullopt;

    static constexpr int kNaturalPitchClass[7] = {9, 11, 0, 2, 4, 5, 7};  // a b c d e f g
    MusicalKey key;
    key.pitch_class = kNaturalPitchClass[compact[0] - 'a'];
    std::string_view rest = std::string_view(compact).substr(1);
    if (rest.starts_with("#") || rest.starts_with("sharp")) {
        key.pitch_class = (key.pitch_class + 1) % 12;
        rest.remove_prefix(rest.starts_with("#") ? 1 : 5);
    } else if (rest.starts_with("flat")) {
        key.pitch_class = (key.pitch_class + 11) % 12;
        rest.remove_prefix(4);
    } else if (rest.starts_with("b")) {  // "Bb", "Ebm": a 'b' after the letter is a flat sign
        key.pitch_class = (key.pitch_class + 11) % 12;
        rest.remove_prefix(1);
    }
    if (rest.empty() || rest == "maj" || rest == "major") {
        key.mode = KeyMode::Major;
    } else if (rest == "m" || rest == "min" || rest == "minor") {
        key.mode = KeyMode::Minor;
    } else {
        return std::nullopt;
    }
    return key;
}

std::string format_duration(std::int64_t microseconds) {
    if (microseconds < 0) return {};
    // Below 9.995 s two decimals never round up to "10.00s"; from there on the m:ss form is used.
    if (microseconds < 9'995'000) return std::format("{:.2f}s", static_cast<double>(microseconds) / 1e6);
    // Rounded like the two-decimal form above: 9.995 s is "0:10", never the smaller "0:09".
    const auto seconds = (microseconds + 500'000) / 1'000'000;
    return std::format("{}:{:02}", seconds / 60, seconds % 60);
}

std::string format_byte_size(std::int64_t bytes) {
    constexpr double kKibibyte = 1024.0;
    const auto value = static_cast<double>(bytes);
    if (bytes < 1024) return std::format("{} B", bytes);
    if (value < kKibibyte * kKibibyte) return std::format("{:.1f} KiB", value / kKibibyte);
    if (value < kKibibyte * kKibibyte * kKibibyte) return std::format("{:.1f} MiB", value / (kKibibyte * kKibibyte));
    return std::format("{:.2f} GiB", value / (kKibibyte * kKibibyte * kKibibyte));
}

std::string format_bpm(std::int64_t milli_bpm) {
    return std::format("{:.2f}", static_cast<double>(milli_bpm) / 1000.0);
}

}  // namespace mso
