// UI model tests (TEST_SUITE "ui"). They run headless on Qt's "offscreen" platform and need no
// display or audio device. tests/CMakeLists.txt also globs this file into the Qt-free mso_tests,
// where MSO_UI_TESTS is undefined and the file contributes nothing.
#include <doctest.h>

#ifdef MSO_UI_TESTS

#include <QApplication>
#include <QDir>
#include <QElapsedTimer>
#include <QFile>
#include <QHeaderView>
#include <QImage>
#include <QKeyEvent>
#include <QMouseEvent>
#include <QStyleOptionViewItem>
#include <QTemporaryDir>
#include <QTimer>
#include <QTreeWidget>

#include <algorithm>
#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "mso/ui/application_session.hpp"
#include "mso/ui/display_format.hpp"
#include "mso/ui/navigator_panel.hpp"
#include "mso/ui/preview_panel.hpp"
#include "mso/ui/sample_row_delegate.hpp"
#include "mso/ui/sample_table_model.hpp"
#include "mso/ui/sample_table_view.hpp"
#include "mso/ui/shortcut_catalog.hpp"
#include "mso/ui/star_rating.hpp"
#include "mso/ui/star_rating_widget.hpp"
#include "mso/ui/waveform_view.hpp"

namespace {

using mso::ui::SampleTableModel;

/// One QApplication for the whole test run, on the offscreen platform.
QApplication& application() {
    static int argc = 1;
    static char name[] = "mso_ui_tests";
    static char* argv[] = {name, nullptr};
    static std::unique_ptr<QApplication> instance = [] {
        qputenv("QT_QPA_PLATFORM", "offscreen");
        return std::make_unique<QApplication>(argc, argv);
    }();
    return *instance;
}

/// Runs the event loop until `done` holds or the timeout passes.
bool wait_until(const std::function<bool()>& done, int timeout_milliseconds = 2000) {
    QElapsedTimer timer;
    timer.start();
    while (!done() && timer.elapsed() < timeout_milliseconds)
        QCoreApplication::processEvents(QEventLoop::AllEvents, 10);
    return done();
}

/// Writes a short 16-bit mono PCM WAV file of silence.
void write_wav(const QString& path, int frames) {
    QFile file(path);
    REQUIRE(file.open(QIODevice::WriteOnly));
    const auto put32 = [&file](quint32 value) { file.write(reinterpret_cast<const char*>(&value), 4); };
    const auto put16 = [&file](quint16 value) { file.write(reinterpret_cast<const char*>(&value), 2); };
    const quint32 data_bytes = static_cast<quint32>(frames) * 2;
    file.write("RIFF", 4);
    put32(36 + data_bytes);
    file.write("WAVEfmt ", 8);
    put32(16);
    put16(1);      // PCM
    put16(1);      // mono
    put32(44100);  // rate
    put32(88200);  // byte rate
    put16(2);      // block align
    put16(16);     // bits
    file.write("data", 4);
    put32(data_bytes);
    file.write(QByteArray(static_cast<qsizetype>(data_bytes), '\0'));
}

/// In-memory search backend: each query text maps to a sorted row list; cursors are offsets.
class FakePageSource final : public mso::ui::SamplePageSource {
public:
    std::map<std::string, std::vector<mso::search::SampleRow>> results;
    std::vector<mso::search::SearchRequest> page_requests;
    int count_requests = 0;
    /// When set, deliveries wait until release() (to interleave generations).
    bool hold = false;
    std::vector<std::function<void()>> held;

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
        schedule(context, [deliver, page] { deliver(page); });
    }

    void request_count(const mso::search::SearchRequest& request, std::optional<uuids::uuid>, QObject* context,
                       CountDelivery deliver) override {
        ++count_requests;
        mso::search::CountResult count;
        count.total = static_cast<std::int64_t>(results[request.text].size());
        schedule(context, [deliver, count] { deliver(count); });
    }

    void release() {
        auto pending = std::move(held);
        held.clear();
        for (auto& delivery : pending)
            delivery();
    }

private:
    void schedule(QObject* context, std::function<void()> delivery) {
        if (hold)
            held.push_back(std::move(delivery));
        else
            QTimer::singleShot(0, context, std::move(delivery));
    }
};

std::vector<mso::search::SampleRow> make_rows(int count, const std::string& prefix) {
    const auto library = mso::new_uuid();
    std::vector<mso::search::SampleRow> rows;
    for (int index = 0; index < count; ++index) {
        mso::search::SampleRow row;
        row.library = library;
        row.sample_id = index + 1;
        row.display_name = prefix + std::to_string(index);
        row.file_name = row.display_name + ".wav";
        row.availability = mso::search::RowAvailability::Available;
        rows.push_back(row);
    }
    return rows;
}

QString name_at(const SampleTableModel& model, int row) {
    return model.data(model.index(row, static_cast<int>(SampleTableModel::Column::Name))).toString();
}

}  // namespace

TEST_SUITE("ui") {
    TEST_CASE("paged model fetches pages on demand and counts once per query") {
        application();
        FakePageSource source;
        source.results[""] = make_rows(450, "kick ");
        SampleTableModel model(source);
        std::vector<qint64> counts;
        QObject::connect(&model, &SampleTableModel::countChanged, [&](qint64 total, bool) { counts.push_back(total); });

        model.set_query("", {});
        REQUIRE(wait_until([&] { return model.rowCount() == SampleTableModel::kPageSize && counts.size() == 1; }));
        CHECK(counts.front() == 450);
        CHECK(model.canFetchMore({}));
        model.fetchMore({});
        REQUIRE(wait_until([&] { return model.rowCount() == 400; }));
        model.fetchMore({});
        REQUIRE(wait_until([&] { return model.rowCount() == 450; }));
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
        source.hold = true;
        model.set_query("old", {});
        model.set_query("new", {});
        source.hold = false;
        source.release();  // delivers "old" first, then "new"
        REQUIRE(wait_until([&] { return model.rowCount() == 3; }));
        CHECK(name_at(model, 0) == QStringLiteral("new 0"));
    }

    TEST_CASE("at most ten pages stay hydrated; evicted rows show loading and are re-read") {
        application();
        FakePageSource source;
        source.results[""] = make_rows(SampleTableModel::kPageSize * 12, "row ");
        SampleTableModel model(source);
        model.set_query("", {});
        REQUIRE(wait_until([&] { return model.rowCount() == SampleTableModel::kPageSize; }));
        for (int page = 1; page < 12; ++page) {
            model.fetchMore({});
            REQUIRE(wait_until([&] { return model.rowCount() == SampleTableModel::kPageSize * (page + 1); }));
        }
        int hydrated_pages = 0;
        for (int page = 0; page < 12; ++page)
            if (model.loaded_row(page * SampleTableModel::kPageSize)) ++hydrated_pages;
        CHECK(hydrated_pages == SampleTableModel::kMaxHydratedPages);
        CHECK(model.loaded_row(0) == nullptr);  // the least recently used page went first

        CHECK(name_at(model, 0) == QStringLiteral("Loading…"));
        CHECK(model.data(model.index(0, 0), SampleTableModel::LoadingRole).toBool());
        REQUIRE(wait_until([&] { return model.loaded_row(0) != nullptr; }));
        CHECK(name_at(model, 0) == QStringLiteral("row 0"));
        CHECK(source.page_requests.back().cursor == std::nullopt);  // re-read with page 0's start cursor
    }

    TEST_CASE("sorting re-queries server-side; unsortable columns are rejected") {
        application();
        FakePageSource source;
        source.results[""] = make_rows(5, "s ");
        SampleTableModel model(source);
        model.set_query("", {});
        REQUIRE(wait_until([&] { return model.rowCount() == 5; }));
        int rejected = 0;
        QObject::connect(&model, &SampleTableModel::sortRejected, [&] { ++rejected; });
        model.sort(static_cast<int>(SampleTableModel::Column::Tags), Qt::AscendingOrder);
        CHECK(rejected == 1);
        const auto requests_before = source.page_requests.size();
        model.sort(static_cast<int>(SampleTableModel::Column::Bpm), Qt::DescendingOrder);
        REQUIRE(wait_until([&] { return source.page_requests.size() > requests_before && model.rowCount() == 5; }));
        CHECK(source.page_requests.back().sort == mso::search::SortField::Bpm);
        CHECK(source.page_requests.back().descending);
    }

    TEST_CASE("refresh replaces rows in place and announces it") {
        application();
        FakePageSource source;
        source.results[""] = make_rows(300, "a ");
        SampleTableModel model(source);
        model.set_query("", {});
        REQUIRE(wait_until([&] { return model.rowCount() == SampleTableModel::kPageSize; }));
        model.fetchMore({});
        REQUIRE(wait_until([&] { return model.rowCount() == 300; }));
        source.results[""] = make_rows(250, "b ");
        int refreshed = 0;
        QObject::connect(&model, &SampleTableModel::refreshed, [&] { ++refreshed; });
        model.refresh(210);
        REQUIRE(wait_until([&] { return refreshed == 1; }));
        CHECK(model.rowCount() == 250);
        CHECK(name_at(model, 0) == QStringLiteral("b 0"));
        CHECK(name_at(model, 249) == QStringLiteral("b 249"));
    }

    TEST_CASE("a new query keeps the previous rows until its first page, and duplicates are skipped") {
        application();
        FakePageSource source;
        source.results["old"] = make_rows(5, "old ");
        source.results["new"] = make_rows(2, "new ");
        SampleTableModel model(source);
        model.set_query("old", {});
        REQUIRE(wait_until([&] { return model.rowCount() == 5; }));

        source.hold = true;
        model.set_query("new", {});
        QCoreApplication::processEvents();
        CHECK(model.rowCount() == 5);  // no flash of an empty or "Loading…" table
        CHECK(name_at(model, 0) == QStringLiteral("old 0"));
        CHECK_FALSE(model.canFetchMore({}));
        source.hold = false;
        source.release();
        REQUIRE(wait_until([&] { return model.rowCount() == 2; }));
        CHECK(name_at(model, 0) == QStringLiteral("new 0"));

        const auto requests = source.page_requests.size();
        model.set_query("new", {});  // the same navigator node clicked again
        QCoreApplication::processEvents();
        CHECK(source.page_requests.size() == requests);
        model.run_query("new", {});  // libraries changed: same query, new data
        REQUIRE(wait_until([&] { return source.page_requests.size() == requests + 1; }));
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

    TEST_CASE("star rating widget: click sets, clicking the current rating clears, keys work") {
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
    }

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
        REQUIRE(wait_until([&] { return model.rowCount() == 3; }));
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
                         [&](const mso::ui::SampleKey& key, int rating) { rated.emplace_back(key.sample_id, rating); });
        const int rating_column = static_cast<int>(SampleTableModel::Column::Rating);
        QStyleOptionViewItem option;
        option.initFrom(&view);
        option.rect = view.visualRect(model.index(2, rating_column));
        const QPoint second_star =
            mso::ui::StarRating::star_rect(mso::ui::SampleRowDelegate::rating_area(option),
                                           mso::ui::SampleRowDelegate::rating_star_size(option), 2)
                .center();
        for (QEvent::Type type : {QEvent::MouseButtonPress, QEvent::MouseButtonRelease}) {
            QMouseEvent event(type, second_star, view.viewport()->mapToGlobal(second_star), Qt::LeftButton,
                              type == QEvent::MouseButtonPress ? Qt::LeftButton : Qt::NoButton, Qt::NoModifier);
            QApplication::sendEvent(view.viewport(), &event);
        }
        REQUIRE(rated.size() == 1);
        CHECK(rated.front() == std::pair<std::int64_t, int>{3, 2});  // the third row's sample, two stars
    }

    TEST_CASE("the session searches a real library through the paged model") {
        application();
        QTemporaryDir temporary;
        REQUIRE(temporary.isValid());
        const QString samples = temporary.filePath(QStringLiteral("samples"));
        REQUIRE(QDir().mkpath(samples));
        write_wav(samples + QStringLiteral("/kick 01.wav"), 4410);
        write_wav(samples + QStringLiteral("/snare 01.wav"), 4410);
        const QString hebrew_name = QStringLiteral("בעיטה kick");  // "kick" in Hebrew
        write_wav(samples + QLatin1Char('/') + hebrew_name + QStringLiteral(".wav"), 4410);
        REQUIRE(QDir().mkpath(samples + QStringLiteral("/drums/kicks")));
        write_wav(samples + QStringLiteral("/drums/kicks/deep kick.wav"), 4410);

        mso::app::ApplicationOptions options;
        options.workspace_directory = mso::ui::DisplayFormat::to_path(temporary.filePath(QStringLiteral("workspace")));
        options.reopen_libraries = false;
        options.monitor_roots = false;
        options.run_analysis_supervisor = false;
        mso::ui::ApplicationSession session(options);
        bool ready = false;
        QObject::connect(&session, &mso::ui::ApplicationSession::ready, [&] { ready = true; });
        session.start();
        REQUIRE(wait_until([&] { return ready; }, 20000));

        std::optional<mso::Result<mso::files::ImportSummary>> imported;
        session.run(
            &session,
            [library_path = mso::ui::DisplayFormat::to_path(temporary.filePath(QStringLiteral("library"))),
             samples_path = mso::ui::DisplayFormat::to_path(samples)](
                mso::app::Application& app) -> mso::Result<mso::files::ImportSummary> {
                auto library = app.create_library(library_path, "UI test");
                if (!library) return std::unexpected(library.error());
                auto root = app.add_root(library->uuid, samples_path, "samples");
                if (!root) return std::unexpected(root.error());
                return app.import_now(library->uuid, root->id);
            },
            [&](mso::Result<mso::files::ImportSummary> summary) { imported = std::move(summary); });
        REQUIRE(wait_until([&] { return imported.has_value(); }, 20000));
        if (!imported->has_value()) FAIL(mso::ui::DisplayFormat::error(imported->error()).toStdString());

        SampleTableModel model(session);
        model.set_query("", {});
        REQUIRE(wait_until([&] { return model.rowCount() == 4; }, 10000));
        QStringList names;
        for (int row = 0; row < 4; ++row)
            names << name_at(model, row);
        CHECK(names.contains(hebrew_name));

        // The navigator reads every root's folder tree up front: nested folders exist before any
        // node is expanded, and there are no "Loading…" placeholders.
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
            REQUIRE(wait_until([&] { return find_item(QStringLiteral("kicks")) != nullptr; }, 10000));
            CHECK(find_item(QStringLiteral("drums")) != nullptr);
            CHECK(tree->findItems(QStringLiteral("Loading…"), Qt::MatchExactly | Qt::MatchRecursive).isEmpty());
        }

        // Previewing the selected sample opens it and builds its waveform lazily (and only its own).
        qputenv("MSO_AUDIO_BACKEND", "null");
        {
            mso::ui::PreviewPanel preview(session);
            const auto* first = model.loaded_row(0);
            REQUIRE(first != nullptr);
            preview.select_sample(mso::ui::SampleKey{first->library, first->sample_id}, name_at(model, 0));
            auto* waveform = preview.findChild<mso::ui::WaveformView*>();
            REQUIRE(waveform != nullptr);
            CHECK(waveform->state() == mso::ui::WaveformView::State::NotGenerated);
            preview.play_selected();
            REQUIRE(wait_until([&] { return waveform->state() == mso::ui::WaveformView::State::Ready; }, 10000));
            preview.stop();
        }

        model.set_query("kick", {});
        REQUIRE(wait_until([&] { return model.rowCount() == 3 && !model.is_loading(); }, 10000));
        model.set_query("bpm:abc", {});
        std::optional<QString> failure;
        QObject::connect(&model, &SampleTableModel::queryFailed, [&](const QString& message) { failure = message; });
        REQUIRE(wait_until([&] { return failure.has_value(); }, 10000));
    }

    TEST_CASE("shortcut bindings round-trip and conflicts are detected") {
        auto bindings = mso::ui::ShortcutCatalog::defaults();
        CHECK(mso::ui::ShortcutCatalog::conflicts(bindings).empty());
        bindings[QStringLiteral("toggle_favorite")] = QKeySequence(Qt::Key_Space);
        const auto conflicts = mso::ui::ShortcutCatalog::conflicts(bindings);
        REQUIRE(conflicts.size() == 1);
        CHECK(conflicts.front().size() == 2);
        const auto restored = mso::ui::ShortcutCatalog::from_json(mso::ui::ShortcutCatalog::to_json(bindings));
        CHECK(restored == bindings);
        // A foreign version falls back to the defaults instead of leaving commands unbound.
        CHECK(mso::ui::ShortcutCatalog::from_json(nlohmann::json{{"version", 99}}) ==
              mso::ui::ShortcutCatalog::defaults());
    }
}

#endif  // MSO_UI_TESTS
