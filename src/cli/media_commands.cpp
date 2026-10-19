// probe, play and capabilities commands.
#include <algorithm>
#include <chrono>
#include <condition_variable>
#include <mutex>

#include "mso/audio/preview_engine.hpp"
#include "mso/cli/command_context.hpp"
#include "mso/cli/interrupt.hpp"
#include "mso/media/media.hpp"

namespace mso::cli {

namespace {

/// Longest `play --seconds` accepted; also the cap of an unbounded `play`.
constexpr double kMaximumPlaySeconds = 3600.0;
/// How often `play` refreshes its position line.
constexpr auto kPlayRefresh = std::chrono::milliseconds(50);
/// How long `play` waits past the file's length for the engine to report the end.
constexpr double kPlayEndGrace = 2.0;

/// A known number, or "?".
std::string optional_number(const std::optional<std::int64_t>& value) {
    return value ? std::to_string(*value) : "?";
}

/// Registers `probe`.
void register_probe(CLI::App& root, CommandContext& context, int& exit_code) {
    auto* command = root.add_subcommand("probe", "Show what a bounded import probe reads from a file");
    auto& file = context.command_options<std::string>();
    command->add_option("file", file, "Audio file")->required();
    command->callback([&] {
        auto probe = media::probe_file(path_from_utf8(file));
        if (!probe) return void(exit_code = context.report(probe.error()));
        context.print("format {} codec {}  {} Hz  {} ch  {} bit{}", probe->format, probe->codec,
                      optional_number(probe->sample_rate), optional_number(probe->channels),
                      optional_number(probe->bit_depth), probe->float_samples ? " float" : "");
        context.print("frames {}  duration {}{}  read {} bytes{}", optional_number(probe->frames),
                      probe->duration_us ? format_duration(*probe->duration_us) : "?",
                      probe->duration_exact ? "" : " (estimate)", probe->bytes_read,
                      probe->partial ? "  [probe limit reached]" : "");
        const auto& tags = probe->tags;
        if (tags.title) context.print("title  {}", *tags.title);
        if (tags.artist) context.print("artist {}", *tags.artist);
        if (tags.bpm_milli) context.print("bpm    {} (tag)", format_bpm(*tags.bpm_milli));
        if (tags.key) context.print("key    {} (tag)", format_key(*tags.key));
        for (const auto& loop : probe->loops)
            context.print("loop   {}..{}", loop.start_frame, loop.end_frame);
        const auto hints = media::filename_hints(path_to_utf8(path_from_utf8(file).stem()));
        if (hints.bpm_milli) context.print("bpm    {} (file name)", format_bpm(*hints.bpm_milli));
        if (hints.key) context.print("key    {} (file name)", format_key(*hints.key));
    });
}

/// Registers `play`.
void register_play(CLI::App& root, CommandContext& context, int& exit_code) {
    auto* command = root.add_subcommand("play", "Preview a file on the default output device");
    /// Every option of `play`.
    struct Options {
        std::string file;
        double volume = -6.0;
        double seconds = 0;
        std::string backend = "system";
    };
    auto& options = context.command_options<Options>();
    command->add_option("file", options.file, "Audio file")->required();
    command->add_option("--volume", options.volume, "Volume in dB")->check(CLI::Range(-96.0, 0.0));
    command->add_option("--seconds", options.seconds, "Stop after this many seconds (0 = whole file, at most an hour)")
        ->check(CLI::Range(0.0, kMaximumPlaySeconds));
    command->add_option("--backend", options.backend, "system, or null (silent; for tests)")
        ->check(CLI::IsMember({"system", "null"}));
    command->callback([&] {
        audio::PreviewOptions preview_options;
        preview_options.backend = options.backend == "null" ? audio::OutputBackend::Null : audio::OutputBackend::System;
        auto engine = audio::PreviewEngine::create(preview_options);
        if (!engine) return void(exit_code = context.report(engine.error()));
        if (auto opened = (*engine)->open(path_from_utf8(options.file)); !opened)
            return void(exit_code = context.report(opened.error()));
        (*engine)->post(audio::transport::SetVolumeDecibels{options.volume});
        (*engine)->post(audio::transport::Play{});
        InterruptScope interrupt;
        const auto started = std::chrono::steady_clock::now();
        double limit = options.seconds > 0 ? options.seconds : kMaximumPlaySeconds;
        bool length_applied = false;
        std::mutex sleep_mutex;
        std::condition_variable_any sleeper;
        while (!interrupt.interrupted()) {
            {
                std::unique_lock lock(sleep_mutex);
                sleeper.wait_for(lock, interrupt.token(), kPlayRefresh, [] { return false; });
            }
            const auto snapshot = (*engine)->snapshot();
            if (snapshot.last_error) {
                exit_code = context.report(Error{*snapshot.last_error, false, "playback stopped", {}, {}});
                break;
            }
            // Once the exact length is known, a Play that never takes effect (or an end the 50 ms
            // polls never saw) still ends shortly after the file would have. An estimated length
            // (a VBR MP3 without a seek header) can be far too short, so it never cuts playback:
            // then only Stopped or the one-hour cap ends it.
            if (!length_applied && snapshot.total_frames && snapshot.total_frames_exact &&
                snapshot.source_sample_rate > 0) {
                length_applied = true;
                const double file_seconds = static_cast<double>(*snapshot.total_frames) / snapshot.source_sample_rate;
                limit = std::min(limit, file_seconds + kPlayEndGrace);
            }
            const double elapsed = std::chrono::duration<double>(std::chrono::steady_clock::now() - started).count();
            if (snapshot.source_sample_rate > 0)
                context.progress("  {} / {}   underruns {}",
                                 format_duration(snapshot.position_frames * 1'000'000 / snapshot.source_sample_rate),
                                 snapshot.total_frames
                                     ? format_duration(*snapshot.total_frames * 1'000'000 / snapshot.source_sample_rate)
                                     : "?",
                                 snapshot.underrun_count);
            // After open() the engine is Loading, Ready or Playing; it reaches Stopped only when
            // playback ended, however short the file (a one-shot can start and end between polls).
            if (snapshot.state == audio::PreviewState::Stopped || elapsed >= limit) break;
        }
        context.end_progress();
        if (interrupt.interrupted()) exit_code = kExitInterrupted;
    });
}

/// Registers `capabilities`.
void register_capabilities(CLI::App& root, CommandContext& context) {
    auto* command = root.add_subcommand("capabilities", "List the codecs this build can decode");
    command->callback([&] {
        for (const auto& codec : media::decoder_capabilities())
            context.print("{}", codec);
    });
}

}  // namespace

void register_media_commands(CLI::App& root, CommandContext& context, int& exit_code) {
    register_probe(root, context, exit_code);
    register_play(root, context, exit_code);
    register_capabilities(root, context);
}

}  // namespace mso::cli
