#include "mso/ui/application_session.hpp"

#include <QMetaObject>
#include <QTimer>

#include <type_traits>
#include <variant>

#include "mso/ui/display_format.hpp"

namespace mso::ui {

namespace {

/// Concurrent blocking calls (searches, counts, details, edits). SQLite readers are pooled per
/// library, so a handful of threads keeps the UI responsive without oversubscribing the disk.
constexpr int kWorkerThreads = 4;

}  // namespace

ApplicationSession::ApplicationSession(app::ApplicationOptions options, QObject* parent)
    : QObject(parent), options_(std::move(options)) {
    pool_.setMaxThreadCount(kWorkerThreads);
    pool_.setObjectName(QStringLiteral("ApplicationSessionPool"));
    preference_writer_.setMaxThreadCount(1);
    preference_writer_.setObjectName(QStringLiteral("PreferenceWriter"));
}

ApplicationSession::~ApplicationSession() {
    shutdown_.request_stop();
    if (application_) application_->set_event_handler({});
    pool_.clear();
    pool_.waitForDone();
    preference_writer_.waitForDone();
    application_.reset();  // joins the Application's own background threads
}

void ApplicationSession::start() {
    using StartResult = Result<std::unique_ptr<app::Application>>;
    run_in_background(
        pool_, this, [options = options_]() -> StartResult { return app::Application::start(options); },
        [this](StartResult started) {
            if (!started) {
                emit startFailed(DisplayFormat::error(started.error()));
                return;
            }
            application_ = std::move(*started);
            application_->set_event_handler([this](const app::ApplicationEvent& event) {
                // Background thread: copy the event and continue on the UI thread.
                QMetaObject::invokeMethod(this, [this, event] { dispatch_event(event); }, Qt::QueuedConnection);
            });
            run(
                this, [](app::Application& application) { return application.libraries(); },
                [this](std::vector<app::OpenLibraryInfo> libraries) {
                    libraries_ = std::move(libraries);
                    emit ready();
                    emit librariesChanged();
                });
        });
}

std::optional<app::OpenLibraryInfo> ApplicationSession::library_info(const uuids::uuid& library) const {
    for (const auto& info : libraries_)
        if (info.uuid == library) return info;
    return std::nullopt;
}

QString ApplicationSession::library_name(const uuids::uuid& library) const {
    if (auto info = library_info(library)) return DisplayFormat::from_utf8(info->name);
    return tr("Closed library");
}

void ApplicationSession::refresh_libraries() {
    if (!application_) return;
    run(
        this, [](app::Application& application) { return application.libraries(); },
        [this](std::vector<app::OpenLibraryInfo> libraries) {
            libraries_ = std::move(libraries);
            emit librariesChanged();
        });
}

nlohmann::json ApplicationSession::preference(std::string_view key) const {
    if (!application_) return nullptr;
    return application_->workspace().preference(key);
}

void ApplicationSession::set_preference(std::string key, nlohmann::json value) {
    if (!application_) return;
    app::Application* application = application_.get();
    mso::ui::run_detached(preference_writer_, [application, key = std::move(key), value = std::move(value)]() mutable {
        // A failed write keeps the in-memory value; the next successful write persists it.
        (void)application->workspace().set_preference(key, std::move(value));
    });
}

void ApplicationSession::request_page(const search::SearchRequest& request, std::optional<uuids::uuid> library,
                                      QObject* context, PageDelivery deliver) {
    if (!application_) {
        QTimer::singleShot(0, context, [deliver = std::move(deliver)] {
            deliver(fail(ErrorCode::Internal, "the workspace is still opening", true));
        });
        return;
    }
    run(
        context, [request, library](app::Application& application) { return application.search(request, library); },
        std::move(deliver));
}

void ApplicationSession::request_count(const search::SearchRequest& request, std::optional<uuids::uuid> library,
                                       QObject* context, CountDelivery deliver) {
    if (!application_) {
        QTimer::singleShot(0, context, [deliver = std::move(deliver)] {
            deliver(fail(ErrorCode::Internal, "the workspace is still opening", true));
        });
        return;
    }
    run(
        context, [request, library](app::Application& application) { return application.count(request, library); },
        std::move(deliver));
}

void ApplicationSession::dispatch_event(const app::ApplicationEvent& event) {
    std::visit(
        [this](const auto& specific) {
            using Event = std::decay_t<decltype(specific)>;
            if constexpr (std::is_same_v<Event, app::ImportProgressEvent>) {
                emit importProgressed(specific);
            } else if constexpr (std::is_same_v<Event, app::CatalogChangedEvent>) {
                emit catalogChanged(specific.library);
            } else if constexpr (std::is_same_v<Event, app::RootAvailabilityEvent>) {
                emit rootAvailabilityChanged(specific);
            } else if constexpr (std::is_same_v<Event, app::AnalysisProgressEvent>) {
                emit analysisProgressed(specific);
            } else if constexpr (std::is_same_v<Event, app::OperationFinishedEvent>) {
                emit operationFinished(specific);
            }
        },
        event);
}

}  // namespace mso::ui
