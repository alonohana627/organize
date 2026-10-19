#include "mso/audio/detail/output_supervisor.hpp"

#include <algorithm>
#include <exception>
#include <new>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>

namespace mso::audio::detail {
namespace {

/// Longest sleep without a request, a due retry or a watched endpoint. Requests, device events and
/// losses wake the thread at once; this only bounds the cost of a wake-up absorbed by a race (see
/// WakeupSignal).
constexpr std::chrono::milliseconds kIdleWait{1000};

/// The error a request gets when it cannot be handed to the output thread (retryable when the
/// queue or memory was short).
Error refusal(ErrorCode code, std::string_view detail) {
    return fail(code, std::string(detail), code == ErrorCode::ResourceLimit).error();
}

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
      watchdog_(options.watchdog),
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

// ---------------------------------------------------------------- requests

template <class Work>
std::invoke_result_t<Work&> OutputSupervisor::run_request(Work& work) {
    if (cancelling_requests_) {
        return fail(ErrorCode::Cancelled, "the audio output is shutting down");
    }
    return work();
}

template <class Work>
std::invoke_result_t<Work&> OutputSupervisor::run_on_output_thread(Work work) {
    if (std::this_thread::get_id() == thread_.get_id()) {
        return fail(ErrorCode::InvalidArgument, "a blocking audio device request cannot be made on the output thread");
    }
    std::packaged_task<std::invoke_result_t<Work&>()> task([this, &work] { return run_request(work); });
    auto outcome = task.get_future();
    {
        const std::scoped_lock lock(requests_mutex_);
        if (requests_.size() >= kMaximumQueuedRequests) {
            return std::unexpected(refusal(ErrorCode::ResourceLimit, "too many audio device requests are waiting"));
        }
        requests_.emplace_back([&task] { task(); });
    }
    wakeup_.notify();
    return outcome.get();  // the task lives in this frame until the output thread has run it
}

template <class Value, class Work>
void OutputSupervisor::post_to_output_thread(Work work, DeviceRequestCompletion<Value> on_done) {
    std::optional<Error> refused;
    try {
        // Copies `on_done`, so it is still here to report a refusal.
        std::packaged_task<void()> request([this, work = std::move(work), on_done]() mutable {
            Result<Value> outcome = fail(ErrorCode::Internal, "the audio device request did not run");
            try {
                outcome = run_request(work);
            } catch (const std::bad_alloc&) {
                outcome = fail(ErrorCode::ResourceLimit, "out of memory in an audio device request");
            } catch (...) {
                // Barrier: a request's failure goes to its completion, never into the output thread.
                outcome = fail(ErrorCode::Internal, "an audio device request failed unexpectedly");
            }
            if (on_done) {
                // An exception thrown by the completion ends up in this task's unread future.
                on_done(std::move(outcome));
            }
        });
        const std::scoped_lock lock(requests_mutex_);
        if (requests_.size() >= kMaximumQueuedRequests) {
            refused = refusal(ErrorCode::ResourceLimit, "too many audio device requests are waiting");
        } else {
            requests_.push_back(std::move(request));
        }
    } catch (const std::bad_alloc&) {
        refused = refusal(ErrorCode::ResourceLimit, "out of memory queuing an audio device request");
    } catch (const std::system_error&) {
        refused = refusal(ErrorCode::Internal, "cannot queue an audio device request");
    }
    if (refused) {
        if (on_done) {
            on_done(std::unexpected(std::move(*refused)));
        }
        return;
    }
    wakeup_.notify();
}

Result<std::vector<OutputDeviceInfo>> OutputSupervisor::list_devices() {
    return run_on_output_thread([this] { return list_devices_now(); });
}

Result<void> OutputSupervisor::select(const OutputDeviceId& device) {
    return run_on_output_thread([this, &device] { return select_now(device); });
}

Result<void> OutputSupervisor::restart() {
    return run_on_output_thread([this] { return restart_now(); });
}

void OutputSupervisor::list_devices_async(DeviceRequestCompletion<std::vector<OutputDeviceInfo>> on_done) {
    post_to_output_thread<std::vector<OutputDeviceInfo>>([this] { return list_devices_now(); }, std::move(on_done));
}

void OutputSupervisor::select_async(OutputDeviceId device, DeviceRequestCompletion<void> on_done) {
    post_to_output_thread<void>([this, device = std::move(device)] { return select_now(device); }, std::move(on_done));
}

void OutputSupervisor::restart_async(DeviceRequestCompletion<void> on_done) {
    post_to_output_thread<void>([this] { return restart_now(); }, std::move(on_done));
}

Result<std::vector<OutputDeviceInfo>> OutputSupervisor::list_devices_now() {
    return context_->list_devices();
}

Result<void> OutputSupervisor::select_now(const OutputDeviceId& device) {
    if (auto switched = switch_to(device); !switched) {
        return switched;
    }
    selected_ = device;
    return {};
}

Result<void> OutputSupervisor::restart_now() {
    // Set before the new configuration is published, so the decoder thread takes it with it.
    shared_.resume_on_restore.store(true, std::memory_order_release);
    auto reopened = reopen_selected();
    if (!reopened) {
        // A later automatic re-open must not start playback by itself.
        shared_.resume_on_restore.store(false, std::memory_order_release);
    }
    return reopened;
}

// ---------------------------------------------------------------- device events

void OutputSupervisor::on_device_lost() noexcept {
    // output_available stays as the output thread last set it; snapshots and the decoder thread
    // treat an endpoint as usable only while device_lost is clear.
    shared_.device_lost.store(true, std::memory_order_release);
    decoder_wakeup_.notify();
    wakeup_.notify();
}

void OutputSupervisor::on_output_device_event(OutputDeviceEvent event) noexcept {
    if (event == OutputDeviceEvent::Rerouted) {
        reroute_announced_.store(true, std::memory_order_release);
    }
    // Started and Stopped only prompt a look: the watchdog decides from the endpoint's health.
    wakeup_.notify();
}

// ---------------------------------------------------------------- output thread

void OutputSupervisor::run(std::stop_token stop) {
    while (!stop.stop_requested()) {
        try {
            run_requests(stop);
            retry_if_due();
            watch_device();
        } catch (...) {
            // The thread body is an exception barrier. Out of memory or a backend exception during a
            // re-open counts as a failed attempt; a request is never lost (it stays queued until it
            // has run, and its own exceptions go to its caller). The thread keeps serving and retrying.
            if (shared_.device_lost.load(std::memory_order_acquire)) {
                next_attempt_ = Clock::now() + options_.retry.delay_after(++failed_attempts_);
            }
        }
        wakeup_.wait_for(time_until_next_wake());
    }
    // Requests still waiting are answered CANCELLED, so an asynchronous caller learns that its
    // request will never run; none of them touches the backend any more.
    cancelling_requests_ = true;
    try {
        run_requests(std::stop_token{});
    } catch (...) {
        // Barrier: each request reports through its own future or completion; shutdown goes on.
    }
    // Closed on this thread: the backend's per-thread setup is undone where it was made.
    watchdog_.unwatch();
    device_.reset();
    context_.reset();
}

void OutputSupervisor::run_requests(const std::stop_token& stop) {
    // On shutdown the loop leaves the rest to run() (which answers them CANCELLED) instead of doing
    // device work nobody will use.
    while (!stop.stop_requested()) {
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
        callbacks_at_loss_.reset();
        return;
    }
    watchdog_.unwatch();  // a lost endpoint is not watched again until it is kept or replaced
    const auto now = Clock::now();
    if (!next_attempt_) {
        next_attempt_ = now + options_.retry.delay_after(0);
        callbacks_at_loss_ = device_ ? std::optional(device_->health().callback_count) : std::nullopt;
        return;
    }
    if (now < *next_attempt_) {
        return;
    }
    if (recover_in_place() || reopen_selected()) {
        failed_attempts_ = 0;
        next_attempt_.reset();
        callbacks_at_loss_.reset();
    } else {
        next_attempt_ = now + options_.retry.delay_after(++failed_attempts_);
    }
}

void OutputSupervisor::watch_device() {
    reroute_pending_ = reroute_announced_.exchange(false, std::memory_order_acq_rel) || reroute_pending_;
    if (!device_ || !watchdog_.watching()) {
        return;
    }
    const OutputDeviceHealth health = device_->health();
    if (reroute_pending_ && health.running) {
        // miniaudio has finished moving the endpoint to the new default device and restarted it.
        reroute_pending_ = false;
        adopt_reroute();
    }
    if (watchdog_.lost(health, Clock::now())) {
        watchdog_.unwatch();
        reroute_pending_ = false;
        on_device_lost();
    }
}

std::chrono::microseconds OutputSupervisor::time_until_next_wake() const {
    std::chrono::microseconds wait = kIdleWait;
    if (watchdog_.watching()) {
        wait = std::min<std::chrono::microseconds>(wait, options_.watchdog.check_interval);
    }
    if (next_attempt_) {
        const auto remaining = std::chrono::duration_cast<std::chrono::microseconds>(*next_attempt_ - Clock::now());
        wait = std::clamp<std::chrono::microseconds>(remaining, std::chrono::microseconds{0}, wait);
    }
    return wait;
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
        watchdog_.unwatch();
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
    watchdog_.unwatch();
    if (device_) {
        device_->stop();
    }
    shared_.device_lost.store(false, std::memory_order_release);  // no callback runs now
    if (auto started = (*opened)->start(); !started) {
        // The previous endpoint plays on. Starting it reconfigures the renderer, which drops the
        // audio buffered so far, so it is published like a new endpoint and the decoder re-anchors.
        if (device_ && device_->start()) {
            start_watching();
            publish_device();
        } else {
            device_.reset();
            mark_device_lost();
        }
        return started;
    }
    device_ = std::move(*opened);
    start_watching();
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

bool OutputSupervisor::recover_in_place() {
    if (!device_ || !callbacks_at_loss_ || device_->follows_system_default() != selected_.is_system_default()) {
        return false;
    }
    const OutputDeviceHealth health = device_->health();
    if (!health.running || health.callback_count == *callbacks_at_loss_) {
        return false;
    }
    shared_.device_lost.store(false, std::memory_order_release);
    start_watching();
    publish_device();
    return true;
}

void OutputSupervisor::start_watching() {
    reroute_pending_ = false;
    watchdog_.watch(device_->follows_system_default(), device_->health(), Clock::now());
}

void OutputSupervisor::adopt_reroute() {
    const OutputFacts facts = device_->facts();
    renderer_.update_latency(facts.latency_frames);
    shared_.output_facts.store(facts);
}

void OutputSupervisor::publish_device() {
    shared_.output_facts.store(device_->facts());
    shared_.output_available.store(true, std::memory_order_release);
    shared_.output_configuration.fetch_add(1, std::memory_order_acq_rel);
    decoder_wakeup_.notify();
}

void OutputSupervisor::mark_device_lost() noexcept {
    watchdog_.unwatch();
    shared_.output_available.store(false, std::memory_order_release);
    shared_.device_lost.store(true, std::memory_order_release);
    decoder_wakeup_.notify();
}

}  // namespace mso::audio::detail
