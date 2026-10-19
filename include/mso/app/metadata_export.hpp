#pragma once
/// @file
/// Streaming CSV/JSON metadata export (docs/03_DATA_AND_SEARCH.md §10). Rows are read page by
/// page, written to an operation-owned temporary file and published by an atomic rename, so a
/// cancelled or failed export never leaves a complete-looking truncated file. A CSV export also
/// gets a `<file>.manifest.json`, fully written before the CSV is renamed into place.
///
/// The recorded catalog revision is read before and after streaming; when edits were committed
/// meanwhile the manifest says so (`"consistent": false`) instead of claiming a snapshot.
#include <filesystem>
#include <functional>
#include <optional>
#include <stop_token>

#include "mso/app/application.hpp"

namespace mso::app {

/// Absolute local folder of a root (managed roots live inside the library, referenced roots use
/// the workspace binding), for the optional absolute-path column; nullopt when unbound here.
using RootPathResolver =
    std::function<std::optional<std::filesystem::path>(const uuids::uuid& root, catalog::RootMode mode)>;

/// Writes the export of `library` to `destination` (replacing it atomically).
Result<ExportSummary> write_metadata_export(catalog::Library& library, const std::filesystem::path& destination,
                                            const ExportOptions& options, const RootPathResolver& root_path,
                                            std::stop_token stop);

/// Spreadsheet-safe text: an apostrophe before a value a spreadsheet would evaluate as a formula
/// (= + - @ tab CR). CSV quoting itself is done by deps/csv_parser.
std::string spreadsheet_safe(std::string value);

}  // namespace mso::app
