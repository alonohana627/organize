// bench: synthetic-metadata scale benchmark (docs/06_QUALITY_AND_OPERATIONS.md). It measures the
// catalog and search on generated rows; it is not a claim about decoding real audio.
#include <algorithm>
#include <chrono>
#include <random>

#include "mso/cli/command_context.hpp"

namespace mso::cli {

namespace {

using Clock = std::chrono::steady_clock;

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

struct Timing {
    double median_ms = 0;
    double p95_ms = 0;
    double max_ms = 0;
};

Timing measure(int repetitions, const std::function<void()>& work) {
    std::vector<double> samples;
    for (int index = 0; index < repetitions; ++index) {
        const auto started = Clock::now();
        work();
        samples.push_back(std::chrono::duration<double, std::milli>(Clock::now() - started).count());
    }
    std::ranges::sort(samples);
    return Timing{samples[samples.size() / 2], samples[std::min(samples.size() - 1, samples.size() * 95 / 100)],
                  samples.back()};
}

}  // namespace

void register_bench_commands(CLI::App& root, CommandContext& context, int& exit_code) {
    auto* command = root.add_subcommand("bench", "Benchmark catalog and search on synthetic metadata rows");
    auto directory = std::make_shared<std::string>();
    auto rows = std::make_shared<std::int64_t>(100'000);
    auto repetitions = std::make_shared<int>(30);
    command->add_option("library", *directory, "New library folder to create")->required();
    command->add_option("--rows", *rows, "Synthetic samples to insert");
    command->add_option("--repetitions", *repetitions, "Measurements per query");
    command->callback([&, directory, rows, repetitions] {
        auto application = context.application();
        if (!application) return void(exit_code = CommandContext::report(application.error()));
        auto created = (*application)->create_library(path_from_utf8(*directory), "Benchmark");
        if (!created) return void(exit_code = CommandContext::report(created.error()));
        auto* library = (*application)->library(created->uuid);
        auto root_record = library->add_root("Synthetic", catalog::RootMode::Referenced);
        if (!root_record) return void(exit_code = CommandContext::report(root_record.error()));
        (void)library->set_root_availability(root_record->id, catalog::Availability::Online);

        std::mt19937_64 random(20261019);
        auto session = library->begin_scan(root_record->id);
        if (!session) return void(exit_code = CommandContext::report(session.error()));
        const auto import_started = Clock::now();
        for (std::int64_t first = 0; first < *rows; first += 500) {
            const auto batch = synthetic_batch(random, first, std::min<std::int64_t>(500, *rows - first));
            auto committed = library->commit_scan_batch(*session, batch, {}, [](std::string_view) { return false; });
            if (!committed) return void(exit_code = CommandContext::report(committed.error()));
        }
        (void)library->finish_scan(*session, false);
        const double import_seconds = std::chrono::duration<double>(Clock::now() - import_started).count();
        std::println("inserted {} synthetic rows in {:.1f} s ({:.0f} rows/s, catalog only, no file I/O)", *rows,
                     import_seconds, static_cast<double>(*rows) / import_seconds);

        struct Scenario {
            std::string_view label;
            search::SearchRequest request;
        };
        const Scenario scenarios[] = {
            {"first page, name sort", {.sort = search::SortField::Name}},
            {"first page, newest first", {.sort = search::SortField::Added, .descending = true}},
            {"text 'kick'", {.text = "kick"}},
            {"text + filters 'kick bpm:120..130 duration:<2s'", {.text = "kick bpm:120..130 duration:<2s"}},
            {"folder filter, duration sort", {.text = "folder:Loops", .sort = search::SortField::Duration}},
            {"prefix 'sha*'", {.text = "sha*"}},
            {"relevance 'kick OR snare'", {.text = "kick OR snare", .sort = search::SortField::Relevance}},
        };
        for (const auto& scenario : scenarios) {
            const auto timing = measure(*repetitions, [&] { (void)(*application)->search(scenario.request); });
            std::println("{:<50} median {:7.2f} ms   p95 {:7.2f} ms   max {:7.2f} ms", scenario.label, timing.median_ms,
                         timing.p95_ms, timing.max_ms);
        }
        const auto deep = measure(std::max(1, *repetitions / 5), [&] {
            search::SearchRequest request{.sort = search::SortField::Duration};
            for (int page = 0; page < 10; ++page) {
                auto result = (*application)->search(request);
                if (!result || !result->next) break;
                request.cursor = result->next;
            }
        });
        std::println("{:<50} median {:7.2f} ms   p95 {:7.2f} ms", "10 consecutive pages (keyset)", deep.median_ms,
                     deep.p95_ms);
        const auto counted = measure(5, [&] { (void)(*application)->count({.text = "kick"}); });
        std::println("{:<50} median {:7.2f} ms", "exact count 'kick'", counted.median_ms);
        std::println("database size {}", format_byte_size(static_cast<std::int64_t>(
                                             std::filesystem::file_size(library->directory() / "catalog.sqlite"))));
    });
}

}  // namespace mso::cli
