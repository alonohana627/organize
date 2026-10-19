#include "mso/catalog/taxonomy_lookup.hpp"

#include "mso/catalog/catalog_internal.hpp"

namespace mso::catalog {

Result<std::optional<std::int64_t>> find_taxonomy_node(sqlite::Connection& connection, TaxonomyKind kind,
                                                       std::string_view path) {
    const auto taxonomy = kind == TaxonomyKind::Tags ? detail::Taxonomy::Tags : detail::Taxonomy::Categories;
    auto id = detail::resolve_taxonomy_path(connection, taxonomy, path, /*create=*/false);
    if (id) return std::optional<std::int64_t>(*id);
    if (id.error().code == ErrorCode::NotFound) return std::optional<std::int64_t>{};
    return std::unexpected(id.error());
}

}  // namespace mso::catalog
