#pragma once
/// @file
/// Non-throwing reads of stored JSON (history patches, receipts, envelopes, checkpoints). Stored
/// documents can be corrupt or written by another version, so every catalog read of persisted JSON
/// goes through these helpers instead of `operator[]`, `.get<T>()`, `value()` or throwing `parse()`.
/// Internal to the catalog module.
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include <nlohmann/json.hpp>

namespace mso::catalog::detail {

/// Parses text; returns a discarded value (`is_discarded()`) instead of throwing on malformed input.
nlohmann::json parse_json(std::string_view text) noexcept;

/// Serializes a document; invalid UTF-8 inside strings is replaced instead of throwing.
std::string to_json_text(const nlohmann::json& document);

/// The member `key` of an object, or nullptr when `object` is not an object or lacks the key.
const nlohmann::json* json_member(const nlohmann::json& object, std::string_view key) noexcept;

/// An integer value (signed or unsigned within int64 range), or nullopt for any other type.
std::optional<std::int64_t> json_integer(const nlohmann::json& value) noexcept;

/// Integer member of an object, or nullopt when absent or of another type.
std::optional<std::int64_t> json_integer_member(const nlohmann::json& object, std::string_view key) noexcept;

/// String member of an object, or nullopt when absent or of another type.
std::optional<std::string> json_string_member(const nlohmann::json& object, std::string_view key);

/// Boolean member of an object, or nullopt when absent or of another type.
std::optional<bool> json_boolean_member(const nlohmann::json& object, std::string_view key) noexcept;

/// Array-of-integers member, or nullopt when absent, not an array, or holding a non-integer.
std::optional<std::vector<std::int64_t>> json_integer_array_member(const nlohmann::json& object, std::string_view key);

/// Array-of-strings member, or nullopt when absent, not an array, or holding a non-string.
std::optional<std::vector<std::string>> json_string_array_member(const nlohmann::json& object, std::string_view key);

}  // namespace mso::catalog::detail
