#include "mso/app/detail/open_progress_relay.hpp"

#include <chrono>
#include <memory>
#include <optional>

namespace mso::app::detail {

std::function<void(const catalog::OpenProgress&)> open_progress_relay(std::function<void(const ApplicationEvent&)> emit,
                                                                      uuids::uuid library,
                                                                      std::filesystem::path directory) {
    /// What was last emitted.
    struct Throttle {
        std::optional<catalog::OpenStep> step;
        std::chrono::steady_clock::time_point last_emitted{};
    };
    return [emit = std::move(emit), library, directory = std::move(directory),
            throttle = std::make_shared<Throttle>()](const catalog::OpenProgress& progress) {
        const auto now = std::chrono::steady_clock::now();
        const bool boundary = throttle->step != progress.step || progress.completed >= progress.total;
        if (!boundary && now - throttle->last_emitted < Application::kOpenProgressInterval) return;
        throttle->step = progress.step;
        throttle->last_emitted = now;
        emit(LibraryOpenProgressEvent{library, directory, progress.step, progress.completed, progress.total});
    };
}

}  // namespace mso::app::detail
