#pragma once
/// @file
/// ApplicationSession: the UI's single gateway to mso::app::Application.
///
/// Every Application method blocks, so the session runs them on its own worker pool and hands the
/// results back to the UI thread. Application events arrive on background threads; the session
/// merges them (one pending catalog change per library, the latest progress per operation) and
/// re-emits them as Qt signals on the UI thread in one batch. Preferences are cached in memory and
/// updated synchronously; writes to the workspace file are coalesced. Destruction stops the pool
/// before the Application, so no worker ever touches a destroyed Application.
#include <QObject>
#include <QString>
#include <QThreadPool>
#include <QTimer>

#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <set>
#include <stop_token>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <nlohmann/json.hpp>

#include "mso/app/application.hpp"
#include "mso/ui/background_tasks.hpp"
#include "mso/ui/edit_gateway.hpp"
#include "mso/ui/sample_page_source.hpp"

namespace mso::ui {

/// Owns the Application, the worker pool used for its blocking calls, and the event bridge.
class ApplicationSession final : public QObject, public SamplePageSource, public EditGateway {
    Q_OBJECT

public:
    /// A merged catalog change bigger than this many sample ids becomes a full reload.
    static constexpr std::size_t kMaxCoalescedSampleIds = 50'000;
    /// Preference writes within this window are combined into one workspace write.
    static constexpr int kPreferenceWriteDelayMilliseconds = 400;

    explicit ApplicationSession(app::ApplicationOptions options, QObject* parent = nullptr);
    ~ApplicationSession() override;

    ApplicationSession(const ApplicationSession&) = delete;
    ApplicationSession& operator=(const ApplicationSession&) = delete;

    /// Reads the UI preferences (UiPreferenceKeys) from the workspace file before the window is
    /// shown, so shortcuts and the saved layout apply before the first paint. Called once from
    /// main() before the event loop starts, when there is nothing to keep responsive yet.
    void preload_preferences();

    /// Starts the Application (workspace load, library reopening, index build, crash recovery) on
    /// the pool. Emits ready() or startFailed() on the UI thread.
    void start();
    /// True once start() succeeded.
    bool is_ready() const { return application_ != nullptr; }

    /// Runs `work(Application&)` on the pool, then `deliver(result)` on `context`'s thread.
    /// Must only be called once ready() was emitted.
    template <class Work, class Deliver>
    void run(QObject* context, Work work, Deliver deliver) {
        Q_ASSERT(application_);
        if (!application_) return;
        app::Application* application = application_.get();
        run_in_background(
            pool_, context, [application, work = std::move(work)]() mutable { return work(*application); },
            std::move(deliver));
    }

    /// Like run() but on a caller-owned `pool` (a dedicated queue such as the drag resolver's).
    /// The caller must wait for that pool before the session is destroyed.
    template <class Work, class Deliver>
    void run_on(QThreadPool& pool, QObject* context, Work work, Deliver deliver) {
        Q_ASSERT(application_);
        if (!application_) return;
        app::Application* application = application_.get();
        run_in_background(
            pool, context, [application, work = std::move(work)]() mutable { return work(*application); },
            std::move(deliver));
    }

    /// Runs `work(catalog::Library&)` through Application::with_library(), which keeps the library
    /// alive for the call (NOT_FOUND when it is no longer open), then delivers like run().
    template <class Work, class Deliver>
    void run_with_library(QObject* context, const uuids::uuid& library, Work work, Deliver deliver) {
        run(
            context,
            [library, work = std::move(work)](app::Application& application) mutable {
                return application.with_library(library, work);
            },
            std::move(deliver));
    }

    /// Runs `work(Application&)` on the pool without a delivery (results are reported by events).
    template <class Work>
    void run_detached(Work work) {
        Q_ASSERT(application_);
        if (!application_) return;
        app::Application* application = application_.get();
        mso::ui::run_detached(pool_, [application, work = std::move(work)]() mutable { work(*application); });
    }

    /// Libraries open in this session, as of the last refresh_libraries() delivery.
    const std::vector<app::OpenLibraryInfo>& libraries() const { return libraries_; }
    /// Information about one open library, or nullopt when it is not open.
    std::optional<app::OpenLibraryInfo> library_info(const uuids::uuid& library) const;
    /// Display name of an open library, or a placeholder for a library that is no longer open.
    QString library_name(const uuids::uuid& library) const override;
    /// Re-reads the open library list on the pool; emits librariesChanged() when delivered.
    void refresh_libraries();

    /// Cached preference value (null when never set). Never blocks.
    nlohmann::json preference(std::string_view key) const;
    /// Updates the cache immediately and schedules a coalesced write to the workspace file.
    void set_preference(std::string_view key, nlohmann::json value);
    /// Writes pending preference changes now and waits for the write (application shutdown).
    void flush_preferences();

    /// Requested when the session shuts down; long tasks (exports) observe it.
    std::stop_token shutdown_token() const { return shutdown_.get_token(); }

    // SamplePageSource
    void request_page(const search::SearchRequest& request, std::optional<uuids::uuid> library, QObject* context,
                      PageDelivery deliver) override;
    void request_count(const search::SearchRequest& request, std::optional<uuids::uuid> library, QObject* context,
                       CountDelivery deliver) override;
    void request_rows(const std::vector<SampleKey>& keys, QObject* context, RowsDelivery deliver) override;

    // EditGateway
    void request_edit(const uuids::uuid& library, const catalog::EditCommand& command, QObject* context,
                      ReceiptDelivery deliver) override;
    void request_taxonomy(const uuids::uuid& library, const catalog::TaxonomyCommand& command, QObject* context,
                          ReceiptDelivery deliver) override;
    void request_undo(const uuids::uuid& library, QObject* context, ReceiptDelivery deliver) override;
    void request_redo(const uuids::uuid& library, QObject* context, ReceiptDelivery deliver) override;

signals:
    /// The Application started; preferences and libraries are available.
    void ready();
    /// The Application could not start (message is user-presentable).
    void startFailed(const QString& message);
    /// The open library list changed (see libraries()).
    void librariesChanged();
    /// Import progress for one operation (only the newest is delivered per batch).
    void importProgressed(const mso::app::ImportProgressEvent& event);
    /// Committed catalog changes of one library, merged since the previous delivery.
    void catalogChanged(const mso::app::CatalogChangedEvent& event);
    /// A root became reachable or unreachable.
    void rootAvailabilityChanged(const mso::app::RootAvailabilityEvent& event);
    /// Analysis queue progress of one library (only the newest is delivered per batch).
    void analysisProgressed(const mso::app::AnalysisProgressEvent& event);
    /// A long operation ended.
    void operationFinished(const mso::app::OperationFinishedEvent& event);

private:
    /// Merges `event` into the pending batch (any thread) and schedules one delivery.
    void enqueue_event(const app::ApplicationEvent& event);
    /// Emits the pending batch in arrival order (UI thread).
    void dispatch_pending_events();
    /// Re-emits one Application event as the matching signal (UI thread).
    void dispatch_event(const app::ApplicationEvent& event);
    /// Hands every changed preference to the single-thread writer.
    void write_dirty_preferences();

    /// How start() creates the Application.
    app::ApplicationOptions options_;
    QThreadPool pool_;               ///< blocking Application calls
    QThreadPool preference_writer_;  ///< one thread: workspace writes stay in order
    std::stop_source shutdown_;
    /// Null until start() succeeds.
    std::unique_ptr<app::Application> application_;
    /// Open libraries as of the last refresh.
    std::vector<app::OpenLibraryInfo> libraries_;

    std::mutex events_mutex_;                            ///< guards the two members below
    std::vector<app::ApplicationEvent> pending_events_;  ///< merged; bounded by libraries + operations
    bool dispatch_scheduled_ = false;

    std::map<std::string, nlohmann::json, std::less<>> preferences_;  ///< authoritative UI-side copy
    std::set<std::string, std::less<>> dirty_preferences_;            ///< changed since the last write
    QTimer preference_write_timer_;
};

}  // namespace mso::ui
