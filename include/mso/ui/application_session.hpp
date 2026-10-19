#pragma once
/// @file
/// ApplicationSession: the UI's single gateway to mso::app::Application.
///
/// Every Application method blocks, so the session runs them on its own worker pool and hands the
/// results back to the UI thread. Application events arrive on background threads and are
/// re-emitted here as Qt signals on the UI thread. Destruction stops the pool before the
/// Application, so no worker ever touches a destroyed Application.
#include <QObject>
#include <QString>
#include <QThreadPool>

#include <memory>
#include <optional>
#include <stop_token>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <nlohmann/json.hpp>

#include "mso/app/application.hpp"
#include "mso/ui/background_tasks.hpp"
#include "mso/ui/sample_page_source.hpp"

namespace mso::ui {

/// Owns the Application, the worker pool used for its blocking calls, and the event bridge.
class ApplicationSession final : public QObject, public SamplePageSource {
    Q_OBJECT

public:
    explicit ApplicationSession(app::ApplicationOptions options, QObject* parent = nullptr);
    ~ApplicationSession() override;

    ApplicationSession(const ApplicationSession&) = delete;
    ApplicationSession& operator=(const ApplicationSession&) = delete;

    /// Starts the Application (workspace load, library reopening, crash recovery) on the pool.
    /// Emits ready() or startFailed() on the UI thread.
    void start();
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
    std::optional<app::OpenLibraryInfo> library_info(const uuids::uuid& library) const;
    /// Display name of an open library, or a placeholder for a library that is no longer open.
    QString library_name(const uuids::uuid& library) const;
    /// Re-reads the open library list on the pool; emits librariesChanged() when delivered.
    void refresh_libraries();

    /// Workspace preference (in-memory lookup); null before ready() or when never set.
    nlohmann::json preference(std::string_view key) const;
    /// Stores a preference. The file write happens on a dedicated single-thread queue, so writes
    /// of the same key are persisted in the order they were made.
    void set_preference(std::string key, nlohmann::json value);

    /// Requested when the session shuts down; long tasks (exports) observe it.
    std::stop_token shutdown_token() const { return shutdown_.get_token(); }

    // SamplePageSource
    void request_page(const search::SearchRequest& request, std::optional<uuids::uuid> library, QObject* context,
                      PageDelivery deliver) override;
    void request_count(const search::SearchRequest& request, std::optional<uuids::uuid> library, QObject* context,
                       CountDelivery deliver) override;

signals:
    void ready();
    void startFailed(const QString& message);
    void librariesChanged();
    void importProgressed(const mso::app::ImportProgressEvent& event);
    void catalogChanged(const uuids::uuid& library);
    void rootAvailabilityChanged(const mso::app::RootAvailabilityEvent& event);
    void analysisProgressed(const mso::app::AnalysisProgressEvent& event);
    void operationFinished(const mso::app::OperationFinishedEvent& event);

private:
    /// Re-emits one Application event as the matching signal (UI thread).
    void dispatch_event(const app::ApplicationEvent& event);

    app::ApplicationOptions options_;
    QThreadPool pool_;
    QThreadPool preference_writer_;
    std::stop_source shutdown_;
    std::unique_ptr<app::Application> application_;
    std::vector<app::OpenLibraryInfo> libraries_;
};

}  // namespace mso::ui
