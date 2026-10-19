#include "mso/domain/json_fields.hpp"

#include <limits>

namespace mso::json_fields {

nlohmann::json parse_json(std::string_view text) noexcept {
    try {
        return nlohmann::json::parse(text, nullptr, /*allow_exceptions=*/false);
    } catch (const std::exception&) {  // only std::bad_alloc can still escape a non-throwing parse
        return nlohmann::json(nlohmann::json::value_t::discarded);
    }
}

std::string dump_json(const nlohmann::json& document, int indent) {
    return document.dump(indent, ' ', false, nlohmann::json::error_handler_t::replace);
}

const nlohmann::json* member_field(const nlohmann::json& object, std::string_view key) noexcept {
    if (!object.is_object()) return nullptr;
    const auto found = object.find(key);
    return found == object.end() ? nullptr : &*found;
}

std::optional<std::int64_t> integer_value(const nlohmann::json& value) noexcept {
    if (value.is_number_unsigned()) {
        const auto unsigned_value = value.get_ref<const nlohmann::json::number_unsigned_t&>();
        if (unsigned_value > static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max())) return std::nullopt;
        return static_cast<std::int64_t>(unsigned_value);
    }
    if (value.is_number_integer()) return value.get_ref<const nlohmann::json::number_integer_t&>();
    return std::nullopt;
}

std::optional<std::string> string_field(const nlohmann::json& object, std::string_view key) {
    const auto* member = member_field(object, key);
    if (member == nullptr || !member->is_string()) return std::nullopt;
    return member->get_ref<const nlohmann::json::string_t&>();
}

std::optional<std::int64_t> integer_field(const nlohmann::json& object, std::string_view key) noexcept {
    const auto* member = member_field(object, key);
    return member != nullptr ? integer_value(*member) : std::nullopt;
}

std::optional<bool> boolean_field(const nlohmann::json& object, std::string_view key) noexcept {
    const auto* member = member_field(object, key);
    if (member == nullptr || !member->is_boolean()) return std::nullopt;
    return member->get_ref<const nlohmann::json::boolean_t&>();
}

std::optional<double> number_field(const nlohmann::json& object, std::string_view key) noexcept {
    const auto* member = member_field(object, key);
    if (member == nullptr) return std::nullopt;
    if (member->is_number_float()) return member->get_ref<const nlohmann::json::number_float_t&>();
    if (member->is_number_unsigned())
        return static_cast<double>(member->get_ref<const nlohmann::json::number_unsigned_t&>());
    if (member->is_number_integer())
        return static_cast<double>(member->get_ref<const nlohmann::json::number_integer_t&>());
    return std::nullopt;
}

const nlohmann::json* array_field(const nlohmann::json& object, std::string_view key) noexcept {
    const auto* member = member_field(object, key);
    return member != nullptr && member->is_array() ? member : nullptr;
}

const nlohmann::json* object_field(const nlohmann::json& object, std::string_view key) noexcept {
    const auto* member = member_field(object, key);
    return member != nullptr && member->is_object() ? member : nullptr;
}

std::optional<std::vector<std::int64_t>> integer_array_field(const nlohmann::json& object, std::string_view key) {
    const auto* array = array_field(object, key);
    if (array == nullptr) return std::nullopt;
    std::vector<std::int64_t> values;
    values.reserve(array->size());
    for (const auto& element : *array) {
        const auto value = integer_value(element);
        if (!value) return std::nullopt;
        values.push_back(*value);
    }
    return values;
}

std::optional<std::vector<std::string>> string_array_field(const nlohmann::json& object, std::string_view key) {
    const auto* array = array_field(object, key);
    if (array == nullptr) return std::nullopt;
    std::vector<std::string> values;
    values.reserve(array->size());
    for (const auto& element : *array) {
        if (!element.is_string()) return std::nullopt;
        values.push_back(element.get_ref<const nlohmann::json::string_t&>());
    }
    return values;
}

}  // namespace mso::json_fields
