#include "mso/app/detail/index_load_schedule.hpp"

namespace mso::app::detail {

IndexLoadSchedule::Request IndexLoadSchedule::request() {
    std::lock_guard lock(mutex_);
    if (running_) {
        // Covered by the follow-up, whose number is the one after the running load.
        follow_up_ = true;
        return Request{false, begun_ + 1};
    }
    const bool enqueue = !queued_;
    queued_ = true;
    return Request{enqueue, begun_ + 1};
}

std::uint64_t IndexLoadSchedule::begin_load() {
    std::lock_guard lock(mutex_);
    queued_ = false;
    running_ = true;
    return ++begun_;
}

bool IndexLoadSchedule::finish_load() {
    bool follow_up = false;
    {
        std::lock_guard lock(mutex_);
        running_ = false;
        finished_ = begun_;
        follow_up = follow_up_;
        follow_up_ = false;
        if (follow_up) queued_ = true;
    }
    changed_.notify_all();
    return follow_up;
}

bool IndexLoadSchedule::load_pending() const {
    std::lock_guard lock(mutex_);
    return queued_ || running_;
}

std::optional<std::uint64_t> IndexLoadSchedule::pending_load() const {
    std::lock_guard lock(mutex_);
    if (running_) return begun_;
    if (queued_) return begun_ + 1;
    return std::nullopt;
}

bool IndexLoadSchedule::wait_for_load(std::uint64_t load_number, std::stop_token stop) const {
    std::unique_lock lock(mutex_);
    changed_.wait(lock, stop, [&] { return finished_ >= load_number || (!queued_ && !running_); });
    return finished_ >= load_number;
}

std::uint64_t IndexLoadSchedule::loads_begun() const {
    std::lock_guard lock(mutex_);
    return begun_;
}

}  // namespace mso::app::detail
