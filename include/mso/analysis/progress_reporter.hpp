#pragma once
/// @file
/// Rate-limited, monotonic progress reporting for analysis runs, plus the work counter a
/// supervisor watches to detect a stuck analysis.
#include <chrono>
#include <optional>

#include "mso/analysis/analysis.hpp"

namespace mso::analysis {

/// Forwards progress fractions to a ProgressCallback at most once per `minimum_interval`
/// (10 Hz by default), never moving backwards, and always delivers a final 1.0 from finish().
/// Throttling uses the steady clock and never influences analysis results. Independently of the
/// throttle, record_work() advances an optional WorkCounter for every unit of work.
class ProgressReporter {
public:
    /// Default throttle: 10 reports per second, the UI progress rate of docs/04_CONTRACTS.md.
    static constexpr std::chrono::milliseconds kDefaultInterval{100};

    /// @param callback may be empty (no fractions are reported).
    /// @param work_counter may be null (no work is counted).
    explicit ProgressReporter(ProgressCallback callback, std::chrono::milliseconds minimum_interval = kDefaultInterval,
                              WorkCounter* work_counter = nullptr);

    /// Reports overall progress in [0, 1]; dropped when too soon after the previous report or
    /// not ahead of it.
    void report(double fraction);
    /// Reports 1.0 exactly once, bypassing the rate limit.
    void finish();
    /// Counts one completed unit of work (never throttled, never blocks).
    void record_work() noexcept;

private:
    ProgressCallback callback_;
    std::chrono::milliseconds minimum_interval_;
    WorkCounter* work_counter_;
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

    /// Reports `phase_fraction` (clamped to [0, 1]) of this phase as overall progress.
    void report(double phase_fraction) const;
    /// Counts one completed unit of work; see ProgressReporter::record_work().
    void record_work() const noexcept { reporter_->record_work(); }

private:
    ProgressReporter* reporter_;
    double begin_;
    double end_;
};

}  // namespace mso::analysis
