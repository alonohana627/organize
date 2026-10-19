// search, show, tags, collections commands.
#include <nlohmann/json.hpp>

#include "mso/cli/command_context.hpp"

namespace mso::cli {

namespace {

std::string optional_text(const std::optional<std::int64_t>& value) {
    return value ? std::to_string(*value) : "-";
}

void print_row(const search::SampleRow& row) {
    const std::string duration =
        row.duration_us ? format_duration(*row.duration_us) + (row.duration_exact ? "" : "~") : "?";
    const std::string bpm = row.bpm_milli ? format_bpm(*row.bpm_milli) : "-";
    const std::string key = row.key ? format_key(*row.key) : "-";
    std::println("{:>6}  {:<36.36} {:<5} {:>8} {:>7} {:>4} {:<4} {:<15} {}", row.sample_id, row.display_name,
                 row.format, duration, bpm, key,
                 std::string(static_cast<std::size_t>(row.rating), '*') + (row.favorite ? "♥" : ""),
                 search::to_string(row.availability), row.tags);
}

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

}  // namespace

void register_search_commands(CLI::App& root, CommandContext& context, int& exit_code) {
    {
        auto* command = root.add_subcommand("search", "Search one or more libraries with the query language");
        auto directories = std::make_shared<std::vector<std::string>>();
        auto query = std::make_shared<std::string>();
        auto sort = std::make_shared<std::string>("name");
        auto descending = std::make_shared<bool>(false);
        auto limit = std::make_shared<std::size_t>(50);
        auto count = std::make_shared<bool>(false);
        command->add_option("--library,-l", *directories, "Library folder (repeat to search several together)")
            ->required()
            ->allow_extra_args(false);
        command->add_option("query", *query, "Query, e.g. 'kick bpm:120..130 tag:Drums rating:>=3'");
        command->add_option("--sort", *sort, "name, added, duration, size, rate, bpm, key, rating, format, relevance");
        command->add_flag("--desc", *descending, "Descending order");
        command->add_option("--limit", *limit, "Maximum rows to print (pages of up to 500 are fetched)");
        command->add_flag("--count", *count, "Also print the exact total");
        command->callback([&, directories, query, sort, descending, limit, count] {
            for (const auto& directory : *directories)
                if (auto library = context.open_library(path_from_utf8(directory)); !library)
                    return void(exit_code = CommandContext::report(library.error()));
            auto field = search::parse_sort_field(*sort);
            if (!field)
                return void(exit_code = CommandContext::report(
                                Error{ErrorCode::InvalidArgument, false, "unknown sort field", {}, {}}));
            search::SearchRequest request{
                .text = *query,
                .sort = *field,
                .descending = *descending,
                .page_size = std::min<std::size_t>(*limit, search::SearchService::kMaxPageSize)};
            auto* application = *context.application();
            std::size_t printed = 0;
            nlohmann::json rows = nlohmann::json::array();
            while (printed < *limit) {
                auto page = application->search(request);
                if (!page) return void(exit_code = CommandContext::report(page.error()));
                for (const auto& coverage : page->coverage)
                    if (!coverage.complete)
                        std::println(stderr, "warning: results exclude library '{}': {}", coverage.name,
                                     coverage.error ? coverage.error->message() : "unavailable");
                for (const auto& row : page->rows) {
                    if (printed++ >= *limit) break;
                    if (context.json_output)
                        rows.push_back(row_json(row));
                    else
                        print_row(row);
                }
                if (!page->next) break;
                request.cursor = page->next;
            }
            if (context.json_output) std::println("{}", rows.dump(2));
            if (*count) {
                auto total = application->count(request);
                if (total) std::println("{} matching sample(s)", total->total);
            }
        });
    }
    {
        auto* command = root.add_subcommand("show", "Show everything known about one sample");
        auto directory = std::make_shared<std::string>();
        auto sample = std::make_shared<std::int64_t>(0);
        command->add_option("library", *directory, "Library folder")->required();
        command->add_option("sample", *sample, "Sample id (from search)")->required();
        command->callback([&, directory, sample] {
            auto library = context.open_library(path_from_utf8(*directory));
            if (!library) return void(exit_code = CommandContext::report(library.error()));
            auto detail = (*library)->sample_detail(*sample);
            if (!detail) return void(exit_code = CommandContext::report(detail.error()));
            std::println("{} ({})", detail->display_name, uuids::to_string(detail->uuid));
            std::println("  file      {}", detail->original_name);
            std::println("  format    {} / {}  {} Hz  {} ch  {} bit  {} frames{}", detail->format, detail->codec,
                         optional_text(detail->sample_rate), optional_text(detail->channels),
                         optional_text(detail->bit_depth), optional_text(detail->frames),
                         detail->duration_exact ? "" : " (estimated)");
            std::println("  bpm       {} [{}]  key {} [{}]",
                         detail->bpm_milli ? format_bpm(*detail->bpm_milli) : "unknown", detail->bpm_source,
                         detail->key ? format_key(*detail->key) : "unknown", detail->key_source);
            std::println("  rating    {}  favorite {}  category {}", detail->rating, detail->favorite,
                         detail->category_path);
            std::string tags;
            for (const auto& tag : detail->tags)
                tags += (tags.empty() ? "" : ", ") + tag.path;
            std::println("  tags      {}", tags);
            if (!detail->notes.empty()) std::println("  notes     {}", detail->notes);
            for (const auto& location : detail->locations)
                std::println("  location  {}/{} [{}]", location.root_name, location.relative_path,
                             to_string(location.state));
            for (const auto& observation : detail->observations)
                std::println("  observed  {}={} via {}{} rev {}", observation.field, observation.value_json,
                             observation.source, observation.valid ? "" : " (superseded)",
                             observation.content_revision);
        });
    }
    {
        auto* command = root.add_subcommand("tags", "List the tag and category trees with counts");
        auto directory = std::make_shared<std::string>();
        command->add_option("library", *directory, "Library folder")->required();
        command->callback([&, directory] {
            auto library = context.open_library(path_from_utf8(*directory));
            if (!library) return void(exit_code = CommandContext::report(library.error()));
            auto tags = (*library)->list_tags();
            auto categories = (*library)->list_categories();
            if (!tags) return void(exit_code = CommandContext::report(tags.error()));
            if (!categories) return void(exit_code = CommandContext::report(categories.error()));
            std::println("tags:");
            for (const auto& tag : *tags)
                std::println("  {:>5}  {:<40} {}", tag.id, tag.path, tag.sample_count);
            std::println("categories:");
            for (const auto& category : *categories)
                std::println("  {:>5}  {:<40} {}", category.id, category.path, category.sample_count);
        });
    }
    {
        auto* command = root.add_subcommand("collections", "List collections");
        auto directory = std::make_shared<std::string>();
        command->add_option("library", *directory, "Library folder")->required();
        command->callback([&, directory] {
            auto library = context.open_library(path_from_utf8(*directory));
            if (!library) return void(exit_code = CommandContext::report(library.error()));
            auto collections = (*library)->list_collections();
            if (!collections) return void(exit_code = CommandContext::report(collections.error()));
            for (const auto& collection : *collections)
                std::println("  {:>5}  {:<30} {}", collection.id, collection.name,
                             collection.smart ? "smart: " + collection.query.value_or("")
                                              : std::to_string(collection.sample_count) + " samples");
        });
    }
}

}  // namespace mso::cli
