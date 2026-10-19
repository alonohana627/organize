// Domain primitives: normalization keys, path safety, musical keys and UUIDs.
#include <doctest.h>

#include <set>

#include "mso/domain/core.hpp"

using namespace mso;

TEST_SUITE("domain") {
    TEST_CASE("fold_key normalizes case, compatibility forms and whitespace") {
        CHECK(fold_key("  Kick   DRUM ") == "kick drum");
        CHECK(fold_key("ＫＩＣＫ") == "kick");             // fullwidth → ASCII (NFKC)
        CHECK(fold_key("Straße") == fold_key("STRASSE"));  // full case folding
        CHECK(fold_key("Ünïcödé") == fold_key("ÜNÏCÖDÉ"));
        CHECK(fold_key("שלום") == "שלום");                       // caseless scripts pass through
        CHECK(fold_key("Cafe\u0301") == fold_key("Caf\u00e9"));  // composed and decomposed forms agree
    }

    TEST_CASE("fold_path_key folds case but keeps distinct file names distinct") {
        CHECK(fold_path_key("Drums\\Kick.WAV") == "drums/kick.wav");
        CHECK(fold_path_key("ﬁle.wav") != fold_path_key("file.wav"));  // no compatibility mapping for paths
    }

    TEST_CASE("relative paths that could escape a root are rejected") {
        CHECK(is_safe_relative_path("Drums/Kick 01.wav"));
        CHECK(is_safe_relative_path("שלום/vox.wav"));
        CHECK_FALSE(is_safe_relative_path(""));
        CHECK_FALSE(is_safe_relative_path("../secret.wav"));
        CHECK_FALSE(is_safe_relative_path("a/../../b.wav"));
        CHECK_FALSE(is_safe_relative_path("/absolute.wav"));
        CHECK_FALSE(is_safe_relative_path("C:/drive.wav"));
        CHECK_FALSE(is_safe_relative_path("a//b.wav"));
        CHECK_FALSE(is_safe_relative_path(std::string("nul\0byte.wav", 12)));
    }

    TEST_CASE("musical keys parse from common spellings") {
        struct Case {
            std::string_view text;
            int pitch_class;
            KeyMode mode;
        };
        for (const auto& [text, pitch_class, mode] :
             {Case{"F#minor", 6, KeyMode::Minor}, Case{"Am", 9, KeyMode::Minor}, Case{"Bb maj", 10, KeyMode::Major},
              Case{"C", 0, KeyMode::Major}, Case{"Ebm", 3, KeyMode::Minor}, Case{"C sharp minor", 1, KeyMode::Minor}}) {
            CAPTURE(text);
            auto key = parse_key(text);
            REQUIRE(key.has_value());
            CHECK(key->pitch_class == pitch_class);
            CHECK(key->mode == mode);
        }
        CHECK_FALSE(parse_key("H").has_value());
        CHECK_FALSE(parse_key("Amaze").has_value());
        CHECK(format_key({6, KeyMode::Minor}) == "F#m");
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
    }

    TEST_CASE("display formatting") {
        CHECK(format_duration(1'500'000) == "1.50s");
        CHECK(format_duration(75'000'000) == "1:15");
        CHECK(format_byte_size(2048) == "2.0 KiB");
        CHECK(format_bpm(128'000) == "128.00");
    }

}  // TEST_SUITE("domain")
