#pragma once
/// @file
/// Internal OS layer behind WorkerProcess (not part of the module API). One implementation per
/// platform: worker_platform_win32.cpp (CreateProcessW + job object) and
/// worker_platform_posix.cpp (fork + exec + rlimit). Everything above this interface is
/// platform-neutral.
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <optional>

#include "mso/analysis/worker_process.hpp"
#include "mso/domain/error.hpp"

namespace mso::worker::platform {

/// A spawned, confined worker process and the supervisor ends of its stdin/stdout pipes.
/// Thread-safe. Destruction kills the process if it is still alive and releases it.
class ChildProcess {
public:
    /// Starts `executable` with `options.arguments` (already validated as plain tokens), the
    /// executable's directory as working directory, only the two pipe ends inherited and stderr
    /// sent to the null device, confined by `options`' memory and CPU limits.
    static Result<std::unique_ptr<ChildProcess>> spawn(const std::filesystem::path& executable,
                                                       const WorkerOptions& options);

    virtual ~ChildProcess() = default;

    /// Supervisor end of the child's stdin.
    virtual NativeHandle to_child() const = 0;
    /// Supervisor end of the child's stdout.
    virtual NativeHandle from_child() const = 0;
    virtual std::uint32_t process_id() const = 0;
    virtual const LimitsStatus& limits() const = 0;

    /// Forced termination. Windows kills the job at once; POSIX sends SIGTERM and escalates to
    /// SIGKILL after shutdown_grace (from poll() or wait_for_exit()). Idempotent.
    virtual void terminate() = 0;
    /// Periodic housekeeping from the supervisor's watchdog: termination escalation and
    /// limit-violation notifications.
    virtual void poll() = 0;
    /// True once the OS reported that the memory cap stopped an allocation (Windows only; on
    /// POSIX the worker reports RESOURCE_LIMIT itself).
    virtual bool memory_limit_hit() const = 0;
    /// Waits up to `timeout` (zero polls) and returns the exit status once the process ended.
    virtual std::optional<std::uint32_t> wait_for_exit(std::chrono::milliseconds timeout) = 0;

    /// Remembers the calling thread as the one blocked reading from_child().
    virtual void register_reader_thread() = 0;
    /// Aborts that blocking read. Needed on Windows when another process inherited the pipe and
    /// keeps it open after the worker died; a no-op on POSIX, where every descriptor is
    /// close-on-exec and the pipe cannot outlive the worker that way.
    virtual void cancel_blocked_read() = 0;
};

}  // namespace mso::worker::platform
