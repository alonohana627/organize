// Desktop UI tests against a real Application (TEST_SUITE "ui"): the navigator's menus while its
// tree is rebuilt, coalesced reloads, the history panel's on-demand reads, and the controllers
// that only make sense with a catalog behind them (library, export, analysis, sample menu, root
// locations). Headless (offscreen platform, null audio) and without fixed sleeps: waiting is always
// "pump events until a condition holds or a deadline passes".
#include <doctest.h>

#include <QAbstractItemModel>
#include <QAction>
#include <QApplication>
#include <QClipboard>
#include <QElapsedTimer>
#include <QGuiApplication>
#include <QInputDialog>
#include <QLabel>
#include <QMenu>
#include <QPushButton>
#include <QSlider>
#include <QTimer>
#include <QTreeWidget>

#include <filesystem>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "mso/ui/analysis_controller.hpp"
#include "mso/ui/application_session.hpp"
#include "mso/ui/display_format.hpp"
#include "mso/ui/export_controller.hpp"
#include "mso/ui/history_panel.hpp"
#include "mso/ui/inspector_panel.hpp"
#include "mso/ui/jobs_panel.hpp"
#include "mso/ui/library_controller.hpp"
#include "mso/ui/navigator_panel.hpp"
#include "mso/ui/preview_panel.hpp"
#include "mso/ui/root_locations.hpp"
#include "mso/ui/sample_context_menu.hpp"
#include "mso/ui/sample_table_model.hpp"
#include "mso/ui/ui_preferences.hpp"
#include "mso/ui/waveform_view.hpp"
#include "test_support.hpp"

namespace {

using mso::search::SampleRow;
using mso::ui::SampleKey;

/// The test run's QApplication (shared with ui_tests.cpp: whichever asks first creates it).
QApplication& application() {
    static int argc = 1;
    static char name[] = "mso_ui_tests";
    static char* argv[] = {name, nullptr};
    static std::unique_ptr<QApplication> instance = []() -> std::unique_ptr<QApplication> {
        if (QCoreApplication::instance()) return nullptr;
        qputenv("QT_QPA_PLATFORM", "offscreen");
        qputenv("MSO_AUDIO_BACKEND", "null");
        return std::make_unique<QApplication>(argc, argv);
    }();
    return *qobject_cast<QApplication*>(QCoreApplication::instance());
}

/// Runs the Qt event loop until `done` holds or the deadline passes; returns the final value.
bool process_events_until(const std::function<bool()>& done, int timeout_milliseconds = 10000) {
    QElapsedTimer timer;
    timer.start();
    while (!done() && timer.elapsed() < timeout_milliseconds)
        QCoreApplication::processEvents(QEventLoop::AllEvents, 10);
    return done();
}

/// A started session over a scratch workspace with one library holding three imported samples.
struct Workbench {
    mso::test::ScratchDirectory scratch{"ui-app"};
    std::unique_ptr<mso::ui::ApplicationSession> session;
    uuids::uuid library;
    std::int64_t root_id = 0;
    std::vector<SampleRow> rows;  ///< every sample, in name order

    /// Runs `work` on the session's pool and waits for its result.
    template <class Work>
    auto run(Work work) {
        using Value = decltype(work(std::declval<mso::app::Application&>()));
        std::optional<Value> result;
        session->run(session.get(), std::move(work), [&result](Value value) { result = std::move(value); });
        REQUIRE(process_events_until([&] { return result.has_value(); }, 30000));
        return std::move(*result);
    }
};

/// Starts a session and imports three short WAV files into a new library.
std::unique_ptr<Workbench> start_workbench() {
    application();
    auto bench = std::make_unique<Workbench>();
    const std::filesystem::path samples = bench->scratch / "samples";
    mso::test::write_sine_wav(samples / "kick 01.wav", 0.1);
    mso::test::write_sine_wav(samples / "loop 01.wav", 0.1);
    mso::test::write_sine_wav(samples / "snare 01.wav", 0.1);
    mso::app::ApplicationOptions options;
    options.workspace_directory = bench->scratch / "workspace";
    options.reopen_libraries = false;
    options.monitor_roots = false;
    options.run_analysis_supervisor = false;
    bench->session = std::make_unique<mso::ui::ApplicationSession>(options);
    bool ready = false;
    QObject guard;  // disconnects the lambda below when this function returns
    QObject::connect(bench->session.get(), &mso::ui::ApplicationSession::ready, &guard, [&ready] { ready = true; });
    bench->session->start();
    REQUIRE(process_events_until([&] { return ready; }, 30000));

    auto imported = bench->run([library_path = bench->scratch / "library", samples](
                                   mso::app::Application& app) -> mso::Result<std::pair<uuids::uuid, std::int64_t>> {
        auto library = app.create_library(library_path, "UI app test");
        if (!library) return std::unexpected(library.error());
        auto root = app.add_root(library->uuid, samples, "samples");
        if (!root) return std::unexpected(root.error());
        if (auto scanned = app.import_now(library->uuid, root->id); !scanned) return std::unexpected(scanned.error());
        return std::pair{library->uuid, root->id};
    });
    if (!imported) FAIL(mso::ui::DisplayFormat::error(imported.error()).toStdString());
    bench->library = imported->first;
    bench->root_id = imported->second;
    bench->session->refresh_libraries();
    REQUIRE(process_events_until([&] { return bench->session->libraries().size() == 1; }));

    // The import reaches the in-memory index through the change feed shortly after import_now()
    // returns: query again until the index has caught up.
    mso::ui::SampleTableModel model(*bench->session);
    model.set_query("", {});
    REQUIRE(process_events_until(
        [&] {
            if (model.is_loading() || model.rowCount() == 3) return model.rowCount() == 3 && !model.is_loading();
            model.run_query("", {});
            return false;
        },
        30000));
    for (int row = 0; row < 3; ++row)
        bench->rows.push_back(*model.loaded_row(row));
    return bench;
}

SampleKey key_of(const SampleRow& row) {
    return SampleKey{row.library, row.sample_id};
}

/// The first tree item whose text starts with `prefix`, or null.
QTreeWidgetItem* find_item(QTreeWidget& tree, const QString& prefix) {
    const auto found = tree.findItems(prefix, Qt::MatchStartsWith | Qt::MatchRecursive);
    return found.isEmpty() ? nullptr : found.front();
}

}  // namespace

TEST_SUITE("ui") {
    TEST_CASE("navigator menus survive a tree rebuild during their event loop, and reloads coalesce") {
        auto bench = start_workbench();
        // Two tags: "Drums/Kick" (to move) and "Loops" (its new parent).
        mso::catalog::EditCommand tagging;
        tagging.sample_ids = {bench->rows[0].sample_id};
        tagging.tags_add = {"Drums/Kick", "Loops"};
        auto tagged = bench->run([library = bench->library, tagging](mso::app::Application& app) {
            return app.apply_edit(library, tagging);
        });
        REQUIRE(tagged.has_value());
        auto tags = bench->run([library = bench->library](mso::app::Application& app) {
            return app.with_library(library, [](mso::catalog::Library& opened) { return opened.list_tags(); });
        });
        REQUIRE(tags.has_value());
        std::int64_t kick_id = 0;
        std::int64_t loops_id = 0;
        for (const auto& tag : *tags) {
            if (tag.path == "Drums/Kick") kick_id = tag.id;
            if (tag.path == "Loops") loops_id = tag.id;
        }
        REQUIRE((kick_id != 0 && loops_id != 0));

        // Declared before the navigator: its destruction still emits into these.
        std::optional<mso::catalog::TaxonomyCommand> requested;
        int resets = 0;
        mso::ui::NavigatorPanel navigator(*bench->session);
        navigator.resize(400, 700);
        navigator.show();
        navigator.reload();
        auto* tree = navigator.findChild<QTreeWidget*>();
        REQUIRE(tree != nullptr);
        REQUIRE(process_events_until([&] { return find_item(*tree, QStringLiteral("Kick")) != nullptr; }));
        tree->expandAll();
        QCoreApplication::processEvents();
        const QPoint position = tree->visualItemRect(find_item(*tree, QStringLiteral("Kick"))).center();

        QObject::connect(
            &navigator, &mso::ui::NavigatorPanel::taxonomyCommandRequested,
            [&](const uuids::uuid&, const mso::catalog::TaxonomyCommand& command) { requested = command; });
        QObject::connect(tree->model(), &QAbstractItemModel::modelReset, [&] { ++resets; });
        bool menu_seen = false;
        bool rebuilt = false;
        QTimer::singleShot(0, [&] {
            auto* menu = qobject_cast<QMenu*>(QApplication::activePopupWidget());
            if (!menu) return;
            menu_seen = true;
            // An outline delivery inside the menu's event loop deletes every item, the
            // right-clicked one included (the review's use-after-free).
            navigator.reload();
            rebuilt =
                process_events_until([&] { return resets > 0 && find_item(*tree, QStringLiteral("Kick")) != nullptr; });
            QAction* move = nullptr;
            for (QAction* action : menu->actions())
                if (action->text().startsWith(QStringLiteral("Move tag"))) move = action;
            QTimer::singleShot(0, [] {
                if (auto* dialog = qobject_cast<QInputDialog*>(QApplication::activeModalWidget())) {
                    dialog->setTextValue(QStringLiteral("Loops"));
                    dialog->accept();
                }
            });
            if (move) move->trigger();  // the dialog runs another nested loop
            menu->close();
        });
        tree->customContextMenuRequested(position);  // blocks in menu.exec() until closed
        CHECK(menu_seen);
        CHECK(rebuilt);
        REQUIRE(requested.has_value());
        CHECK(requested->kind == mso::catalog::TaxonomyCommand::Kind::MoveTag);
        CHECK(requested->target_id == kick_id);
        CHECK(requested->new_parent_id == loops_id);

        // Structure changes during an import arrive every 150 ms: reads are coalesced per library.
        const std::uint64_t before = navigator.outline_reads_started();
        for (int change = 0; change < 5; ++change)
            navigator.reload_library(bench->library);
        CHECK(navigator.outline_reads_started() == before + 1);  // the first starts at once
        REQUIRE(process_events_until([&] { return navigator.outline_reads_started() == before + 2; }));
        CHECK(navigator.outline_reads_started() == before + 2);  // the four others became one
    }

    TEST_CASE("history is read on demand: never per event while hidden, throttled while visible") {
        auto bench = start_workbench();
        mso::ui::HistoryPanel history(*bench->session);
        QPushButton* undo = nullptr;
        QPushButton* redo = nullptr;
        for (auto* button : history.findChildren<QPushButton*>()) {
            if (button->text() == QStringLiteral("Undo")) undo = button;
            if (button->text() == QStringLiteral("Redo")) redo = button;
        }
        REQUIRE((undo && redo));
        history.show_library(bench->library);
        REQUIRE(process_events_until([&] { return history.reads_started() == 1; }));
        CHECK_FALSE(undo->isEnabled());  // nothing to undo yet
        CHECK_FALSE(redo->isEnabled());

        mso::catalog::EditCommand rating;
        rating.sample_ids = {bench->rows[1].sample_id};
        rating.rating = 4;
        REQUIRE(bench
                    ->run([library = bench->library, rating](mso::app::Application& app) {
                        return app.apply_edit(library, rating);
                    })
                    .has_value());
        history.reload();  // what EditController::historyChanged does
        REQUIRE(process_events_until([&] { return undo->isEnabled(); }));
        CHECK_FALSE(redo->isEnabled());

        const std::uint64_t reads = history.reads_started();
        for (int event = 0; event < 5; ++event)
            history.catalog_changed(bench->library);  // hidden: only marked stale
        QCoreApplication::processEvents();
        CHECK(history.reads_started() == reads);
        history.show();
        CHECK(history.reads_started() == reads + 1);  // shown: read once
        for (int event = 0; event < 5; ++event)
            history.catalog_changed(bench->library);
        REQUIRE(process_events_until([&] { return history.reads_started() == reads + 2; }));
        CHECK(history.reads_started() == reads + 2);
    }

    TEST_CASE("root locations, library, export, analysis and sample menu controllers on a real catalog") {
        auto bench = start_workbench();
        auto& session = *bench->session;
        const SampleRow& first = bench->rows.front();

        // RootLocations builds drag paths from catalog data.
        {
            mso::ui::RootLocations locations(session);
            bool reloaded = false;
            QObject::connect(&locations, &mso::ui::RootLocations::reloaded, [&] { reloaded = true; });
            locations.reload();
            REQUIRE(process_events_until([&] { return reloaded; }));
            const auto path = locations.path_of(first);
            REQUIRE(path.has_value());
            CHECK(
                std::filesystem::equivalent(*path, bench->scratch / "samples" / mso::path_from_utf8(first.file_name)));
        }

        // LibraryController: one candidate needs no dialog; a rescan starts an import.
        {
            QWidget parent;
            mso::ui::LibraryController controller(session, &parent);
            CHECK(mso::ui::LibraryController::choose_library(&parent, session.libraries(), QStringLiteral("Pick"),
                                                             false, std::nullopt) == bench->library);
            std::optional<mso::app::OperationId> started;
            QObject::connect(&controller, &mso::ui::LibraryController::importStarted,
                             [&](mso::app::OperationId operation, const QString&) { started = operation; });
            bool finished = false;
            QObject::connect(&session, &mso::ui::ApplicationSession::operationFinished, &controller,
                             [&](const mso::app::OperationFinishedEvent& event) {
                                 if (started && event.operation == *started) finished = true;
                             });
            controller.rescan(bench->library, bench->root_id);
            REQUIRE(process_events_until([&] { return started.has_value(); }));
            REQUIRE(process_events_until([&] { return finished; }, 30000));
        }

        // ExportController: an export and a backup run as Jobs tasks on the long-task pool.
        {
            QWidget parent;
            mso::ui::JobsPanel jobs;
            mso::ui::ExportController controller(session, jobs, &parent);
            std::vector<QString> messages;
            QObject::connect(&controller, &mso::ui::ExportController::statusMessage,
                             [&](const QString& message) { messages.push_back(message); });
            const std::filesystem::path destination = bench->scratch / "export.csv";
            controller.start_export(bench->library, destination, mso::app::ExportOptions{});
            CHECK(controller.running_exports() == 1);
            REQUIRE(process_events_until([&] { return controller.running_exports() == 0; }, 30000));
            CHECK(std::filesystem::exists(destination));
            controller.backup(bench->library);
            REQUIRE(process_events_until([&] { return messages.size() == 2; }, 30000));
            CHECK(messages.front().contains(QStringLiteral("written")));
            CHECK(messages.back().contains(QStringLiteral("verified backup")));
            auto* tree = jobs.findChild<QTreeWidget*>();
            REQUIRE(tree != nullptr);
            for (int row = 0; row < tree->topLevelItemCount(); ++row)
                CHECK(tree->topLevelItem(row)->text(2).startsWith(QStringLiteral("Done.")));
        }

        // AnalysisController asks first; a "no" queues nothing.
        {
            QWidget parent;
            mso::ui::AnalysisController controller(session, &parent);
            std::vector<QString> messages;
            QObject::connect(&controller, &mso::ui::AnalysisController::statusMessage,
                             [&](const QString& message) { messages.push_back(message); });
            int questions = 0;
            controller.set_confirmation([&](const QString&, const QString&) {
                ++questions;
                return false;
            });
            controller.start({key_of(first)}, "Waveforms", QStringLiteral("Generate waveforms"));
            QCoreApplication::processEvents();
            CHECK(questions == 1);
            CHECK(messages.empty());
            controller.set_confirmation([](const QString&, const QString&) { return true; });
            controller.start({key_of(first)}, "Waveforms", QStringLiteral("Generate waveforms"));
            REQUIRE(process_events_until([&] { return !messages.empty(); }));
            CHECK(messages.back().contains(QStringLiteral("queued")));
        }

        // SampleContextMenu copies paths only when every sample resolved.
        {
            QWidget parent;
            mso::ui::SampleMenuCommands commands;
            mso::ui::SampleContextMenu menu(session, commands, &parent);
            std::vector<QString> messages;
            QObject::connect(&menu, &mso::ui::SampleContextMenu::statusMessage,
                             [&](const QString& message) { messages.push_back(message); });
            menu.copy_paths({key_of(first)});
            REQUIRE(process_events_until([&] { return messages.size() == 1; }));
            CHECK(QGuiApplication::clipboard()->text().contains(mso::ui::DisplayFormat::from_utf8(first.file_name)));
            menu.copy_paths({key_of(first), SampleKey{bench->library, 999'999}});
            REQUIRE(process_events_until([&] { return messages.size() == 2; }));
            CHECK(messages.back().startsWith(QStringLiteral("Nothing done")));
        }
    }

    TEST_CASE("inspector keeps the detail's format line; preview draws waveforms without replacing playback") {
        auto bench = start_workbench();
        auto& session = *bench->session;
        SampleRow row = bench->rows.front();

        {
            mso::ui::InspectorPanel inspector(session);
            inspector.show_selection({row}, 1);
            QLabel* format = nullptr;
            for (auto* label : inspector.findChildren<QLabel*>())
                if (label->accessibleName() == QStringLiteral("Format")) format = label;
            REQUIRE(format != nullptr);
            // The row's "WAV" shows at once; the detail adds codec, rate, channels … joined by "·".
            REQUIRE(process_events_until([&] { return format->text().contains(QStringLiteral("·")); }));
            const QString detail_format = format->text();
            row.rating = 3;  // the same sample refreshed in place (analysis, another edit)
            inspector.show_selection({row}, 1);
            CHECK(format->text() == detail_format);  // the review's disappearing Format line
        }

        {
            mso::ui::PreviewPanel preview(session);
            REQUIRE(process_events_until([&] { return preview.engine() != nullptr; }));
            CHECK_FALSE(preview.device_notice_visible());  // the null device is never lost
            preview.select_sample(key_of(row), QStringLiteral("kick"));
            QPushButton* generate = nullptr;
            for (auto* button : preview.findChildren<QPushButton*>())
                if (button->text() == QStringLiteral("Generate waveform")) generate = button;
            REQUIRE(generate != nullptr);
            generate->click();
            auto* waveform = preview.findChild<mso::ui::WaveformView*>();
            REQUIRE(waveform != nullptr);
            REQUIRE(process_events_until([&] { return waveform->state() == mso::ui::WaveformView::State::Ready; }));
            CHECK(preview.engine()->snapshot().generation == 0);  // nothing was opened in the engine

            QSlider* volume = nullptr;
            for (auto* slider : preview.findChildren<QSlider*>())
                if (slider->accessibleName() == QStringLiteral("Preview volume")) volume = slider;
            REQUIRE(volume != nullptr);
            volume->setValue(-20);
            CHECK(session.preference(mso::ui::UiPreferenceKeys::kPreviewVolume) == -20);
            preview.retry_output_device();  // harmless while the device works
            CHECK_FALSE(preview.device_notice_visible());
        }
    }
}
