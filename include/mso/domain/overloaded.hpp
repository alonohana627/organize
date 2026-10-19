#pragma once
/// @file
/// The overload-set helper for `std::visit`, shared by every module (one definition instead of a
/// copy per module).
///
///     std::visit(mso::Overloaded{[](const Started&) { ... }, [](const Finished&) { ... }}, event);

namespace mso {

/// Combines several callables into one overload set: `Overloaded{lambda_a, lambda_b}` has every
/// `operator()` of its bases, so `std::visit` picks the one matching the active alternative.
template <class... Callables>
struct Overloaded : Callables... {
    using Callables::operator()...;
};

/// Deduces the callable types from the constructor arguments (implicit in C++20; kept explicit).
template <class... Callables>
Overloaded(Callables...) -> Overloaded<Callables...>;

}  // namespace mso
