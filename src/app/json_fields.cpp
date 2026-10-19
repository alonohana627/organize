#include "mso/app/json_fields.hpp"

namespace mso::app {

namespace {

/// The member `key` of `object`, or nullptr when `object` is not an object or lacks the key.
const nlohmann::json* member(const nlohmann::json& object, std::string_view key) {
    if (!object.is_object()) return nullptr;
    const auto found = object.find(std::string(key));
    return found == object.end() ? nullptr : &*found;
}

}  // namespace

std::optional<std::string> string_field(const nlohmann::json& object, std::string_view key) {
    const auto* value = member(object, key);
    if (value == nullptr || !value->is_string()) return std::nullopt;
    return value->get_ref<const std::string&>();
}

std::optional<bool> boolean_field(const nlohmann::json& object, std::string_view key) {
    const auto* value = member(object, key);
    if (value == nullptr || !value->is_boolean()) return std::nullopt;
    return value->get<bool>();
}

std::optional<std::int64_t> integer_field(const nlohmann::json& object, std::string_view key) {
    const auto* value = member(object, key);
    if (value == nullptr || !value->is_number_integer()) return std::nullopt;
    return value->get<std::int64_t>();
}

const nlohmann::json* array_field(const nlohmann::json& object, std::string_view key) {
    const auto* value = member(object, key);
    return value != nullptr && value->is_array() ? value : nullptr;
}

const nlohmann::json* object_field(const nlohmann::json& object, std::string_view key) {
    const auto* value = member(object, key);
    return value != nullptr && value->is_object() ? value : nullptr;
}

std::string dump_json(const nlohmann::json& document, int indent) {
    return document.dump(indent, ' ', false, nlohmann::json::error_handler_t::replace);
}

}  // namespace mso::app
