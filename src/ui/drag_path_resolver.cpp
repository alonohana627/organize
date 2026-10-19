#include "mso/ui/drag_path_resolver.hpp"

#include <algorithm>
#include <utility>

#include "mso/ui/application_session.hpp"
#include "mso/ui/background_tasks.hpp"

namespace mso::ui {

DragPathResolver::DragPathResolver(ApplicationSession& session, QObject* parent)
    : DragPathResolver(PathLookup{}, parent) {
    session_ = &session;
}

DragPathResolver::DragPathResolver(PathLookup lookup, QObject* parent) : QObject(parent), lookup_(std::move(lookup)) {
    pool_.setMaxThreadCount(1);
    pool_.setObjectName(QStringLiteral("DragPathResolver"));
    debounce_.setSingleShot(true);
    debounce_.setInterval(kDebounceMilliseconds);
    connect(&debounce_, &QTimer::timeout, this, &DragPathResolver::start_check);
}

DragPathResolver::~DragPathResolver() {
    stop_.request_stop();
    pool_.clear();
    pool_.waitForDone();
}

void DragPathResolver::resolve(std::vector<SampleKey> keys) {
    // Same selection (e.g. after an in-place row refresh): a finished, pending or running check
    // already covers it.
    if (keys == keys_ && (ready_ || checking_ || debounce_.isActive())) return;
    ++generation_;
    stop_.request_stop();  // the previous check is obsolete
    ready_ = false;
    checking_ = false;
    keys_ = std::move(keys);
    results_.clear();
    index_of_key_.clear();
    if (keys_.empty() || keys_.size() > kMaxDragSamples) {
        debounce_.stop();
        return;
    }
    debounce_.start();
}

std::vector<Result<std::filesystem::path>> DragPathResolver::check_keys(const std::vector<SampleKey>& keys,
                                                                        const PathLookup& lookup,
                                                                        const std::stop_token& stop) {
    std::vector<Result<std::filesystem::path>> results;
    results.reserve(keys.size());
    for (const SampleKey& key : keys) {
        if (stop.stop_requested()) break;  // a newer selection: stop between two files
        results.push_back(lookup(key));
    }
    return results;
}

void DragPathResolver::start_check() {
    if (keys_.empty() || (session_ && !session_->is_ready())) return;
    stop_ = std::stop_source();
    checking_ = true;
    const std::uint64_t generation = generation_;
    const auto deliver = [this, generation](std::vector<Result<std::filesystem::path>> results) {
        finish_check(generation, std::move(results));
    };
    if (session_) {
        session_->run_on(
            pool_, this,
            [keys = keys_, stop = stop_.get_token()](app::Application& application) {
                return check_keys(
                    keys,
                    [&application](const SampleKey& key) {
                        return application.resolve_playable_path(key.library, key.sample_id);
                    },
                    stop);
            },
            deliver);
        return;
    }
    run_in_background(
        pool_, this,
        [keys = keys_, lookup = lookup_, stop = stop_.get_token()] { return check_keys(keys, lookup, stop); }, deliver);
}

void DragPathResolver::finish_check(std::uint64_t generation, std::vector<Result<std::filesystem::path>> results) {
    if (generation != generation_) return;  // superseded by a newer selection
    checking_ = false;
    if (results.size() != keys_.size()) return;  // cancelled
    results_ = std::move(results);
    for (std::size_t index = 0; index < keys_.size(); ++index)
        index_of_key_.emplace(keys_[index], index);
    ready_ = true;
    emit resolved();
}

bool DragPathResolver::is_ready_for(const std::vector<SampleKey>& keys) const {
    return ready_ && keys == keys_;
}

std::vector<std::filesystem::path> DragPathResolver::available_paths() const {
    std::vector<std::filesystem::path> paths;
    for (const auto& result : results_)
        if (result) paths.push_back(*result);
    return paths;
}

std::vector<SampleKey> DragPathResolver::unavailable_keys() const {
    std::vector<SampleKey> keys;
    for (std::size_t index = 0; index < results_.size(); ++index)
        if (!results_[index]) keys.push_back(keys_[index]);
    return keys;
}

std::vector<SampleKey> DragPathResolver::available_keys() const {
    std::vector<SampleKey> keys;
    for (std::size_t index = 0; index < results_.size(); ++index)
        if (results_[index]) keys.push_back(keys_[index]);
    return keys;
}

std::optional<std::filesystem::path> DragPathResolver::path_of(const SampleKey& key) const {
    // A map lookup: startDrag() asks once per dragged sample (up to kMaxDragSamples).
    const auto found = index_of_key_.find(key);
    if (found == index_of_key_.end() || !ready_) return std::nullopt;
    const auto& result = results_[found->second];
    if (!result) return std::nullopt;
    return *result;
}

}  // namespace mso::ui
