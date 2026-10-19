// Desktop UI tests (TEST_SUITE "ui"). They run headless on Qt's "offscreen" platform with the null
// audio backend and need no display or sound card. Waiting is always "pump events until a condition
// holds or a deadline passes" (process_events_until), never a fixed sleep.
#include <doctest.h>

#include <QAbstractButton>
#include <QAccessible>
#include <QAccessibleValueInterface>
#include <QApplication>
#include <QCheckBox>
#include <QComboBox>
#include <QDialogButtonBox>
#include <QElapsedTimer>
#include <QHeaderView>
#include <QImage>
#include <QKeyEvent>
#include <QKeySequenceEdit>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QLocale>
#include <QMainWindow>
#include <QMenu>
#include <QMouseEvent>
#include <QPersistentModelIndex>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QRadioButton>
#include <QStyleOptionViewItem>
#include <QTableView>
#include <QThreadPool>
#include <QTimer>
#include <QTreeWidget>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <set>
#include <stdexcept>
#include <stop_token>
#include <string>
#include <utility>
#include <vector>

#include "mso/domain/analysis_flags.hpp"
#include "mso/generated/dependencies.hpp"
#include "mso/ui/about_dialog.hpp"
#include "mso/ui/add_folder_dialog.hpp"
#include "mso/ui/application_session.hpp"
#include "mso/ui/background_tasks.hpp"
#include "mso/ui/detail/parent_cycles.hpp"
#include "mso/ui/display_format.hpp"
#include "mso/ui/drag_path_resolver.hpp"
#include "mso/ui/edit_controller.hpp"
#include "mso/ui/edit_gateway.hpp"
#include "mso/ui/empty_state_widget.hpp"
#include "mso/ui/file_manager.hpp"
#include "mso/ui/history_panel.hpp"
#include "mso/ui/inspector_panel.hpp"
#include "mso/ui/jobs_panel.hpp"
#include "mso/ui/layout_controller.hpp"
#include "mso/ui/layout_preset_store.hpp"
#include "mso/ui/layout_store.hpp"
#include "mso/ui/main_window.hpp"
#include "mso/ui/navigator_panel.hpp"
#include "mso/ui/notification_bar.hpp"
#include "mso/ui/optimistic_edit.hpp"
#include "mso/ui/preferences_dialog.hpp"
#include "mso/ui/preview_panel.hpp"
#include "mso/ui/sample_context_menu.hpp"
#include "mso/ui/sample_row_delegate.hpp"
#include "mso/ui/sample_table_model.hpp"
#include "mso/ui/sample_table_view.hpp"
#include "mso/ui/search_bar.hpp"
#include "mso/ui/shortcut_catalog.hpp"
#include "mso/ui/star_rating.hpp"
#include "mso/ui/star_rating_widget.hpp"
#include "mso/ui/undo_router.hpp"
#include "mso/ui/waveform_view.hpp"
#include "test_support.hpp"

namespace {

using mso::search::SampleRow;
using mso::ui::SampleKey;
using mso::ui::SampleTableModel;

/// One QApplication for the whole test run, on the offscreen platform with silent audio (shared
/// with the other ui_*.cpp files: whichever asks first creates it).
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
bool process_events_until(const std::function<bool()>& done, int timeout_milliseconds = 5000) {
    QElapsedTimer timer;
    timer.start();
    while (!done() && timer.elapsed() < timeout_milliseconds)
        QCoreApplication::processEvents(QEventLoop::AllEvents, 10);
    return done();
}

/// Delivers now (queued on `context`) or holds deliveries until release(), to interleave events.
class DeliveryQueue {
public:
    bool hold = false;  ///< when set, deliveries wait for release()

    void schedule(QObject* context, std::function<void()> delivery) {
        if (hold)
            held_.push_back(std::move(delivery));
        else
            QTimer::singleShot(0, context, std::move(delivery));
    }
    void release() {
        auto pending = std::exchange(held_, {});
        for (auto& delivery : pending)
            delivery();
    }
    /// Delivers only the most recently held delivery (out of order, as a busy pool may).
    void release_newest() {
        if (held_.empty()) return;
        auto delivery = std::move(held_.back());
        held_.pop_back();
        delivery();
    }
    std::size_t held_count() const { return held_.size(); }

private:
    std::vector<std::function<void()>> held_;
};

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

/// In-memory search backend: each query text maps to a sorted row list; cursors are offsets
/// tagged with the structure generation they were issued in. Bumping `structure_generation`
/// makes every older cursor STALE_CURSOR, like the Application's index after rows are added.
class FakePageSource final : public mso::ui::SamplePageSource {
public:
    std::map<std::string, std::vector<SampleRow>> results;
    std::vector<mso::search::SearchRequest> page_requests;
    int count_requests = 0;
    int rows_requests = 0;
    int stale_failures = 0;                ///< pages refused because their cursor went stale
    int skipped_obsolete = 0;              ///< requests whose stop token was set before they ran
    int structure_generation = 0;          ///< cursors of older generations are stale
    std::optional<mso::Error> rows_error;  ///< when set, request_rows() fails with it
    std::function<void()> after_page;      ///< runs after every page delivery (a stream of change events)
    DeliveryQueue deliveries;

    void request_page(const mso::search::SearchRequest& request, std::optional<uuids::uuid>, std::stop_token obsolete,
                      QObject* context, PageDelivery deliver) override {
        page_requests.push_back(request);
        // The page is computed when the "work" runs (the delivery), like on the session's pool: a
        // structure change between the request and then makes its cursor stale.
        deliveries.schedule(context, [this, request, deliver, obsolete] {
            if (obsolete.stop_requested()) ++skipped_obsolete;
            deliver(page_for(request));
            if (after_page) after_page();
        });
    }

    mso::Result<mso::search::SearchPage> page_for(const mso::search::SearchRequest& request) {
        std::size_t offset = 0;
        if (request.cursor) {
            const std::string& token = request.cursor->token;
            const auto separator = token.find(':');
            offset = std::stoul(token.substr(0, separator));
            if (std::stoi(token.substr(separator + 1)) != structure_generation) {
                ++stale_failures;
                return mso::fail(mso::ErrorCode::StaleCursor, "the result changed");
            }
        }
        const auto& rows = results[request.text];
        mso::search::SearchPage page;
        for (std::size_t index = offset; index < rows.size() && page.rows.size() < request.page_size; ++index)
            page.rows.push_back(rows[index]);
        if (offset + page.rows.size() < rows.size())
            page.next = mso::search::Cursor{std::to_string(offset + page.rows.size()) + ":" +
                                            std::to_string(structure_generation)};
        return page;
    }

    void request_count(const mso::search::SearchRequest& request, std::optional<uuids::uuid>, std::stop_token,
                       QObject* context, CountDelivery deliver) override {
        ++count_requests;
        mso::search::CountResult count;
        count.total = static_cast<std::int64_t>(results[request.text].size());
        deliveries.schedule(context, [deliver, count] { deliver(count); });
    }

    void request_rows(const std::vector<SampleKey>& keys, QObject* context, RowsDelivery deliver) override {
        ++rows_requests;
        if (rows_error) {
            deliveries.schedule(context, [deliver, error = *rows_error] { deliver(std::unexpected(error)); });
            return;
        }
        std::vector<SampleRow> found;
        for (const SampleKey& key : keys) {
            for (const auto& [text, rows] : results) {
                const auto match = std::ranges::find_if(rows, [&key](const SampleRow& row) {
                    return row.library == key.library && row.sample_id == key.sample_id;
                });
                if (match != rows.end()) {
                    found.push_back(*match);
                    break;
                }
            }
        }
        deliveries.schedule(context, [deliver, found] { deliver(found); });
    }
};

/// Edit backend that succeeds or fails on demand, optionally changing the fake "catalog".
class FakeEditGateway final : public mso::ui::EditGateway {
public:
    bool succeed = true;
    std::function<void(const mso::catalog::EditCommand&)> on_success;  ///< simulates the catalog write
    std::vector<mso::catalog::EditCommand> edits;
    int undo_requests = 0;
    DeliveryQueue deliveries;

    void request_edit(const uuids::uuid&, const mso::catalog::EditCommand& command, QObject* context,
                      ReceiptDelivery deliver) override {
        edits.push_back(command);
        deliveries.schedule(context, [this, command, deliver] { deliver(outcome(command)); });
    }
    void request_taxonomy(const uuids::uuid&, const mso::catalog::TaxonomyCommand&, QObject* context,
                          ReceiptDelivery deliver) override {
        deliveries.schedule(context, [this, deliver] { deliver(outcome({})); });
    }
    void request_undo(const uuids::uuid&, QObject* context, ReceiptDelivery deliver) override {
        ++undo_requests;
        deliveries.schedule(context, [this, deliver] { deliver(outcome({})); });
    }
    void request_redo(const uuids::uuid&, QObject* context, ReceiptDelivery deliver) override {
        deliveries.schedule(context, [this, deliver] { deliver(outcome({})); });
    }
    QString library_name(const uuids::uuid&) const override { return QStringLiteral("Test library"); }

private:
    mso::Result<mso::catalog::MutationReceipt> outcome(const mso::catalog::EditCommand& command) {
        if (!succeed) return mso::fail(mso::ErrorCode::DbBusy, "the catalog is busy");
        if (on_success) on_success(command);
        mso::catalog::MutationReceipt receipt;
        receipt.applied = static_cast<std::int64_t>(command.sample_ids.size());
        return receipt;
    }
};

std::vector<SampleRow> make_rows(int count, const std::string& prefix, const uuids::uuid& library = mso::new_uuid()) {
    std::vector<SampleRow> rows;
    for (int index = 0; index < count; ++index) {
        SampleRow row;
        row.library = library;
        row.sample_id = index + 1;
        row.display_name = prefix + std::to_string(index);
        row.file_name = row.display_name + ".wav";
        row.availability = mso::search::RowAvailability::Available;
        rows.push_back(row);
    }
    return rows;
}

SampleKey key_of(const SampleRow& row) {
    return SampleKey{row.library, row.sample_id};
}

QString name_at(const SampleTableModel& model, int row) {
    return model.data(model.index(row, static_cast<int>(SampleTableModel::Column::Name))).toString();
}

/// Sends a full left click (press + release) to a table viewport at `position`.
void click(mso::ui::SampleTableView& view, const QPoint& position) {
    for (QEvent::Type type : {QEvent::MouseButtonPress, QEvent::MouseButtonRelease}) {
        QMouseEvent event(type, position, view.viewport()->mapToGlobal(position), Qt::LeftButton,
                          type == QEvent::MouseButtonPress ? Qt::LeftButton : Qt::NoButton, Qt::NoModifier);
        QApplication::sendEvent(view.viewport(), &event);
    }
}

/// Center of star `star` in the rating cell of `row`.
QPoint star_position(const mso::ui::SampleTableView& view, const SampleTableModel& model, int row, int star) {
    QStyleOptionViewItem option;
    option.initFrom(&view);
    option.rect = view.visualRect(model.index(row, static_cast<int>(SampleTableModel::Column::Rating)));
    return mso::ui::StarRating::star_rect(mso::ui::SampleRowDelegate::rating_area(option),
                                          mso::ui::SampleRowDelegate::rating_star_size(option), star)
        .center();
}

/// A widget child with the given accessible name (fails the test when absent).
template <class Widget>
Widget* child_named(QWidget& parent, const QString& accessible_name) {
    for (Widget* widget : parent.findChildren<Widget*>())
        if (widget->accessibleName() == accessible_name) return widget;
    FAIL("no child named " << accessible_name.toStdString());
    return nullptr;
}

}  // namespace

TEST_SUITE("ui") {
    // ---------------------------------------------------------------- paged model

    TEST_CASE("paged model fetches pages on demand and counts once per query") {
        application();
        FakePageSource source;
        source.results[""] = make_rows(450, "kick ");
        SampleTableModel model(source);
        std::vector<qint64> counts;
        QObject::connect(&model, &SampleTableModel::countChanged, [&](qint64 total, bool) { counts.push_back(total); });

        model.set_query("", {});
        REQUIRE(process_events_until(
            [&] { return model.rowCount() == SampleTableModel::kPageSize && counts.size() == 1; }));
        CHECK(counts.front() == 450);
        CHECK(model.canFetchMore({}));
        model.fetchMore({});
        REQUIRE(process_events_until([&] { return model.rowCount() == 400; }));
        model.fetchMore({});
        REQUIRE(process_events_until([&] { return model.rowCount() == 450; }));
        CHECK_FALSE(model.canFetchMore({}));
        CHECK(name_at(model, 449) == QStringLiteral("kick 449"));
        CHECK(source.count_requests == 1);
        CHECK(source.page_requests.size() == 3);
        CHECK(source.page_requests.back().cursor->token == "400:0");
    }

    TEST_CASE("results of an obsolete query generation are discarded") {
        application();
        FakePageSource source;
        source.results["old"] = make_rows(10, "old ");
        source.results["new"] = make_rows(3, "new ");
        SampleTableModel model(source);
        source.deliveries.hold = true;
        model.set_query("old", {});
        model.set_query("new", {});
        source.deliveries.hold = false;
        source.deliveries.release();  // delivers "old" first, then "new"
        REQUIRE(process_events_until([&] { return model.rowCount() == 3; }));
        CHECK(name_at(model, 0) == QStringLiteral("new 0"));
    }

    TEST_CASE("at most ten pages stay hydrated; evicted rows show loading and are re-read") {
        application();
        FakePageSource source;
        source.results[""] = make_rows(SampleTableModel::kPageSize * 12, "row ");
        SampleTableModel model(source);
        model.set_query("", {});
        REQUIRE(process_events_until([&] { return model.rowCount() == SampleTableModel::kPageSize; }));
        for (int page = 1; page < 12; ++page) {
            model.fetchMore({});
            REQUIRE(process_events_until([&] { return model.rowCount() == SampleTableModel::kPageSize * (page + 1); }));
        }
        int hydrated_pages = 0;
        for (int page = 0; page < 12; ++page)
            if (model.loaded_row(page * SampleTableModel::kPageSize)) ++hydrated_pages;
        CHECK(hydrated_pages == SampleTableModel::kMaxHydratedPages);
        CHECK(model.loaded_row(0) == nullptr);  // the least recently used page went first

        CHECK(name_at(model, 0) == QStringLiteral("Loading…"));
        CHECK(model.data(model.index(0, 0), SampleTableModel::LoadingRole).toBool());
        REQUIRE(process_events_until([&] { return model.loaded_row(0) != nullptr; }));
        CHECK(name_at(model, 0) == QStringLiteral("row 0"));
        CHECK(source.page_requests.back().cursor == std::nullopt);  // re-read with page 0's start cursor
    }

    TEST_CASE("sorting re-queries server-side; unsortable columns are rejected") {
        application();
        FakePageSource source;
        source.results[""] = make_rows(5, "s ");
        SampleTableModel model(source);
        model.set_query("", {});
        REQUIRE(process_events_until([&] { return model.rowCount() == 5; }));
        int rejected = 0;
        QObject::connect(&model, &SampleTableModel::sortRejected, [&] { ++rejected; });
        model.sort(static_cast<int>(SampleTableModel::Column::Tags), Qt::AscendingOrder);
        CHECK(rejected == 1);
        const auto requests_before = source.page_requests.size();
        model.sort(static_cast<int>(SampleTableModel::Column::Bpm), Qt::DescendingOrder);
        REQUIRE(process_events_until(
            [&] { return source.page_requests.size() > requests_before && model.rowCount() == 5; }));
        CHECK(source.page_requests.back().sort == mso::search::SortField::Bpm);
        CHECK(source.page_requests.back().descending);
    }

    TEST_CASE("a full refresh re-reads the window and announces it") {
        application();
        FakePageSource source;
        source.results[""] = make_rows(300, "a ");
        SampleTableModel model(source);
        model.set_query("", {});
        REQUIRE(process_events_until([&] { return model.rowCount() == SampleTableModel::kPageSize; }));
        model.fetchMore({});
        REQUIRE(process_events_until([&] { return model.rowCount() == 300; }));
        source.results[""] = make_rows(250, "b ");
        int refreshed = 0;
        QObject::connect(&model, &SampleTableModel::refreshed, [&] { ++refreshed; });
        model.refresh(210);
        REQUIRE(process_events_until([&] { return refreshed == 1; }));
        CHECK(model.rowCount() == 250);
        CHECK(name_at(model, 0) == QStringLiteral("b 0"));
        CHECK(name_at(model, 249) == QStringLiteral("b 249"));
    }

    TEST_CASE("a refresh while a new query is pending never stops paging") {
        application();
        FakePageSource source;
        source.results["old"] = make_rows(5, "old ");
        source.results["new"] = make_rows(450, "new ");
        SampleTableModel model(source);
        model.set_query("old", {});
        REQUIRE(process_events_until([&] { return model.rowCount() == 5; }));
        source.deliveries.hold = true;
        model.set_query("new", {});
        model.refresh(0);  // a catalog change arrives before the first page
        source.deliveries.hold = false;
        source.deliveries.release();
        REQUIRE(process_events_until([&] { return model.rowCount() == SampleTableModel::kPageSize; }));
        CHECK(name_at(model, 0) == QStringLiteral("new 0"));
        CHECK(model.canFetchMore({}));  // the review's stall: replacing_ stayed set and paging stopped
        model.fetchMore({});
        REQUIRE(process_events_until([&] { return model.rowCount() == 2 * SampleTableModel::kPageSize; }));
    }

    TEST_CASE("a new query keeps the previous rows until its first page, and duplicates are skipped") {
        application();
        FakePageSource source;
        source.results["old"] = make_rows(5, "old ");
        source.results["new"] = make_rows(2, "new ");
        SampleTableModel model(source);
        model.set_query("old", {});
        REQUIRE(process_events_until([&] { return model.rowCount() == 5; }));

        source.deliveries.hold = true;
        model.set_query("new", {});
        QCoreApplication::processEvents();
        CHECK(model.rowCount() == 5);  // no flash of an empty or "Loading…" table
        CHECK(name_at(model, 0) == QStringLiteral("old 0"));
        CHECK_FALSE(model.canFetchMore({}));
        source.deliveries.hold = false;
        source.deliveries.release();
        REQUIRE(process_events_until([&] { return model.rowCount() == 2; }));
        CHECK(name_at(model, 0) == QStringLiteral("new 0"));

        const auto requests = source.page_requests.size();
        model.set_query("new", {});  // the same navigator node clicked again
        QCoreApplication::processEvents();
        CHECK(source.page_requests.size() == requests);
        model.run_query("new", {});  // libraries changed: same query, new data
        REQUIRE(process_events_until([&] { return source.page_requests.size() == requests + 1; }));
    }

    TEST_CASE("catalog changes refresh only the named rows, in place") {
        application();
        FakePageSource source;
        source.results[""] = make_rows(5, "row ");
        SampleTableModel model(source);
        model.set_query("", {});
        REQUIRE(process_events_until([&] { return model.rowCount() == 5; }));
        const auto requests = source.page_requests.size();
        auto& rows = source.results[""];
        rows[1].display_name = "renamed";
        const SampleKey deleted = key_of(rows[3]);
        rows.erase(rows.begin() + 3);

        model.refresh_rows({key_of(rows[1]), deleted});
        REQUIRE(process_events_until([&] { return model.rowCount() == 4; }));
        CHECK(name_at(model, 1) == QStringLiteral("renamed"));
        CHECK(source.page_requests.size() == requests);  // no re-query

        // A row with an optimistic edit in flight is left alone until the edit settles.
        model.begin_pending_edit({key_of(rows[0])});
        rows[0].display_name = "server value";
        model.refresh_rows({key_of(rows[0])});
        QCoreApplication::processEvents();
        CHECK(name_at(model, 0) == QStringLiteral("row 0"));
        model.end_pending_edit({key_of(rows[0])});
        model.refresh_rows({key_of(rows[0])});
        REQUIRE(process_events_until([&] { return name_at(model, 0) == QStringLiteral("server value"); }));
    }

    // ---------------------------------------------------------------- optimistic edits

    TEST_CASE("optimistic edits predict the visible row") {
        SampleRow row;
        row.tag_paths = {"Drums/Kick", "Drums, Acoustic"};
        row.tag_ids = {7, 8};
        row.category_id = 9;
        mso::catalog::EditCommand command;
        command.rating = 4;
        command.favorite = true;
        command.display_name = "Big kick";
        command.tags_remove = {"Drums, Acoustic"};  // a tag name with ", " must survive intact
        command.tags_add = {"Punchy"};
        command.category = std::optional<std::string>("Drums/Kicks");
        command.bpm = mso::catalog::BpmOverrideEdit{mso::catalog::OverrideState::Value, 128'000};
        command.key = mso::catalog::KeyOverrideEdit{mso::catalog::OverrideState::Unknown, std::nullopt};
        mso::ui::OptimisticEdit::apply(command, row);
        CHECK(row.rating == 4);
        CHECK(row.favorite);
        CHECK(row.display_name == "Big kick");
        CHECK(row.tag_paths == std::vector<std::string>{"Drums/Kick", "Punchy"});
        CHECK(row.tag_ids == std::vector<std::int64_t>{7, 0});
        CHECK(mso::ui::DisplayFormat::tag_list(row.tag_paths) == QStringLiteral("Drums/Kick, Punchy"));
        CHECK(row.category == "Drums/Kicks");
        CHECK_FALSE(row.category_id.has_value());  // the catalog assigns it on reconciliation
        CHECK(row.bpm_milli == 128'000);
        CHECK(row.bpm_source == "user");
        CHECK_FALSE(row.key.has_value());
        CHECK(row.key_source == "user_unknown");
        CHECK_FALSE(mso::ui::OptimisticEdit::removes_rows(command));
        command.removed = true;
        CHECK(mso::ui::OptimisticEdit::removes_rows(command));
    }

    TEST_CASE("a rating click updates the model synchronously, before any background work") {
        application();
        FakePageSource source;
        source.results[""] = make_rows(3, "row ");
        SampleTableModel model(source);
        mso::ui::SampleTableView view;
        view.set_sample_model(&model);
        view.resize(900, 300);
        view.show();
        model.set_query("", {});
        REQUIRE(process_events_until([&] { return model.rowCount() == 3; }));

        FakeEditGateway gateway;
        gateway.deliveries.hold = true;  // SQLite "never finishes" during this test
        mso::ui::EditController controller(gateway, model);
        QObject::connect(&view, &mso::ui::SampleTableView::ratingClicked, [&](const SampleKey& key, int rating) {
            mso::catalog::EditCommand command;
            command.rating = rating;
            controller.apply({key}, command, QStringLiteral("Rated"));
        });
        click(view, star_position(view, model, 0, 4));
        // No event processing between the click and this check: the row already shows four stars.
        CHECK(model.loaded_row(0)->rating == 4);
        CHECK(model.data(model.index(0, 0), SampleTableModel::RatingRole).toInt() == 4);
        CHECK(gateway.edits.size() == 1);
        CHECK(gateway.deliveries.held_count() == 1);
    }

    TEST_CASE("a failed edit is reverted from the index and reported prominently") {
        application();
        FakePageSource source;
        source.results[""] = make_rows(2, "row ");
        SampleTableModel model(source);
        model.set_query("", {});
        REQUIRE(process_events_until([&] { return model.rowCount() == 2; }));
        FakeEditGateway gateway;
        gateway.succeed = false;
        mso::ui::EditController controller(gateway, model);
        std::optional<QString> failure;
        QObject::connect(&controller, &mso::ui::EditController::failed,
                         [&](const QString& title, const QString&) { failure = title; });

        mso::catalog::EditCommand command;
        command.favorite = true;
        controller.apply({key_of(source.results[""][0])}, command, QStringLiteral("Marked favorite"));
        CHECK(model.loaded_row(0)->favorite);  // optimistic
        REQUIRE(process_events_until([&] { return failure.has_value(); }));
        REQUIRE(process_events_until([&] { return !model.loaded_row(0)->favorite; }));  // reverted
        CHECK_FALSE(model.has_pending_edit(key_of(source.results[""][0])));
    }

    TEST_CASE("a successful edit is reconciled with the catalog's values") {
        application();
        FakePageSource source;
        source.results[""] = make_rows(2, "row ");
        SampleTableModel model(source);
        model.set_query("", {});
        REQUIRE(process_events_until([&] { return model.rowCount() == 2; }));
        FakeEditGateway gateway;
        gateway.on_success = [&source](const mso::catalog::EditCommand& command) {
            // The catalog computes what the prediction cannot: the automatic BPM after "Automatic".
            for (auto& row : source.results[""])
                if (std::ranges::find(command.sample_ids, row.sample_id) != command.sample_ids.end())
                    row.bpm_milli = 93'000;
        };
        mso::ui::EditController controller(gateway, model);
        int successes = 0;
        QObject::connect(&controller, &mso::ui::EditController::succeeded, [&] { ++successes; });
        mso::catalog::EditCommand command;
        command.bpm = mso::catalog::BpmOverrideEdit{mso::catalog::OverrideState::Automatic, std::nullopt};
        controller.apply({key_of(source.results[""][1])}, command, QStringLiteral("BPM"));
        REQUIRE(process_events_until([&] { return successes == 1 && model.loaded_row(1)->bpm_milli == 93'000; }));
        CHECK(gateway.edits.front().sample_ids == std::vector<std::int64_t>{2});
    }

    TEST_CASE("an optimistic removal takes rows out at once") {
        application();
        FakePageSource source;
        source.results[""] = make_rows(4, "row ");
        SampleTableModel model(source);
        model.set_query("", {});
        REQUIRE(process_events_until([&] { return model.rowCount() == 4; }));
        FakeEditGateway gateway;
        gateway.deliveries.hold = true;
        mso::ui::EditController controller(gateway, model);
        mso::catalog::EditCommand command;
        command.removed = true;
        controller.apply({key_of(source.results[""][1]), key_of(source.results[""][2])}, command,
                         QStringLiteral("Removed"));
        CHECK(model.rowCount() == 2);
        CHECK(name_at(model, 1) == QStringLiteral("row 3"));
    }

    TEST_CASE("undo routing prefers the current row, then history, navigator, last edit, only library") {
        const auto first = mso::new_uuid();
        const auto second = mso::new_uuid();
        mso::ui::UndoContext context;
        context.open_libraries = {first, second};
        CHECK_FALSE(mso::ui::UndoRouter::choose(context).has_value());
        context.last_edited_library = second;
        CHECK(mso::ui::UndoRouter::choose(context) == second);
        context.navigator_library = first;
        CHECK(mso::ui::UndoRouter::choose(context) == first);
        context.current_row_library = second;
        CHECK(mso::ui::UndoRouter::choose(context) == second);
        context = {};
        context.open_libraries = {first};
        context.last_edited_library = mso::new_uuid();  // closed since: ignored
        CHECK(mso::ui::UndoRouter::choose(context) == first);
    }

    // ---------------------------------------------------------------- table view and delegate

    TEST_CASE("sample rows paint as one continuous band and stars in the table are clickable") {
        application();
        FakePageSource source;
        source.results[""] = make_rows(3, "row ");
        SampleTableModel model(source);
        mso::ui::SampleTableView view;
        view.set_sample_model(&model);
        view.resize(900, 300);
        view.show();
        model.set_query("", {});
        REQUIRE(process_events_until([&] { return model.rowCount() == 3; }));
        view.selectRow(1);
        QCoreApplication::processEvents();

        // Every pixel along the selected row's top edge, across all cell boundaries, is the band color.
        const QImage image = view.viewport()->grab().toImage();
        const int band_y = view.rowViewportPosition(1) + 1;
        const QColor band = image.pixelColor(2, band_y);
        int mismatches = 0;
        const int band_right = std::min(image.width(), view.horizontalHeader()->length()) - 2;
        for (int x = 2; x < band_right; ++x)
            if (image.pixelColor(x, band_y) != band) ++mismatches;
        CHECK(mismatches == 0);
        CHECK(image.pixelColor(2, view.rowViewportPosition(0) + 1) != band);  // unselected rows stay plain

        std::vector<std::pair<std::int64_t, int>> rated;
        QObject::connect(&view, &mso::ui::SampleTableView::ratingClicked,
                         [&](const SampleKey& key, int rating) { rated.emplace_back(key.sample_id, rating); });
        click(view, star_position(view, model, 2, 2));
        REQUIRE(rated.size() == 1);
        CHECK(rated.front() == std::pair<std::int64_t, int>{3, 2});  // the third row's sample, two stars

        // A double click on a star is two rating clicks, never "play".
        int played = 0;
        QObject::connect(&view, &QAbstractItemView::doubleClicked, [&] { ++played; });
        const QPoint star = star_position(view, model, 2, 2);
        QMouseEvent double_click(QEvent::MouseButtonDblClick, star, view.viewport()->mapToGlobal(star), Qt::LeftButton,
                                 Qt::LeftButton, Qt::NoModifier);
        QApplication::sendEvent(view.viewport(), &double_click);
        CHECK(played == 0);
    }

    TEST_CASE("select_keys selects exactly the loaded keys") {
        application();
        FakePageSource source;
        source.results[""] = make_rows(10, "row ");
        SampleTableModel model(source);
        mso::ui::SampleTableView view;
        view.set_sample_model(&model);
        model.set_query("", {});
        REQUIRE(process_events_until([&] { return model.rowCount() == 10; }));
        const auto& rows = source.results[""];
        const std::vector<SampleKey> wanted{key_of(rows[2]), key_of(rows[7])};
        CHECK(view.select_keys(wanted, key_of(rows[7])) == 2);
        CHECK(view.selected_keys() == wanted);
        CHECK(view.current_key() == key_of(rows[7]));
        CHECK(view.select_keys({SampleKey{mso::new_uuid(), 1}}, std::nullopt) == 0);  // unknown keys are skipped
        CHECK(view.selected_row_count() == 0);
    }

    TEST_CASE("star strip hit-testing maps every x position to one star") {
        const QRect area(10, 0, 200, 20);
        constexpr int kStar = 16;
        const int step = kStar + mso::ui::StarRating::spacing(kStar);
        CHECK(mso::ui::StarRating::star_at(area, kStar, QPoint(10, 10)) == 1);
        CHECK(mso::ui::StarRating::star_at(area, kStar, QPoint(10 + step - 1, 10)) == 1);  // gap belongs to the left
        CHECK(mso::ui::StarRating::star_at(area, kStar, QPoint(10 + step, 10)) == 2);
        CHECK(mso::ui::StarRating::star_at(area, kStar, QPoint(10 + 4 * step + kStar - 1, 10)) == 5);
        CHECK(mso::ui::StarRating::star_at(area, kStar, QPoint(9, 10)) == 0);
        CHECK(mso::ui::StarRating::star_at(area, kStar,
                                           QPoint(10 + mso::ui::StarRating::strip_size(kStar).width(), 10)) == 0);
        CHECK(mso::ui::StarRating::star_rect(area, kStar, 3).left() == 10 + 2 * step);
    }

    TEST_CASE("star rating widget: click sets, clicking the current rating clears, keys and accessibility work") {
        application();
        mso::ui::StarRatingWidget stars;
        stars.resize(stars.sizeHint());
        std::vector<int> chosen;
        QObject::connect(&stars, &mso::ui::StarRatingWidget::ratingChosen,
                         [&](int rating) { chosen.push_back(rating); });
        const auto click_star = [&](int star) {
            const QRect area = stars.rect().adjusted(3, 3, -3, -3);  // matches the widget's focus margin
            const QPoint centre =
                mso::ui::StarRating::star_rect(area, std::max(12, stars.fontMetrics().height()), star).center();
            QMouseEvent release(QEvent::MouseButtonRelease, centre, stars.mapToGlobal(centre), Qt::LeftButton,
                                Qt::NoButton, Qt::NoModifier);
            QApplication::sendEvent(&stars, &release);
        };
        click_star(3);
        stars.set_rating(3);
        click_star(3);
        stars.set_rating(0);
        QKeyEvent four(QEvent::KeyPress, Qt::Key_4, Qt::NoModifier, QStringLiteral("4"));
        QApplication::sendEvent(&stars, &four);
        stars.set_rating(4);
        QKeyEvent right(QEvent::KeyPress, Qt::Key_Right, Qt::NoModifier);
        QApplication::sendEvent(&stars, &right);
        stars.set_mixed(true);
        click_star(1);  // a mixed selection is set, never "cleared"
        CHECK(chosen == std::vector<int>{3, 0, 4, 5, 1});
        CHECK(stars.accessibleName() == QStringLiteral("Rating"));

        stars.set_rating(2);
        QAccessibleInterface* accessible = QAccessible::queryAccessibleInterface(&stars);
        REQUIRE(accessible != nullptr);
        CHECK(accessible->role() == QAccessible::Slider);
        REQUIRE(accessible->valueInterface() != nullptr);
        CHECK(accessible->valueInterface()->currentValue().toInt() == 2);
        CHECK(accessible->valueInterface()->maximumValue().toInt() == 5);
        accessible->valueInterface()->setCurrentValue(5);
        CHECK(chosen.back() == 5);
    }

    // ---------------------------------------------------------------- panels and dialogs

    TEST_CASE("inspector never overwrites a field being edited, and multi-library selections disable combos") {
        application();
        mso::ui::ApplicationSession idle_session(mso::app::ApplicationOptions{});  // never started
        mso::ui::InspectorPanel inspector(idle_session);
        auto rows = make_rows(2, "row ");
        inspector.show_selection({rows[0]}, 1);
        auto* name_edit = child_named<QLineEdit>(inspector, QStringLiteral("Display name"));
        CHECK(name_edit->text() == QStringLiteral("row 0"));

        name_edit->setText(QStringLiteral("typed by the user"));
        name_edit->setModified(true);
        rows[0].display_name = "changed in the catalog";
        inspector.show_selection({rows[0]}, 1);  // the same sample refreshed in place
        CHECK(name_edit->text() == QStringLiteral("typed by the user"));

        inspector.show_selection({rows[1]}, 1);  // another sample: the draft is discarded
        CHECK(name_edit->text() == QStringLiteral("row 1"));

        // Tags come from the path list, so a tag whose name contains ", " stays one tag.
        rows[0].tag_paths = {"Drums, Acoustic"};
        rows[1].tag_paths = {"Drums, Acoustic", "Loops"};
        inspector.show_selection(rows, 2);
        auto* tags = child_named<QListWidget>(inspector, QStringLiteral("Tags"));
        REQUIRE(tags->count() == 2);
        CHECK(tags->item(0)->data(Qt::UserRole).toString() == QStringLiteral("Drums, Acoustic"));
        CHECK(tags->item(1)->text() == QStringLiteral("Loops (1 of 2)"));

        auto other_library = make_rows(1, "other ");
        inspector.show_selection({rows[0], other_library[0]}, 2);
        CHECK_FALSE(child_named<QComboBox>(inspector, QStringLiteral("Category to set"))->isEnabled());
    }

    TEST_CASE("jobs panel keeps cleared runs cleared and bounds finished rows") {
        application();
        mso::ui::JobsPanel jobs;
        mso::app::AnalysisProgressEvent event;
        event.library = mso::new_uuid();
        mso::catalog::Library::AnalysisRunStatus run;
        run.id = 1;
        run.profile_name = "Musical";
        run.state = "running";
        run.queued = 2;
        event.runs = {run};
        jobs.update_analysis(event, QStringLiteral("Library"));
        CHECK(jobs.row_count() == 1);
        event.runs.front().state = "completed";
        jobs.update_analysis(event, QStringLiteral("Library"));
        QPushButton* clear = nullptr;
        for (auto* button : jobs.findChildren<QPushButton*>())
            if (button->text() == QStringLiteral("Clear finished")) clear = button;
        REQUIRE(clear != nullptr);
        clear->click();
        CHECK(jobs.row_count() == 0);
        jobs.update_analysis(event, QStringLiteral("Library"));  // still reported by the Application
        CHECK(jobs.row_count() == 0);

        for (int task = 0; task < mso::ui::JobsPanel::kMaxFinishedRows + 10; ++task)
            jobs.finish_local_task(jobs.start_local_task(QStringLiteral("Export %1").arg(task), {}),
                                   QStringLiteral("ok"), true);
        CHECK(jobs.row_count() == mso::ui::JobsPanel::kMaxFinishedRows);
    }

    TEST_CASE("history panel without a library says so") {
        application();
        mso::ui::ApplicationSession idle_session(mso::app::ApplicationOptions{});
        mso::ui::HistoryPanel history(idle_session);
        history.show_library(std::nullopt);
        bool found = false;
        for (auto* label : history.findChildren<QLabel*>())
            found = found || label->text() == QStringLiteral("No library selected");
        CHECK(found);
        CHECK_FALSE(history.library().has_value());
    }

    TEST_CASE("search bar submits valid queries once and shows positioned parse errors") {
        application();
        mso::ui::SearchBar bar;
        std::vector<QString> submitted;
        QObject::connect(&bar, &mso::ui::SearchBar::querySubmitted,
                         [&](const QString& text) { submitted.push_back(text); });
        bar.set_text(QStringLiteral("kick bpm:abc"));
        CHECK(submitted.empty());
        CHECK(bar.error_text().contains(QStringLiteral("position")));
        bar.set_text(QStringLiteral("kick"));
        CHECK(submitted == std::vector<QString>{QStringLiteral("kick")});
        CHECK(bar.error_text().isEmpty());
        bar.set_text(QStringLiteral("kick"));
        CHECK(submitted.size() == 1);  // unchanged text is not run again
    }

    TEST_CASE("preferences dialog blocks conflicting shortcuts") {
        application();
        mso::ui::PreferencesDialog dialog(mso::ui::ShortcutCatalog::defaults(), true);
        auto* buttons = dialog.findChild<QDialogButtonBox*>();
        REQUIRE(buttons != nullptr);
        CHECK(buttons->button(QDialogButtonBox::Ok)->isEnabled());
        CHECK(dialog.audition_on_selection());
        auto* favorite = child_named<QKeySequenceEdit>(dialog, QStringLiteral("Shortcut for Toggle favorite"));
        favorite->setKeySequence(QKeySequence(Qt::Key_Space));  // already Play / pause
        CHECK_FALSE(buttons->button(QDialogButtonBox::Ok)->isEnabled());
        CHECK(dialog.bindings().at(QStringLiteral("toggle_favorite")) == QKeySequence(Qt::Key_Space));
    }

    TEST_CASE("shortcut bindings round-trip, conflicts are detected, single keys belong to the table") {
        auto bindings = mso::ui::ShortcutCatalog::defaults();
        CHECK(mso::ui::ShortcutCatalog::conflicts(bindings).empty());
        bindings[QStringLiteral("toggle_favorite")] = QKeySequence(Qt::Key_Space);
        const auto conflicts = mso::ui::ShortcutCatalog::conflicts(bindings);
        REQUIRE(conflicts.size() == 1);
        CHECK(conflicts.front().size() == 2);
        const auto restored = mso::ui::ShortcutCatalog::from_json(mso::ui::ShortcutCatalog::to_json(bindings));
        CHECK(restored == bindings);
        // A foreign version or a damaged document falls back to the defaults, without throwing.
        CHECK(mso::ui::ShortcutCatalog::from_json(nlohmann::json{{"version", 99}}) ==
              mso::ui::ShortcutCatalog::defaults());
        CHECK(mso::ui::ShortcutCatalog::from_json(nlohmann::json{{"version", "1"}, {"bindings", 3}}) ==
              mso::ui::ShortcutCatalog::defaults());
        for (const auto& definition : mso::ui::ShortcutCatalog::definitions()) {
            const bool single_key = definition.default_sequence.count() == 1 &&
                                    (definition.default_sequence[0].keyboardModifiers() & ~Qt::KeypadModifier) == 0;
            if (single_key) CHECK(definition.scope == mso::ui::ShortcutScope::SampleTable);
        }
        CHECK(mso::ui::ShortcutCatalog::defaults().at(QStringLiteral("redo")) == QKeySequence(QKeySequence::Redo));
    }

    TEST_CASE("layout store round-trips and rejects foreign or damaged layouts without throwing") {
        application();
        QMainWindow window;
        window.setObjectName(QStringLiteral("LayoutWindow"));
        QTableView table;
        window.setCentralWidget(&table);
        window.resize(640, 480);
        const nlohmann::json layout = mso::ui::LayoutStore::capture(window, *table.horizontalHeader());
        CHECK(mso::ui::LayoutStore::restore(window, *table.horizontalHeader(), layout));
        CHECK_FALSE(mso::ui::LayoutStore::restore(window, *table.horizontalHeader(), nlohmann::json{{"version", 99}}));
        CHECK_FALSE(mso::ui::LayoutStore::restore(window, *table.horizontalHeader(), nlohmann::json{{"version", "1"}}));
        CHECK_FALSE(mso::ui::LayoutStore::restore(window, *table.horizontalHeader(), nlohmann::json::array()));
        window.takeCentralWidget();
    }

    TEST_CASE("layout controller saves, applies, resets and deletes presets") {
        application();
        mso::ui::ApplicationSession idle_session(mso::app::ApplicationOptions{});
        QMainWindow window;
        window.setObjectName(QStringLiteral("LayoutControllerWindow"));
        auto* table = new QTableView;
        window.setCentralWidget(table);
        mso::ui::LayoutController controller(idle_session, window, *table->horizontalHeader());
        controller.capture_default();
        QMenu view;
        controller.add_menu_entries(view);
        int restored = 0;
        QObject::connect(&controller, &mso::ui::LayoutController::headerRestored, [&] { ++restored; });
        controller.save_preset_as(QStringLiteral("Mixing"));
        CHECK(controller.preset_names() == QStringList{QStringLiteral("Mixing")});
        controller.apply_preset(QStringLiteral("Mixing"));
        CHECK(restored == 1);
        controller.reset();
        CHECK(restored == 2);
        controller.save_current();
        CHECK(idle_session.preference(mso::ui::LayoutStore::kCurrentLayoutKey).is_object());
        controller.delete_preset(QStringLiteral("Mixing"));
        CHECK(controller.preset_names().isEmpty());
    }

    TEST_CASE("preferences are cached synchronously and layout presets are kept by name") {
        application();
        mso::ui::ApplicationSession idle_session(mso::app::ApplicationOptions{});
        idle_session.set_preference("preview.volume_db", -12);
        CHECK(idle_session.preference("preview.volume_db") == -12);  // no wait for a file write

        mso::ui::LayoutPresetStore presets(idle_session);
        presets.save(QStringLiteral("Mixing"), nlohmann::json{{"version", 1}});
        presets.save(QStringLiteral("arranging"), nlohmann::json{{"version", 1}});
        CHECK(presets.names() == QStringList{QStringLiteral("arranging"), QStringLiteral("Mixing")});
        CHECK(presets.layout(QStringLiteral("Mixing")).has_value());
        presets.remove(QStringLiteral("Mixing"));
        CHECK_FALSE(presets.layout(QStringLiteral("Mixing")).has_value());
    }

    TEST_CASE("display format: paths round-trip Unicode, sizes use IEC units, UUIDs parse") {
        application();
        const std::filesystem::path hebrew =
            std::filesystem::path(u"samples") / std::filesystem::path(u"\u05D1\u05E2\u05D9\u05D8\u05D4.wav");
        CHECK(mso::ui::DisplayFormat::to_path(mso::ui::DisplayFormat::from_path(hebrew)) == hebrew);
        CHECK(mso::ui::DisplayFormat::from_path(hebrew).contains(QStringLiteral("\u05D1\u05E2\u05D9\u05D8\u05D4")));
        CHECK(mso::ui::DisplayFormat::to_file_url(std::filesystem::absolute(hebrew)).isLocalFile());
        CHECK(mso::ui::DisplayFormat::byte_size(1536).contains(QStringLiteral("KiB")));
        CHECK(mso::ui::DisplayFormat::byte_size(std::nullopt).isEmpty());
        CHECK(mso::ui::DisplayFormat::duration(std::nullopt, true).isEmpty());
        CHECK(mso::ui::DisplayFormat::duration(1'500'000, false).startsWith(QChar(0x2248)));
        CHECK(mso::ui::DisplayFormat::rating_stars(3) == QStringLiteral("\u2605\u2605\u2605\u2606\u2606"));
        CHECK(mso::ui::DisplayFormat::analysis_flags(0) == QStringLiteral("None"));
        CHECK(mso::ui::DisplayFormat::analysis_flags(mso::kAnalyzedTempo | mso::kAnalyzedKey)
                  .contains(QStringLiteral("tempo")));
        const auto id = mso::new_uuid();
        CHECK(mso::ui::DisplayFormat::to_uuid(mso::ui::DisplayFormat::from_uuid(id)) == id);
        CHECK_FALSE(mso::ui::DisplayFormat::to_uuid(QStringLiteral("not a uuid")).has_value());
    }

    TEST_CASE("waveform view seeks only when seekable and exposes its position to accessibility") {
        application();
        mso::ui::WaveformView view;
        view.resize(400, 80);
        std::vector<double> seeks;
        QObject::connect(&view, &mso::ui::WaveformView::seekRequested,
                         [&](double fraction) { seeks.push_back(fraction); });
        view.request_seek(0.5);
        CHECK(seeks.empty());
        view.set_seekable(true);
        view.request_seek(1.7);
        REQUIRE(seeks.size() == 1);
        CHECK(seeks.front() == doctest::Approx(1.0));

        view.show_waveform({{-0.5f, -0.25f}, {0.5f, 0.25f}, {0.3f, 0.1f}});
        CHECK(view.state() == mso::ui::WaveformView::State::Ready);
        view.set_playhead(0.25);
        CHECK(view.playhead() == doctest::Approx(0.25));
        const QImage painted = view.grab().toImage();  // renders the cached pixmap at the device ratio
        CHECK_FALSE(painted.isNull());
        QAccessibleInterface* accessible = QAccessible::queryAccessibleInterface(&view);
        REQUIRE(accessible != nullptr);
        CHECK(accessible->role() == QAccessible::Slider);
        CHECK(accessible->valueInterface()->currentValue().toInt() == 25);
        accessible->valueInterface()->setCurrentValue(40);
        CHECK(seeks.back() == doctest::Approx(0.4));
    }

    TEST_CASE("preview panel offers seam fades only while looping") {
        application();
        mso::ui::ApplicationSession idle_session(mso::app::ApplicationOptions{});
        mso::ui::PreviewPanel preview(idle_session);
        auto* loop = child_named<QCheckBox>(preview, QStringLiteral("Loop preview"));
        auto* seam_fades = child_named<QCheckBox>(preview, QStringLiteral("Fade at loop seams"));
        CHECK_FALSE(seam_fades->isChecked());  // opt-in: they alter what is auditioned
        CHECK_FALSE(seam_fades->isEnabled());
        loop->setChecked(true);
        CHECK(seam_fades->isEnabled());
        CHECK_FALSE(seam_fades->toolTip().isEmpty());
    }

    TEST_CASE("add folder dialog preselects the library and offers copying only as coming later") {
        application();
        const auto first = mso::new_uuid();
        const auto second = mso::new_uuid();
        mso::ui::AddFolderDialog dialog(QStringLiteral("C:/Samples"),
                                        {{QStringLiteral("One"), first}, {QStringLiteral("Two"), second}}, second);
        CHECK(dialog.library() == second);
        int disabled = 0;
        for (auto* option : dialog.findChildren<QRadioButton*>())
            if (!option->isEnabled()) ++disabled;
        CHECK(disabled == 1);
    }

    TEST_CASE("empty state offers adding a folder and opening a library") {
        application();
        mso::ui::EmptyStateWidget empty;
        int add_requests = 0;
        QObject::connect(&empty, &mso::ui::EmptyStateWidget::addFolderRequested, [&] { ++add_requests; });
        empty.show_welcome();
        int visible_buttons = 0;
        for (auto* button : empty.findChildren<QPushButton*>()) {
            if (!button->isVisibleTo(&empty)) continue;
            ++visible_buttons;
            if (button->text().contains(QStringLiteral("folder"), Qt::CaseInsensitive)) button->click();
        }
        CHECK(visible_buttons == 2);
        CHECK(add_requests == 1);
        empty.show_busy(QStringLiteral("Opening workspace…"));
        for (auto* button : empty.findChildren<QPushButton*>())
            CHECK_FALSE(button->isVisibleTo(&empty));
    }

    TEST_CASE("file manager names its action for the platform") {
        application();
#ifdef _WIN32
        CHECK(mso::ui::FileManager::reveal_action_text() == QStringLiteral("Show in Explorer"));
#else
        CHECK(mso::ui::FileManager::reveal_action_text() == QStringLiteral("Show in Folder"));
#endif
    }

    TEST_CASE("about dialog lists every generated dependency notice") {
        application();
        const QString html = mso::ui::AboutDialog::html();
        for (const auto& dependency : mso::generated::kDependencies)
            CHECK(html.contains(mso::ui::DisplayFormat::from_utf8(dependency.name).toHtmlEscaped()));
        CHECK(html.contains(QStringLiteral("GPL-3.0")));
    }

    TEST_CASE("notification bar keeps an error until dismissed") {
        application();
        mso::ui::NotificationBar bar;
        CHECK(bar.message().isEmpty());
        bar.show_error(QStringLiteral("The change was not saved"), QStringLiteral("the catalog is busy"));
        CHECK(bar.message().contains(QStringLiteral("not saved")));
        bar.dismiss();
        CHECK(bar.message().isEmpty());
    }

    TEST_CASE("background tasks turn escaping exceptions into errors instead of terminating") {
        application();
        QThreadPool pool;
        QObject context;
        std::optional<mso::Result<int>> delivered;
        mso::ui::run_in_background(
            pool, &context, []() -> mso::Result<int> { throw std::runtime_error("boom"); },
            [&](mso::Result<int> result) { delivered = std::move(result); });
        REQUIRE(process_events_until([&] { return delivered.has_value(); }));
        REQUIRE_FALSE(delivered->has_value());
        CHECK(delivered->error().code == mso::ErrorCode::Internal);
        pool.waitForDone();
    }

    // ---------------------------------------------------------------- stale cursors and refreshes

    TEST_CASE("a stale cursor runs an anchored refresh that keeps every loaded row and the anchor") {
        application();
        FakePageSource source;
        auto& rows = source.results[""];
        rows = make_rows(3000, "row ");
        SampleTableModel model(source);
        model.set_query("", {});
        REQUIRE(process_events_until([&] { return model.rowCount() == SampleTableModel::kPageSize; }));
        for (int page = 1; page < 6; ++page) {
            model.fetchMore({});
            REQUIRE(process_events_until([&] { return model.rowCount() == SampleTableModel::kPageSize * (page + 1); }));
        }
        const QPersistentModelIndex watched = model.index(1100, 0);  // the view's current row, say
        const SampleKey watched_key = key_of(*model.loaded_row(1100));
        int refreshed = 0;
        QObject::connect(&model, &SampleTableModel::refreshed, [&] { ++refreshed; });

        // An import adds two rows before the anchor: the result's structure changes, every cursor
        // goes stale, and the next page cannot be appended with the old one.
        auto added = make_rows(2, "added ", rows.front().library);
        added[0].sample_id = 10'001;
        added[1].sample_id = 10'002;
        rows.insert(rows.begin(), added.begin(), added.end());
        ++source.structure_generation;
        model.fetchMore({});
        REQUIRE(process_events_until([&] { return refreshed == 1; }));
        CHECK(source.stale_failures == 1);
        CHECK(model.rowCount() >= 1200);  // never cut back to the first pages (the review's row 400)
        REQUIRE(watched.isValid());
        CHECK(watched.row() == 1102);  // the persistent index followed its sample
        CHECK(key_of(*model.loaded_row(watched.row())) == watched_key);
        static_cast<void>(name_at(model, 0));  // outside the refreshed window: the view asks for it
        REQUIRE(process_events_until([&] { return model.loaded_row(0) != nullptr; }));
        CHECK(name_at(model, 0) == QStringLiteral("added 0"));
        // Paging continues with the fresh cursors.
        const int before = model.rowCount();
        REQUIRE(model.canFetchMore({}));
        model.fetchMore({});
        REQUIRE(process_events_until([&] { return model.rowCount() > before; }));
        CHECK(name_at(model, model.rowCount() - 1) == QStringLiteral("row %1").arg(model.rowCount() - 3));
    }

    TEST_CASE("a page re-read with a stale cursor refreshes around that page instead of failing") {
        application();
        FakePageSource source;
        source.results[""] = make_rows(SampleTableModel::kPageSize * 12, "row ");
        SampleTableModel model(source);
        model.set_query("", {});
        REQUIRE(process_events_until([&] { return model.rowCount() == SampleTableModel::kPageSize; }));
        for (int page = 1; page < 12; ++page) {
            model.fetchMore({});
            REQUIRE(process_events_until([&] { return model.rowCount() == SampleTableModel::kPageSize * (page + 1); }));
        }
        REQUIRE(model.loaded_row(SampleTableModel::kPageSize) == nullptr);  // page 1 was evicted
        int refreshed = 0;
        int failures = 0;
        QObject::connect(&model, &SampleTableModel::refreshed, [&] { ++refreshed; });
        QObject::connect(&model, &SampleTableModel::queryFailed, [&] { ++failures; });
        ++source.structure_generation;                                   // page 1's start cursor is stale now
        static_cast<void>(name_at(model, SampleTableModel::kPageSize));  // the view asks for its rows
        REQUIRE(process_events_until([&] { return refreshed == 1; }));
        CHECK(failures == 0);
        CHECK(model.rowCount() == SampleTableModel::kPageSize * 12);
        REQUIRE(process_events_until([&] { return model.loaded_row(SampleTableModel::kPageSize) != nullptr; }));
        CHECK(name_at(model, SampleTableModel::kPageSize) == QStringLiteral("row 200"));
    }

    TEST_CASE("an anchored refresh that meets another structure change walks again and still commits") {
        application();
        FakePageSource source;
        source.results[""] = make_rows(700, "row ");
        SampleTableModel model(source);
        model.set_query("", {});
        REQUIRE(process_events_until([&] { return model.rowCount() == SampleTableModel::kPageSize; }));
        for (int page = 1; page < 4; ++page) {
            model.fetchMore({});
            REQUIRE(process_events_until([&] { return model.rowCount() == std::min(700, 200 * (page + 1)); }));
        }
        int refreshed = 0;
        QObject::connect(&model, &SampleTableModel::refreshed, [&] { ++refreshed; });
        source.deliveries.hold = true;
        model.refresh(650);
        source.deliveries.release();    // the first page (no cursor) arrives; the second is requested
        ++source.structure_generation;  // … and its cursor is stale by the time it runs
        source.deliveries.hold = false;
        source.deliveries.release();
        REQUIRE(process_events_until([&] { return refreshed == 1; }));
        CHECK(source.stale_failures >= 1);
        CHECK(model.rowCount() == 700);
        CHECK_FALSE(model.is_refreshing());
    }

    TEST_CASE("a row repeated on a later page (its sort value changed across the cursor) is shown once") {
        application();
        FakePageSource source;
        auto& rows = source.results[""];
        rows = make_rows(250, "row ");
        rows[210] = rows[5];  // the same sample again, as a keyset cursor can return it
        SampleTableModel model(source);
        model.set_query("", {});
        REQUIRE(process_events_until([&] { return model.rowCount() == SampleTableModel::kPageSize; }));
        model.fetchMore({});
        REQUIRE(process_events_until([&] { return model.rowCount() == 249; }));
        std::set<SampleKey> keys;
        for (int row = 0; row < model.rowCount(); ++row)
            keys.insert(key_of(*model.loaded_row(row)));
        CHECK(keys.size() == 249);
        // A full refresh skips the repeat as well.
        int refreshed = 0;
        QObject::connect(&model, &SampleTableModel::refreshed, [&] { ++refreshed; });
        model.refresh(0);
        REQUIRE(process_events_until([&] { return refreshed == 1; }));
        CHECK(model.rowCount() == 249);
    }

    TEST_CASE("rows with an edit in flight keep the prediction through refreshed and re-read pages") {
        application();
        FakePageSource source;
        source.results[""] = make_rows(3, "row ");
        SampleTableModel model(source);
        model.set_query("", {});
        REQUIRE(process_events_until([&] { return model.rowCount() == 3; }));
        const SampleKey key = key_of(source.results[""][1]);
        model.patch_rows({key}, [](SampleRow& row) { row.rating = 5; });
        model.begin_pending_edit({key});
        int refreshed = 0;
        QObject::connect(&model, &SampleTableModel::refreshed, [&] { ++refreshed; });
        model.refresh(0);  // the catalog still has the old rating
        REQUIRE(process_events_until([&] { return refreshed == 1; }));
        CHECK(model.loaded_row(1)->rating == 5);  // no flicker back to the old value
        model.end_pending_edit({key});
        model.refresh(0);
        REQUIRE(process_events_until([&] { return refreshed == 2; }));
        CHECK(model.loaded_row(1)->rating == 0);
    }

    TEST_CASE("a failed row read is reported and never treated as deleted") {
        application();
        FakePageSource source;
        source.results[""] = make_rows(3, "row ");
        SampleTableModel model(source);
        model.set_query("", {});
        REQUIRE(process_events_until([&] { return model.rowCount() == 3; }));
        std::optional<QString> failure;
        QObject::connect(&model, &SampleTableModel::queryFailed, [&](const QString& message) { failure = message; });
        source.rows_error = mso::fail(mso::ErrorCode::DbBusy, "the library is busy").error();
        CHECK(model.refresh_rows({key_of(source.results[""][0]), key_of(source.results[""][2])}) == 2);
        REQUIRE(process_events_until([&] { return failure.has_value(); }));
        CHECK(model.rowCount() == 3);
        CHECK(failure->contains(QStringLiteral("busy")));
        // A key that is not loaded is reported as such, so the caller can re-read the window.
        CHECK(model.refresh_rows({SampleKey{mso::new_uuid(), 99}}) == 0);
    }

    TEST_CASE("requests of an obsolete generation are told to skip their work") {
        application();
        FakePageSource source;
        source.results["first"] = make_rows(3, "first ");
        source.results["second"] = make_rows(2, "second ");
        SampleTableModel model(source);
        source.deliveries.hold = true;
        model.set_query("first", {});
        model.set_query("second", {});
        source.deliveries.hold = false;
        source.deliveries.release();
        REQUIRE(process_events_until([&] { return model.rowCount() == 2; }));
        CHECK(source.skipped_obsolete == 1);
        CHECK(name_at(model, 0) == QStringLiteral("second 0"));
    }

    TEST_CASE("edits of one library are written one at a time, in order; other libraries in parallel") {
        application();
        FakePageSource source;
        const auto library = mso::new_uuid();
        auto rows = make_rows(2, "row ", library);
        const auto other_rows = make_rows(1, "other ");
        rows.push_back(other_rows.front());
        source.results[""] = rows;
        SampleTableModel model(source);
        model.set_query("", {});
        REQUIRE(process_events_until([&] { return model.rowCount() == 3; }));
        FakeEditGateway gateway;
        gateway.deliveries.hold = true;
        mso::ui::EditController controller(gateway, model);
        int settled = 0;
        QObject::connect(&controller, &mso::ui::EditController::succeeded, [&] { ++settled; });
        mso::catalog::EditCommand first;
        first.rating = 1;
        mso::catalog::EditCommand second;
        second.rating = 2;
        controller.apply({key_of(rows[0])}, first, QStringLiteral("first"));
        controller.apply({key_of(rows[0])}, second, QStringLiteral("second"));
        controller.apply({key_of(rows[2])}, second, QStringLiteral("other library"));
        CHECK(gateway.edits.size() == 2);  // the second edit of `library` waits for the first
        CHECK(controller.queued_writes(library) == 1);
        CHECK(gateway.edits[0].rating == 1);
        gateway.deliveries.hold = false;
        gateway.deliveries.release();
        REQUIRE(process_events_until([&] { return settled == 3; }));
        REQUIRE(gateway.edits.size() == 3);
        CHECK(gateway.edits[2].rating == 2);
        CHECK(controller.queued_writes(library) == 0);
        int undo_requests_before = gateway.undo_requests;
        gateway.deliveries.hold = true;
        controller.undo(library);
        controller.undo(library);
        CHECK(gateway.undo_requests == undo_requests_before + 1);  // undo is ordered like edits
        gateway.deliveries.hold = false;
        gateway.deliveries.release();
        REQUIRE(process_events_until([&] { return gateway.undo_requests == undo_requests_before + 2; }));
    }

    // ---------------------------------------------------------------- inspector drafts and partial selections

    TEST_CASE("inspector edits a partly loaded selection as a whole and says what it summarizes") {
        application();
        mso::ui::ApplicationSession idle_session(mso::app::ApplicationOptions{});
        mso::ui::InspectorPanel inspector(idle_session);
        int requests = 0;
        QObject::connect(&inspector, &mso::ui::InspectorPanel::editRequested, [&] { ++requests; });
        const auto rows = make_rows(2, "row ");
        // Five selected, two loaded: the window resolves all five keys, so editing stays on.
        inspector.show_selection(rows, 5, std::set<uuids::uuid>{rows.front().library});
        auto* stars = inspector.findChild<mso::ui::StarRatingWidget*>();
        REQUIRE(stars != nullptr);
        CHECK(stars->isEnabled());
        bool explained = false;
        for (auto* label : inspector.findChildren<QLabel*>())
            explained = explained || (label->isVisibleTo(&inspector) &&
                                      label->text().contains(QStringLiteral("2 loaded of the 5 selected")));
        CHECK(explained);
        child_named<QPushButton>(inspector, QStringLiteral("Unmark favorite"))->click();
        CHECK(requests == 1);
        // One loaded row of a larger selection is not a single selection: no rename.
        inspector.show_selection({rows.front()}, 3, std::set<uuids::uuid>{rows.front().library});
        CHECK_FALSE(child_named<QPushButton>(inspector, QStringLiteral("Apply display name"))->isEnabled());
        // Libraries of the whole selection decide about categories: a second library outside the
        // loaded rows switches them off.
        inspector.show_selection(rows, 5, std::set<uuids::uuid>{rows.front().library, mso::new_uuid()});
        CHECK_FALSE(child_named<QComboBox>(inspector, QStringLiteral("Category to set"))->isEnabled());
    }

    TEST_CASE("inspector shows differing manual BPMs and keys as mixed, never one sample's value") {
        application();
        mso::ui::ApplicationSession idle_session(mso::app::ApplicationOptions{});
        mso::ui::InspectorPanel inspector(idle_session);
        auto rows = make_rows(2, "row ");
        rows[0].bpm_source = rows[1].bpm_source = "user";
        rows[0].bpm_milli = 120'000;
        rows[1].bpm_milli = 128'000;
        rows[0].key_source = rows[1].key_source = "user";
        rows[0].key = mso::MusicalKey{0, mso::KeyMode::Major};
        rows[1].key = mso::MusicalKey{9, mso::KeyMode::Minor};
        inspector.show_selection(rows, 2);
        auto* bpm_mode = child_named<QComboBox>(inspector, QStringLiteral("BPM override mode"));
        auto* key_mode = child_named<QComboBox>(inspector, QStringLiteral("Key override mode"));
        CHECK(bpm_mode->currentIndex() == -1);
        CHECK(key_mode->currentIndex() == -1);
        CHECK_FALSE(child_named<QPushButton>(inspector, QStringLiteral("Apply BPM"))->isEnabled());
        CHECK_FALSE(child_named<QPushButton>(inspector, QStringLiteral("Apply key"))->isEnabled());
        // The same manual value everywhere is a value.
        rows[1].bpm_milli = 120'000;
        inspector.show_selection({rows[0], rows[1]}, 2);
        inspector.show_selection(make_rows(1, "other "), 1);  // a new selection resets the drafts
        inspector.show_selection(rows, 2);
        CHECK(bpm_mode->currentIndex() == 2);
    }

    TEST_CASE("inspector drafts survive refreshes and are discarded with the selection") {
        application();
        mso::ui::ApplicationSession idle_session(mso::app::ApplicationOptions{});
        mso::ui::InspectorPanel inspector(idle_session);
        auto rows = make_rows(3, "row ");
        rows[0].bpm_source = "user";
        rows[0].bpm_milli = 90'000;
        rows[1].bpm_source = "analysis";
        rows[1].bpm_milli = 120'000;
        auto* bpm_mode = child_named<QComboBox>(inspector, QStringLiteral("BPM override mode"));
        auto* bpm_apply = child_named<QPushButton>(inspector, QStringLiteral("Apply BPM"));
        auto* name = child_named<QLineEdit>(inspector, QStringLiteral("Display name"));

        inspector.show_selection({rows[0]}, 1);
        CHECK(bpm_mode->currentIndex() == 2);  // a manual value
        bpm_mode->setCurrentIndex(1);          // the user picks "Unknown" and does not apply yet
        rows[0].rating = 3;
        inspector.show_selection({rows[0]}, 1);  // the same sample refreshed (not focused)
        CHECK(bpm_mode->currentIndex() == 1);

        name->setText(QStringLiteral("draft name"));
        name->setModified(true);
        inspector.show_selection({rows[1]}, 1);  // another sample: every draft goes
        CHECK(bpm_mode->currentIndex() == 0);
        CHECK(name->text() == QStringLiteral("row 1"));

        // Differing overrides: no mode is preselected, so Apply cannot write one sample's to all.
        inspector.show_selection({rows[0], rows[1]}, 2);
        CHECK(bpm_mode->currentIndex() == -1);
        CHECK_FALSE(bpm_apply->isEnabled());
        bpm_mode->setCurrentIndex(0);
        CHECK(bpm_apply->isEnabled());

        // Notes: Clear notes leaves no draft behind.
        inspector.show_selection({rows[2]}, 1);
        auto* notes = child_named<QPlainTextEdit>(inspector, QStringLiteral("Notes"));
        notes->setPlainText(QStringLiteral("typed"));
        QPushButton* save = nullptr;
        QPushButton* clear = nullptr;
        for (auto* button : inspector.findChildren<QPushButton*>()) {
            if (button->text() == QStringLiteral("Save notes")) save = button;
            if (button->text() == QStringLiteral("Clear notes")) clear = button;
        }
        REQUIRE((save && clear));
        CHECK(save->isEnabled());
        clear->click();
        CHECK(notes->toPlainText().isEmpty());
        CHECK_FALSE(save->isEnabled());

        // Nothing selected: no stars left over from the last sample.
        rows[2].rating = 4;
        inspector.show_selection({rows[2]}, 1);
        inspector.show_selection({}, 0);
        CHECK(inspector.findChild<mso::ui::StarRatingWidget*>()->rating() == 0);
    }

    TEST_CASE("inspector tag counts never substitute placeholders inside tag names") {
        application();
        mso::ui::ApplicationSession idle_session(mso::app::ApplicationOptions{});
        mso::ui::InspectorPanel inspector(idle_session);
        auto rows = make_rows(2, "row ");
        rows[0].tag_paths = {"100%2 legit"};
        inspector.show_selection(rows, 2);
        auto* tags = child_named<QListWidget>(inspector, QStringLiteral("Tags"));
        REQUIRE(tags->count() == 1);
        CHECK(tags->item(0)->text() == QStringLiteral("100%2 legit (1 of 2)"));
    }

    // ---------------------------------------------------------------- panels, menus and small helpers

    TEST_CASE("jobs panel closes the runs of a closed library and lists paused jobs") {
        application();
        mso::ui::JobsPanel jobs;
        mso::app::AnalysisProgressEvent event;
        event.library = mso::new_uuid();
        mso::catalog::Library::AnalysisRunStatus run;
        run.id = 7;
        run.profile_name = "Full";
        run.state = "running";
        run.queued = 3;
        run.paused = 2;
        event.runs = {run};
        jobs.update_analysis(event, QStringLiteral("Library"));
        auto* tree = jobs.findChild<QTreeWidget*>();
        REQUIRE(tree != nullptr);
        CHECK(tree->topLevelItem(0)->text(2).contains(QStringLiteral("2 paused")));
        tree->setCurrentItem(tree->topLevelItem(0));
        auto* cancel = child_named<QPushButton>(jobs, QStringLiteral("Cancel selected job"));
        CHECK(cancel->isEnabled());
        jobs.retain_libraries({});
        CHECK(tree->topLevelItem(0)->text(2) == QStringLiteral("Library closed"));
        CHECK_FALSE(cancel->isEnabled());
    }

    TEST_CASE("the welcome message shows its whole text at every window size") {
        application();
        mso::ui::EmptyStateWidget empty;
        empty.show_welcome();
        const QLabel* message = empty.message_label();
        REQUIRE(message != nullptr);
        CHECK(message->text().endsWith(QStringLiteral("everything works offline.")));
        empty.show();
        for (const QSize size : {QSize(1400, 860), QSize(640, 480), QSize(420, 360), QSize(300, 200)}) {
            empty.resize(size);
            REQUIRE(process_events_until([&] { return message->width() > 0; }));
            QCoreApplication::processEvents();
            INFO("window " << size.width() << "x" << size.height());
            CHECK(message->height() >= message->heightForWidth(message->width()));
        }
    }

    TEST_CASE("sample context menu disables copying when part of the selection is not loaded") {
        application();
        mso::ui::ApplicationSession idle_session(mso::app::ApplicationOptions{});
        QWidget parent;
        mso::ui::SampleMenuCommands commands;
        std::vector<std::unique_ptr<QAction>> actions;
        const auto make_action = [&actions] {
            actions.push_back(std::make_unique<QAction>(QStringLiteral("command")));
            return actions.back().get();
        };
        commands.play = make_action();
        for (auto& rating : commands.ratings)
            rating = make_action();
        commands.toggle_favorite = make_action();
        commands.remove_from_library = make_action();
        commands.analyze_waveforms = make_action();
        commands.analyze_musical = make_action();
        commands.analyze_duplicates = make_action();
        mso::ui::SampleContextMenu menu(idle_session, commands, &parent);
        const auto rows = make_rows(2, "row ");
        const auto copy_enabled = [](const QMenu& built) {
            int enabled = 0;
            for (const QAction* action : built.actions())
                if (action->text().startsWith(QStringLiteral("Copy")) && action->isEnabled()) ++enabled;
            return enabled;
        };
        CHECK(copy_enabled(*menu.build_menu(rows, 2, rows[0])) == 2);
        const auto partial = menu.build_menu(rows, 9, rows[0]);
        CHECK(copy_enabled(*partial) == 0);
        auto unavailable = rows;
        unavailable[1].availability = mso::search::RowAvailability::Missing;
        CHECK(copy_enabled(*menu.build_menu(unavailable, 2, unavailable[0])) == 0);
    }

    TEST_CASE("a command rebound to a single key moves to the sample table") {
        application();
        mso::ui::ApplicationSession idle_session(mso::app::ApplicationOptions{});
        mso::ui::MainWindow window(idle_session);
        QAction* undo = window.command(QStringLiteral("undo"));
        REQUIRE(undo != nullptr);
        CHECK(undo->shortcutContext() == Qt::WindowShortcut);
        CHECK(window.actions().contains(undo));
        auto bindings = mso::ui::ShortcutCatalog::defaults();
        bindings[QStringLiteral("undo")] = QKeySequence(Qt::Key_U);
        window.apply_shortcuts(bindings);
        CHECK(undo->shortcutContext() == Qt::WidgetWithChildrenShortcut);
        CHECK(window.sample_table()->actions().contains(undo));
        CHECK_FALSE(window.actions().contains(undo));
        CHECK(mso::ui::ShortcutCatalog::is_single_key(QKeySequence(Qt::SHIFT | Qt::Key_U)));
        CHECK_FALSE(mso::ui::ShortcutCatalog::is_single_key(QKeySequence(Qt::CTRL | Qt::Key_U)));
    }

    TEST_CASE("search errors mark a whole code point and sizes follow the locale") {
        application();
        mso::ui::SearchBar bar;
        bar.set_text(QStringLiteral("bpm:\U0001F941"));  // a drum emoji: one code point, two UTF-16 units
        const QString html = child_named<QLabel>(bar, QStringLiteral("Search error"))->text();
        const auto high_surrogate = html.indexOf(QChar(0xD83E));
        REQUIRE(high_surrogate >= 0);  // the error quotes the offending text
        CHECK(html.mid(high_surrogate, 2) == QStringLiteral("\U0001F941"));

        const QLocale previous;
        QLocale::setDefault(QLocale(QLocale::German, QLocale::Germany));
        CHECK(mso::ui::DisplayFormat::sample_rate(44'100) == QStringLiteral("44,1 kHz"));
        QLocale::setDefault(previous);
        CHECK(mso::ui::DisplayFormat::sample_rate(48'000) == QStringLiteral("48 kHz"));
    }

#ifdef _WIN32
    TEST_CASE("file manager commands quote paths for Explorer and never wait for it") {
        const std::filesystem::path file = std::filesystem::path(u"C:\\Samples, old\\kick 01.wav");
        const auto reveal = mso::ui::FileManager::reveal_command(file);
        CHECK(reveal.program == QStringLiteral("explorer.exe"));
        CHECK(reveal.native_arguments == QStringLiteral("/select,\"C:\\Samples, old\\kick 01.wav\""));
        const auto open = mso::ui::FileManager::open_folder_command(file.parent_path());
        CHECK(open.native_arguments == QStringLiteral("\"C:\\Samples, old\""));
    }
#endif

    // ---------------------------------------------------------------- one refresh at a time

    TEST_CASE("one refresh at a time: changes during a long refresh owe one more, and paging goes on") {
        application();
        FakePageSource source;
        source.results[""] = make_rows(8000, "row ");
        SampleTableModel model(source);
        model.set_query("", {});
        REQUIRE(process_events_until([&] { return model.rowCount() == SampleTableModel::kPageSize; }));
        for (int page = 1; page < 10; ++page) {
            model.fetchMore({});
            REQUIRE(process_events_until([&] { return model.rowCount() == SampleTableModel::kPageSize * (page + 1); }));
        }
        int refreshed = 0;
        QObject::connect(&model, &SampleTableModel::refreshed, [&] { ++refreshed; });

        // A refresh walks ten pages, one delivery per event-loop pass; changes arriving meanwhile
        // (an analysis run reporting results) never restart it, they owe exactly one more.
        const auto requests_before = source.page_requests.size();
        model.refresh(0);
        for (int change = 0; change < 50; ++change)
            model.refresh(0);
        CHECK(model.refresh_owed());
        CHECK(source.page_requests.size() == requests_before + 1);  // one walk started, not fifty-one
        REQUIRE(process_events_until([&] { return refreshed == 1; }));
        // Between the commit and the owed refresh, the table pages again.
        CHECK(model.refresh_owed());
        REQUIRE(model.canFetchMore({}));
        model.fetchMore({});
        REQUIRE(process_events_until([&] { return model.rowCount() == 11 * SampleTableModel::kPageSize; }));
        REQUIRE(process_events_until([&] { return refreshed == 2; }));
        CHECK_FALSE(model.refresh_owed());

        // A source that reports a change with every page it delivers (a long analysis run) while
        // the view keeps scrolling to the end: every refresh still commits, and paging reaches the
        // end of the result (before, each change restarted the walk and paging waited forever).
        source.after_page = [&] { model.refresh(std::max(0, model.rowCount() - 1)); };
        REQUIRE(process_events_until(
            [&] {
                if (model.canFetchMore({})) model.fetchMore({});
                return model.rowCount() == 8000;
            },
            60000));
        source.after_page = nullptr;
        REQUIRE(process_events_until([&] { return !model.is_refreshing() && !model.refresh_owed(); }));
        CHECK(refreshed >= 3);
        CHECK(model.rowCount() == 8000);
        CHECK(model.key_at(7999) == key_of(source.results[""][7999]));
        CHECK_FALSE(model.canFetchMore({}));
    }

    TEST_CASE("changes to evicted rows are known rows: no window re-read, no row read") {
        application();
        FakePageSource source;
        source.results[""] = make_rows(SampleTableModel::kPageSize * 12, "row ");
        SampleTableModel model(source);
        model.set_query("", {});
        REQUIRE(process_events_until([&] { return model.rowCount() == SampleTableModel::kPageSize; }));
        for (int page = 1; page < 12; ++page) {
            model.fetchMore({});
            REQUIRE(process_events_until([&] { return model.rowCount() == SampleTableModel::kPageSize * (page + 1); }));
        }
        const int evicted_row = SampleTableModel::kPageSize;  // page 1 was evicted
        REQUIRE(model.loaded_row(evicted_row) == nullptr);
        const SampleKey evicted = *model.key_at(evicted_row);
        CHECK(evicted == key_of(source.results[""][static_cast<std::size_t>(evicted_row)]));
        const SampleKey loaded = key_of(*model.loaded_row(model.rowCount() - 1));
        const SampleKey unknown{evicted.library, 999'999};
        const int rows_requests = source.rows_requests;
        CHECK(model.refresh_rows({evicted}) == 1);  // known, but nothing in memory to re-read
        CHECK(source.rows_requests == rows_requests);
        CHECK(model.refresh_rows({evicted, loaded, unknown}) == 2);
        CHECK(source.rows_requests == rows_requests + 1);
    }

    TEST_CASE("row reads that arrive during or after a refresh are never lost to its older pages") {
        application();
        FakePageSource source;
        auto& rows = source.results[""];
        rows = make_rows(3 * SampleTableModel::kPageSize, "row ");
        SampleTableModel model(source);
        model.set_query("", {});
        REQUIRE(process_events_until([&] { return model.rowCount() == SampleTableModel::kPageSize; }));
        for (int page = 1; page < 3; ++page) {
            model.fetchMore({});
            REQUIRE(process_events_until([&] { return model.rowCount() == SampleTableModel::kPageSize * (page + 1); }));
        }
        int refreshed = 0;
        QObject::connect(&model, &SampleTableModel::refreshed, [&] { ++refreshed; });
        const int row = SampleTableModel::kPageSize + 1;  // on the walk's second page
        const SampleKey key = key_of(rows[static_cast<std::size_t>(row)]);

        // During the walk: its page with the sample is read, then the sample changes and its read
        // (this generation) arrives before the walk commits; the commit installs the older page.
        source.deliveries.hold = true;
        model.refresh(0);
        source.deliveries.release();  // page 1 arrives; page 2 is requested
        source.deliveries.release();  // page 2 (the old value) arrives; page 3 is requested
        rows[static_cast<std::size_t>(row)].rating = 4;
        CHECK(model.refresh_rows({key}) == 1);  // the analysis result's row read
        source.deliveries.release_newest();     // the read arrives first …
        CHECK(model.loaded_row(row)->rating == 4);
        source.deliveries.release();  // … then page 3: the walk commits with page 2's older value
        REQUIRE(refreshed == 1);
        source.deliveries.hold = false;
        source.deliveries.release();  // the read owed after the commit
        REQUIRE(process_events_until([&] { return model.loaded_row(row)->rating == 4; }));

        // After the walk: a read requested before the refresh arrives once the generation moved;
        // it is read again instead of being dropped.
        source.deliveries.hold = true;
        rows[static_cast<std::size_t>(row)].rating = 2;
        const int rows_requests = source.rows_requests;
        CHECK(model.refresh_rows({key}) == 1);
        model.refresh(0);
        source.deliveries.release_newest();  // the walk's first page arrives before the read
        source.deliveries.hold = false;
        source.deliveries.release();
        REQUIRE(process_events_until([&] { return refreshed == 2 && model.loaded_row(row)->rating == 2; }));
        CHECK(source.rows_requests == rows_requests + 2);
    }

    TEST_CASE("a refresh keeps the selection of rows pushed past the old end or pulled up from it") {
        application();
        FakePageSource source;
        auto& rows = source.results[""];
        rows = make_rows(5, "row ");
        SampleTableModel model(source);
        model.set_query("", {});
        REQUIRE(process_events_until([&] { return model.rowCount() == 5; }));
        std::vector<QAbstractItemModel::LayoutChangeHint> hints;
        QObject::connect(&model, &QAbstractItemModel::layoutAboutToBeChanged,
                         [&](const QList<QPersistentModelIndex>&, QAbstractItemModel::LayoutChangeHint hint) {
                             hints.push_back(hint);
                         });
        int refreshed = 0;
        QObject::connect(&model, &SampleTableModel::refreshed, [&] { ++refreshed; });

        // Three imported rows sort first: the selected last row moves to row 7, past the old end.
        const QPersistentModelIndex last = model.index(4, 3);
        const SampleKey last_key = key_of(rows[4]);
        auto added = make_rows(3, "added ", rows.front().library);
        for (std::size_t index = 0; index < added.size(); ++index)
            added[index].sample_id = 100 + static_cast<std::int64_t>(index);
        rows.insert(rows.begin(), added.begin(), added.end());
        model.refresh(0);
        REQUIRE(process_events_until([&] { return refreshed == 1; }));
        CHECK(model.rowCount() == 8);
        REQUIRE(last.isValid());
        CHECK(last.row() == 7);
        CHECK(last.column() == 3);
        CHECK(*model.key_at(last.row()) == last_key);
        REQUIRE_FALSE(hints.empty());
        CHECK(hints.back() == QAbstractItemModel::VerticalSortHint);

        // They are removed again: the row moves back up before the tail rows go.
        rows.erase(rows.begin(), rows.begin() + 3);
        model.refresh(0);
        REQUIRE(process_events_until([&] { return refreshed == 2; }));
        CHECK(model.rowCount() == 5);
        REQUIRE(last.isValid());
        CHECK(last.row() == 4);
        CHECK(*model.key_at(4) == last_key);
    }

    TEST_CASE("while a refresh waits to walk again, evicted rows are not re-read with stale cursors") {
        application();
        FakePageSource source;
        source.results[""] = make_rows(SampleTableModel::kPageSize * 12, "row ");
        SampleTableModel model(source);
        model.set_query("", {});
        REQUIRE(process_events_until([&] { return model.rowCount() == SampleTableModel::kPageSize; }));
        for (int page = 1; page < 12; ++page) {
            model.fetchMore({});
            REQUIRE(process_events_until([&] { return model.rowCount() == SampleTableModel::kPageSize * (page + 1); }));
        }
        int refreshed = 0;
        QObject::connect(&model, &SampleTableModel::refreshed, [&] { ++refreshed; });
        source.deliveries.hold = true;
        model.refresh(0);
        source.deliveries.release();    // the first walk page arrives; the second is requested …
        ++source.structure_generation;  // … and is stale when it runs: the walk backs off
        source.deliveries.release();
        REQUIRE(source.stale_failures == 1);
        REQUIRE(model.is_refreshing());  // still running during the back-off
        const auto requests = source.page_requests.size();
        static_cast<void>(name_at(model, SampleTableModel::kPageSize));  // an evicted row is shown
        QCoreApplication::processEvents();
        CHECK(source.page_requests.size() == requests);  // no re-read with its stale cursor
        CHECK_FALSE(model.canFetchMore({}));
        source.deliveries.hold = false;
        REQUIRE(process_events_until([&] { return refreshed == 1; }));
        CHECK(source.stale_failures == 1);
    }

    TEST_CASE("an edit of evicted rows shows its prediction when their page is read again") {
        application();
        FakePageSource source;
        source.results[""] = make_rows(SampleTableModel::kPageSize * 12, "row ");
        SampleTableModel model(source);
        model.set_query("", {});
        REQUIRE(process_events_until([&] { return model.rowCount() == SampleTableModel::kPageSize; }));
        for (int page = 1; page < 12; ++page) {
            model.fetchMore({});
            REQUIRE(process_events_until([&] { return model.rowCount() == SampleTableModel::kPageSize * (page + 1); }));
        }
        const int evicted_row = SampleTableModel::kPageSize + 3;
        REQUIRE(model.loaded_row(evicted_row) == nullptr);
        const SampleKey key = *model.key_at(evicted_row);
        const SampleTableModel::RowPatch rate = [](SampleRow& row) { row.rating = 5; };
        CHECK(model.patch_rows({key}, rate).empty());  // nothing loaded to patch now
        model.begin_pending_edit({key}, rate);
        static_cast<void>(name_at(model, evicted_row));  // the view shows the row: its page is read
        REQUIRE(process_events_until([&] { return model.loaded_row(evicted_row) != nullptr; }));
        CHECK(model.loaded_row(evicted_row)->rating == 5);  // the catalog still says 0
        model.end_pending_edit({key});
        CHECK_FALSE(model.has_pending_edit(key));
    }

    TEST_CASE("a failed edit is undone on screen even when the reconciling read fails too") {
        application();
        FakePageSource source;
        source.results[""] = make_rows(3, "row ");
        SampleTableModel model(source);
        model.set_query("", {});
        REQUIRE(process_events_until([&] { return model.rowCount() == 3; }));
        FakeEditGateway gateway;
        gateway.succeed = false;
        source.rows_error = mso::fail(mso::ErrorCode::DbBusy, "the library is busy").error();
        mso::ui::EditController controller(gateway, model);
        std::optional<QString> failure;
        QObject::connect(&controller, &mso::ui::EditController::failed,
                         [&](const QString&, const QString& details) { failure = details; });
        mso::catalog::EditCommand command;
        command.rating = 4;
        controller.apply({key_of(source.results[""][0])}, command, QStringLiteral("Rated 4"));
        CHECK(model.loaded_row(0)->rating == 4);  // the prediction, at once
        REQUIRE(process_events_until([&] { return failure.has_value(); }));
        CHECK(failure->contains(QStringLiteral("undone on screen")));
        CHECK(model.loaded_row(0)->rating == 0);  // and it really was
    }

    TEST_CASE("the selection's keys include rows whose data was evicted") {
        application();
        FakePageSource source;
        source.results[""] = make_rows(SampleTableModel::kPageSize * 12, "row ");
        SampleTableModel model(source);
        mso::ui::SampleTableView view;
        view.set_sample_model(&model);
        model.set_query("", {});
        REQUIRE(process_events_until([&] { return model.rowCount() == SampleTableModel::kPageSize; }));
        for (int page = 1; page < 12; ++page) {
            model.fetchMore({});
            REQUIRE(process_events_until([&] { return model.rowCount() == SampleTableModel::kPageSize * (page + 1); }));
        }
        view.selectAll();
        const std::size_t selected = view.selected_row_count();
        CHECK(selected == static_cast<std::size_t>(SampleTableModel::kPageSize * 12));
        CHECK(view.selected_samples().size() < selected);  // only about 2,000 rows keep their data
        const std::vector<SampleKey> keys = view.selected_keys();
        REQUIRE(keys.size() == selected);
        CHECK(keys.front() == key_of(source.results[""].front()));
        CHECK(keys.back() == key_of(source.results[""].back()));
        view.setCurrentIndex(model.index(SampleTableModel::kPageSize, 0));  // an evicted row
        CHECK(view.current_key() == keys[SampleTableModel::kPageSize]);
    }

    // ---------------------------------------------------------------- library upgrades on open

    TEST_CASE("jobs panel shows a library upgrade step by step and ends it when the library is open") {
        application();
        mso::ui::JobsPanel jobs;
        auto* tree = jobs.findChild<QTreeWidget*>();
        REQUIRE(tree != nullptr);
        const auto library = mso::new_uuid();
        mso::app::LibraryOpenProgressEvent event;
        event.library = library;
        event.directory = std::filesystem::path("C:/Libraries/Drums.msolib");
        event.step = mso::catalog::OpenStep::BackingUp;
        event.completed = 0;
        event.total = 1;
        jobs.update_library_open(event);
        REQUIRE(jobs.row_count() == 1);
        QTreeWidgetItem* row = tree->topLevelItem(0);
        CHECK(row->text(0).contains(QStringLiteral("Upgrading library Drums.msolib")));
        CHECK(row->text(2).startsWith(QStringLiteral("Backing up")));
        tree->setCurrentItem(row);
        CHECK_FALSE(child_named<QPushButton>(jobs, QStringLiteral("Cancel selected job"))->isEnabled());

        event.step = mso::catalog::OpenStep::RebuildingKeys;
        event.completed = 2;
        event.total = 5;
        jobs.update_library_open(event);
        CHECK(jobs.row_count() == 1);  // one row per library
        CHECK(row->text(1).contains(QStringLiteral("40")));
        CHECK(row->text(2).startsWith(QStringLiteral("Rebuilding name and path keys")));
        jobs.finish_completed_library_opens({library});  // mid-step: not done yet
        CHECK_FALSE(row->text(2).startsWith(QStringLiteral("Done")));
        event.completed = 5;
        jobs.update_library_open(event);
        jobs.finish_completed_library_opens({});  // the step ended but the library is not open yet
        CHECK_FALSE(row->text(2).startsWith(QStringLiteral("Done")));
        jobs.finish_completed_library_opens({library});
        CHECK(row->text(2).startsWith(QStringLiteral("Done")));

        // An open that fails ends its row as failed.
        mso::app::LibraryOpenProgressEvent other = event;
        other.library = mso::new_uuid();
        other.directory = std::filesystem::path("C:/Libraries/Loops.msolib");
        other.completed = 1;
        jobs.update_library_open(other);
        jobs.fail_unopened_library_opens({library}, QStringLiteral("the disk is full"));
        CHECK(tree->topLevelItem(1)->text(2) == QStringLiteral("Failed: the disk is full"));
    }

    TEST_CASE("names that collided in a library upgrade are reported once, with the backup") {
        application();
        mso::app::OpenLibraryInfo clean;
        clean.uuid = mso::new_uuid();
        clean.name = "Clean";
        mso::app::OpenLibraryInfo collided;
        collided.uuid = mso::new_uuid();
        collided.name = "Drums";
        collided.key_conflicts = 3;
        collided.pre_rekey_backup = std::filesystem::path("C:/Libraries/Drums/backups/pre-rekey.sqlite");
        std::set<uuids::uuid> reported;
        const auto notice = mso::ui::MainWindow::key_conflict_notice({clean, collided}, reported);
        REQUIRE(notice.has_value());
        CHECK(notice->first.contains(QStringLiteral("collided")));
        CHECK(notice->second.contains(QStringLiteral("Drums: 3 name(s) or path(s)")));
        CHECK(notice->second.contains(QStringLiteral("pre-rekey.sqlite")));
        CHECK_FALSE(notice->second.contains(QStringLiteral("Clean")));
        CHECK_FALSE(mso::ui::MainWindow::key_conflict_notice({clean, collided}, reported).has_value());
    }

    // ---------------------------------------------------------------- small helpers

    TEST_CASE("parent cycles of a damaged catalog are found, chains into them are not") {
        using ParentMap = std::map<std::int64_t, std::optional<std::int64_t>>;
        CHECK(mso::ui::detail::nodes_on_parent_cycles(ParentMap{{1, std::nullopt}, {2, 1}, {3, 2}}).empty());
        CHECK(mso::ui::detail::nodes_on_parent_cycles(ParentMap{{1, 2}, {2, 1}, {3, 1}}) ==
              std::set<std::int64_t>{1, 2});
        CHECK(mso::ui::detail::nodes_on_parent_cycles(ParentMap{{7, 7}}) == std::set<std::int64_t>{7});
        CHECK(mso::ui::detail::nodes_on_parent_cycles(ParentMap{{1, 99}, {2, 3}, {3, 4}, {4, 2}, {5, 4}}) ==
              std::set<std::int64_t>{2, 3, 4});
    }

    TEST_CASE("tool tips show user text literally, run states and provenance are translated whole") {
        application();
        const QString tip = mso::ui::DisplayFormat::plain_tooltip(QStringLiteral("<b>kick</b> & snare\nloop"));
        CHECK(tip.contains(QStringLiteral("&lt;b&gt;kick&lt;/b&gt; &amp; snare<br>loop")));
        CHECK(mso::ui::DisplayFormat::plain_tooltip({}).isEmpty());
        CHECK(mso::ui::DisplayFormat::run_state("completed_with_errors") == QStringLiteral("completed with errors"));
        CHECK(mso::ui::DisplayFormat::run_state("new_state") == QStringLiteral("new_state"));
        CHECK(mso::ui::DisplayFormat::provenance("user", 0.8).contains(QStringLiteral("80%")));
    }

    // ---------------------------------------------------------------- drag path resolver

    TEST_CASE("drag resolver: debounced check, partition of available paths, size limit") {
        application();
        std::atomic<int> lookups{0};
        mso::ui::DragPathResolver resolver([&lookups](const SampleKey& key) -> mso::Result<std::filesystem::path> {
            ++lookups;
            if (key.sample_id % 2 == 0) return mso::fail(mso::ErrorCode::FileMissing, "gone");
            return std::filesystem::path("C:/Samples") / ("sample " + std::to_string(key.sample_id) + ".wav");
        });
        int resolved = 0;
        QObject::connect(&resolver, &mso::ui::DragPathResolver::resolved, [&] { ++resolved; });
        const auto library = mso::new_uuid();
        const std::vector<SampleKey> keys{{library, 1}, {library, 2}, {library, 3}};
        resolver.resolve(keys);
        CHECK_FALSE(resolver.is_ready_for(keys));  // debounced: nothing runs at once
        CHECK(lookups == 0);
        REQUIRE(process_events_until([&] { return resolved == 1; }));
        CHECK(resolver.is_ready_for(keys));
        CHECK(resolver.available_keys() == std::vector<SampleKey>{keys[0], keys[2]});
        CHECK(resolver.unavailable_keys() == std::vector<SampleKey>{keys[1]});
        CHECK(resolver.available_paths().size() == 2);
        CHECK(resolver.path_of(keys[2]) == std::filesystem::path("C:/Samples/sample 3.wav"));
        CHECK_FALSE(resolver.path_of(keys[1]).has_value());
        CHECK_FALSE(resolver.path_of(SampleKey{library, 42}).has_value());

        // The same selection again (a row refresh) is not checked again.
        resolver.resolve(keys);
        CHECK(resolver.is_ready_for(keys));
        QCoreApplication::processEvents();
        CHECK(lookups == 3);

        // A selection over the limit is never checked (the drag is refused with a reason).
        std::vector<SampleKey> many;
        for (std::size_t index = 0; index <= mso::ui::DragPathResolver::kMaxDragSamples; ++index)
            many.push_back({library, static_cast<std::int64_t>(index) + 10});
        resolver.resolve(many);
        CHECK_FALSE(resolver.is_ready_for(many));
        CHECK_FALSE(resolver.is_ready_for(keys));
        CHECK(resolver.available_keys().empty());
    }

    TEST_CASE("drag resolver: a newer selection stops the running check between two files") {
        application();
        Gate first_lookup_entered;
        Gate release_first_lookup;
        std::atomic<int> lookups{0};
        const auto library = mso::new_uuid();
        mso::ui::DragPathResolver resolver([&](const SampleKey& key) -> mso::Result<std::filesystem::path> {
            if (++lookups == 1) {
                first_lookup_entered.open();
                release_first_lookup.wait();
            }
            return std::filesystem::path("C:/Samples") / std::to_string(key.sample_id);
        });
        int resolved = 0;
        QObject::connect(&resolver, &mso::ui::DragPathResolver::resolved, [&] { ++resolved; });
        const std::vector<SampleKey> first{{library, 1}, {library, 2}, {library, 3}, {library, 4}};
        const std::vector<SampleKey> second{{library, 9}};
        resolver.resolve(first);
        REQUIRE(process_events_until([&] { return first_lookup_entered.is_open(); }));
        resolver.resolve(second);  // the first check is inside its first lookup now
        release_first_lookup.open();
        REQUIRE(process_events_until([&] { return resolved == 1; }));
        CHECK(resolver.is_ready_for(second));
        CHECK_FALSE(resolver.is_ready_for(first));
        CHECK(lookups == 2);  // the first check stopped after its first file; the second ran once
        CHECK(resolver.available_keys() == second);
        QCoreApplication::processEvents();
        CHECK(resolved == 1);  // the superseded check never announced anything
    }

    // ---------------------------------------------------------------- the real application

    TEST_CASE("end to end: search, navigator, preview, drag check, optimistic rating and main window") {
        application();
        mso::test::ScratchDirectory scratch("ui");
        const std::filesystem::path samples = scratch / "samples";
        mso::test::write_sine_wav(samples / "kick 01.wav", 0.1);
        mso::test::write_sine_wav(samples / "snare 01.wav", 0.1);
        const QString hebrew_name = QStringLiteral("\u05D1\u05E2\u05D9\u05D8\u05D4 kick");  // "kick" in Hebrew
        mso::test::write_sine_wav(samples / mso::ui::DisplayFormat::to_path(hebrew_name + QStringLiteral(".wav")), 0.1);
        mso::test::write_sine_wav(samples / "drums" / "kicks" / "deep kick.wav", 0.1);

        mso::app::ApplicationOptions options;
        options.workspace_directory = scratch / "workspace";
        options.reopen_libraries = false;
        options.monitor_roots = false;
        options.run_analysis_supervisor = false;
        mso::ui::ApplicationSession session(options);
        bool ready = false;
        QObject::connect(&session, &mso::ui::ApplicationSession::ready, [&] { ready = true; });
        session.start();
        REQUIRE(process_events_until([&] { return ready; }, 30000));

        std::optional<mso::Result<mso::files::ImportSummary>> imported;
        session.run(
            &session,
            [library_path = scratch / "library",
             samples](mso::app::Application& app) -> mso::Result<mso::files::ImportSummary> {
                auto library = app.create_library(library_path, "UI test");
                if (!library) return std::unexpected(library.error());
                auto root = app.add_root(library->uuid, samples, "samples");
                if (!root) return std::unexpected(root.error());
                return app.import_now(library->uuid, root->id);
            },
            [&](mso::Result<mso::files::ImportSummary> summary) { imported = std::move(summary); });
        REQUIRE(process_events_until([&] { return imported.has_value(); }, 30000));
        if (!imported->has_value()) FAIL(mso::ui::DisplayFormat::error(imported->error()).toStdString());

        // The import reaches the in-memory index through the change feed shortly after import_now()
        // returns (nothing here listens for the change events): query again until it has caught up.
        SampleTableModel model(session);
        model.set_query("", {});
        REQUIRE(process_events_until(
            [&] {
                if (model.is_loading() || model.rowCount() == 4) return model.rowCount() == 4 && !model.is_loading();
                model.run_query("", {});
                return false;
            },
            30000));
        QStringList names;
        for (int row = 0; row < 4; ++row)
            names << name_at(model, row);
        CHECK(names.contains(hebrew_name));

        // The navigator holds every root's folder tree: nested folders exist before any expansion.
        {
            mso::ui::NavigatorPanel navigator(session);
            session.refresh_libraries();
            navigator.reload();
            auto* tree = navigator.findChild<QTreeWidget*>();
            REQUIRE(tree != nullptr);
            const auto find_item = [tree](const QString& text) -> QTreeWidgetItem* {
                const auto found = tree->findItems(text, Qt::MatchExactly | Qt::MatchRecursive);
                return found.isEmpty() ? nullptr : found.front();
            };
            REQUIRE(process_events_until([&] { return find_item(QStringLiteral("kicks")) != nullptr; }, 10000));
            CHECK(find_item(QStringLiteral("drums")) != nullptr);
            CHECK(tree->findItems(QStringLiteral("Loading…"), Qt::MatchExactly | Qt::MatchRecursive).isEmpty());
        }

        const SampleRow first = *model.loaded_row(0);

        // The drag check resolves the selection off the UI thread.
        {
            mso::ui::DragPathResolver resolver(session);
            int resolved = 0;
            QObject::connect(&resolver, &mso::ui::DragPathResolver::resolved, [&] { ++resolved; });
            resolver.resolve({key_of(first)});
            REQUIRE(process_events_until([&] { return resolved == 1; }, 10000));
            CHECK(resolver.is_ready_for({key_of(first)}));
            CHECK(resolver.available_keys().size() == 1);
            CHECK(resolver.path_of(key_of(first)).has_value());
        }

        // Previewing the selected sample opens it and builds its waveform lazily (and only its own).
        {
            mso::ui::PreviewPanel preview(session);
            preview.select_sample(key_of(first), name_at(model, 0));
            auto* waveform = preview.findChild<mso::ui::WaveformView*>();
            REQUIRE(waveform != nullptr);
            CHECK(waveform->state() == mso::ui::WaveformView::State::NotGenerated);
            preview.play_selected();
            REQUIRE(
                process_events_until([&] { return waveform->state() == mso::ui::WaveformView::State::Ready; }, 15000));
            preview.stop();
        }

        // Optimistic rating through the real Application: visible at once, then stored and indexed.
        {
            mso::ui::EditController controller(session, model);
            int successes = 0;
            QObject::connect(&controller, &mso::ui::EditController::succeeded, [&] { ++successes; });
            mso::catalog::EditCommand command;
            command.rating = 5;
            controller.apply({key_of(first)}, command, QStringLiteral("Rated 5"));
            CHECK(model.loaded_row(0)->rating == 5);
            REQUIRE(process_events_until([&] { return successes == 1; }, 10000));
            std::optional<mso::Result<std::vector<SampleRow>>> stored;
            session.request_rows({key_of(first)}, &session,
                                 [&](mso::Result<std::vector<SampleRow>> rows) { stored = std::move(rows); });
            REQUIRE(process_events_until([&] { return stored.has_value(); }, 10000));
            REQUIRE(stored->has_value());
            REQUIRE((*stored)->size() == 1);
            CHECK((*stored)->front().rating == 5);
        }

        // The main window starts on this session, shows the results and scopes single-key shortcuts.
        {
            mso::ui::MainWindow window(session);
            window.restore_saved_state();
            window.show();
            session.refresh_libraries();
            auto* table = window.sample_table();
            REQUIRE(process_events_until([&] { return table->model()->rowCount() == 4; }, 10000));
            bool delete_on_table = false;
            for (QAction* action : table->actions())
                delete_on_table = delete_on_table || (action->shortcut() == QKeySequence(QKeySequence::Delete) &&
                                                      action->shortcutContext() == Qt::WidgetWithChildrenShortcut);
            CHECK(delete_on_table);
            bool undo_on_window = false;
            for (QAction* action : window.actions())
                undo_on_window = undo_on_window || (action->shortcut() == QKeySequence(QKeySequence::Undo) &&
                                                    action->shortcutContext() == Qt::WindowShortcut);
            CHECK(undo_on_window);
            CHECK(window.notification_bar()->message().isEmpty());
            window.close();
        }

        model.set_query("kick", {});
        REQUIRE(process_events_until([&] { return model.rowCount() == 3 && !model.is_loading(); }, 10000));
        std::optional<QString> failure;
        QObject::connect(&model, &SampleTableModel::queryFailed, [&](const QString& message) { failure = message; });
        model.set_query("bpm:abc", {});
        REQUIRE(process_events_until([&] { return failure.has_value(); }, 10000));
    }
}
