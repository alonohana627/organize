#pragma once
/// @file
/// Non-throwing, type-checked field reads from untrusted JSON documents (workspace.json, library
/// manifests). nlohmann's `value()` and `operator[]` throw or invoke undefined behaviour on missing
/// keys and wrong types; these helpers return nullopt instead, so a damaged file can never escape a
/// Result<> API as an exception. Public so the UI's stored settings (layouts, shortcuts) use the same
/// reads; catalog has an equivalent internal copy (see the integration note: one shared helper in
/// domain would replace both).
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

#include <nlohmann/json.hpp>

namespace mso::app {

/// The string stored at `key`, or nullopt when `object` is not an object, the key is absent or
/// the value is not a string.
std::optional<std::string> string_field(const nlohmann::json& object, std::string_view key);

/// The boolean stored at `key`, or nullopt when absent or not a boolean.
std::optional<bool> boolean_field(const nlohmann::json& object, std::string_view key);

/// The integer stored at `key`, or nullopt when absent or not an integer.
std::optional<std::int64_t> integer_field(const nlohmann::json& object, std::string_view key);

/// The value stored at `key` when it is an array or an object of the requested kind, else nullptr.
const nlohmann::json* array_field(const nlohmann::json& object, std::string_view key);
const nlohmann::json* object_field(const nlohmann::json& object, std::string_view key);

/// Serializes without ever throwing: invalid UTF-8 is replaced with U+FFFD instead of raising
/// json::type_error 316.
std::string dump_json(const nlohmann::json& document, int indent = -1);

}  // namespace mso::app
