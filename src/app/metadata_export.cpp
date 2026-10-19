#include "mso/app/metadata_export.hpp"

#include <format>
#include <fstream>

#include <csv.hpp>
#include <nlohmann/json.hpp>

namespace mso::app {

namespace {

constexpr int kExportSchemaVersion = 1;

/// Stable, documented CSV column order.
constexpr std::string_view kCsvColumns[] = {
    "sample_uuid", "display_name", "file_name",   "root",           "relative_path", "absolute_path",
    "format",      "codec",        "duration_us", "duration_exact", "sample_rate",   "channels",
    "bit_depth",   "byte_size",    "bpm",         "bpm_source",     "key",           "key_source",
    "category",    "tags",         "rating",      "favorite",       "availability",  "created_at"};

std::string optional_number(const std::optional<std::int64_t>& value) {
    return value ? std::to_string(*value) : std::string();
}

/// Values of one row in kCsvColumns order (nulls are empty strings).
std::vector<std::string> csv_values(const search::SampleRow& row, const std::string& absolute_path) {
    return {uuids::to_string(row.sample_uuid),
            row.display_name,
            row.file_name,
            row.root_name,
            row.relative_path,
            absolute_path,
            row.format,
            row.codec,
            optional_number(row.duration_us),
            row.duration_us ? (row.duration_exact ? "true" : "false") : "",
            optional_number(row.sample_rate),
            optional_number(row.channels),
            optional_number(row.bit_depth),
            optional_number(row.byte_size),
            row.bpm_milli ? format_bpm(*row.bpm_milli) : "",
            row.bpm_source,
            row.key ? format_key(*row.key) : "",
            row.key_source,
            row.category,
            nlohmann::json(row.tag_paths).dump(),
            std::to_string(row.rating),
            row.favorite ? "true" : "false",
            std::string(search::to_string(row.availability)),
            std::to_string(row.created_at)};
}

nlohmann::json json_row(const search::SampleRow& row, const std::string& absolute_path, bool include_absolute) {
    auto nullable = [](const std::optional<std::int64_t>& value) {
        return value ? nlohmann::json(*value) : nlohmann::json();
    };
    nlohmann::json location = {
        {"root", row.root_name}, {"root_uuid", uuids::to_string(row.root_uuid)}, {"relative_path", row.relative_path}};
    if (include_absolute) location["absolute_path"] = absolute_path;
    return nlohmann::json{{"uuid", uuids::to_string(row.sample_uuid)},
                          {"display_name", row.display_name},
                          {"file_name", row.file_name},
                          {"location", location},
                          {"format", row.format.empty() ? nlohmann::json() : nlohmann::json(row.format)},
                          {"codec", row.codec.empty() ? nlohmann::json() : nlohmann::json(row.codec)},
                          {"duration_us", nullable(row.duration_us)},
                          {"duration_exact", row.duration_exact},
                          {"sample_rate", nullable(row.sample_rate)},
                          {"channels", nullable(row.channels)},
                          {"bit_depth", nullable(row.bit_depth)},
                          {"byte_size", nullable(row.byte_size)},
                          {"bpm_milli", nullable(row.bpm_milli)},
                          {"bpm_source", row.bpm_source.empty() ? nlohmann::json() : nlohmann::json(row.bpm_source)},
                          {"key", row.key ? nlohmann::json{{"pitch_class", row.key->pitch_class},
                                                           {"mode", std::string(to_string(row.key->mode))}}
                                          : nlohmann::json()},
                          {"key_source", row.key_source.empty() ? nlohmann::json() : nlohmann::json(row.key_source)},
                          {"category", row.category.empty() ? nlohmann::json() : nlohmann::json(row.category)},
                          {"tags", row.tag_paths},
                          {"rating", row.rating},
                          {"favorite", row.favorite},
                          {"availability", std::string(search::to_string(row.availability))},
                          {"created_at", row.created_at}};
}

}  // namespace

std::string spreadsheet_safe(std::string value) {
    static constexpr std::string_view kFormulaStarters = "=+-@\t\r";
    if (!value.empty() && kFormulaStarters.find(value.front()) != std::string_view::npos)
        value.insert(value.begin(), '\'');
    return value;
}

Result<ExportSummary> write_metadata_export(catalog::Library& library, const std::filesystem::path& destination,
                                            const ExportOptions& options, const RootPathResolver& root_path,
                                            std::stop_token stop) {
    auto temporary = destination;
    temporary += std::format(".{}.partial", uuids::to_string(new_uuid()));
    ExportSummary summary{0, destination};
    const auto identity = library.identity();
    {
        std::ofstream out(temporary, std::ios::binary | std::ios::trunc);
        if (!out) return fail(ErrorCode::AccessDenied, "cannot create the export file");
        auto cleanup_on_failure = [&](Error error) {
            out.close();
            std::error_code ignored;
            std::filesystem::remove(temporary, ignored);
            return std::unexpected(std::move(error));
        };

        const bool csv = options.format == ExportFormat::Csv;
        auto csv_writer = csv::make_csv_writer(out);  // RFC 4180 quoting, CRLF records (patched)
        if (csv) {
            if (options.utf8_bom) out << "\xEF\xBB\xBF";
            csv_writer << std::vector<std::string>(std::begin(kCsvColumns), std::end(kCsvColumns));
        } else {
            const nlohmann::json manifest = {{"schema_version", kExportSchemaVersion},
                                             {"library",
                                              {{"uuid", uuids::to_string(identity.uuid)},
                                               {"name", identity.name},
                                               {"catalog_revision", identity.catalog_revision}}},
                                             {"exported_at", now_unix_ms()},
                                             {"absolute_paths", options.include_absolute_paths}};
            const std::string head = manifest.dump();
            out << head.substr(0, head.size() - 1) << ",\"samples\":[";
        }

        search::SearchService service;
        search::SearchRequest request{
            .text = options.query, .sort = search::SortField::Added, .page_size = search::SearchService::kMaxPageSize};
        catalog::Library* scope[] = {&library};
        bool first = true;
        while (true) {
            if (stop.stop_requested())
                return cleanup_on_failure(Error{ErrorCode::Cancelled, false, "export cancelled", {}, {}});
            auto page = service.search(scope, request);
            if (!page) return cleanup_on_failure(page.error());
            for (const auto& row : page->rows) {
                std::string absolute;
                if (options.include_absolute_paths)
                    if (auto base = root_path(row.root_uuid))
                        absolute = path_to_utf8(*base / path_from_utf8(row.relative_path));
                if (csv) {
                    auto values = csv_values(row, absolute);
                    if (options.spreadsheet_safe)
                        for (auto& value : values)
                            value = spreadsheet_safe(std::move(value));
                    csv_writer << values;
                } else {
                    out << (first ? "" : ",") << json_row(row, absolute, options.include_absolute_paths).dump();
                    first = false;
                }
                ++summary.rows;
            }
            if (!page->next) break;
            request.cursor = page->next;
        }
        if (csv)
            csv_writer.flush();
        else
            out << "]}";
        out.flush();
        if (!out) return cleanup_on_failure(Error{ErrorCode::DiskFull, false, "writing the export failed", {}, {}});
    }
    std::error_code error;
    std::filesystem::rename(temporary, destination, error);
    if (error) {
        std::filesystem::remove(temporary, error);
        return fail(ErrorCode::AccessDenied, "cannot replace the export destination");
    }
    if (options.format == ExportFormat::Csv) {
        // The CSV manifest records what a spreadsheet reader needs to interpret the file.
        auto manifest_path = destination;
        manifest_path += ".manifest.json";
        std::ofstream manifest(manifest_path, std::ios::binary | std::ios::trunc);
        manifest << nlohmann::json{{"schema_version", kExportSchemaVersion},
                                   {"library", uuids::to_string(identity.uuid)},
                                   {"catalog_revision", identity.catalog_revision},
                                   {"rows", summary.rows},
                                   {"spreadsheet_safe", options.spreadsheet_safe},
                                   {"tags_encoding", "JSON array per cell"}}
                        .dump(2);
    }
    return summary;
}

}  // namespace mso::app
