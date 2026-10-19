/// @file
/// Progress throttling.
#include "mso/analysis/progress_reporter.hpp"

#include <algorithm>
#include <cmath>
#include <utility>

namespace mso::analysis {

ProgressReporter::ProgressReporter(ProgressCallback callback, std::chrono::milliseconds minimum_interval)
    : callback_(std::move(callback)), minimum_interval_(minimum_interval) {}

void ProgressReporter::report(double fraction) {
    if (!callback_ || finished_ || !std::isfinite(fraction)) return;
    fraction = std::clamp(fraction, 0.0, 1.0);
    if (fraction <= last_fraction_) return;

    const auto now = std::chrono::steady_clock::now();
    if (last_report_time_ && now - *last_report_time_ < minimum_interval_) return;
    last_report_time_ = now;
    last_fraction_ = fraction;
    callback_(fraction);
}

void ProgressReporter::finish() {
    if (!callback_ || finished_) return;
    finished_ = true;
    callback_(1.0);
}

void ProgressPhase::report(double phase_fraction) const {
    reporter_->report(begin_ + (end_ - begin_) * std::clamp(phase_fraction, 0.0, 1.0));
}

}  // namespace mso::analysis
