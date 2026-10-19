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

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <filesystem>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <variant>
#include <vector>

#include "mso/ui/analysis_controller.hpp"
#include "mso/ui/application_session.hpp"
#include "mso/ui/display_format.hpp"
#include "mso/ui/export_controller.hpp"
#include "mso/ui/history_panel.hpp"
#include "mso/ui/inspector_panel.hpp"
#include "mso/ui/jobs_panel.hpp"
#include "mso/ui/library_controller.hpp"
#include "mso/ui/main_window.hpp"
#include "mso/ui/navigator_panel.hpp"
#include "mso/ui/notification_bar.hpp"
#include "mso/ui/preview_panel.hpp"
#include "mso/ui/root_locations.hpp"
#include "mso/ui/sample_context_menu.hpp"
#include "mso/ui/sample_table_model.hpp"
#include "mso/ui/sample_table_view.hpp"
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

/// A one-shot signal between a worker thread and the test: open() once, wait (bounded) elsewhere.
class Gate {
public:
    void open() {
        {
            const std::scoped_lock lock(mutex_);
            open_ = true;
        }
        changed_.notify_all();
    }
    bool is_open() {
        const std::scoped_lock lock(mutex_);
        return open_;
    }
    /// Waits until open() (at most ten seconds, so a failing test never hangs).
    void wait() {
        std::unique_lock lock(mutex_);
        changed_.wait_for(lock, std::chrono::seconds(10), [this] { return open_; });
    }

private:
    std::mutex mutex_;
    std::condition_variable changed_;
    bool open_ = false;
};

/// What a FakePreviewEngine was asked and what its device does. Device calls arrive on the panel's
/// engine pool, everything else on the UI thread, so the shared parts are atomics or locked.
struct FakeDeviceControl {
    std::atomic<bool> lost{false};                ///< the snapshot reports a lost device
    std::atomic<bool> block_device_calls{false};  ///< device calls wait for release_device_call
    Gate device_call_entered;                     ///< a held-back device call has started
    Gate release_device_call;                     ///< lets held-back device calls finish
    std::atomic<int> restarts{0};
    std::atomic<int> listings{0};
    std::atomic<int> selections{0};
    std::mutex mutex;                                                  ///< guards the members below
    std::vector<mso::audio::TransportCommand> posted;                  ///< UI thread
    mso::audio::PreviewState state = mso::audio::PreviewState::Empty;  ///< from posted commands
    std::uint64_t generation = 0;

    /// A device call: waits (bounded) while the test holds device calls back.
    void device_call() {
        if (!block_device_calls) return;
        device_call_entered.open();
        release_device_call.wait();
    }
    /// Commands of type Command posted so far.
    template <class Command>
    int posted_count() {
        const std::scoped_lock lock(mutex);
        return static_cast<int>(std::ranges::count_if(posted, [](const mso::audio::TransportCommand& command) {
            return std::holds_alternative<Command>(command);
        }));
    }
};

/// A preview engine whose output device the test controls (the null device is never lost and
/// answers at once). It plays nothing.
class FakePreviewEngine final : public mso::audio::PreviewEngine {
public:
    explicit FakePreviewEngine(std::shared_ptr<FakeDeviceControl> control) : control_(std::move(control)) {}

    mso::Result<std::vector<mso::audio::OutputDeviceInfo>> list_output_devices() override {
        ++control_->listings;
        control_->device_call();
        return std::vector<mso::audio::OutputDeviceInfo>{{mso::audio::OutputDeviceId{"fake:1"}, "Headphones", false},
                                                         {mso::audio::OutputDeviceId{"fake:2"}, "Speakers", true}};
    }
    mso::Result<void> select_output_device(const mso::audio::OutputDeviceId&) override {
        ++control_->selections;
        control_->device_call();
        return {};
    }
    mso::Result<void> restart_output_device() override {
        ++control_->restarts;
        control_->device_call();
        control_->lost = false;  // plugged back in
        return {};
    }
    // The asynchronous forms answer on a thread of their own, like the engine's output thread.
    void list_output_devices_async(
        mso::audio::DeviceRequestCompletion<std::vector<mso::audio::OutputDeviceInfo>> on_done) override {
        answer_later([this, on_done] { on_done(list_output_devices()); });
    }
    void select_output_device_async(mso::audio::OutputDeviceId device,
                                    mso::audio::DeviceRequestCompletion<void> on_done) override {
        answer_later([this, device, on_done] { on_done(select_output_device(device)); });
    }
    void restart_output_device_async(mso::audio::DeviceRequestCompletion<void> on_done) override {
        answer_later([this, on_done] { on_done(restart_output_device()); });
    }
    mso::Result<void> open(const std::filesystem::path&) override {
        const std::scoped_lock lock(control_->mutex);
        ++control_->generation;
        control_->state = mso::audio::PreviewState::Paused;
        return {};
    }
    mso::Result<void> open(std::unique_ptr<mso::media::Decoder>) override { return open(std::filesystem::path("x")); }
    void post(mso::audio::TransportCommand command) noexcept override {
        const std::scoped_lock lock(control_->mutex);
        if (std::holds_alternative<mso::audio::transport::Play>(command))
            control_->state = mso::audio::PreviewState::Playing;
        else if (std::holds_alternative<mso::audio::transport::Pause>(command))
            control_->state = mso::audio::PreviewState::Paused;
        else if (std::holds_alternative<mso::audio::transport::TogglePlayPause>(command))
            control_->state = control_->state == mso::audio::PreviewState::Playing ? mso::audio::PreviewState::Paused
                                                                                   : mso::audio::PreviewState::Playing;
        control_->posted.push_back(std::move(command));
    }
    mso::audio::PreviewSnapshot snapshot() const noexcept override {
        mso::audio::PreviewSnapshot snapshot;
        const std::scoped_lock lock(control_->mutex);
        snapshot.state = control_->state;
        snapshot.generation = control_->generation;
        snapshot.output_device_lost = control_->lost;
        snapshot.output_device_available = !control_->lost;
        return snapshot;
    }
    mso::Result<void> render_offline(std::span<float>) override { return {}; }

private:
    /// Runs `answer` on a new thread (requests come from the UI thread only; joined on destruction).
    void answer_later(std::function<void()> answer) { answer_threads_.emplace_back(std::move(answer)); }

    std::shared_ptr<FakeDeviceControl> control_;
    std::vector<std::jthread> answer_threads_;  ///< last: joined before anything else is destroyed
};

/// A preview engine factory over `control` that fails its first `failures` creations.
mso::ui::PreviewPanel::EngineFactory fake_engine_factory(std::shared_ptr<FakeDeviceControl> control, int failures = 0) {
    auto remaining = std::make_shared<std::atomic<int>>(failures);
    return [control,
            remaining](const mso::audio::PreviewOptions&) -> mso::Result<std::unique_ptr<mso::audio::PreviewEngine>> {
        if (remaining->fetch_sub(1) > 0) return mso::fail(mso::ErrorCode::DeviceLost, "no audio subsystem");
        return std::unique_ptr<mso::audio::PreviewEngine>(new FakePreviewEngine(control));
    };
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
            CHECK(controller.running_tasks() == 1);
            REQUIRE(process_events_until([&] { return controller.running_tasks() == 0; }, 30000));
            CHECK(std::filesystem::exists(destination));
            controller.backup(bench->library);
            REQUIRE(process_events_until([&] { return messages.size() == 2; }, 30000));
            CHECK(messages.front().contains(QStringLiteral("written")));
            CHECK(messages.back().contains(QStringLiteral("verified backup")));
            auto* tree = jobs.findChild<QTreeWidget*>();
            REQUIRE(tree != nullptr);
            for (int row = 0; row < tree->topLevelItemCount(); ++row)
                CHECK(tree->topLevelItem(row)->text(2).startsWith(QStringLiteral("Done.")));

            // A backup offers Cancel, and a cancelled one keeps no file (window close: cancel_all()).
            const auto backups = [&] {
                std::size_t count = 0;
                for (const auto& entry : std::filesystem::recursive_directory_iterator(bench->scratch / "library"))
                    if (entry.path().extension() == ".sqlite" && entry.path().parent_path().filename() == "backups")
                        ++count;
                return count;
            };
            const std::size_t backups_before = backups();
            controller.backup(bench->library);
            tree->setCurrentItem(tree->topLevelItem(tree->topLevelItemCount() - 1));
            QPushButton* cancel = nullptr;
            for (auto* button : jobs.findChildren<QPushButton*>())
                if (button->accessibleName() == QStringLiteral("Cancel selected job")) cancel = button;
            REQUIRE(cancel != nullptr);
            CHECK(cancel->isEnabled());
            controller.cancel_all();  // before the long-task pool has even started it
            REQUIRE(process_events_until([&] { return controller.running_tasks() == 0; }, 30000));
            CHECK(tree->topLevelItem(tree->topLevelItemCount() - 1)->text(2).contains(QStringLiteral("cancelled")));
            CHECK(backups() == backups_before);
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

    TEST_CASE("preview device calls run off the UI thread; Play and Retry join one restart, then play") {
        auto bench = start_workbench();
        auto control = std::make_shared<FakeDeviceControl>();
        control->lost = true;  // headphones unplugged before the first preview
        mso::ui::PreviewPanel preview(*bench->session, nullptr, fake_engine_factory(control));
        REQUIRE(process_events_until([&] { return preview.engine() != nullptr; }));
        CHECK(preview.device_notice_visible());

        control->block_device_calls = true;
        preview.select_sample(key_of(bench->rows.front()), QStringLiteral("kick"));
        preview.play_selected();  // resolves the path, opens it, and finds the device lost
        REQUIRE(process_events_until([&] { return control->device_call_entered.is_open(); }));
        // The restart waits for the device on the engine pool; the UI thread keeps running.
        CHECK(preview.device_request_running());
        CHECK(preview.status_text().contains(QStringLiteral("Reconnecting")));
        preview.retry_output_device();
        preview.toggle_play_pause();  // Space while lost: Play (never a toggle), joining the restart
        QCoreApplication::processEvents();
        CHECK(control->restarts == 1);
        CHECK(control->posted_count<mso::audio::transport::Play>() == 0);  // not before the device is back
        control->release_device_call.open();
        REQUIRE(process_events_until([&] { return !preview.device_request_running(); }));
        CHECK(control->posted_count<mso::audio::transport::Play>() == 1);
        CHECK(control->posted_count<mso::audio::transport::TogglePlayPause>() == 0);
        CHECK_FALSE(preview.device_notice_visible());
        CHECK(preview.engine()->snapshot().state == mso::audio::PreviewState::Playing);
        preview.toggle_play_pause();  // playing: Space pauses
        CHECK(control->posted_count<mso::audio::transport::Pause>() == 1);

        // The Output menu lists devices off the UI thread: the last list shows at once.
        auto* menu = preview.findChild<QMenu*>();
        REQUIRE(menu != nullptr);
        control->block_device_calls = false;
        emit menu->aboutToShow();
        REQUIRE(process_events_until([&] { return !preview.device_request_running(); }));
        CHECK(control->listings == 1);
        emit menu->aboutToShow();  // opened again: the list is there before the new one arrives
        QAction* headphones = nullptr;
        for (QAction* action : menu->actions())
            if (action->text() == QStringLiteral("Headphones")) headphones = action;
        REQUIRE(headphones != nullptr);
        headphones->trigger();
        REQUIRE(process_events_until([&] { return !preview.device_request_running() && control->selections == 1; }));
        REQUIRE(process_events_until(
            [&] { return bench->session->preference(mso::ui::UiPreferenceKeys::kPreviewOutputDevice) == "fake:1"; }));
    }

    TEST_CASE("preview survives a failed engine creation: Retry creates the engine again") {
        auto bench = start_workbench();
        auto control = std::make_shared<FakeDeviceControl>();
        mso::ui::PreviewPanel preview(*bench->session, nullptr, fake_engine_factory(control, 1));
        REQUIRE(process_events_until([&] { return preview.status_text().contains(QStringLiteral("unavailable")); }));
        CHECK(preview.engine() == nullptr);
        preview.retry_output_device();
        REQUIRE(process_events_until([&] { return preview.engine() != nullptr; }));
        CHECK(preview.status_text().isEmpty());
    }

    TEST_CASE("main window: changes to rows it holds never re-read the window; unknown samples do") {
        auto bench = start_workbench();
        mso::ui::MainWindow window(*bench->session);
        window.show();
        bench->session->refresh_libraries();
        auto* model = qobject_cast<mso::ui::SampleTableModel*>(window.sample_table()->model());
        REQUIRE(model != nullptr);
        REQUIRE(process_events_until([&] { return model->rowCount() == 3 && !model->is_loading(); }));
        int refreshed = 0;
        QObject::connect(model, &mso::ui::SampleTableModel::refreshed, [&] { ++refreshed; });

        mso::app::CatalogChangedEvent known;
        known.library = bench->library;
        known.sample_ids = {bench->rows[0].sample_id, bench->rows[2].sample_id};
        emit bench->session->catalogChanged(known);  // analysis results for rows on screen
        CHECK_FALSE(window.window_refresh_pending());

        mso::app::CatalogChangedEvent unknown;
        unknown.library = bench->library;
        unknown.sample_ids = {999'999};  // a sample the table does not hold (an import)
        emit bench->session->catalogChanged(unknown);
        CHECK(window.window_refresh_pending());
        REQUIRE(process_events_until([&] { return refreshed == 1; }));

        // A whole-selection edit uses every selected key.
        window.sample_table()->selectAll();
        QCoreApplication::processEvents();
        CHECK(window.sample_table()->selected_keys().size() == 3);
        window.close();
    }

    TEST_CASE("main window: a library upgrade shows in the Jobs panel until the library is open") {
        auto bench = start_workbench();
        mso::ui::MainWindow window(*bench->session);
        auto* jobs = window.findChild<mso::ui::JobsPanel*>();
        REQUIRE(jobs != nullptr);
        auto* tree = jobs->findChild<QTreeWidget*>();
        REQUIRE(tree != nullptr);
        mso::app::LibraryOpenProgressEvent event;  // synthetic: the bench library opened without work
        event.library = bench->library;
        event.directory = bench->scratch / "library";
        event.step = mso::catalog::OpenStep::Migrating;
        event.completed = 1;
        event.total = 2;
        emit bench->session->libraryOpenProgressed(event);
        REQUIRE(jobs->row_count() == 1);
        CHECK(tree->topLevelItem(0)->text(2).startsWith(QStringLiteral("Updating the catalog")));
        event.completed = 2;
        emit bench->session->libraryOpenProgressed(event);
        bench->session->refresh_libraries();  // the open completed: the library is listed
        REQUIRE(
            process_events_until([&] { return tree->topLevelItem(0)->text(2).startsWith(QStringLiteral("Done")); }));
        CHECK(window.notification_bar()->message().isEmpty());  // no collisions to report
    }

    TEST_CASE("inspector re-reads category and collection lists only for its library, coalesced") {
        auto bench = start_workbench();
        mso::ui::InspectorPanel inspector(*bench->session);
        inspector.show_selection({bench->rows.front()}, 1);
        CHECK(inspector.choice_reads_started() == 1);          // the selection's library: read at once
        inspector.library_structure_changed(mso::new_uuid());  // another library's import
        CHECK(inspector.choice_reads_started() == 1);
        for (int change = 0; change < 5; ++change)
            inspector.library_structure_changed(bench->library);  // this library, five times quickly
        REQUIRE(process_events_until([&] { return inspector.choice_reads_started() == 2; }));
        QCoreApplication::processEvents();
        CHECK(inspector.choice_reads_started() == 2);  // the five became one read
    }
}
