#pragma once
/// @file
/// The usual visitor-combining helper for std::visit, shared by the audio sources.

namespace mso::audio::detail {

/// Combines lambdas into one overload set: `std::visit(Overloaded{[](A){}, [](B){}}, variant)`.
template <class... Handlers>
struct Overloaded : Handlers... {
    using Handlers::operator()...;
};

}  // namespace mso::audio::detail
