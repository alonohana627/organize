// bench: synthetic-metadata scale benchmark (docs/06_QUALITY_AND_OPERATIONS.md). It measures the
// catalog, SQLite search and the in-memory SampleIndex on generated rows; it is not a claim about
// decoding real audio.
#include <algorithm>
#include <chrono>
#include <functional>
#include <random>

#include "mso/app/sample_index.hpp"
#include "mso/cli/command_context.hpp"

namespace mso::cli {

namespace {

using Clock = std::chrono::steady_clock;

/// Rows committed per synthetic scan batch (the catalog's batch limit).
constexpr std::int64_t kBatchRows = 500;
/// Upper bounds of the benchmark options.
constexpr std::int64_t kMaximumRows = 10'000'000;
constexpr int kMaximumRepetitions = 10'000;

/// Deterministic synthetic catalog rows with skewed names, folders, durations, BPM and missing values.
std::vector<catalog::ScannedFile> synthetic_batch(std::mt19937_64& random, std::int64_t first, std::int64_t count) {
    static constexpr std::string_view kWords[] = {"kick", "snare",  "hat",   "clap",    "bass", "pad",
                                                  "lead", "vox",    "riser", "impact",  "loop", "perc",
                                                  "tom",  "shaker", "שלום",  "サンプル"};
    static constexpr std::string_view kFolders[] = {"Drums/Kicks", "Drums/Snares", "Loops/House", "Loops/Techno",
                                                    "FX/Risers",   "Vocals",       "Bass",        "Pads/Ambient"};
    std::vector<catalog::ScannedFile> files;
    files.reserve(static_cast<std::size_t>(count));
    std::geometric_distribution<int> skew(0.35);
    for (std::int64_t index = first; index < first + count; ++index) {
        const auto word = kWords[static_cast<std::size_t>(skew(random)) % std::size(kWords)];
        const auto folder = kFolders[random() % std::size(kFolders)];
        catalog::ScannedFile file;
        file.relative_path = std::format("{}/{} {:07}.wav", folder, word, index);
        file.byte_size = static_cast<std::int64_t>(10'000 + random() % 50'000'000);
        file.mtime_ns = 1'700'000'000'000'000'000 + index;
        catalog::ObservedMetadata metadata;
        metadata.format = random() % 5 == 0 ? "flac" : "wav";
        metadata.codec = "pcm_s24";
        metadata.sample_rate = random() % 2 ? 44100 : 48000;
        metadata.channels = 1 + static_cast<std::int64_t>(random() % 2);
        if (random() % 10 != 0) {  // ~10% unknown durations
            metadata.duration_us = static_cast<std::int64_t>(50'000 + random() % 30'000'000);
            metadata.duration_exact = true;
        }
        if (random() % 3 == 0) metadata.filename_bpm_milli = static_cast<std::int64_t>(70 + random() % 110) * 1000;
        file.metadata = metadata;
        files.push_back(std::move(file));
    }
    return files;
}

/// Median, 95th percentile and maximum of repeated measurements.
struct Timing {
    double median_ms = 0;
    double p95_ms = 0;
    double max_ms = 0;
};

/// Runs `work` `repetitions` times (at least once); `work` returns false on failure, counted in
/// `failures`. `prepare` runs before each repetition, outside the measured time.
Timing measure(int repetitions, int& failures, const std::function<bool()>& work,
               const std::function<void()>& prepare = {}) {
    std::vector<double> samples;
    for (int index = 0; index < std::max(1, repetitions); ++index) {
        if (prepare) prepare();
        const auto started = Clock::now();
        if (!work()) ++failures;
        samples.push_back(std::chrono::duration<double, std::milli>(Clock::now() - started).count());
    }
    std::ranges::sort(samples);
    return Timing{samples[samples.size() / 2], samples[std::min(samples.size() - 1, samples.size() * 95 / 100)],
                  samples.back()};
}

/// A named request measured against SQLite and against the index.
struct Scenario {
    std::string_view label;
    search::SearchRequest request;
};

}  // namespace

void register_bench_commands(CLI::App& root, CommandContext& context, int& exit_code) {
    auto* command = root.add_subcommand("bench", "Benchmark catalog, SQLite search and the in-memory index");
    auto directory = std::make_shared<std::string>();
    auto rows = std::make_shared<std::int64_t>(100'000);
    auto repetitions = std::make_shared<int>(30);
    command->add_option("library", *directory, "New library folder to create")->required();
    command->add_option("--rows", *rows, "Synthetic samples to insert")
        ->check(CLI::Range(std::int64_t{1}, kMaximumRows));
    command->add_option("--repetitions", *repetitions, "Measurements per query")
        ->check(CLI::Range(1, kMaximumRepetitions));
    command->callback([&, directory, rows, repetitions] {
        auto application = context.application();
        if (!application) return void(exit_code = context.report(application.error()));
        auto created = (*application)->create_library(path_from_utf8(*directory), "Benchmark");
        if (!created) return void(exit_code = context.report(created.error()));
        int failures = 0;
        auto ran = (*application)->with_library(created->uuid, [&](catalog::Library& library) -> Result<void> {
            auto root_record = library.add_root("Synthetic", catalog::RootMode::Referenced);
            if (!root_record) return std::unexpected(root_record.error());
            if (auto online = library.set_root_availability(root_record->id, catalog::Availability::Online); !online)
                return online;

            std::mt19937_64 random(20261019);
            auto session = library.begin_scan(root_record->id);
            if (!session) return std::unexpected(session.error());
            const auto import_started = Clock::now();
            for (std::int64_t first = 0; first < *rows; first += kBatchRows) {
                const auto batch = synthetic_batch(random, first, std::min(kBatchRows, *rows - first));
                auto committed = library.commit_scan_batch(*session, batch, {}, [](std::string_view) { return false; });
                if (!committed) return std::unexpected(committed.error());
            }
            if (auto finished = library.finish_scan(*session, false); !finished)
                return std::unexpected(finished.error());
            const double import_seconds = std::chrono::duration<double>(Clock::now() - import_started).count();
            context.print("inserted {} synthetic rows in {:.1f} s ({:.0f} rows/s, catalog only, no file I/O)", *rows,
                          import_seconds, static_cast<double>(*rows) / import_seconds);

            const std::vector<Scenario> scenarios = {
                {"first page, name sort", {.sort = search::SortField::Name}},
                {"first page, newest first", {.sort = search::SortField::Added, .descending = true}},
                {"folder click Drums/Kicks, name sort",
                 {.scope = {.root_id = root_record->id, .folder = "Drums/Kicks"}, .sort = search::SortField::Name}},
                {"sort by duration, descending", {.sort = search::SortField::Duration, .descending = true}},
                {"text 'kick'", {.text = "kick"}},
                {"text + filters 'kick bpm:120..130 duration:<2s'", {.text = "kick bpm:120..130 duration:<2s"}},
                {"filter only 'bpm:120..130'", {.text = "bpm:120..130", .sort = search::SortField::Bpm}},
                {"folder filter, duration sort", {.text = "folder:Loops", .sort = search::SortField::Duration}},
                {"prefix 'sha*'", {.text = "sha*"}},
            };
            catalog::Library* sql_scope[] = {&library};
            search::SearchService service;
            context.print("-- SQLite (search::SearchService)");
            for (const auto& scenario : scenarios) {
                const auto timing = measure(*repetitions, failures,
                                            [&] { return service.search(sql_scope, scenario.request).has_value(); });
                context.print("{:<50} median {:7.2f} ms   p95 {:7.2f} ms   max {:7.2f} ms", scenario.label,
                              timing.median_ms, timing.p95_ms, timing.max_ms);
            }
            const auto deep = measure(std::max(1, *repetitions / 5), failures, [&] {
                search::SearchRequest request{.sort = search::SortField::Duration};
                for (int page = 0; page < 10; ++page) {
                    auto result = service.search(sql_scope, request);
                    if (!result) return false;
                    if (!result->next) break;
                    request.cursor = result->next;
                }
                return true;
            });
            context.print("{:<50} median {:7.2f} ms   p95 {:7.2f} ms", "10 consecutive pages (keyset)", deep.median_ms,
                          deep.p95_ms);
            const auto counted =
                measure(5, failures, [&] { return service.count(sql_scope, {.text = "kick"}).has_value(); });
            context.print("{:<50} median {:7.2f} ms", "exact count 'kick'", counted.median_ms);

            // The desktop app answers from the in-memory index instead.
            context.print("-- in-memory index (app::SampleIndex)");
            app::SampleIndex index(library.uuid());
            const auto load_started = Clock::now();
            if (auto loaded = index.load(library); !loaded) return loaded;
            if (auto caught_up = index.synchronize(library); !caught_up) return std::unexpected(caught_up.error());
            context.print("{:<50} {:7.0f} ms for {} rows", "load (for_each_row)",
                          std::chrono::duration<double, std::milli>(Clock::now() - load_started).count(), index.size());
            const app::IndexedLibrary sources[] = {{&index, &library, "Benchmark"}};
            for (const auto& scenario : scenarios) {
                // The first query of a sort field also builds that field's order (kept up to date
                // incrementally afterwards); later queries drop the cached filtered results first, so
                // they measure a fresh folder click or sort, not a cache hit.
                const auto first_started = Clock::now();
                if (!app::search_indexes(sources, scenario.request)) ++failures;
                const double first_ms = std::chrono::duration<double, std::milli>(Clock::now() - first_started).count();
                const auto timing = measure(
                    *repetitions, failures, [&] { return app::search_indexes(sources, scenario.request).has_value(); },
                    [&] { index.drop_query_caches(); });
                context.print("{:<50} median {:7.2f} ms   p95 {:7.2f} ms   max {:7.2f} ms   first {:7.2f} ms",
                              scenario.label, timing.median_ms, timing.p95_ms, timing.max_ms, first_ms);
            }
            const auto deep_index = measure(std::max(1, *repetitions / 5), failures, [&] {
                search::SearchRequest request{.sort = search::SortField::Duration};
                for (int page = 0; page < 10; ++page) {
                    auto result = app::search_indexes(sources, request);
                    if (!result) return false;
                    if (!result->next) break;
                    request.cursor = result->next;
                }
                return true;
            });
            context.print("{:<50} median {:7.2f} ms   p95 {:7.2f} ms", "10 consecutive pages (offset cursor)",
                          deep_index.median_ms, deep_index.p95_ms);
            const auto counted_index = measure(
                5, failures, [&] { return app::count_indexes(sources, {.text = "bpm:120..130"}).has_value(); },
                [&] { index.drop_query_caches(); });
            context.print("{:<50} median {:7.2f} ms", "exact count 'bpm:120..130'", counted_index.median_ms);
            std::int64_t rating = 0;
            const auto edit = measure(std::max(1, *repetitions / 5), failures, [&] {
                catalog::EditCommand command;
                command.sample_ids = {1};
                command.rating = static_cast<int>(++rating % 6);
                return library.apply_edit(command).has_value() && index.synchronize(library).has_value();
            });
            context.print("{:<50} median {:7.2f} ms   p95 {:7.2f} ms", "rating edit committed + index patched",
                          edit.median_ms, edit.p95_ms);

            std::error_code size_error;
            const auto database_size = std::filesystem::file_size(library.directory() / "catalog.sqlite", size_error);
            if (!size_error)
                context.print("database size {}", format_byte_size(static_cast<std::int64_t>(database_size)));
            return {};
        });
        if (!ran) return void(exit_code = context.report(ran.error()));
        if (failures > 0)
            exit_code = context.report(
                Error{ErrorCode::Internal, false, std::format("{} measured operation(s) failed", failures), {}, {}});
    });
}

}  // namespace mso::cli
