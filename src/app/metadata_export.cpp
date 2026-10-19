#include "mso/app/metadata_export.hpp"

#include <format>
#include <fstream>
#include <optional>
#include <ostream>

#include <csv.hpp>
#include <nlohmann/json.hpp>

#include "mso/catalog/durable_file.hpp"
#include "mso/domain/json_fields.hpp"

#ifdef _WIN32
#include <windows.h>  // CreateFileW, FlushFileBuffers
#else
#include <fcntl.h>   // open
#include <unistd.h>  // fsync, close
#endif

namespace mso::app {

namespace {

constexpr int kExportSchemaVersion = 1;

/// Stable, documented CSV column order.
constexpr std::string_view kCsvColumns[] = {
    "sample_uuid", "display_name", "file_name",   "root",           "relative_path", "absolute_path",
    "format",      "codec",        "duration_us", "duration_exact", "sample_rate",   "channels",
    "bit_depth",   "byte_size",    "bpm",         "bpm_source",     "key",           "key_source",
    "category",    "tags",         "rating",      "favorite",       "availability",  "created_at"};

/// A known number as text, or "" (an empty CSV cell).
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
            json_fields::dump_json(nlohmann::json(row.tag_paths)),
            std::to_string(row.rating),
            row.favorite ? "true" : "false",
            std::string(search::to_string(row.availability)),
            std::to_string(row.created_at)};
}

/// One sample of the JSON export.
nlohmann::json json_row(const search::SampleRow& row, const std::string& absolute_path, bool include_absolute) {
    auto nullable = [](const std::optional<std::int64_t>& value) {
        return value ? nlohmann::json(*value) : nlohmann::json();
    };
    auto nullable_text = [](const std::string& value) {
        return value.empty() ? nlohmann::json() : nlohmann::json(value);
    };
    nlohmann::json location = {
        {"root", row.root_name}, {"root_uuid", uuids::to_string(row.root_uuid)}, {"relative_path", row.relative_path}};
    if (include_absolute) location["absolute_path"] = absolute_path;
    return nlohmann::json{{"uuid", uuids::to_string(row.sample_uuid)},
                          {"display_name", row.display_name},
                          {"file_name", row.file_name},
                          {"location", location},
                          {"format", nullable_text(row.format)},
                          {"codec", nullable_text(row.codec)},
                          {"duration_us", nullable(row.duration_us)},
                          {"duration_exact", row.duration_exact},
                          {"sample_rate", nullable(row.sample_rate)},
                          {"channels", nullable(row.channels)},
                          {"bit_depth", nullable(row.bit_depth)},
                          {"byte_size", nullable(row.byte_size)},
                          {"bpm_milli", nullable(row.bpm_milli)},
                          {"bpm_source", nullable_text(row.bpm_source)},
                          {"key", row.key ? nlohmann::json{{"pitch_class", row.key->pitch_class},
                                                           {"mode", std::string(to_string(row.key->mode))}}
                                          : nlohmann::json()},
                          {"key_source", nullable_text(row.key_source)},
                          {"category", nullable_text(row.category)},
                          {"tags", row.tag_paths},
                          {"rating", row.rating},
                          {"favorite", row.favorite},
                          {"availability", std::string(search::to_string(row.availability))},
                          {"created_at", row.created_at}};
}

/// Writes one JSON object member by member, so an array of any length can be streamed between
/// ordinary members without the whole document in memory. Every key and value is encoded by
/// nlohmann::json; the writer only places the braces, colons and commas, and it tracks whether a
/// separator is due, so no member or element can be spliced in malformed.
class JsonObjectWriter {
public:
    /// Opens the object.
    explicit JsonObjectWriter(std::ostream& out) : out_(out) { out_ << '{'; }

    /// Writes `"key":value`.
    void member(std::string_view key, const nlohmann::json& value) {
        write_key(key);
        out_ << json_fields::dump_json(value);
    }
    /// Writes `"key":[` and starts streaming elements into it.
    void begin_array(std::string_view key) {
        write_key(key);
        out_ << '[';
        first_element_ = true;
    }
    /// Writes one element of the open array.
    void element(const nlohmann::json& value) {
        if (!first_element_) out_ << ',';
        first_element_ = false;
        out_ << json_fields::dump_json(value);
    }
    /// Closes the open array.
    void end_array() { out_ << ']'; }
    /// Closes the object.
    void finish() { out_ << '}'; }

private:
    void write_key(std::string_view key) {
        if (!first_member_) out_ << ',';
        first_member_ = false;
        out_ << json_fields::dump_json(nlohmann::json(key)) << ':';
    }

    std::ostream& out_;
    bool first_member_ = true;
    bool first_element_ = true;
};

/// Sibling temporary name owned by this export (unique, so concurrent exports never collide).
std::filesystem::path temporary_sibling(const std::filesystem::path& file) {
    auto temporary = file;
    temporary += std::format(".{}.partial", uuids::to_string(new_uuid()));
    return temporary;
}

/// Removes one of our temporary files; a leftover is harmless.
void remove_quietly(const std::filesystem::path& file) {
    std::error_code ignored;  // cleanup of our own temporary file; a leftover is harmless
    std::filesystem::remove(file, ignored);
}

/// Flushes a closed file's data to stable storage, so the rename that publishes it can never
/// expose a file whose contents a power loss would lose.
Result<void> flush_to_disk(const std::filesystem::path& file) {
#ifdef _WIN32
    const HANDLE handle = CreateFileW(file.c_str(), GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr,
                                      OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (handle == INVALID_HANDLE_VALUE) return fail(ErrorCode::AccessDenied, "cannot reopen the export to flush it");
    const bool flushed = FlushFileBuffers(handle) != 0;
    CloseHandle(handle);
#else
    const int descriptor = ::open(file.c_str(), O_WRONLY | O_CLOEXEC);
    if (descriptor < 0) return fail(ErrorCode::AccessDenied, "cannot reopen the export to flush it");
    const bool flushed = ::fsync(descriptor) == 0;
    ::close(descriptor);
#endif
    if (!flushed) return fail(ErrorCode::DiskFull, "the export could not be flushed to disk");
    return {};
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
    const auto temporary = temporary_sibling(destination);
    ExportSummary summary{0, destination};
    auto identity = library.identity();
    auto start_revision = library.catalog_revision();  // identity() is cached; read the database
    if (!start_revision) return std::unexpected(start_revision.error());
    identity.catalog_revision = *start_revision;
    const bool csv = options.format == ExportFormat::Csv;
    std::int64_t revision_at_end = identity.catalog_revision;
    {
        std::ofstream out(temporary, std::ios::binary | std::ios::trunc);
        if (!out) return fail(ErrorCode::AccessDenied, "cannot create the export file");
        auto cleanup_on_failure = [&](Error error) {
            out.close();
            remove_quietly(temporary);
            return std::unexpected(std::move(error));
        };
        auto write_failed = [&] {
            return cleanup_on_failure(Error{ErrorCode::DiskFull, false, "writing the export failed", {}, {}});
        };

        auto csv_writer = csv::make_csv_writer(out);  // RFC 4180 quoting, CRLF records (patched)
        std::optional<JsonObjectWriter> json_writer;
        if (csv) {
            if (options.utf8_bom) out << "\xEF\xBB\xBF";
            csv_writer << std::vector<std::string>(std::begin(kCsvColumns), std::end(kCsvColumns));
        } else {
            // The envelope is written member by member around the streamed sample array.
            json_writer.emplace(out);
            json_writer->member("schema_version", kExportSchemaVersion);
            json_writer->member("library", {{"uuid", uuids::to_string(identity.uuid)},
                                            {"name", identity.name},
                                            {"catalog_revision", identity.catalog_revision}});
            json_writer->member("exported_at", now_unix_ms());
            json_writer->member("absolute_paths", options.include_absolute_paths);
            json_writer->begin_array("samples");
        }
        if (!out) return write_failed();

        search::SearchService service;
        search::SearchRequest request{
            .text = options.query, .sort = search::SortField::Added, .page_size = search::SearchService::kMaxPageSize};
        catalog::Library* scope[] = {&library};
        while (true) {
            if (stop.stop_requested())
                return cleanup_on_failure(Error{ErrorCode::Cancelled, false, "export cancelled", {}, {}});
            auto page = service.search(scope, request);
            if (!page) return cleanup_on_failure(page.error());
            for (const auto& row : page->rows) {
                std::string absolute;
                if (options.include_absolute_paths)
                    if (auto base = root_path(row.root_uuid, row.root_mode))
                        absolute = path_to_utf8(*base / path_from_utf8(row.relative_path));
                if (csv) {
                    auto values = csv_values(row, absolute);
                    if (options.spreadsheet_safe)
                        for (auto& value : values)
                            value = spreadsheet_safe(std::move(value));
                    csv_writer << values;
                } else {
                    json_writer->element(json_row(row, absolute, options.include_absolute_paths));
                }
                ++summary.rows;
            }
            if (!out) return write_failed();  // checked per page, not only at the end
            if (!page->next) break;
            request.cursor = page->next;
        }
        auto end_revision = library.catalog_revision();
        if (!end_revision) return cleanup_on_failure(end_revision.error());
        revision_at_end = *end_revision;
        if (csv) {
            csv_writer.flush();
        } else {
            json_writer->end_array();
            json_writer->member("catalog_revision_at_end", revision_at_end);
            json_writer->member("consistent", revision_at_end == identity.catalog_revision);
            json_writer->finish();
        }
        out.flush();
        if (!out) return write_failed();
    }

    if (auto flushed = flush_to_disk(temporary); !flushed) {
        remove_quietly(temporary);
        return std::unexpected(flushed.error());
    }
    std::error_code error;
    std::filesystem::rename(temporary, destination, error);
    if (error) {
        const auto message = std::format("cannot replace the export destination: {}", error.message());
        remove_quietly(temporary);
        return fail(ErrorCode::AccessDenied, message);
    }
    if (csv) {
        // The manifest describes the CSV just published. If it cannot be written, an older manifest
        // must not stay next to the new CSV claiming to describe it.
        auto manifest_path = destination;
        manifest_path += ".manifest.json";
        const auto manifest = json_fields::dump_json({{"schema_version", kExportSchemaVersion},
                                                      {"library", uuids::to_string(identity.uuid)},
                                                      {"catalog_revision", identity.catalog_revision},
                                                      {"catalog_revision_at_end", revision_at_end},
                                                      {"consistent", revision_at_end == identity.catalog_revision},
                                                      {"rows", summary.rows},
                                                      {"spreadsheet_safe", options.spreadsheet_safe},
                                                      {"absolute_paths", options.include_absolute_paths},
                                                      {"tags_encoding", "JSON array per cell"}},
                                                     2);
        if (auto written = catalog::write_file_atomically(manifest_path, manifest); !written) {
            remove_quietly(manifest_path);
            return fail(written.error().code, std::format("the export was written but its manifest could not be: {}",
                                                          written.error().message()));
        }
    }
    return summary;
}

}  // namespace mso::app
