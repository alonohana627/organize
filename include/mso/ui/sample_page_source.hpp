#pragma once
/// @file
/// SamplePageSource: where the paged table model gets its rows. The application session
/// implements it with the Application's in-memory index on a worker pool; tests use a fake.
#include <QObject>

#include <functional>
#include <optional>
#include <vector>

#include "mso/domain/core.hpp"
#include "mso/domain/error.hpp"
#include "mso/search/search_service.hpp"
#include "mso/ui/sample_key.hpp"

namespace mso::ui {

/// Asynchronous search backend of SampleTableModel. Every delivery runs later on `context`'s
/// thread (never before the request call returns) and is skipped when `context` was destroyed.
class SamplePageSource {
public:
    /// Receives one page (or the error that prevented it).
    using PageDelivery = std::function<void(Result<search::SearchPage>)>;
    /// Receives the size of a whole result.
    using CountDelivery = std::function<void(Result<search::CountResult>)>;
    /// Receives the current rows of requested samples; deleted samples are simply absent.
    using RowsDelivery = std::function<void(std::vector<search::SampleRow>)>;

    virtual ~SamplePageSource() = default;

    /// Fetches one page off the UI thread. `library` restricts the search to one library;
    /// nullopt searches every open library.
    virtual void request_page(const search::SearchRequest& request, std::optional<uuids::uuid> library,
                              QObject* context, PageDelivery deliver) = 0;

    /// Counts the whole result off the UI thread.
    virtual void request_count(const search::SearchRequest& request, std::optional<uuids::uuid> library,
                               QObject* context, CountDelivery deliver) = 0;

    /// Reads the current rows of specific samples (Application::rows), for in-place refreshes.
    /// A source that cannot know (not started) makes no delivery rather than an empty one.
    virtual void request_rows(const std::vector<SampleKey>& keys, QObject* context, RowsDelivery deliver) = 0;

protected:
    SamplePageSource() = default;
    SamplePageSource(const SamplePageSource&) = default;
    SamplePageSource& operator=(const SamplePageSource&) = default;
};

}  // namespace mso::ui
