#include "mso/domain/core.hpp"

#include <utf8proc.h>

#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <format>
#include <random>

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

std::string fold_key(std::string_view utf8) {
    utf8proc_uint8_t* mapped = nullptr;
    const auto options = static_cast<utf8proc_option_t>(UTF8PROC_STABLE | UTF8PROC_COMPOSE | UTF8PROC_COMPAT |
                                                        UTF8PROC_CASEFOLD | UTF8PROC_IGNORE);
    const utf8proc_ssize_t length = utf8proc_map(reinterpret_cast<const utf8proc_uint8_t*>(utf8.data()),
                                                 static_cast<utf8proc_ssize_t>(utf8.size()), &mapped, options);
    std::string normalized;
    if (length >= 0 && mapped) {
        normalized.assign(reinterpret_cast<const char*>(mapped), static_cast<std::size_t>(length));
    } else {
        normalized.assign(utf8);  // malformed input: keep bytes so the key is still deterministic
    }
    std::free(mapped);

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

std::string fold_path_key(std::string_view relative_path_utf8) {
    // NFC, then simple (1:1) lowercase mapping per code point: like NTFS's case-insensitive
    // comparison, and unlike full case folding, it never merges distinct names such as "ﬁ"/"fi".
    utf8proc_uint8_t* composed = nullptr;
    const utf8proc_ssize_t length = utf8proc_map(reinterpret_cast<const utf8proc_uint8_t*>(relative_path_utf8.data()),
                                                 static_cast<utf8proc_ssize_t>(relative_path_utf8.size()), &composed,
                                                 static_cast<utf8proc_option_t>(UTF8PROC_STABLE | UTF8PROC_COMPOSE));
    const std::string normalized = (length >= 0 && composed) ? std::string(reinterpret_cast<const char*>(composed),
                                                                           static_cast<std::size_t>(length))
                                                             : std::string(relative_path_utf8);
    std::free(composed);

    std::string key;
    key.reserve(normalized.size());
    const auto* bytes = reinterpret_cast<const utf8proc_uint8_t*>(normalized.data());
    const auto size = static_cast<utf8proc_ssize_t>(normalized.size());
    for (utf8proc_ssize_t offset = 0; offset < size;) {
        utf8proc_int32_t code_point = 0;
        const utf8proc_ssize_t consumed = utf8proc_iterate(bytes + offset, size - offset, &code_point);
        if (consumed <= 0) {  // keep malformed bytes verbatim so the key stays deterministic
            key.push_back(static_cast<char>(bytes[offset]));
            ++offset;
            continue;
        }
        offset += consumed;
        if (code_point == U'\\') code_point = U'/';
        utf8proc_uint8_t encoded[4];
        const auto written = utf8proc_encode_char(utf8proc_tolower(code_point), encoded);
        key.append(reinterpret_cast<const char*>(encoded), static_cast<std::size_t>(written));
    }
    return key;
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

std::string path_to_utf8(const std::filesystem::path& path) {
    const auto text = path.generic_u8string();
    return std::string(reinterpret_cast<const char*>(text.data()), text.size());
}

std::filesystem::path path_from_utf8(std::string_view utf8) {
    return std::filesystem::path(std::u8string(reinterpret_cast<const char8_t*>(utf8.data()), utf8.size()));
}

bool is_safe_relative_path(std::string_view relative) noexcept {
    constexpr std::size_t kMaxWindowsPathUtf16 = 32767;
    if (relative.empty() || relative.size() > kMaxWindowsPathUtf16 * 3) return false;
    if (relative.find('\0') != std::string_view::npos) return false;
    if (relative.front() == '/' || relative.front() == '\\') return false;
    if (relative.size() >= 2 && relative[1] == ':') return false;  // drive letter
    std::size_t segment_start = 0;
    while (segment_start <= relative.size()) {
        auto segment_end = relative.find_first_of("/\\", segment_start);
        if (segment_end == std::string_view::npos) segment_end = relative.size();
        const auto segment = relative.substr(segment_start, segment_end - segment_start);
        if (segment.empty() || segment == "." || segment == "..") return false;
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
    if (microseconds < 10'000'000) return std::format("{:.2f}s", static_cast<double>(microseconds) / 1e6);
    const auto seconds = microseconds / 1'000'000;
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
