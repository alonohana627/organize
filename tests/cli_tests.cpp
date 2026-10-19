// The `mso` command line, driven in-process through mso::cli::run() against a scratch workspace:
// every command, exit codes, --json output, option exclusivity, the library lock shared with the
// desktop app, Ctrl+C handling and an analyze that cannot hang.
#include <doctest.h>

#include <cstdio>
#include <format>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

#include "mso/app/application.hpp"
#include "mso/app/workspace.hpp"
#include "mso/cli/command_context.hpp"
#include "mso/cli/interrupt.hpp"
#include "test_support.hpp"

using namespace mso;
using mso::test::ScratchDirectory;

namespace {

/// Exit code and captured output of one in-process `mso` run.
struct Outcome {
    int exit_code = 0;
    std::string output;
    std::string errors;
};

/// Reads back everything written to a temporary file.
std::string drain(std::FILE* file) {
    std::fflush(file);
    std::rewind(file);
    std::string text;
    char buffer[4096];
    while (const auto read = std::fread(buffer, 1, sizeof(buffer), file))
        text.append(buffer, read);
    return text;
}

/// A scratch workspace and library folder, plus a runner that adds `--workspace`.
class CommandLine {
public:
    CommandLine() { test::write_sine_wav(scratch_ / "samples/Drums/Kick 01.wav", 0.2); }

    /// Runs `mso --workspace <scratch> arguments...`.
    Outcome run(std::vector<std::string> arguments) const {
        arguments.insert(arguments.begin(), {"--workspace", path_to_utf8(workspace())});
        std::FILE* output = std::tmpfile();
        std::FILE* errors = std::tmpfile();
        REQUIRE(output != nullptr);
        REQUIRE(errors != nullptr);
        Outcome outcome;
        outcome.exit_code = cli::run(arguments, output, errors);
        outcome.output = drain(output);
        outcome.errors = drain(errors);
        std::fclose(output);
        std::fclose(errors);
        return outcome;
    }

    /// Creates the library, adds the samples folder and imports it.
    void prepare_library() const {
        REQUIRE(run({"create", library(), "--name", "CLI"}).exit_code == cli::kExitSuccess);
        REQUIRE(run({"add-root", library(), samples()}).exit_code == cli::kExitSuccess);
        REQUIRE(run({"import", library()}).exit_code == cli::kExitSuccess);
    }

    std::filesystem::path workspace() const { return scratch_ / "workspace"; }
    std::string library() const { return path_to_utf8(scratch_ / "lib"); }
    std::string samples() const { return path_to_utf8(scratch_ / "samples"); }
    const ScratchDirectory& scratch() const { return scratch_; }

private:
    ScratchDirectory scratch_{"cli"};
};

bool contains(const std::string& text, std::string_view part) {
    return text.find(part) != std::string::npos;
}

}  // namespace

TEST_SUITE("cli") {
    TEST_CASE("version, help and usage errors map to the documented exit codes") {
        CommandLine cli;
        auto version = cli.run({"--version"});
        CHECK(version.exit_code == cli::kExitSuccess);
        CHECK(contains(version.output, "mso "));
        CHECK(cli.run({"--help"}).exit_code == cli::kExitSuccess);
        CHECK(cli.run({}).exit_code == cli::kExitUsage);  // a subcommand is required
        CHECK(cli.run({"no-such-command"}).exit_code == cli::kExitUsage);
        CHECK(cli.run({"search", "kick"}).exit_code == cli::kExitUsage);  // --library is required
        CHECK(cli.run({"edit", cli.library(), "1", "--rating", "9"}).exit_code == cli::kExitUsage);
    }

    TEST_CASE("create, add-root, import, info, search, show and tags work end to end") {
        CommandLine cli;
        cli.prepare_library();
        auto info = cli.run({"info", cli.library()});
        CHECK(info.exit_code == cli::kExitSuccess);
        CHECK(contains(info.output, "samples   1"));
        CHECK(contains(info.output, "samples"));

        auto search = cli.run({"search", "--library", cli.library(), "kick", "--count"});
        CHECK(search.exit_code == cli::kExitSuccess);
        CHECK(contains(search.output, "Kick 01"));
        CHECK(contains(search.output, "1 matching sample(s)"));

        auto json = cli.run({"--json", "search", "-l", cli.library(), "kick", "--count"});
        REQUIRE(json.exit_code == cli::kExitSuccess);
        const auto document = nlohmann::json::parse(json.output, nullptr, false);
        REQUIRE_FALSE(document.is_discarded());  // one JSON document, no trailing text
        CHECK(document["total"] == 1);
        CHECK(document["complete"] == true);
        REQUIRE(document["rows"].size() == 1);
        CHECK(document["rows"][0]["name"] == "Kick 01");

        auto plain_json = cli.run({"--json", "search", "-l", cli.library()});
        REQUIRE(plain_json.exit_code == cli::kExitSuccess);
        CHECK(nlohmann::json::parse(plain_json.output, nullptr, false).is_array());

        CHECK(cli.run({"search", "-l", cli.library(), "bpm:(("}).exit_code == cli::kExitFailure);
        CHECK(cli.run({"search", "-l", cli.library(), "--sort", "sideways"}).exit_code == cli::kExitUsage);

        auto show = cli.run({"show", cli.library(), "1"});
        CHECK(show.exit_code == cli::kExitSuccess);
        CHECK(contains(show.output, "Kick 01"));
        CHECK(cli.run({"show", cli.library(), "999"}).exit_code == cli::kExitFailure);

        CHECK(cli.run({"tags", cli.library()}).exit_code == cli::kExitSuccess);
        CHECK(cli.run({"collections", cli.library()}).exit_code == cli::kExitSuccess);
        CHECK(cli.run({"import", cli.library(), "--root", "99"}).exit_code == cli::kExitFailure);
        CHECK(cli.run({"info", path_to_utf8(cli.scratch() / "not-a-library")}).exit_code == cli::kExitFailure);
    }

    TEST_CASE("edit, tag, collection, undo, redo and history") {
        CommandLine cli;
        cli.prepare_library();
        CHECK(
            cli.run({"edit", cli.library(), "1", "--rating", "4", "--tag", "Drums/Kick", "--bpm", "128.5"}).exit_code ==
            cli::kExitSuccess);
        auto rated = cli.run({"--json", "search", "-l", cli.library(), "rating:4"});
        REQUIRE(rated.exit_code == cli::kExitSuccess);
        const auto rows = nlohmann::json::parse(rated.output);
        REQUIRE(rows.size() == 1);
        CHECK(rows[0]["bpm_milli"] == 128'500);

        // Locale-independent, fully consumed, range-checked BPM values.
        CHECK(cli.run({"edit", cli.library(), "1", "--bpm", "12x"}).exit_code == cli::kExitUsage);
        CHECK(cli.run({"edit", cli.library(), "1", "--bpm", "-5"}).exit_code == cli::kExitUsage);
        CHECK(cli.run({"edit", cli.library(), "1", "--bpm", "nan"}).exit_code == cli::kExitUsage);
        CHECK(cli.run({"edit", cli.library(), "1", "--bpm", "1e3"}).exit_code == cli::kExitUsage);
        CHECK(cli.run({"edit", cli.library(), "1", "--bpm", "auto"}).exit_code == cli::kExitSuccess);
        CHECK(cli.run({"edit", cli.library(), "1"}).exit_code == cli::kExitUsage);  // nothing to change
        CHECK(cli.run({"edit", cli.library(), "1", "--remove", "--restore"}).exit_code == cli::kExitUsage);
        CHECK(cli.run({"edit", cli.library(), "1", "--category", "Bass", "--clear-category"}).exit_code ==
              cli::kExitUsage);
        CHECK(cli.run({"edit", cli.library(), "1", "--operation", "not-a-uuid", "--rating", "1"}).exit_code ==
              cli::kExitUsage);

        auto undo = cli.run({"undo", cli.library()});
        CHECK(undo.exit_code == cli::kExitSuccess);
        CHECK(contains(undo.output, "undid"));
        CHECK(cli.run({"redo", cli.library()}).exit_code == cli::kExitSuccess);
        auto history = cli.run({"history", cli.library()});
        CHECK(history.exit_code == cli::kExitSuccess);
        CHECK_FALSE(history.output.empty());

        CHECK(cli.run({"tag", cli.library(), "1"}).exit_code == cli::kExitUsage);  // no action
        CHECK(cli.run({"tag", cli.library(), "1", "--rename", "X", "--move-to-top"}).exit_code == cli::kExitUsage);
        CHECK(cli.run({"tag", cli.library(), "1", "--rename", "Percussion"}).exit_code == cli::kExitSuccess);
        CHECK(contains(cli.run({"tags", cli.library()}).output, "Percussion"));

        CHECK(cli.run({"collection", cli.library()}).exit_code == cli::kExitUsage);
        CHECK(cli.run({"collection", cli.library(), "--rename", "X"}).exit_code == cli::kExitUsage);  // needs --id
        CHECK(cli.run({"collection", cli.library(), "--create", "Smart", "--smart", "bpm:(("}).exit_code ==
              cli::kExitUsage);
        CHECK(cli.run({"collection", cli.library(), "--create", "Picks"}).exit_code == cli::kExitSuccess);
        CHECK(contains(cli.run({"collections", cli.library()}).output, "Picks"));
        CHECK(cli.run({"collection", cli.library(), "--id", "1", "--rename", "Best"}).exit_code == cli::kExitSuccess);
        CHECK(cli.run({"collection", cli.library(), "--id", "1", "--delete"}).exit_code == cli::kExitSuccess);
    }

    TEST_CASE("backup, check and export") {
        CommandLine cli;
        cli.prepare_library();
        auto backup = cli.run({"backup", cli.library()});
        CHECK(backup.exit_code == cli::kExitSuccess);
        CHECK(contains(backup.output, "verified backup"));
        CHECK(cli.run({"check", cli.library()}).exit_code == cli::kExitSuccess);

        const auto csv = path_to_utf8(cli.scratch() / "out.csv");
        auto exported = cli.run({"export", cli.library(), csv, "--absolute-paths"});
        CHECK(exported.exit_code == cli::kExitSuccess);
        CHECK(contains(exported.output, "exported 1 sample(s)"));
        CHECK(contains(test::read_file(cli.scratch() / "out.csv"), "Kick 01.wav"));
        const auto json = path_to_utf8(cli.scratch() / "out.json");
        CHECK(cli.run({"export", cli.library(), json, "--format", "json", "--query", "kick"}).exit_code ==
              cli::kExitSuccess);
        CHECK(nlohmann::json::parse(test::read_file(cli.scratch() / "out.json"))["samples"].size() == 1);
        CHECK(cli.run({"export", cli.library(), json, "--format", "json", "--raw"}).exit_code == cli::kExitUsage);
        CHECK(cli.run({"export", cli.library(), json, "--format", "xml"}).exit_code == cli::kExitUsage);
    }

    TEST_CASE("probe, capabilities and play on the null device") {
        CommandLine cli;
        const auto wav = path_to_utf8(cli.scratch() / "samples/Drums/Kick 01.wav");
        auto probe = cli.run({"probe", wav});
        CHECK(probe.exit_code == cli::kExitSuccess);
        CHECK(contains(probe.output, "44100 Hz"));
        CHECK(cli.run({"probe", path_to_utf8(cli.scratch() / "missing.wav")}).exit_code == cli::kExitFailure);
        auto capabilities = cli.run({"capabilities"});
        CHECK(capabilities.exit_code == cli::kExitSuccess);
        CHECK(contains(capabilities.output, "pcm"));
        CHECK(cli.run({"play", wav, "--backend", "null", "--seconds", "0.1"}).exit_code == cli::kExitSuccess);
        CHECK(cli.run({"play", wav, "--volume", "6"}).exit_code == cli::kExitUsage);
    }

    TEST_CASE("analyze fails fast without a worker and completes with one") {
        CommandLine cli;
        cli.prepare_library();
        auto workerless = cli.run({"--worker", path_to_utf8(cli.scratch() / "no-worker.exe"), "analyze", cli.library(),
                                   "--profile", "Waveforms"});
        CHECK(workerless.exit_code == cli::kExitFailure);
        CHECK(contains(workerless.errors, "worker unavailable"));

        auto analyzed = cli.run({"--worker", path_to_utf8(test::worker_executable()), "analyze", cli.library(),
                                 "--profile", "Waveforms", "--force", "--timeout", "120"});
        CHECK(analyzed.exit_code == cli::kExitSuccess);
        CHECK(contains(analyzed.output, "1 analyzed"));
        auto nothing = cli.run({"analyze", cli.library(), "no-such-sample-name"});
        CHECK(nothing.exit_code == cli::kExitSuccess);
        CHECK(contains(nothing.output, "no samples match"));
    }

    TEST_CASE("the CLI never marks libraries open and respects the desktop app's lock") {
        CommandLine cli;
        cli.prepare_library();
        auto workspace = app::Workspace::load(cli.workspace());
        REQUIRE(workspace.has_value());
        for (const auto& known : workspace->known_libraries())
            CHECK_FALSE(known.open);

        // A desktop-like Application holds the library: reads still work, writes are refused.
        app::ApplicationOptions options;
        options.workspace_directory = cli.scratch() / "desktop";
        options.monitor_roots = false;
        options.run_analysis_supervisor = false;
        auto desktop = app::Application::start(options);
        REQUIRE(desktop.has_value());
        REQUIRE((*desktop)->open_library(path_from_utf8(cli.library())).has_value());
        CHECK(cli.run({"search", "-l", cli.library(), "kick"}).exit_code == cli::kExitSuccess);
        auto refused = cli.run({"edit", cli.library(), "1", "--rating", "2"});
        CHECK(refused.exit_code == cli::kExitFailure);
        CHECK(contains(refused.errors, "another Music Sample Organizer process"));
    }

    TEST_CASE("bench runs on a small synthetic library") {
        CommandLine cli;
        auto bench = cli.run({"bench", path_to_utf8(cli.scratch() / "bench"), "--rows", "700", "--repetitions", "2"});
        CHECK(bench.exit_code == cli::kExitSuccess);
        CHECK(contains(bench.output, "in-memory index"));
        CHECK(contains(bench.output, "folder click"));
        CHECK(cli.run({"bench", path_to_utf8(cli.scratch() / "bench2"), "--repetitions", "0"}).exit_code ==
              cli::kExitUsage);
    }

    TEST_CASE("Ctrl+C becomes a stop request through the watcher, not inside the handler") {
        cli::InterruptScope scope;
        CHECK_FALSE(scope.interrupted());
        cli::InterruptScope::simulate_interrupt();
        CHECK(test::wait_until([&] { return scope.interrupted(); }, std::chrono::milliseconds(5'000)));
        CHECK(scope.token().stop_requested());
    }

    TEST_CASE("BPM text parsing is strict") {
        CHECK(cli::parse_bpm_milli("128") == 128'000);
        CHECK(cli::parse_bpm_milli("92.5") == 92'500);
        CHECK_FALSE(cli::parse_bpm_milli("").has_value());
        CHECK_FALSE(cli::parse_bpm_milli("128bpm").has_value());
        CHECK_FALSE(cli::parse_bpm_milli("0").has_value());
        CHECK_FALSE(cli::parse_bpm_milli("1000").has_value());
        CHECK_FALSE(cli::parse_bpm_milli("inf").has_value());
        CHECK_FALSE(cli::parse_bpm_milli("+120").has_value());
    }

}  // TEST_SUITE("cli")
