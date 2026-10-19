// analyze: run an explicit analysis profile and wait for it, without ever waiting forever.
#include <chrono>
#include <condition_variable>
#include <mutex>

#include "mso/cli/command_context.hpp"
#include "mso/cli/interrupt.hpp"

namespace mso::cli {

namespace {

using Clock = std::chrono::steady_clock;

/// How often the run counts are re-read while waiting.
constexpr auto kPollInterval = std::chrono::milliseconds(250);
/// A run whose counts do not move for this long is reported as stalled.
constexpr auto kStallTimeout = std::chrono::minutes(15);
/// Runs listed when looking a run up by id (the newest first; ours were just created).
constexpr std::size_t kRunLookupLimit = 1000;

/// Profiles accepted by `analyze --profile`.
const std::vector<std::string> kProfiles = {"Waveforms", "Musical", "Duplicates", "Full"};

/// The current status of every run in `run_ids`; NOT_FOUND when one has disappeared.
Result<std::vector<catalog::Library::AnalysisRunStatus>> current_runs(app::Application& application,
                                                                      const uuids::uuid& library,
                                                                      const std::vector<std::int64_t>& run_ids) {
    auto listed = application.with_library(
        library, [](catalog::Library& opened) { return opened.list_analysis_runs(kRunLookupLimit); });
    if (!listed) return std::unexpected(listed.error());
    std::vector<catalog::Library::AnalysisRunStatus> runs;
    for (const auto run_id : run_ids) {
        const auto found = std::ranges::find(*listed, run_id, &catalog::Library::AnalysisRunStatus::id);
        if (found == listed->end())
            return fail(ErrorCode::NotFound, std::format("analysis run {} is no longer listed", run_id));
        runs.push_back(*found);
    }
    return runs;
}

/// Cancels every run (Ctrl+C or timeout); failures are reported but do not change the outcome.
void cancel_runs(const CommandContext& context, app::Application& application, const uuids::uuid& library,
                 const std::vector<std::int64_t>& run_ids) {
    for (const auto run_id : run_ids)
        if (auto cancelled = application.control_analysis(library, run_id, catalog::Library::RunControl::Cancel);
            !cancelled)
            context.warn("could not cancel run {}: {}", run_id, cancelled.error().message());
}

/// Waits until every run has finished. Exits early (with a non-zero code) on Ctrl+C, timeout, a
/// paused queue or an unavailable worker, a vanished run, or a run that stops making progress.
int wait_for_runs(CommandContext& context, app::Application& application, const uuids::uuid& library,
                  const std::vector<std::int64_t>& run_ids, std::chrono::seconds timeout,
                  const InterruptScope& interrupt) {
    const auto started = Clock::now();
    auto last_movement = started;
    std::int64_t last_finished = -1;
    std::mutex sleep_mutex;
    std::condition_variable_any sleeper;
    while (true) {
        auto runs = current_runs(application, library, run_ids);
        if (!runs) return context.report(runs.error());
        std::int64_t pending = 0, completed = 0, failed = 0, blocked = 0, stale = 0, finished = 0;
        for (const auto& run : *runs) {
            pending += run.queued + run.running;
            completed += run.completed;
            failed += run.failed;
            blocked += run.blocked;
            stale += run.stale;
            finished += run.completed + run.failed + run.cancelled + run.stale;
        }
        context.progress("  {} done, {} pending, {} failed, {} blocked", completed, pending, failed, blocked);
        if (pending == 0) {
            context.end_progress();
            context.print("finished: {} analyzed, {} failed, {} blocked (offline), {} changed while analyzed",
                          completed, failed, blocked, stale);
            return failed + blocked + stale > 0 ? kExitPartial : kExitSuccess;
        }
        const auto now = Clock::now();
        if (finished != last_finished) {
            last_finished = finished;
            last_movement = now;
        }
        const auto status = application.analysis_status();
        std::optional<Error> stop_reason;
        if (status.worker_unavailable)
            stop_reason = Error{ErrorCode::WorkerCrash, false, status.detail, {}, {}};
        else if (status.paused)
            stop_reason = Error{ErrorCode::Cancelled, false, "the analysis queue was paused", {}, {}};
        else if (timeout.count() > 0 && now - started >= timeout)
            stop_reason = Error{ErrorCode::Cancelled, false, std::format("timed out after {}", timeout), {}, {}};
        else if (now - last_movement >= kStallTimeout)
            stop_reason = Error{ErrorCode::WorkerCrash, false, "no job finished for 15 minutes", {}, {}};
        if (interrupt.interrupted() || stop_reason) {
            context.end_progress();
            cancel_runs(context, application, library, run_ids);
            if (interrupt.interrupted()) {
                context.print("cancelled; results already published are kept");
                return kExitInterrupted;
            }
            return context.report(*stop_reason);
        }
        std::unique_lock lock(sleep_mutex);
        sleeper.wait_for(lock, interrupt.token(), kPollInterval, [] { return false; });
    }
}

}  // namespace

void register_analysis_commands(CLI::App& root, CommandContext& context, int& exit_code) {
    auto* command = root.add_subcommand("analyze", "Run an explicit analysis profile on matching samples and wait");
    auto directory = std::make_shared<std::string>();
    auto profile = std::make_shared<std::string>("Musical");
    auto query = std::make_shared<std::string>();
    auto force = std::make_shared<bool>(false);
    auto timeout_seconds = std::make_shared<std::int64_t>(0);
    command->add_option("library", *directory, "Library folder")->required();
    command->add_option("query", *query, "Only samples matching this search (default: all)");
    command->add_option("--profile", *profile, "Analysis profile")->check(CLI::IsMember(kProfiles));
    command->add_flag("--force", *force, "Re-analyze even when current results exist");
    command->add_option("--timeout", *timeout_seconds, "Give up (and cancel) after this many seconds; 0 = no limit")
        ->check(CLI::Range(std::int64_t{0}, std::int64_t{7 * 24 * 3600}));
    command->callback([&, directory, profile, query, force, timeout_seconds] {
        auto application = context.application(/*with_analysis=*/true);
        if (!application) return void(exit_code = context.report(application.error()));
        auto library = context.open_library(path_from_utf8(*directory), Access::Write);
        if (!library) return void(exit_code = context.report(library.error()));
        auto& app = **application;
        app.set_analysis_paused(false);

        // Freeze the selection now: later matches are not added to these runs.
        std::vector<std::int64_t> run_ids;
        std::int64_t queued = 0, skipped = 0;
        auto record = [&](const catalog::Library::AnalysisRunStatus& run) {
            run_ids.push_back(run.id);
            queued += run.queued;
            skipped += run.skipped;
        };
        if (query->empty()) {
            // Everything: one set-based run per root, no id list in memory.
            auto roots = app.with_library(library->uuid, [](catalog::Library& opened) { return opened.list_roots(); });
            if (!roots) return void(exit_code = context.report(roots.error()));
            for (const auto& root_record : *roots) {
                auto run = app.start_root_analysis(library->uuid, root_record.id, *profile, *force);
                if (!run) return void(exit_code = context.report(run.error()));
                record(*run);
            }
        } else {
            search::SearchRequest request{.text = *query};
            auto ids = app.with_library(
                library->uuid, [&](catalog::Library& opened) { return search::matching_ids(opened, request); });
            if (!ids) return void(exit_code = context.report(ids.error()));
            if (!ids->empty()) {
                auto run = app.start_analysis(library->uuid, *profile, *ids, *force);
                if (!run) return void(exit_code = context.report(run.error()));
                record(*run);
            }
        }
        if (run_ids.empty()) return void(context.print("no samples match"));
        context.print("{} queued, {} not queued (results current or sample removed)", queued, skipped);
        if (queued == 0) return;
        InterruptScope interrupt;
        exit_code =
            wait_for_runs(context, app, library->uuid, run_ids, std::chrono::seconds(*timeout_seconds), interrupt);
    });
}

}  // namespace mso::cli
