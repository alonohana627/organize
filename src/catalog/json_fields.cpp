#include "mso/catalog/detail/json_fields.hpp"

#include <limits>

namespace mso::catalog::detail {

nlohmann::json parse_json(std::string_view text) noexcept {
    try {
        return nlohmann::json::parse(text, nullptr, /*allow_exceptions=*/false);
    } catch (const std::exception&) {  // only std::bad_alloc can still escape a non-throwing parse
        return nlohmann::json(nlohmann::json::value_t::discarded);
    }
}

std::string to_json_text(const nlohmann::json& document) {
    return document.dump(-1, ' ', false, nlohmann::json::error_handler_t::replace);
}

const nlohmann::json* json_member(const nlohmann::json& object, std::string_view key) noexcept {
    if (!object.is_object()) return nullptr;
    const auto found = object.find(key);
    return found == object.end() ? nullptr : &*found;
}

std::optional<std::int64_t> json_integer(const nlohmann::json& value) noexcept {
    if (value.is_number_unsigned()) {
        const auto unsigned_value = value.get_ref<const nlohmann::json::number_unsigned_t&>();
        if (unsigned_value > static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max())) return std::nullopt;
        return static_cast<std::int64_t>(unsigned_value);
    }
    if (value.is_number_integer()) return value.get_ref<const nlohmann::json::number_integer_t&>();
    return std::nullopt;
}

std::optional<std::int64_t> json_integer_member(const nlohmann::json& object, std::string_view key) noexcept {
    const auto* member = json_member(object, key);
    return member ? json_integer(*member) : std::nullopt;
}

std::optional<std::string> json_string_member(const nlohmann::json& object, std::string_view key) {
    const auto* member = json_member(object, key);
    if (!member || !member->is_string()) return std::nullopt;
    return member->get_ref<const nlohmann::json::string_t&>();
}

std::optional<bool> json_boolean_member(const nlohmann::json& object, std::string_view key) noexcept {
    const auto* member = json_member(object, key);
    if (!member || !member->is_boolean()) return std::nullopt;
    return member->get_ref<const nlohmann::json::boolean_t&>();
}

std::optional<std::vector<std::int64_t>> json_integer_array_member(const nlohmann::json& object, std::string_view key) {
    const auto* member = json_member(object, key);
    if (!member || !member->is_array()) return std::nullopt;
    std::vector<std::int64_t> values;
    values.reserve(member->size());
    for (const auto& element : *member) {
        const auto value = json_integer(element);
        if (!value) return std::nullopt;
        values.push_back(*value);
    }
    return values;
}

std::optional<std::vector<std::string>> json_string_array_member(const nlohmann::json& object, std::string_view key) {
    const auto* member = json_member(object, key);
    if (!member || !member->is_array()) return std::nullopt;
    std::vector<std::string> values;
    values.reserve(member->size());
    for (const auto& element : *member) {
        if (!element.is_string()) return std::nullopt;
        values.push_back(element.get_ref<const nlohmann::json::string_t&>());
    }
    return values;
}

}  // namespace mso::catalog::detail
