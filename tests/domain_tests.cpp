// Domain primitives: errors, normalization keys, UTF-8 helpers, path safety and conversion,
// taxonomy paths, analysis flags, musical keys, UUIDs and display formatting.
#include <doctest.h>

#include <array>
#include <cstdint>
#include <filesystem>
#include <format>
#include <fstream>
#include <set>
#include <string>
#include <variant>
#include <vector>

#include "mso/domain/analysis_flags.hpp"
#include "mso/domain/core.hpp"
#include "mso/domain/error.hpp"
#include "mso/domain/json_fields.hpp"
#include "mso/domain/overloaded.hpp"
#include "mso/domain/taxonomy.hpp"
#include "test_support.hpp"

using namespace mso;

TEST_SUITE("domain") {
    TEST_CASE("error codes have stable wire names and messages") {
        CHECK(to_string(ErrorCode::SourceOffline) == "SOURCE_OFFLINE");
        CHECK(to_string(ErrorCode::StaleCursor) == "STALE_CURSOR");
        CHECK(to_string(ErrorCode::IdempotencyMismatch) == "IDEMPOTENCY_MISMATCH");
        CHECK(to_string(ErrorCode::Internal) == "INTERNAL");
        CHECK(to_string(static_cast<ErrorCode>(200)) == "INTERNAL");
        std::set<std::string_view> names;
        for (int code = 0; code <= static_cast<int>(ErrorCode::Internal); ++code)
            names.insert(to_string(static_cast<ErrorCode>(code)));
        CHECK(names.size() == static_cast<std::size_t>(ErrorCode::Internal) + 1);  // every code is distinct

        CHECK(Error{ErrorCode::NotFound}.message() == "NOT_FOUND");
        CHECK(Error{ErrorCode::DbBusy, true, "catalog locked"}.message() == "DB_BUSY: catalog locked");
        Error parse{ErrorCode::ParseError, false, "unexpected input", std::nullopt, 7u};
        CHECK(parse.message() == "PARSE_ERROR at 7: unexpected input");

        const Result<int> failed = fail(ErrorCode::DiskFull, "no space", true);
        REQUIRE_FALSE(failed.has_value());
        CHECK(failed.error().code == ErrorCode::DiskFull);
        CHECK(failed.error().retryable);
        CHECK(failed.error().safe_detail == "no space");
        CHECK_FALSE(failed.error().sample_id.has_value());
        CHECK_FALSE(failed.error().position.has_value());
    }

    TEST_CASE("fold_key normalizes case, compatibility forms and whitespace") {
        CHECK(fold_key("  Kick   DRUM ") == "kick drum");
        CHECK(fold_key("ＫＩＣＫ") == "kick");             // fullwidth → ASCII (NFKC)
        CHECK(fold_key("Straße") == fold_key("STRASSE"));  // full case folding
        CHECK(fold_key("Ünïcödé") == fold_key("ÜNÏCÖDÉ"));
        CHECK(fold_key("שלום") == "שלום");                       // caseless scripts pass through
        CHECK(fold_key("Cafe\u0301") == fold_key("Caf\u00e9"));  // composed and decomposed forms agree
        CHECK(fold_key("") == "");
    }

    TEST_CASE("fold_key still folds the valid parts of malformed UTF-8") {
        CHECK(fold_key("KICK\xFF") == "kick\xFF");
        CHECK(fold_key("KICK\xFF") == fold_key("kick\xFF"));
        CHECK(fold_key("\xFF") != fold_key("\xFE"));  // distinct invalid bytes stay distinct
        CHECK(fold_key("A\xC3 B") == "a\xC3 b");      // truncated sequence kept verbatim
        CHECK(fold_path_key("DIR\xFF/Kick.WAV") == "DIR\xFF/KICK.WAV");
    }

    TEST_CASE("path keys fold case on case-insensitive roots and keep it on case-sensitive ones") {
        CHECK(path_key("Drums/Kick.WAV", false) == "DRUMS/KICK.WAV");
        CHECK(path_key("Drums/Kick.WAV", true) == "Drums/Kick.WAV");
        CHECK(path_key("Kick.wav", true) != path_key("kick.wav", true));
        CHECK(path_key("Kick.wav", false) == path_key("kick.wav", false));
        CHECK(fold_path_key("Drums/Kick.WAV") == path_key("Drums/Kick.WAV", false));
        CHECK(fold_path_key("ﬁle.wav") != fold_path_key("file.wav"));  // no compatibility mapping for paths
        // Like NTFS: simple uppercase per code point, no normalization, so names NTFS keeps apart
        // keep different keys, and names it treats as one file share a key.
        CHECK(fold_path_key("Caf\u00e9.wav") == fold_path_key("CAF\u00c9.WAV"));
        CHECK(fold_path_key("Cafe\u0301.wav") != fold_path_key("Caf\u00e9.wav"));    // decomposed vs composed
        CHECK(fold_path_key("\u0130.wav") != fold_path_key("i.wav"));                // dotted capital I vs i
        CHECK(fold_path_key("\u212A.wav") != fold_path_key("k.wav"));                // Kelvin sign vs k
        CHECK(fold_path_key("Stra\u00dfe") != fold_path_key("STRASSE"));             // no full case folding
        CHECK(path_key("Cafe\u0301.wav", true) != path_key("Caf\u00e9.wav", true));  // exact bytes
#ifdef _WIN32
        CHECK(fold_path_key("Drums\\Kick.WAV") == "DRUMS/KICK.WAV");  // '\' is a separator on Windows
        CHECK(path_key("Drums\\Kick.WAV", true) == "Drums/Kick.WAV");
#else
        CHECK(fold_path_key("a\\b.wav") == "a\\b.wav");  // a legal file-name character on POSIX
        CHECK(fold_path_key("a\\b.wav") != fold_path_key("a/b.wav"));
        CHECK(path_key("A\\B.wav", true) == "A\\B.wav");
#endif
    }

    TEST_CASE("case-insensitive path keys follow NTFS for the pinned letter pairs, known exceptions included") {
        struct LetterPair {
            std::string_view first, second;
            bool same_file;  ///< what NTFS does (checked below on Windows with real files)
            bool same_key;   ///< what path_key does: equal to same_file except for the documented exceptions
        };
        constexpr std::array<LetterPair, 14> kPairs = {{
            {"k", "K", true, true},
            {"\u00e9", "\u00c9", true, true},            // e acute
            {"\u03c3", "\u03a3", true, true},            // sigma
            {"\u0434", "\u0414", true, true},            // Cyrillic de
            {"\u017f", "S", false, false},               // long s
            {"\u0131", "I", false, false},               // dotless i
            {"\u00b5", "\u039c", false, false},          // micro sign vs capital mu
            {"\u03c2", "\u03a3", false, false},          // final sigma
            {"\u01c5", "\u01c4", false, false},          // title-case digraph vs uppercase
            {"\u0130", "i", false, false},               // dotted capital I
            {"\u212a", "k", false, false},               // Kelvin sign
            {"\U00010428", "\U00010400", false, false},  // Deseret: outside the Basic Multilingual Plane
            // Case pairs newer than the volume's $UpCase table: NTFS keeps them apart, path_key does not
            // (utf8proc has no Unicode age to tell them apart; see path_key in core.hpp).
            {"\u00df", "\u1e9e", false, true},  // sharp s vs capital sharp s (Unicode 5.1)
            {"\u10d0", "\u1c90", false, true},  // Georgian Mkhedruli vs Mtavruli (Unicode 11)
        }};
        for (const auto& [first, second, same_file, same_key] : kPairs) {
            CAPTURE(first);
            CAPTURE(second);
            CHECK((fold_path_key(std::format("x{}.wav", first)) == fold_path_key(std::format("x{}.wav", second))) ==
                  same_key);
        }
#ifdef _WIN32
        // The pins are what NTFS does: a file created under the first spelling is found, or not, under
        // the second.
        test::ScratchDirectory scratch;
        for (const auto& [first, second, same_file, same_key] : kPairs) {
            CAPTURE(first);
            CAPTURE(second);
            const auto created = scratch / std::format("x{}.wav", first);
            REQUIRE(std::ofstream(created).good());
            std::error_code error;
            CHECK(std::filesystem::exists(scratch / std::format("x{}.wav", second), error) == same_file);
            std::filesystem::remove(created, error);
        }
#endif
    }

    TEST_CASE("utf8_prefix shortens text without splitting a UTF-8 sequence") {
        CHECK(utf8_prefix("Kick", 10) == "Kick");
        CHECK(utf8_prefix("Kick", 2) == "Ki");
        CHECK(utf8_prefix("Kick", 0).empty());
        const std::string_view hebrew = "של";  // two 2-byte letters
        CHECK(utf8_prefix(hebrew, 3) == "ש");
        CHECK(utf8_prefix(hebrew, 4) == hebrew);
        const std::string_view note = "a♯";  // '♯' is 3 bytes
        CHECK(utf8_prefix(note, 1) == "a");
        CHECK(utf8_prefix(note, 2) == "a");
        CHECK(utf8_prefix(note, 3) == "a");
        CHECK(utf8_prefix("\U0001F3B9x", 3).empty());         // inside a 4-byte sequence
        CHECK(utf8_prefix("\xFF\xFF\xFF", 2) == "\xFF\xFF");  // malformed bytes are cut where asked
        for (std::size_t length = 0; length <= 12; ++length)
            CHECK(is_valid_utf8(utf8_prefix("ש♯\U0001F3B9ab", length)));
    }

    TEST_CASE("UTF-8 helpers validate, count and classify") {
        CHECK(is_valid_utf8(""));
        CHECK(is_valid_utf8("plain"));
        CHECK(is_valid_utf8("שלום サンプル ♯"));
        CHECK_FALSE(is_valid_utf8("\xFF"));
        CHECK_FALSE(is_valid_utf8("ok\xC3"));        // truncated two-byte sequence
        CHECK_FALSE(is_valid_utf8("\xED\xA0\x80"));  // UTF-16 surrogate
        CHECK_FALSE(is_valid_utf8("\xC0\xAF"));      // overlong '/'
        CHECK(count_code_points("") == 0);
        CHECK(count_code_points("abc") == 3);
        CHECK(count_code_points("שלום") == 4);
        CHECK(count_code_points("a\xFF\xFE"
                                "b") == 4);  // malformed bytes count one each
        CHECK(has_letter_or_digit("kick"));
        CHECK(has_letter_or_digit("808"));
        CHECK(has_letter_or_digit("!!!שלום"));
        CHECK(has_letter_or_digit("\u00bd"));  // ½ is a number (No)
        CHECK_FALSE(has_letter_or_digit("!!!"));
        CHECK_FALSE(has_letter_or_digit("*?-_ \u266f"));
        CHECK_FALSE(has_letter_or_digit(""));
    }

    TEST_CASE("paths round-trip between native form and stored UTF-8") {
        for (const auto text : {"Drums/Kick 01.wav", "שלום/vox.wav", "サンプル/音.flac", "Café/Ünïcode.aiff", "a"}) {
            const std::string_view shown_text = text;
            CAPTURE(shown_text);
            const auto path = path_from_utf8(text);
            CHECK(path_to_utf8(path) == text);
            CHECK(path_from_utf8(path_to_utf8(path)) == path);
        }
#ifdef _WIN32
        CHECK(path_to_utf8(std::filesystem::path(L"Drums\\Kick.wav")) == "Drums/Kick.wav");  // stored with '/'
#endif
        CHECK(path_to_utf8(path_from_utf8("a") / "b" / "c.wav") == "a/b/c.wav");
    }

    TEST_CASE("relative paths that could escape a root are rejected") {
        CHECK(is_safe_relative_path("Drums/Kick 01.wav"));
        CHECK(is_safe_relative_path("שלום/vox.wav"));
        CHECK(is_safe_relative_path("a.b/c..d/.hidden"));
        CHECK_FALSE(is_safe_relative_path(""));
        CHECK_FALSE(is_safe_relative_path("../secret.wav"));
        CHECK_FALSE(is_safe_relative_path("a/../../b.wav"));
        CHECK_FALSE(is_safe_relative_path("a/./b.wav"));
        CHECK_FALSE(is_safe_relative_path("/absolute.wav"));
        CHECK_FALSE(is_safe_relative_path("C:/drive.wav"));
        CHECK_FALSE(is_safe_relative_path("C:drive-relative.wav"));
        CHECK_FALSE(is_safe_relative_path("a//b.wav"));
        CHECK_FALSE(is_safe_relative_path("a/"));
        CHECK_FALSE(is_safe_relative_path(std::string("nul\0byte.wav", 12)));
        CHECK_FALSE(is_safe_relative_path(std::string(32767 * 3 + 1, 'a')));
#ifdef _WIN32
        CHECK_FALSE(is_safe_relative_path("\\server\\share.wav"));
        CHECK_FALSE(is_safe_relative_path("a\\..\\..\\b.wav"));
        CHECK_FALSE(is_safe_relative_path("kick.wav:stream"));  // alternate data stream
        CHECK_FALSE(is_safe_relative_path("dir/kick.wav::$DATA"));
        for (const auto reserved : {"CON", "nul.wav", "Aux", "PRN.txt", "com1", "LPT9.wav", "COM1 .wav", "conin$",
                                    "lpt\u00b9", "Drums/NUL/kick.wav"}) {
            const std::string_view shown_reserved = reserved;
            CAPTURE(shown_reserved);
            CHECK_FALSE(is_safe_relative_path(reserved));
        }
        CHECK(is_safe_relative_path("console.wav"));
        CHECK(is_safe_relative_path("com10.wav"));
        CHECK(is_safe_relative_path("nullify/kick.wav"));
        CHECK_FALSE(is_safe_relative_path("trailing./kick.wav"));
        CHECK_FALSE(is_safe_relative_path("kick.wav "));
        CHECK_FALSE(is_safe_relative_path("what?.wav"));
        CHECK_FALSE(is_safe_relative_path("a|b.wav"));
        CHECK_FALSE(is_safe_relative_path("tab\there.wav"));
#else
        CHECK(is_safe_relative_path("kick.wav:stream"));  // legal names on POSIX
        CHECK(is_safe_relative_path("CON"));
        CHECK(is_safe_relative_path("trailing./kick.wav "));
        CHECK(is_safe_relative_path("a\\..\\b.wav"));  // one file name: '\' is not a separator
        CHECK_FALSE(is_safe_relative_path("a/../b.wav"));
#endif
    }

    TEST_CASE("trim and ASCII lowercase touch only what they promise") {
        CHECK(trim("  kick \t\r\n") == "kick");
        CHECK(trim("\t \n") == "");
        CHECK(trim("") == "");
        CHECK(trim("a b") == "a b");
        CHECK(trim("\u00a0x\u00a0") == "\u00a0x\u00a0");  // only ASCII whitespace
        CHECK(to_lower_ascii("KiCK 808") == "kick 808");
        CHECK(to_lower_ascii("ÜBER") == "Über");  // non-ASCII bytes unchanged
    }

    TEST_CASE("taxonomy paths split into trimmed, bounded segments") {
        auto segments = split_taxonomy_path(" Drums / Kick /Acoustic");
        REQUIRE(segments.has_value());
        CHECK(*segments == std::vector<std::string>{"Drums", "Kick", "Acoustic"});
        CHECK(split_taxonomy_path("Single")->size() == 1);
        for (const auto invalid : {"", "/", "a//b", "a/", "/a", " / "}) {
            const std::string_view shown_invalid = invalid;
            CAPTURE(shown_invalid);
            auto split = split_taxonomy_path(invalid);
            REQUIRE_FALSE(split.has_value());
            CHECK(split.error().code == ErrorCode::InvalidArgument);
        }
        std::string deep = "a";
        for (std::size_t level = 1; level < kMaxTaxonomyDepth; ++level)
            deep += "/a";
        CHECK(split_taxonomy_path(deep).has_value());
        CHECK_FALSE(split_taxonomy_path(deep + "/a").has_value());
        CHECK(split_taxonomy_path(std::string(kMaxTaxonomyNameCodePoints, 'x')).has_value());
        CHECK_FALSE(split_taxonomy_path(std::string(kMaxTaxonomyNameCodePoints + 1, 'x')).has_value());
        CHECK_FALSE(split_taxonomy_path("bad\xFF").has_value());
    }

    TEST_CASE("analysis stages map to their persisted flag bits") {
        CHECK(analysis_flag_for_stage("tempo") == kAnalyzedTempo);
        CHECK(analysis_flag_for_stage("key") == kAnalyzedKey);
        CHECK(analysis_flag_for_stage("waveform") == kAnalyzedWaveform);
        CHECK(analysis_flag_for_stage("byte_hash") == kAnalyzedHash);
        CHECK(analysis_flag_for_stage("pcm_hash") == kAnalyzedHash);
        CHECK(analysis_flag_for_stage("loudness") == kAnalyzedLoudness);
        CHECK(analysis_flag_for_stage("unknown") == 0);
        static_assert(kAnalyzedTempo == 1 && kAnalyzedKey == 2 && kAnalyzedWaveform == 4 && kAnalyzedHash == 8 &&
                          kAnalyzedLoudness == 16,
                      "persisted bits never change");
        CHECK(kAnalyzedAny == 31);
    }

    TEST_CASE("musical keys parse from common spellings") {
        struct Case {
            std::string_view text;
            int pitch_class;
            KeyMode mode;
        };
        for (const auto& [text, pitch_class, mode] :
             {Case{"F#minor", 6, KeyMode::Minor}, Case{"Am", 9, KeyMode::Minor}, Case{"Bb maj", 10, KeyMode::Major},
              Case{"C", 0, KeyMode::Major}, Case{"Ebm", 3, KeyMode::Minor}, Case{"C sharp minor", 1, KeyMode::Minor},
              Case{"F\u266fm", 6, KeyMode::Minor}, Case{"B\u266d", 10, KeyMode::Major},
              Case{"E\u266d minor", 3, KeyMode::Minor}, Case{"g_flat-major", 6, KeyMode::Major}}) {
            const std::string_view shown_text = text;
            CAPTURE(shown_text);
            auto key = parse_key(text);
            REQUIRE(key.has_value());
            CHECK(key->pitch_class == pitch_class);
            CHECK(key->mode == mode);
        }
        CHECK_FALSE(parse_key("H").has_value());
        CHECK_FALSE(parse_key("Amaze").has_value());
        CHECK_FALSE(parse_key("").has_value());
        CHECK_FALSE(parse_key("\u266f").has_value());
        CHECK(format_key({6, KeyMode::Minor}) == "F#m");
        CHECK(format_key({0, KeyMode::Major}) == "C");
        CHECK(format_key({9, KeyMode::Unknown}) == "A?");
        CHECK(format_key({12, KeyMode::Major}).empty());
    }

    TEST_CASE("key modes round-trip through their storage names") {
        for (const auto mode : {KeyMode::Major, KeyMode::Minor, KeyMode::Unknown})
            CHECK(parse_key_mode(to_string(mode)) == mode);
        CHECK(to_string(KeyMode::Minor) == "minor");
        CHECK_FALSE(parse_key_mode("Major").has_value());  // exact storage names only
        CHECK_FALSE(parse_key_mode("").has_value());
    }

    TEST_CASE("new UUIDs are version 4 and unique") {
        std::set<std::string> seen;
        for (int index = 0; index < 1000; ++index) {
            const auto uuid = new_uuid();
            CHECK(uuid.version() == uuids::uuid_version::random_number_based);
            seen.insert(uuids::to_string(uuid));
        }
        CHECK(seen.size() == 1000);
        const auto parsed = uuids::uuid::from_string("2c3cfc37-1af5-4da3-b14a-6564bfd87312");
        REQUIRE(parsed.has_value());
        CHECK(uuids::to_string(*parsed) == "2c3cfc37-1af5-4da3-b14a-6564bfd87312");
        CHECK(now_unix_ms() > 1'700'000'000'000);
    }

    TEST_CASE("Overloaded dispatches std::visit to the matching callable") {
        const auto describe = [](const std::variant<int, std::string>& value) {
            return std::visit(Overloaded{[](int number) { return std::format("int {}", number); },
                                         [](const std::string& text) { return "text " + text; }},
                              value);
        };
        CHECK(describe(7) == "int 7");
        CHECK(describe(std::string("kick")) == "text kick");
    }

    TEST_CASE("display formatting") {
        CHECK(format_duration(1'500'000) == "1.50s");
        CHECK(format_duration(9'994'999) == "9.99s");
        CHECK(format_duration(9'995'000) == "0:10");  // never "10.00s", and never the smaller "0:09"
        CHECK(format_duration(9'999'999) == "0:10");
        CHECK(format_duration(74'499'999) == "1:14");
        CHECK(format_duration(74'500'000) == "1:15");
        CHECK(format_duration(10'000'000) == "0:10");
        CHECK(format_duration(75'000'000) == "1:15");
        CHECK(format_duration(-1).empty());
        CHECK(format_byte_size(512) == "512 B");
        CHECK(format_byte_size(2048) == "2.0 KiB");
        CHECK(format_byte_size(3 * 1024 * 1024) == "3.0 MiB");
        CHECK(format_bpm(128'000) == "128.00");
    }

    TEST_CASE("JSON fields read damaged documents without throwing") {
        CHECK(json_fields::parse_json("{\"broken\":").is_discarded());
        CHECK(json_fields::parse_json("").is_discarded());
        const auto document = json_fields::parse_json(
            R"({"name":"Kick","count":7,"big":18446744073709551615,"negative":-3,"ratio":0.5,"flag":true,)"
            R"("list":[1,2,3],"mixed":[1,"two"],"names":["a","b"],"child":{"x":1}})");
        REQUIRE_FALSE(document.is_discarded());

        CHECK(json_fields::string_field(document, "name") == "Kick");
        CHECK_FALSE(json_fields::string_field(document, "count").has_value());  // wrong type
        CHECK_FALSE(json_fields::string_field(document, "absent").has_value());
        CHECK_FALSE(json_fields::string_field(nlohmann::json::array(), "name").has_value());  // not an object

        CHECK(json_fields::integer_field(document, "count") == 7);
        CHECK(json_fields::integer_field(document, "negative") == -3);
        CHECK_FALSE(json_fields::integer_field(document, "big").has_value());    // above INT64_MAX
        CHECK_FALSE(json_fields::integer_field(document, "ratio").has_value());  // not an integer
        CHECK_FALSE(json_fields::integer_field(document, "flag").has_value());

        CHECK(json_fields::number_field(document, "ratio") == 0.5);
        CHECK(json_fields::number_field(document, "count") == 7.0);
        CHECK(json_fields::number_field(document, "negative") == -3.0);
        CHECK_FALSE(json_fields::number_field(document, "name").has_value());

        CHECK(json_fields::boolean_field(document, "flag") == true);
        CHECK_FALSE(json_fields::boolean_field(document, "count").has_value());

        REQUIRE(json_fields::array_field(document, "list") != nullptr);
        CHECK(json_fields::array_field(document, "list")->size() == 3);
        CHECK(json_fields::array_field(document, "child") == nullptr);
        REQUIRE(json_fields::object_field(document, "child") != nullptr);
        CHECK(json_fields::integer_field(*json_fields::object_field(document, "child"), "x") == 1);
        CHECK(json_fields::object_field(document, "list") == nullptr);
        CHECK(json_fields::member_field(document, "flag") != nullptr);
        CHECK(json_fields::member_field(document, "absent") == nullptr);

        CHECK(json_fields::integer_array_field(document, "list") == std::vector<std::int64_t>{1, 2, 3});
        CHECK_FALSE(json_fields::integer_array_field(document, "mixed").has_value());
        CHECK(json_fields::string_array_field(document, "names") == std::vector<std::string>{"a", "b"});
        CHECK_FALSE(json_fields::string_array_field(document, "mixed").has_value());
        CHECK_FALSE(json_fields::string_array_field(document, "name").has_value());

        CHECK(json_fields::integer_value(nlohmann::json(42u)) == 42);
        CHECK_FALSE(json_fields::integer_value(nlohmann::json("42")).has_value());
    }

    TEST_CASE("JSON dump replaces invalid UTF-8 instead of throwing") {
        const nlohmann::json document = {{"name", std::string("bad\xFF")}};
        std::string text;
        CHECK_NOTHROW(text = json_fields::dump_json(document));
        CHECK(text == "{\"name\":\"bad\xEF\xBF\xBD\"}");
        CHECK(json_fields::dump_json(nlohmann::json{{"a", 1}}, 2) == "{\n  \"a\": 1\n}");
    }

}  // TEST_SUITE("domain")
