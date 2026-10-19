#pragma once
/// @file
/// SamplePageSource: where the paged table model gets its rows. The application session
/// implements it with Application::search/count on a worker pool; tests use a fake.
#include <QObject>

#include <functional>
#include <optional>

#include "mso/domain/core.hpp"
#include "mso/domain/error.hpp"
#include "mso/search/search_service.hpp"

namespace mso::ui {

/// Asynchronous search backend of SampleTableModel.
class SamplePageSource {
public:
    using PageDelivery = std::function<void(Result<search::SearchPage>)>;
    using CountDelivery = std::function<void(Result<search::CountResult>)>;

    virtual ~SamplePageSource() = default;

    /// Fetches one page off the UI thread. `deliver` runs later on `context`'s thread (never
    /// before this call returns) and is skipped when `context` has been destroyed.
    /// `library` restricts the search to one library; nullopt searches every open library.
    virtual void request_page(const search::SearchRequest& request, std::optional<uuids::uuid> library,
                              QObject* context, PageDelivery deliver) = 0;

    /// Counts the whole result off the UI thread, with the same delivery rules as request_page().
    virtual void request_count(const search::SearchRequest& request, std::optional<uuids::uuid> library,
                               QObject* context, CountDelivery deliver) = 0;

protected:
    SamplePageSource() = default;
    SamplePageSource(const SamplePageSource&) = default;
    SamplePageSource& operator=(const SamplePageSource&) = default;
};

}  // namespace mso::ui
