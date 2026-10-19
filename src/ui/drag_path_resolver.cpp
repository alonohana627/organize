#include "mso/ui/drag_path_resolver.hpp"

#include "mso/ui/application_session.hpp"

namespace mso::ui {

DragPathResolver::DragPathResolver(ApplicationSession& session, QObject* parent) : QObject(parent), session_(session) {}

void DragPathResolver::resolve(std::vector<SampleKey> keys) {
    const std::uint64_t generation = ++generation_;
    ready_ = false;
    keys_ = std::move(keys);
    results_.clear();
    if (keys_.empty() || keys_.size() > kMaxDragSamples || !session_.is_ready()) return;
    session_.run(
        this,
        [keys = keys_](app::Application& application) {
            std::vector<Result<std::filesystem::path>> results;
            results.reserve(keys.size());
            for (const SampleKey& key : keys)
                results.push_back(application.resolve_playable_path(key.library, key.sample_id));
            return results;
        },
        [this, generation](std::vector<Result<std::filesystem::path>> results) {
            if (generation != generation_) return;  // the selection changed meanwhile
            results_ = std::move(results);
            ready_ = true;
            emit resolved();
        });
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
    for (std::size_t index = 0; index < results_.size(); ++index)
        if (keys_[index] == key && results_[index]) return *results_[index];
    return std::nullopt;
}

}  // namespace mso::ui
