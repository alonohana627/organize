// search, show, tags, collections commands.
#include <set>

#include <nlohmann/json.hpp>

#include "mso/cli/command_context.hpp"
#include "mso/domain/json_fields.hpp"

namespace mso::cli {

namespace {

/// Sort names accepted by `search --sort`.
const std::vector<std::string> kSortNames = {"name", "added", "duration", "size",   "rate",
                                             "bpm",  "key",   "rating",   "format", "relevance"};

/// A known number, or "-".
std::string optional_text(const std::optional<std::int64_t>& value) {
    return value ? std::to_string(*value) : "-";
}

/// Tag paths as one cell: "Drums/Kick, Mood/Dark".
std::string join_tags(const std::vector<std::string>& paths) {
    std::string joined;
    for (const auto& path : paths)
        joined += (joined.empty() ? "" : ", ") + path;
    return joined;
}

/// One table line of `search`.
void print_row(const CommandContext& context, const search::SampleRow& row) {
    const std::string duration =
        row.duration_us ? format_duration(*row.duration_us) + (row.duration_exact ? "" : "~") : "?";
    const std::string bpm = row.bpm_milli ? format_bpm(*row.bpm_milli) : "-";
    const std::string key = row.key ? format_key(*row.key) : "-";
    context.print("{:>6}  {:<36.36} {:<5} {:>8} {:>7} {:>4} {:<4} {:<15} {}", row.sample_id, row.display_name,
                  row.format, duration, bpm, key,
                  std::string(static_cast<std::size_t>(row.rating), '*') + (row.favorite ? "♥" : ""),
                  search::to_string(row.availability), join_tags(row.tag_paths));
}

/// One row of `search --json`.
nlohmann::json row_json(const search::SampleRow& row) {
    auto nullable = [](const std::optional<std::int64_t>& value) {
        return value ? nlohmann::json(*value) : nlohmann::json();
    };
    return {{"library", uuids::to_string(row.library)},
            {"id", row.sample_id},
            {"uuid", uuids::to_string(row.sample_uuid)},
            {"name", row.display_name},
            {"file", row.file_name},
            {"path", row.relative_path},
            {"format", row.format},
            {"duration_us", nullable(row.duration_us)},
            {"sample_rate", nullable(row.sample_rate)},
            {"channels", nullable(row.channels)},
            {"bpm_milli", nullable(row.bpm_milli)},
            {"bpm_source", row.bpm_source},
            {"key", row.key ? nlohmann::json(format_key(*row.key)) : nlohmann::json()},
            {"rating", row.rating},
            {"favorite", row.favorite},
            {"tags", row.tag_paths},
            {"category", row.category},
            {"availability", std::string(search::to_string(row.availability))}};
}

/// Warns once per library missing from the results (every page reports coverage, so a library
/// that stays offline would otherwise be reported once per page).
class CoverageWarnings {
public:
    explicit CoverageWarnings(const CommandContext& context) : context_(context) {}

    /// Warns about each library of `coverage` that is missing and was not reported yet.
    void check(const std::vector<search::LibraryCoverage>& coverage) {
        for (const auto& library : coverage) {
            if (library.complete) continue;
            incomplete_ = true;
            if (!warned_.insert(uuids::to_string(library.library)).second) continue;
            context_.warn("results exclude library '{}': {}", library.name,
                          library.error ? library.error->message() : "unavailable");
        }
    }
    /// True when any page or count missed a library.
    bool incomplete() const { return incomplete_; }

private:
    const CommandContext& context_;
    std::set<std::string> warned_;  ///< library UUIDs already reported
    bool incomplete_ = false;
};

/// Prints `search --json` output as it arrives: a JSON array of rows, or with --count the object
/// {"rows": [...], "total": n, "complete": b}. Rows are printed one per line as they are read,
/// so memory stays bounded whatever --limit allows. Each line is held back until the next one
/// (or the end) is known, to put the separating comma on the line it belongs to. Nothing is
/// printed before the first row, so a search that fails at once prints no partial document.
class StreamedRows {
public:
    StreamedRows(const CommandContext& context, bool with_total) : context_(context), with_total_(with_total) {}

    /// Prints one row.
    void add(const nlohmann::json& row) {
        open();
        if (pending_) context_.print("  {},", *pending_);
        pending_ = json_fields::dump_json(row);
    }
    /// Closes the array (and the object with its total).
    void finish(std::optional<std::int64_t> total, bool complete) {
        open();
        if (pending_) context_.print("  {}", *pending_);
        pending_.reset();
        if (with_total_)
            context_.print("], \"total\": {}, \"complete\": {}}}", total.value_or(0), complete ? "true" : "false");
        else
            context_.print("]");
    }

private:
    /// Prints the opening bracket (or brace) once.
    void open() {
        if (opened_) return;
        opened_ = true;
        context_.print("{}", with_total_ ? "{\"rows\": [" : "[");
    }

    const CommandContext& context_;
    bool with_total_ = false;
    bool opened_ = false;
    std::optional<std::string> pending_;
};

/// Registers `search`.
void register_search(CLI::App& root, CommandContext& context, int& exit_code) {
    auto* command = root.add_subcommand("search", "Search one or more libraries with the query language");
    /// Every option of `search`.
    struct Options {
        std::vector<std::string> directories;
        std::string query;
        std::string sort = "name";
        bool descending = false;
        std::size_t limit = 50;
        bool count = false;
    };
    auto& options = context.command_options<Options>();
    command->add_option("--library,-l", options.directories, "Library folder (repeat to search several together)")
        ->required()
        ->allow_extra_args(false);
    command->add_option("query", options.query, "Query, e.g. 'kick bpm:120..130 tag:Drums rating:>=3'");
    command->add_option("--sort", options.sort, "Sort field")->check(CLI::IsMember(kSortNames));
    command->add_flag("--desc", options.descending, "Descending order");
    command
        ->add_option("--limit", options.limit,
                     "Maximum rows to print (pages of up to 500 are fetched and printed as they arrive)")
        ->check(CLI::Range(std::size_t{0}, std::size_t{10'000'000}));
    command->add_flag("--count", options.count, "Also report the exact total");
    command->callback([&] {
        const auto& [directories, query, sort, descending, limit, count] = options;
        for (const auto& directory : directories)
            if (auto library = context.open_library(path_from_utf8(directory)); !library)
                return void(exit_code = context.report(library.error()));
        const auto field = search::parse_sort_field(sort);
        if (!field) return void(exit_code = context.usage("unknown sort field"));
        search::SearchRequest request{
            .text = query,
            .sort = *field,
            .descending = descending,
            .page_size = std::clamp<std::size_t>(limit, 1, search::SearchService::kMaxPageSize)};
        auto* application = *context.application();
        CoverageWarnings coverage(context);
        // With --count the JSON output stays one document: {"rows": [...], "total": n, "complete": b}.
        std::optional<StreamedRows> json_rows;
        if (context.json_output) json_rows.emplace(context, count);
        std::size_t printed = 0;
        while (printed < limit) {
            auto page = application->search(request);
            if (!page) return void(exit_code = context.report(page.error()));
            coverage.check(page->coverage);
            for (const auto& row : page->rows) {
                if (printed >= limit) break;
                ++printed;
                if (json_rows)
                    json_rows->add(row_json(row));
                else
                    print_row(context, row);
            }
            if (!page->next) break;
            request.cursor = page->next;
        }
        std::optional<std::int64_t> total;
        if (count) {
            request.cursor.reset();
            auto counted = application->count(request);
            if (!counted) return void(exit_code = context.report(counted.error()));
            coverage.check(counted->coverage);
            total = counted->total;
        }
        if (json_rows)
            json_rows->finish(total, !coverage.incomplete());
        else if (total)
            context.print("{} matching sample(s)", *total);
        if (coverage.incomplete()) exit_code = kExitPartial;
    });
}

/// Registers `show`.
void register_show(CLI::App& root, CommandContext& context, int& exit_code) {
    auto* command = root.add_subcommand("show", "Show everything known about one sample");
    /// Every option of `show`.
    struct Options {
        std::string directory;
        std::int64_t sample = 0;
    };
    auto& options = context.command_options<Options>();
    command->add_option("library", options.directory, "Library folder")->required();
    command->add_option("sample", options.sample, "Sample id (from search)")->required();
    command->callback([&] {
        const auto& [directory, sample] = options;
        auto library = context.open_library(path_from_utf8(directory));
        if (!library) return void(exit_code = context.report(library.error()));
        auto detail = (*context.application())->with_library(library->uuid, [&](catalog::Library& opened) {
            return opened.sample_detail(sample);
        });
        if (!detail) return void(exit_code = context.report(detail.error()));
        context.print("{} ({})", detail->display_name, uuids::to_string(detail->uuid));
        context.print("  file      {}", detail->original_name);
        context.print("  format    {} / {}  {} Hz  {} ch  {} bit  {} frames{}", detail->format, detail->codec,
                      optional_text(detail->sample_rate), optional_text(detail->channels),
                      optional_text(detail->bit_depth), optional_text(detail->frames),
                      detail->duration_exact ? "" : " (estimated)");
        context.print("  bpm       {} [{}]  key {} [{}]",
                      detail->bpm_milli ? format_bpm(*detail->bpm_milli) : "unknown", detail->bpm_source,
                      detail->key ? format_key(*detail->key) : "unknown", detail->key_source);
        context.print("  rating    {}  favorite {}  category {}", detail->rating, detail->favorite,
                      detail->category_path);
        std::string tags;
        for (const auto& tag : detail->tags)
            tags += (tags.empty() ? "" : ", ") + tag.path;
        context.print("  tags      {}", tags);
        if (!detail->notes.empty()) context.print("  notes     {}", detail->notes);
        for (const auto& location : detail->locations)
            context.print("  location  {}/{} [{}]", location.root_name, location.relative_path,
                          to_string(location.state));
        for (const auto& observation : detail->observations)
            context.print("  observed  {}={} via {}{} rev {}", observation.field, observation.value_json,
                          observation.source, observation.valid ? "" : " (superseded)", observation.content_revision);
    });
}

/// Registers `tags` and `collections`.
void register_taxonomy_listings(CLI::App& root, CommandContext& context, int& exit_code) {
    {
        auto* command = root.add_subcommand("tags", "List the tag and category trees with counts");
        auto& directory = context.command_options<std::string>();
        command->add_option("library", directory, "Library folder")->required();
        command->callback([&] {
            auto library = context.open_library(path_from_utf8(directory));
            if (!library) return void(exit_code = context.report(library.error()));
            auto listed =
                (*context.application())->with_library(library->uuid, [&](catalog::Library& opened) -> Result<void> {
                    auto tags = opened.list_tags();
                    if (!tags) return std::unexpected(tags.error());
                    auto categories = opened.list_categories();
                    if (!categories) return std::unexpected(categories.error());
                    context.print("tags:");
                    for (const auto& tag : *tags)
                        context.print("  {:>5}  {:<40} {}", tag.id, tag.path, tag.sample_count);
                    context.print("categories:");
                    for (const auto& category : *categories)
                        context.print("  {:>5}  {:<40} {}", category.id, category.path, category.sample_count);
                    return {};
                });
            if (!listed) exit_code = context.report(listed.error());
        });
    }
    {
        auto* command = root.add_subcommand("collections", "List collections");
        auto& directory = context.command_options<std::string>();
        command->add_option("library", directory, "Library folder")->required();
        command->callback([&] {
            auto library = context.open_library(path_from_utf8(directory));
            if (!library) return void(exit_code = context.report(library.error()));
            auto collections = (*context.application())->with_library(library->uuid, [](catalog::Library& opened) {
                return opened.list_collections();
            });
            if (!collections) return void(exit_code = context.report(collections.error()));
            for (const auto& collection : *collections)
                context.print("  {:>5}  {:<30} {}", collection.id, collection.name,
                              collection.smart ? "smart: " + collection.query.value_or("")
                                               : std::to_string(collection.sample_count) + " samples");
        });
    }
}

}  // namespace

void register_search_commands(CLI::App& root, CommandContext& context, int& exit_code) {
    register_search(root, context, exit_code);
    register_show(root, context, exit_code);
    register_taxonomy_listings(root, context, exit_code);
}

}  // namespace mso::cli
