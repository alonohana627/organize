// probe, play, capabilities and analyze commands.
#include <chrono>
#include <thread>

#include "mso/audio/preview_engine.hpp"
#include "mso/cli/command_context.hpp"
#include "mso/media/media.hpp"

namespace mso::cli {

namespace {

std::string optional_number(const std::optional<std::int64_t>& value) {
    return value ? std::to_string(*value) : "?";
}

/// Waits until every listed run of a library is finished, printing progress.
int wait_for_runs(catalog::Library& library, const std::vector<std::int64_t>& run_ids) {
    while (true) {
        auto runs = library.list_analysis_runs(100);
        if (!runs) return CommandContext::report(runs.error());
        bool pending = false;
        for (const auto& run : *runs) {
            if (std::ranges::find(run_ids, run.id) == run_ids.end()) continue;
            std::print("\r  run {}: {} done, {} queued, {} running, {} failed, {} blocked   ", run.id, run.completed,
                       run.queued, run.running, run.failed, run.blocked);
            std::fflush(stdout);
            pending = pending || run.queued + run.running > 0;
        }
        if (!pending) break;
        std::this_thread::sleep_for(std::chrono::milliseconds(250));
    }
    std::println("");
    return kExitSuccess;
}

}  // namespace

void register_media_commands(CLI::App& root, CommandContext& /*context*/, int& exit_code) {
    {
        auto* command = root.add_subcommand("probe", "Show what a bounded import probe reads from a file");
        auto file = std::make_shared<std::string>();
        command->add_option("file", *file, "Audio file")->required();
        command->callback([&, file] {
            auto probe = media::probe_file(path_from_utf8(*file));
            if (!probe) return void(exit_code = CommandContext::report(probe.error()));
            std::println("format {} codec {}  {} Hz  {} ch  {} bit{}", probe->format, probe->codec,
                         optional_number(probe->sample_rate), optional_number(probe->channels),
                         optional_number(probe->bit_depth), probe->float_samples ? " float" : "");
            std::println("frames {}  duration {}{}  read {} bytes{}", optional_number(probe->frames),
                         probe->duration_us ? format_duration(*probe->duration_us) : "?",
                         probe->duration_exact ? "" : " (estimate)", probe->bytes_read,
                         probe->partial ? "  [probe limit reached]" : "");
            const auto& tags = probe->tags;
            if (tags.title) std::println("title  {}", *tags.title);
            if (tags.artist) std::println("artist {}", *tags.artist);
            if (tags.bpm_milli) std::println("bpm    {} (tag)", format_bpm(*tags.bpm_milli));
            if (tags.key) std::println("key    {} (tag)", format_key(*tags.key));
            for (const auto& loop : probe->loops)
                std::println("loop   {}..{}", loop.start_frame, loop.end_frame);
            const auto hints = media::filename_hints(path_to_utf8(path_from_utf8(*file).stem()));
            if (hints.bpm_milli) std::println("bpm    {} (file name)", format_bpm(*hints.bpm_milli));
            if (hints.key) std::println("key    {} (file name)", format_key(*hints.key));
        });
    }
    {
        auto* command = root.add_subcommand("play", "Preview a file on the default output device");
        auto file = std::make_shared<std::string>();
        auto volume = std::make_shared<double>(-6.0);
        auto seconds = std::make_shared<double>(0);
        command->add_option("file", *file, "Audio file")->required();
        command->add_option("--volume", *volume, "Volume in dB (<= 0)");
        command->add_option("--seconds", *seconds, "Stop after this many seconds (0 = whole file)");
        command->callback([&, file, volume, seconds] {
            auto engine = audio::PreviewEngine::create({});
            if (!engine) return void(exit_code = CommandContext::report(engine.error()));
            if (auto opened = (*engine)->open(path_from_utf8(*file)); !opened)
                return void(exit_code = CommandContext::report(opened.error()));
            (*engine)->post(audio::transport::SetVolumeDecibels{std::min(0.0, *volume)});
            (*engine)->post(audio::transport::Play{});
            const auto started = std::chrono::steady_clock::now();
            bool was_playing = false;
            while (true) {
                std::this_thread::sleep_for(std::chrono::milliseconds(50));
                const auto snapshot = (*engine)->snapshot();
                if (snapshot.last_error) {
                    exit_code = CommandContext::report(Error{*snapshot.last_error, false, "playback stopped", {}, {}});
                    break;
                }
                if (snapshot.state == audio::PreviewState::Playing) was_playing = true;
                const double elapsed =
                    std::chrono::duration<double>(std::chrono::steady_clock::now() - started).count();
                if (snapshot.source_sample_rate > 0)
                    std::print("\r  {} / {}   underruns {}   ",
                               format_duration(snapshot.position_frames * 1'000'000 / snapshot.source_sample_rate),
                               snapshot.total_frames
                                   ? format_duration(*snapshot.total_frames * 1'000'000 / snapshot.source_sample_rate)
                                   : "?",
                               snapshot.underrun_count);
                std::fflush(stdout);
                if ((was_playing && snapshot.state != audio::PreviewState::Playing) ||
                    (*seconds > 0 && elapsed >= *seconds))
                    break;
                if (elapsed > 3600) break;
            }
            std::println("");
        });
    }
    {
        auto* command = root.add_subcommand("capabilities", "List the codecs this build can decode");
        command->callback([] {
            for (const auto& codec : media::decoder_capabilities())
                std::println("{}", codec);
        });
    }
}

void register_analysis_commands(CLI::App& root, CommandContext& context, int& exit_code) {
    auto* command = root.add_subcommand("analyze", "Run an explicit analysis profile on matching samples and wait");
    auto directory = std::make_shared<std::string>();
    auto profile = std::make_shared<std::string>("Musical");
    auto query = std::make_shared<std::string>();
    auto force = std::make_shared<bool>(false);
    command->add_option("library", *directory, "Library folder")->required();
    command->add_option("query", *query, "Only samples matching this search (default: all)");
    command->add_option("--profile", *profile, "Waveforms, Musical, Duplicates or Full")
        ->check(CLI::IsMember({"Waveforms", "Musical", "Duplicates", "Full"}));
    command->add_flag("--force", *force, "Re-analyze even when current results exist");
    command->callback([&, directory, profile, query, force] {
        auto application = context.application(/*with_analysis=*/true);
        if (!application) return void(exit_code = CommandContext::report(application.error()));
        auto library = context.open_library(path_from_utf8(*directory));
        if (!library) return void(exit_code = CommandContext::report(library.error()));
        (*application)->set_analysis_paused(false);

        // Freeze the selection now: later matches are not added to this run.
        std::vector<std::int64_t> samples;
        search::SearchRequest request{
            .text = *query, .sort = search::SortField::Added, .page_size = search::SearchService::kMaxPageSize};
        while (true) {
            auto page = (*application)->search(request, (*library)->uuid());
            if (!page) return void(exit_code = CommandContext::report(page.error()));
            for (const auto& row : page->rows)
                samples.push_back(row.sample_id);
            if (!page->next) break;
            request.cursor = page->next;
        }
        if (samples.empty()) return void(std::println("no samples match"));
        auto run = (*application)->start_analysis((*library)->uuid(), *profile, samples, *force);
        if (!run) return void(exit_code = CommandContext::report(run.error()));
        std::println("run {}: {} queued, {} already analyzed", run->id, run->queued, run->skipped);
        exit_code = wait_for_runs(**library, {run->id});
    });
}

}  // namespace mso::cli
