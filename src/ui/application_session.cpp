#include "mso/ui/application_session.hpp"

#include <QMetaObject>
#include <QTimer>

#include <algorithm>
#include <type_traits>
#include <variant>

#include "mso/app/workspace.hpp"
#include "mso/ui/display_format.hpp"
#include "mso/ui/ui_preferences.hpp"

namespace mso::ui {

namespace {

/// Concurrent blocking calls (pages, counts, details, edits). Index-backed searches are
/// memory-fast, so a handful of threads keeps the UI responsive without oversubscribing SQLite.
constexpr int kWorkerThreads = 4;

/// Merges a catalog change into a pending one of the same library.
void merge_catalog_change(app::CatalogChangedEvent& pending, const app::CatalogChangedEvent& incoming) {
    pending.structure_changed = pending.structure_changed || incoming.structure_changed;
    pending.full_reload = pending.full_reload || incoming.full_reload;
    if (pending.full_reload) {
        pending.sample_ids.clear();  // a full reload covers every row anyway
        return;
    }
    pending.sample_ids.insert(pending.sample_ids.end(), incoming.sample_ids.begin(), incoming.sample_ids.end());
    std::ranges::sort(pending.sample_ids);
    const auto duplicates = std::ranges::unique(pending.sample_ids);
    pending.sample_ids.erase(duplicates.begin(), duplicates.end());
    if (pending.sample_ids.size() > ApplicationSession::kMaxCoalescedSampleIds) {
        pending.full_reload = true;
        pending.sample_ids.clear();
    }
}

/// Folds `incoming` into `pending` when both describe the same subject (same library, operation
/// or root); returns false when they must stay separate events.
bool merge_event(app::ApplicationEvent& pending, const app::ApplicationEvent& incoming) {
    if (pending.index() != incoming.index()) return false;
    if (auto* change = std::get_if<app::CatalogChangedEvent>(&pending)) {
        const auto& next = std::get<app::CatalogChangedEvent>(incoming);
        if (change->library != next.library) return false;
        merge_catalog_change(*change, next);
        return true;
    }
    if (auto* progress = std::get_if<app::ImportProgressEvent>(&pending)) {
        const auto& next = std::get<app::ImportProgressEvent>(incoming);
        if (progress->operation != next.operation) return false;
        *progress = next;  // only the newest progress matters
        return true;
    }
    if (auto* analysis = std::get_if<app::AnalysisProgressEvent>(&pending)) {
        const auto& next = std::get<app::AnalysisProgressEvent>(incoming);
        if (analysis->library != next.library) return false;
        *analysis = next;
        return true;
    }
    if (auto* availability = std::get_if<app::RootAvailabilityEvent>(&pending)) {
        const auto& next = std::get<app::RootAvailabilityEvent>(incoming);
        if (availability->library != next.library || availability->root_id != next.root_id) return false;
        *availability = next;
        return true;
    }
    return false;  // finished operations are all reported
}

}  // namespace

ApplicationSession::ApplicationSession(app::ApplicationOptions options, QObject* parent)
    : QObject(parent), options_(std::move(options)) {
    pool_.setMaxThreadCount(kWorkerThreads);
    pool_.setObjectName(QStringLiteral("ApplicationSessionPool"));
    preference_writer_.setMaxThreadCount(1);
    preference_writer_.setObjectName(QStringLiteral("PreferenceWriter"));
    preference_write_timer_.setSingleShot(true);
    preference_write_timer_.setInterval(kPreferenceWriteDelayMilliseconds);
    connect(&preference_write_timer_, &QTimer::timeout, this, &ApplicationSession::write_dirty_preferences);
}

ApplicationSession::~ApplicationSession() {
    shutdown_.request_stop();
    flush_preferences();
    if (application_) application_->set_event_handler({});
    pool_.clear();
    pool_.waitForDone();
    preference_writer_.waitForDone();
    application_.reset();  // joins the Application's own background threads
}

void ApplicationSession::preload_preferences() {
    const auto directory = options_.workspace_directory.value_or(app::Workspace::default_directory());
    auto workspace = app::Workspace::load(directory);
    if (!workspace) {
        qWarning("Preferences could not be read before start-up: %s", workspace.error().message().c_str());
        return;
    }
    for (std::string_view key : UiPreferenceKeys::kAll) {
        nlohmann::json value = workspace->preference(key);
        if (!value.is_null()) preferences_.insert_or_assign(std::string(key), std::move(value));
    }
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
            application_->set_event_handler([this](const app::ApplicationEvent& event) { enqueue_event(event); });
            // Values preloaded before start-up stay authoritative; fill in anything not yet cached.
            for (std::string_view key : UiPreferenceKeys::kAll) {
                if (preferences_.contains(key)) continue;
                nlohmann::json value = application_->workspace().preference(key);
                if (!value.is_null()) preferences_.insert_or_assign(std::string(key), std::move(value));
            }
            if (!dirty_preferences_.empty()) preference_write_timer_.start();
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
    const auto found = std::ranges::find(libraries_, library, &app::OpenLibraryInfo::uuid);
    if (found == libraries_.end()) return std::nullopt;
    return *found;
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
    const auto found = preferences_.find(key);
    return found == preferences_.end() ? nlohmann::json() : found->second;
}

void ApplicationSession::set_preference(std::string_view key, nlohmann::json value) {
    preferences_.insert_or_assign(std::string(key), std::move(value));
    dirty_preferences_.insert(std::string(key));
    if (application_) preference_write_timer_.start();  // restarts the window: rapid changes coalesce
}

void ApplicationSession::write_dirty_preferences() {
    if (!application_ || dirty_preferences_.empty()) return;
    std::vector<std::pair<std::string, nlohmann::json>> changes;
    for (const std::string& key : dirty_preferences_)
        changes.emplace_back(key, preferences_.at(key));
    dirty_preferences_.clear();
    app::Application* application = application_.get();
    mso::ui::run_detached(preference_writer_, [application, changes = std::move(changes)]() mutable {
        for (auto& [key, value] : changes) {
            if (auto written = application->workspace().set_preference(key, std::move(value)); !written)
                qWarning("Preference %s was not saved: %s", key.c_str(), written.error().message().c_str());
        }
    });
}

void ApplicationSession::flush_preferences() {
    preference_write_timer_.stop();
    write_dirty_preferences();
    preference_writer_.waitForDone();
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

void ApplicationSession::request_rows(const std::vector<SampleKey>& keys, QObject* context, RowsDelivery deliver) {
    // Before start-up nothing is known about any row: answering "no rows" would read as "all
    // deleted", so there is no delivery at all.
    if (!application_) return;
    run(context, [keys](app::Application& application) { return application.rows(keys); }, std::move(deliver));
}

void ApplicationSession::request_edit(const uuids::uuid& library, const catalog::EditCommand& command, QObject* context,
                                      ReceiptDelivery deliver) {
    run(
        context, [library, command](app::Application& application) { return application.apply_edit(library, command); },
        std::move(deliver));
}

void ApplicationSession::request_taxonomy(const uuids::uuid& library, const catalog::TaxonomyCommand& command,
                                          QObject* context, ReceiptDelivery deliver) {
    run_with_library(
        context, library, [command](catalog::Library& opened) { return opened.apply_taxonomy(command); },
        std::move(deliver));
}

void ApplicationSession::request_undo(const uuids::uuid& library, QObject* context, ReceiptDelivery deliver) {
    run(context, [library](app::Application& application) { return application.undo(library); }, std::move(deliver));
}

void ApplicationSession::request_redo(const uuids::uuid& library, QObject* context, ReceiptDelivery deliver) {
    run(context, [library](app::Application& application) { return application.redo(library); }, std::move(deliver));
}

void ApplicationSession::enqueue_event(const app::ApplicationEvent& event) {
    std::lock_guard lock(events_mutex_);
    const bool merged = std::ranges::any_of(
        pending_events_, [&event](app::ApplicationEvent& pending) { return merge_event(pending, event); });
    if (!merged) pending_events_.push_back(event);
    if (dispatch_scheduled_) return;
    dispatch_scheduled_ = true;
    // `this` outlives every queued call: the handler is cleared before destruction, and Qt drops
    // events queued for a destroyed receiver.
    QMetaObject::invokeMethod(this, [this] { dispatch_pending_events(); }, Qt::QueuedConnection);
}

void ApplicationSession::dispatch_pending_events() {
    std::vector<app::ApplicationEvent> batch;
    {
        std::lock_guard lock(events_mutex_);
        batch.swap(pending_events_);
        dispatch_scheduled_ = false;
    }
    for (const app::ApplicationEvent& event : batch)
        dispatch_event(event);
}

void ApplicationSession::dispatch_event(const app::ApplicationEvent& event) {
    std::visit(
        [this](const auto& specific) {
            using Event = std::decay_t<decltype(specific)>;
            if constexpr (std::is_same_v<Event, app::ImportProgressEvent>) {
                emit importProgressed(specific);
            } else if constexpr (std::is_same_v<Event, app::CatalogChangedEvent>) {
                emit catalogChanged(specific);
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
