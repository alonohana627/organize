#pragma once
/// @file
/// Background supervisor of the durable analysis queue: leases one job at a time from the open
/// libraries (oldest run first, rotating libraries), runs it in the mso-worker child process,
/// renews the lease while the worker is alive, fences the result and publishes it through the
/// catalog writer. Never starts work on its own: only jobs of explicit user runs exist.
#include <atomic>
#include <condition_variable>
#include <filesystem>
#include <functional>
#include <memory>
#include <mutex>
#include <stop_token>
#include <string>
#include <thread>
#include <vector>

#include "mso/catalog/library.hpp"

namespace mso::app {

class AnalysisSupervisor {
public:
    /// Supplies the libraries to serve and resolves a sample's playable path.
    struct Hooks {
        std::function<std::vector<std::shared_ptr<catalog::Library>>()> libraries;
        std::function<Result<std::filesystem::path>(catalog::Library&, std::int64_t sample_id)> resolve_path;
        std::function<void(catalog::Library&, const std::string& worker_status)> on_progress;
    };

    AnalysisSupervisor(std::filesystem::path worker_executable, Hooks hooks);
    ~AnalysisSupervisor();
    AnalysisSupervisor(const AnalysisSupervisor&) = delete;
    AnalysisSupervisor& operator=(const AnalysisSupervisor&) = delete;

    void set_paused(bool paused) { paused_.store(paused); }
    bool paused() const { return paused_.load(); }
    /// Wakes the loop immediately (after a new run was created or resumed).
    void notify();

private:
    void run(std::stop_token stop);
    /// Runs one leased job to a published or recorded outcome.
    void process(catalog::Library& library, const catalog::Library::JobLease& lease, std::stop_token stop);

    std::filesystem::path worker_executable_;
    Hooks hooks_;
    std::atomic<bool> paused_{false};
    std::mutex wake_mutex_;
    std::condition_variable_any wake_;
    bool wake_requested_ = false;
    struct WorkerSlot;
    std::unique_ptr<WorkerSlot> worker_;
    std::jthread thread_;
};

}  // namespace mso::app
