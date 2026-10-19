// Hand-run measurement of the sample table's refresh commit (TEST_SUITE "ui", skipped by default:
// timings are not assertions). Run it on a release build:
//   mso_ui_tests --no-skip -tc="benchmark*"
// It selects 20,000 loaded rows (a Shift-selection over a deep view), then times the delivery that
// commits an anchored refresh: the layout change that moves every persistent index with its sample.
#include <doctest.h>

#include <QApplication>
#include <QElapsedTimer>
#include <QItemSelectionModel>
#include <QTableView>
#include <QTimer>

#include <algorithm>
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <stop_token>
#include <string>
#include <vector>

#include "mso/ui/sample_page_source.hpp"
#include "mso/ui/sample_table_model.hpp"

namespace {

using mso::search::SampleRow;
using mso::ui::SampleKey;
using mso::ui::SampleTableModel;

/// The test run's QApplication (shared with the other ui_*.cpp files).
QApplication& benchmark_application() {
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

/// Rows in memory, offset cursors; every delivery is timed and the longest one is kept (the
/// delivery that commits a refresh does the layout change).
class TimedPageSource final : public mso::ui::SamplePageSource {
public:
    std::vector<SampleRow> rows;
    qint64 longest_delivery_nanoseconds = 0;

    void request_page(const mso::search::SearchRequest& request, std::optional<uuids::uuid>, std::stop_token,
                      QObject* context, PageDelivery deliver) override {
        QTimer::singleShot(0, context, [this, request, deliver] {
            const std::size_t offset = request.cursor ? std::stoul(request.cursor->token) : 0;
            mso::search::SearchPage page;
            for (std::size_t index = offset; index < rows.size() && page.rows.size() < request.page_size; ++index)
                page.rows.push_back(rows[index]);
            if (offset + page.rows.size() < rows.size())
                page.next = mso::search::Cursor{std::to_string(offset + page.rows.size())};
            QElapsedTimer timer;
            timer.start();
            deliver(std::move(page));
            longest_delivery_nanoseconds = std::max(longest_delivery_nanoseconds, timer.nsecsElapsed());
        });
    }
    void request_count(const mso::search::SearchRequest&, std::optional<uuids::uuid>, std::stop_token, QObject* context,
                       CountDelivery deliver) override {
        mso::search::CountResult count;
        count.total = static_cast<std::int64_t>(rows.size());
        QTimer::singleShot(0, context, [deliver, count] { deliver(count); });
    }
    void request_rows(const std::vector<SampleKey>&, QObject*, RowsDelivery) override {}
};

bool pump_until(const std::function<bool()>& done, int timeout_milliseconds = 60000) {
    QElapsedTimer timer;
    timer.start();
    while (!done() && timer.elapsed() < timeout_milliseconds)
        QCoreApplication::processEvents(QEventLoop::AllEvents, 10);
    return done();
}

}  // namespace

TEST_SUITE("ui") {
    TEST_CASE("benchmark: refresh commit with 20,000 selected rows" * doctest::skip()) {
        benchmark_application();
        TimedPageSource source;
        const auto library = mso::new_uuid();
        for (int index = 0; index < 30'000; ++index) {
            SampleRow row;
            row.library = library;
            row.sample_id = index + 1;
            row.display_name = "row " + std::to_string(index);
            row.availability = mso::search::RowAvailability::Available;
            source.rows.push_back(row);
        }
        SampleTableModel model(source);
        QTableView view;
        view.setSelectionBehavior(QAbstractItemView::SelectRows);
        view.setModel(&model);
        model.set_query("", {});
        REQUIRE(pump_until([&] {
            if (model.canFetchMore({})) model.fetchMore({});
            return model.rowCount() >= 20'000;
        }));
        view.selectionModel()->select(QItemSelection(model.index(0, 0), model.index(19'999, 0)),
                                      QItemSelectionModel::ClearAndSelect | QItemSelectionModel::Rows);
        view.selectionModel()->setCurrentIndex(model.index(19'999, 0), QItemSelectionModel::NoUpdate);
        int refreshed = 0;
        QObject::connect(&model, &SampleTableModel::refreshed, [&] { ++refreshed; });
        std::vector<double> milliseconds;
        for (int run = 0; run < 7; ++run) {
            source.longest_delivery_nanoseconds = 0;
            model.refresh(0);
            REQUIRE(pump_until([&] { return refreshed == run + 1; }));
            milliseconds.push_back(static_cast<double>(source.longest_delivery_nanoseconds) / 1e6);
            REQUIRE(pump_until([&] { return !model.is_loading(); }));
        }
        std::ranges::sort(milliseconds);
        MESSAGE("refresh commit with 20,000 selected of ", model.rowCount(), " rows: median ", milliseconds[3],
                " ms, fastest ", milliseconds.front(), " ms, slowest ", milliseconds.back(), " ms");
        CHECK(view.selectionModel()->selectedRows().size() == 20'000);
    }
}
