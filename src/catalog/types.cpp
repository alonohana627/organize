#include "mso/catalog/types.hpp"

namespace mso::catalog {

std::string_view to_string(RootMode mode) noexcept {
    return mode == RootMode::Managed ? "managed" : "referenced";
}

std::string_view to_string(Availability availability) noexcept {
    switch (availability) {
        case Availability::Online:
            return "online";
        case Availability::Offline:
            return "offline";
        case Availability::Unknown:
            return "unknown";
        case Availability::Denied:
            return "denied";
    }
    return "unknown";
}

std::string_view to_string(LocationState state) noexcept {
    switch (state) {
        case LocationState::Present:
            return "present";
        case LocationState::Missing:
            return "missing";
        case LocationState::Unknown:
            return "unknown";
        case LocationState::Denied:
            return "denied";
        case LocationState::Changed:
            return "changed";
        case LocationState::Retired:
            return "retired";
    }
    return "unknown";
}

std::string_view to_string(OverrideState state) noexcept {
    switch (state) {
        case OverrideState::Automatic:
            return "auto";
        case OverrideState::Value:
            return "value";
        case OverrideState::Unknown:
            return "unknown";
    }
    return "auto";
}

std::optional<RootMode> parse_root_mode(std::string_view text) noexcept {
    if (text == "referenced") return RootMode::Referenced;
    if (text == "managed") return RootMode::Managed;
    return std::nullopt;
}

std::optional<Availability> parse_availability(std::string_view text) noexcept {
    if (text == "online") return Availability::Online;
    if (text == "offline") return Availability::Offline;
    if (text == "unknown") return Availability::Unknown;
    if (text == "denied") return Availability::Denied;
    return std::nullopt;
}

std::optional<LocationState> parse_location_state(std::string_view text) noexcept {
    if (text == "present") return LocationState::Present;
    if (text == "missing") return LocationState::Missing;
    if (text == "unknown") return LocationState::Unknown;
    if (text == "denied") return LocationState::Denied;
    if (text == "changed") return LocationState::Changed;
    if (text == "retired") return LocationState::Retired;
    return std::nullopt;
}

std::optional<OverrideState> parse_override_state(std::string_view text) noexcept {
    if (text == "auto") return OverrideState::Automatic;
    if (text == "value") return OverrideState::Value;
    if (text == "unknown") return OverrideState::Unknown;
    return std::nullopt;
}

}  // namespace mso::catalog
