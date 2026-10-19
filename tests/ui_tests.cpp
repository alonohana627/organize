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
#include <QMainWindow>
#include <QMouseEvent>
#include <QPushButton>
#include <QRadioButton>
#include <QStyleOptionViewItem>
#include <QTableView>
#include <QThreadPool>
#include <QTimer>
#include <QTreeWidget>

#include <algorithm>
#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

#include "mso/domain/analysis_flags.hpp"
#include "mso/generated/dependencies.hpp"
#include "mso/ui/about_dialog.hpp"
#include "mso/ui/add_folder_dialog.hpp"
#include "mso/ui/application_session.hpp"
#include "mso/ui/background_tasks.hpp"
#include "mso/ui/display_format.hpp"
#include "mso/ui/drag_path_resolver.hpp"
#include "mso/ui/edit_controller.hpp"
#include "mso/ui/edit_gateway.hpp"
#include "mso/ui/empty_state_widget.hpp"
#include "mso/ui/file_manager.hpp"
#include "mso/ui/history_panel.hpp"
#include "mso/ui/inspector_panel.hpp"
#include "mso/ui/jobs_panel.hpp"
#include "mso/ui/layout_preset_store.hpp"
#include "mso/ui/layout_store.hpp"
#include "mso/ui/main_window.hpp"
#include "mso/ui/navigator_panel.hpp"
#include "mso/ui/notification_bar.hpp"
#include "mso/ui/optimistic_edit.hpp"
#include "mso/ui/preferences_dialog.hpp"
#include "mso/ui/preview_panel.hpp"
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

/// One QApplication for the whole test run, on the offscreen platform with silent audio.
QApplication& application() {
    static int argc = 1;
    static char name[] = "mso_ui_tests";
    static char* argv[] = {name, nullptr};
    static std::unique_ptr<QApplication> instance = [] {
        qputenv("QT_QPA_PLATFORM", "offscreen");
        qputenv("MSO_AUDIO_BACKEND", "null");
        return std::make_unique<QApplication>(argc, argv);
    }();
    return *instance;
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
    std::size_t held_count() const { return held_.size(); }

private:
    std::vector<std::function<void()>> held_;
};

/// In-memory search backend: each query text maps to a sorted row list; cursors are offsets.
class FakePageSource final : public mso::ui::SamplePageSource {
public:
    std::map<std::string, std::vector<SampleRow>> results;
    std::vector<mso::search::SearchRequest> page_requests;
    int count_requests = 0;
    int rows_requests = 0;
    DeliveryQueue deliveries;

    void request_page(const mso::search::SearchRequest& request, std::optional<uuids::uuid>, QObject* context,
                      PageDelivery deliver) override {
        page_requests.push_back(request);
        const auto& rows = results[request.text];
        const std::size_t offset = request.cursor ? std::stoul(request.cursor->token) : 0;
        mso::search::SearchPage page;
        for (std::size_t index = offset; index < rows.size() && page.rows.size() < request.page_size; ++index)
            page.rows.push_back(rows[index]);
        if (offset + page.rows.size() < rows.size())
            page.next = mso::search::Cursor{std::to_string(offset + page.rows.size())};
        deliveries.schedule(context, [deliver, page] { deliver(page); });
    }

    void request_count(const mso::search::SearchRequest& request, std::optional<uuids::uuid>, QObject* context,
                       CountDelivery deliver) override {
        ++count_requests;
        mso::search::CountResult count;
        count.total = static_cast<std::int64_t>(results[request.text].size());
        deliveries.schedule(context, [deliver, count] { deliver(count); });
    }

    void request_rows(const std::vector<SampleKey>& keys, QObject* context, RowsDelivery deliver) override {
        ++rows_requests;
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
        CHECK(source.page_requests.back().cursor->token == "400");
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
        CHECK(row.tags == "Drums/Kick, Punchy");
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

        SampleTableModel model(session);
        model.set_query("", {});
        REQUIRE(process_events_until([&] { return model.rowCount() == 4; }, 10000));
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
            std::optional<std::vector<SampleRow>> stored;
            session.request_rows({key_of(first)}, &session,
                                 [&](std::vector<SampleRow> rows) { stored = std::move(rows); });
            REQUIRE(process_events_until([&] { return stored.has_value(); }, 10000));
            REQUIRE(stored->size() == 1);
            CHECK(stored->front().rating == 5);
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
