#pragma once
/// @file
/// Non-throwing, type-checked reads and writes of JSON documents that may be damaged or written by
/// another version: workspace.json, library manifests, stored settings, history patches, receipts,
/// event envelopes and job checkpoints. nlohmann's `operator[]`, `value()`, `.get<T>()` and throwing
/// `parse()` raise exceptions (or invoke undefined behaviour) on a missing key, a wrong type or bad
/// UTF-8; these helpers return nullopt, nullptr or a discarded value instead, so a damaged document can
/// never escape a `Result<>` API as an exception. The one shared helper for every module.
///
///     const auto document = mso::json_fields::parse_json(text);
///     if (document.is_discarded()) return fail(ErrorCode::DbCorrupt, "unreadable document");
///     const auto name = mso::json_fields::string_field(document, "name");  // nullopt when absent
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include <nlohmann/json.hpp>

namespace mso::json_fields {

/// Parses `text`; returns a discarded value (`is_discarded()`) instead of throwing on malformed input
/// or when memory runs out.
[[nodiscard]] nlohmann::json parse_json(std::string_view text) noexcept;

/// Serializes `document` without ever throwing a type error: invalid UTF-8 inside strings is replaced
/// with U+FFFD instead of raising `json::type_error` 316. `indent` follows `nlohmann::json::dump`
/// (-1 is the compact form).
[[nodiscard]] std::string dump_json(const nlohmann::json& document, int indent = -1);

/// The member `key` of `object` whatever its type, or nullptr when `object` is not an object or lacks
/// the key.
[[nodiscard]] const nlohmann::json* member_field(const nlohmann::json& object, std::string_view key) noexcept;

/// `value` as a signed 64-bit integer (a signed or unsigned JSON integer within int64 range), or
/// nullopt for any other type, a floating-point number or an unsigned value above INT64_MAX.
[[nodiscard]] std::optional<std::int64_t> integer_value(const nlohmann::json& value) noexcept;

/// The string stored at `key`, or nullopt when `object` is not an object, the key is absent or the
/// value is not a string.
[[nodiscard]] std::optional<std::string> string_field(const nlohmann::json& object, std::string_view key);

/// The integer stored at `key` (see `integer_value`), or nullopt when absent or of another type.
[[nodiscard]] std::optional<std::int64_t> integer_field(const nlohmann::json& object, std::string_view key) noexcept;

/// The boolean stored at `key`, or nullopt when absent or not a boolean.
[[nodiscard]] std::optional<bool> boolean_field(const nlohmann::json& object, std::string_view key) noexcept;

/// The number stored at `key` as a double (integers convert), or nullopt when absent or not a number.
[[nodiscard]] std::optional<double> number_field(const nlohmann::json& object, std::string_view key) noexcept;

/// The array stored at `key`, or nullptr when absent or not an array.
[[nodiscard]] const nlohmann::json* array_field(const nlohmann::json& object, std::string_view key) noexcept;

/// The object stored at `key`, or nullptr when absent or not an object.
[[nodiscard]] const nlohmann::json* object_field(const nlohmann::json& object, std::string_view key) noexcept;

/// The array of integers stored at `key`, or nullopt when absent, not an array, or holding any element
/// that `integer_value` rejects.
[[nodiscard]] std::optional<std::vector<std::int64_t>> integer_array_field(const nlohmann::json& object,
                                                                           std::string_view key);

/// The array of strings stored at `key`, or nullopt when absent, not an array, or holding a non-string.
[[nodiscard]] std::optional<std::vector<std::string>> string_array_field(const nlohmann::json& object,
                                                                         std::string_view key);

}  // namespace mso::json_fields
