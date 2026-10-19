#include "mso/audio/detail/output_supervisor.hpp"

#include <algorithm>
#include <exception>
#include <utility>

namespace mso::audio::detail {
namespace {

/// Longest sleep without a request or a due retry. Requests and losses wake the thread at once;
/// this only bounds the cost of a wake-up absorbed by a race (see WakeupSignal).
constexpr std::chrono::milliseconds kIdleWait{1000};

}  // namespace

std::chrono::milliseconds OutputRetrySchedule::delay_after(std::uint32_t failed_attempts) const noexcept {
    const std::chrono::milliseconds longest = std::max(longest_delay, first_delay);
    std::chrono::milliseconds delay = first_delay;
    for (std::uint32_t attempt = 0; attempt < failed_attempts && delay < longest; ++attempt) {
        delay *= 2;
    }
    return std::min(delay, longest);
}

OutputSupervisor::OutputSupervisor(const OutputSupervisorOptions& options, PlaybackRenderer& renderer,
                                   SharedPlaybackState& shared, WakeupSignal& decoder_wakeup)
    : options_(options),
      renderer_(renderer),
      shared_(shared),
      decoder_wakeup_(decoder_wakeup),
      selected_(options.device),
      thread_([this](std::stop_token stop) { run(stop); }) {}

OutputSupervisor::~OutputSupervisor() {
    thread_.request_stop();
    wakeup_.notify();
    thread_.join();
}

Result<std::unique_ptr<OutputSupervisor>> OutputSupervisor::start(const OutputSupervisorOptions& options,
                                                                  PlaybackRenderer& renderer,
                                                                  SharedPlaybackState& shared,
                                                                  WakeupSignal& decoder_wakeup) {
    std::unique_ptr<OutputSupervisor> supervisor(new OutputSupervisor(options, renderer, shared, decoder_wakeup));
    if (auto initialized = supervisor->run_on_output_thread([&supervisor] { return supervisor->initialize(); });
        !initialized) {
        return std::unexpected(initialized.error());
    }
    return supervisor;
}

// ---------------------------------------------------------------- control-thread requests

template <class Work>
std::invoke_result_t<Work&> OutputSupervisor::run_on_output_thread(Work work) {
    std::packaged_task<std::invoke_result_t<Work&>()> task(std::move(work));
    auto outcome = task.get_future();
    {
        const std::scoped_lock lock(requests_mutex_);
        requests_.emplace_back([&task] { task(); });
    }
    wakeup_.notify();
    return outcome.get();  // the task lives in this frame until the output thread has run it
}

Result<std::vector<OutputDeviceInfo>> OutputSupervisor::list_devices() {
    return run_on_output_thread([this] { return context_->list_devices(); });
}

Result<void> OutputSupervisor::select(const OutputDeviceId& device) {
    return run_on_output_thread([this, &device]() -> Result<void> {
        if (auto switched = switch_to(device); !switched) {
            return switched;
        }
        selected_ = device;
        return {};
    });
}

Result<void> OutputSupervisor::restart() {
    return run_on_output_thread([this] {
        // Set before the new configuration is published, so the decoder thread takes it with it.
        shared_.resume_on_restore.store(true, std::memory_order_release);
        auto reopened = reopen_selected();
        if (!reopened) {
            // A later automatic re-open must not start playback by itself.
            shared_.resume_on_restore.store(false, std::memory_order_release);
        }
        return reopened;
    });
}

void OutputSupervisor::on_device_lost() noexcept {
    // output_available stays as the output thread last set it; snapshots and the decoder thread
    // treat an endpoint as usable only while device_lost is clear.
    shared_.device_lost.store(true, std::memory_order_release);
    decoder_wakeup_.notify();
    wakeup_.notify();
}

// ---------------------------------------------------------------- output thread

void OutputSupervisor::run(std::stop_token stop) {
    while (!stop.stop_requested()) {
        try {
            run_requests();
            retry_if_due();
        } catch (...) {
            // The thread body is an exception barrier. Out of memory or a backend exception during a
            // re-open counts as a failed attempt; a request is never lost (it stays queued until it
            // has run, and its own exceptions go to its caller). The thread keeps serving and retrying.
            if (shared_.device_lost.load(std::memory_order_acquire)) {
                next_attempt_ = std::chrono::steady_clock::now() + options_.retry.delay_after(++failed_attempts_);
            }
        }
        wakeup_.wait_for(time_until_next_wake());
    }
    // Closed on this thread: the backend's per-thread setup is undone where it was made.
    device_.reset();
    context_.reset();
}

void OutputSupervisor::run_requests() {
    for (;;) {
        std::packaged_task<void()> request;
        {
            const std::scoped_lock lock(requests_mutex_);
            if (requests_.empty()) {
                return;
            }
            request = std::move(requests_.front());
            requests_.pop_front();
        }
        request();  // a packaged task stores its result or exception for the waiting caller
    }
}

void OutputSupervisor::retry_if_due() {
    if (!shared_.device_lost.load(std::memory_order_acquire)) {
        failed_attempts_ = 0;
        next_attempt_.reset();
        return;
    }
    const auto now = std::chrono::steady_clock::now();
    if (!next_attempt_) {
        next_attempt_ = now + options_.retry.delay_after(0);
        return;
    }
    if (now < *next_attempt_) {
        return;
    }
    if (reopen_selected()) {
        failed_attempts_ = 0;
        next_attempt_.reset();
    } else {
        next_attempt_ = now + options_.retry.delay_after(++failed_attempts_);
    }
}

std::chrono::microseconds OutputSupervisor::time_until_next_wake() const {
    if (!next_attempt_) {
        return kIdleWait;
    }
    const auto remaining =
        std::chrono::duration_cast<std::chrono::microseconds>(*next_attempt_ - std::chrono::steady_clock::now());
    return std::clamp<std::chrono::microseconds>(remaining, std::chrono::microseconds{0}, kIdleWait);
}

Result<void> OutputSupervisor::initialize() {
    auto context = OutputContext::create(options_.backend);
    if (!context) {
        return std::unexpected(context.error());
    }
    context_ = std::move(*context);
    // An endpoint that cannot be opened now is recorded as lost and retried on the schedule; the
    // engine stays usable, so this is not a failure of the engine.
    (void)reopen_selected();
    return {};
}

Result<void> OutputSupervisor::switch_to(const OutputDeviceId& device) {
    if (shared_.device_lost.load(std::memory_order_acquire)) {
        device_.reset();  // a lost endpoint plays nothing and is no fallback: close it first
    }
    auto opened = OutputDevice::open(*context_, device, options_.device_settings, renderer_, *this);
    if (!opened) {
        if (!device_) {
            mark_device_lost();
        }
        return std::unexpected(opened.error());  // a working current endpoint keeps playing
    }
    // The renderer must never run on two callbacks: stop the current endpoint (keeping it open as a
    // fallback) before the new one starts.
    if (device_) {
        device_->stop();
    }
    shared_.device_lost.store(false, std::memory_order_release);  // no callback runs now
    if (auto started = (*opened)->start(); !started) {
        if (!device_ || !device_->start()) {
            device_.reset();
            mark_device_lost();
        }
        return started;
    }
    device_ = std::move(*opened);
    publish_device();
    return {};
}

Result<void> OutputSupervisor::reopen_selected() {
    auto reopened = switch_to(selected_);
    if (!reopened && !selected_.is_system_default()) {
        // The chosen endpoint is gone (unplugged headphones): the system default plays meanwhile.
        // The selection stays, so the next selection or restart tries the chosen endpoint first.
        reopened = switch_to(OutputDeviceId{});
    }
    return reopened;
}

void OutputSupervisor::publish_device() {
    shared_.output_facts.store(device_->facts());
    shared_.output_available.store(true, std::memory_order_release);
    shared_.output_configuration.fetch_add(1, std::memory_order_acq_rel);
    decoder_wakeup_.notify();
}

void OutputSupervisor::mark_device_lost() noexcept {
    shared_.output_available.store(false, std::memory_order_release);
    shared_.device_lost.store(true, std::memory_order_release);
    decoder_wakeup_.notify();
}

}  // namespace mso::audio::detail
