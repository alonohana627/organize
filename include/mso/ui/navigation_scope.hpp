#pragma once
/// @file
/// NavigationScope: what the navigator selected (all libraries, one library, a root, a folder,
/// a tag, a category or a collection). The search box text is applied on top of it.
#include <QString>

#include <optional>

#include "mso/domain/core.hpp"
#include "mso/search/search_service.hpp"

namespace mso::ui {

/// The navigator's current selection, translated into a search scope.
struct NavigationScope {
    /// Library to search; nullopt searches every open library (federated).
    std::optional<uuids::uuid> library;
    /// Root/folder/tag/category/collection restriction inside that library.
    search::Scope scope;
    /// Human-readable description for the status bar, e.g. "Drums/Kick in Main library".
    QString label;
};

}  // namespace mso::ui
