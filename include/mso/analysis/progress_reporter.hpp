#pragma once
/// @file
/// Rate-limited, monotonic progress reporting for analysis runs.
#include <chrono>
#include <optional>

#include "mso/analysis/analysis.hpp"

namespace mso::analysis {

/// Forwards progress fractions to a ProgressCallback at most once per `minimum_interval`
/// (10 Hz by default), never moving backwards, and always delivers a final 1.0 from finish().
/// Throttling uses the steady clock and never influences analysis results.
class ProgressReporter {
public:
    explicit ProgressReporter(ProgressCallback callback,
                              std::chrono::milliseconds minimum_interval = std::chrono::milliseconds(100));

    /// Reports overall progress in [0, 1]; dropped when too soon after the previous report or
    /// not ahead of it.
    void report(double fraction);
    /// Reports 1.0 exactly once, bypassing the rate limit.
    void finish();

private:
    ProgressCallback callback_;
    std::chrono::milliseconds minimum_interval_;
    std::optional<std::chrono::steady_clock::time_point> last_report_time_;
    double last_fraction_ = -1.0;
    bool finished_ = false;
};

/// One phase of a run mapped onto the sub-range [begin, end] of the overall progress, so each
/// phase can report its own fraction in [0, 1].
class ProgressPhase {
public:
    ProgressPhase(ProgressReporter& reporter, double begin, double end) noexcept
        : reporter_(&reporter), begin_(begin), end_(end) {}

    void report(double phase_fraction) const;

private:
    ProgressReporter* reporter_;
    double begin_;
    double end_;
};

}  // namespace mso::analysis
