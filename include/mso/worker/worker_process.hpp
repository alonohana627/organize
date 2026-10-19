#pragma once
/// @file
/// Supervisor-side client of one mso-worker child process (docs/02_ARCHITECTURE.md section 3,
/// docs/04_CONTRACTS.md section 4), plus the small OS plumbing the worker shares with it.
///
/// The child talks over two anonymous pipes (its stdin and stdout); no other handle or file
/// descriptor is inherited. No TCP or HTTP is involved anywhere. Confinement per platform:
///   Windows  CreateProcessW into a job object: killed with the supervisor, per-process memory cap,
///            hard CPU-rate cap where supported, one process only, below-normal priority.
///   POSIX    fork + exec: RLIMIT_AS soft limit and nice 10 applied in the child, every signal
///            reset to its default, and on Linux a parent-death SIGKILL; no hard CPU-rate cap
///            exists, which limits_status() reports. Forced termination is SIGTERM, then SIGKILL
///            after shutdown_grace.
///
/// Process management is hand-written on both platforms. A library such as reproc (MIT) covers
/// spawning, pipes and killing, but not the confinement (job object, rlimits, explicit handle
/// list, parent-death signal) that is most of this code, so it would add a dependency without
/// removing the platform layer.
///
/// Hang detection works at two levels: the lease (no frame at all for lease_deadline means the
/// whole process is frozen) and the task stall deadline (heartbeats keep arriving but the task's
/// work counter has not moved for task_stall_deadline, so one decoder call or file read is
/// stuck). A stalled task gets a cooperative Cancel, and the worker is terminated if that does
/// not end the task within cancel_grace; either way the task fails as a retryable WORKER_CRASH.
///
/// No OS header leaks from here: native handles are carried as NativeHandle.
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

#include "mso/domain/error.hpp"
#include "mso/worker/worker_protocol.hpp"

namespace mso::worker {

// ---------------------------------------------------------------- OS plumbing (both sides)

#ifdef _WIN32
using NativeHandle = void*;  ///< Win32 HANDLE
inline constexpr NativeHandle kNoNativeHandle = nullptr;
inline constexpr std::string_view kWorkerExecutableName = "mso-worker.exe";
#else
using NativeHandle = int;  ///< POSIX file descriptor
inline constexpr NativeHandle kNoNativeHandle = -1;
inline constexpr std::string_view kWorkerExecutableName = "mso-worker";
#endif

/// Owns one native handle (Win32 HANDLE or POSIX file descriptor) and closes it exactly once.
/// On Windows both null and INVALID_HANDLE_VALUE count as empty, because Win32 APIs use either
/// to report failure.
class UniqueHandle {
public:
    UniqueHandle() = default;
    explicit UniqueHandle(NativeHandle handle) noexcept : handle_(handle) {}
    UniqueHandle(UniqueHandle&& other) noexcept : handle_(other.release()) {}
    UniqueHandle& operator=(UniqueHandle&& other) noexcept;
    UniqueHandle(const UniqueHandle&) = delete;
    UniqueHandle& operator=(const UniqueHandle&) = delete;
    ~UniqueHandle() { reset(); }

    /// The handle, still owned.
    NativeHandle get() const noexcept { return handle_; }
    /// Whether a handle is held.
    bool valid() const noexcept;
    explicit operator bool() const noexcept { return valid(); }
    /// Gives up ownership without closing.
    NativeHandle release() noexcept;
    /// Closes the current handle (if any) and takes ownership of `handle`.
    void reset(NativeHandle handle = kNoNativeHandle) noexcept;

private:
    NativeHandle handle_ = kNoNativeHandle;
};

/// Blocking byte stream over a pipe or file it does not own. A broken pipe reads as end of
/// stream; on POSIX a write to a closed pipe fails with an error instead of raising SIGPIPE.
class HandleStream final : public ByteSource, public ByteSink {
public:
    explicit HandleStream(NativeHandle handle) noexcept : handle_(handle) {}

    Result<std::size_t> read_some(std::span<std::uint8_t> destination) override;
    Result<void> write_all(std::span<const std::uint8_t> bytes) override;

private:
    NativeHandle handle_;
};

/// The calling process's standard input (not owned).
NativeHandle standard_input_handle() noexcept;
/// The calling process's standard output (not owned).
NativeHandle standard_output_handle() noexcept;

/// The operating system's identifier of the calling process.
std::uint32_t current_process_id() noexcept;

/// The worker executable (kWorkerExecutableName) in the directory of the running executable. When
/// that directory cannot be determined, the bare name, which WorkerProcess resolves against the
/// current directory.
std::filesystem::path default_worker_executable();

/// Writes `bytes` so that `destination` only ever holds a complete file: a sibling ".partial"
/// file is written and flushed to stable storage, then renamed over the destination. The
/// temporary file is removed on failure. Errors: DISK_FULL, ACCESS_DENIED, FILE_MISSING (no such
/// directory) or INTERNAL.
Result<void> write_file_atomically(const std::filesystem::path& destination, std::span<const std::uint8_t> bytes);

// ---------------------------------------------------------------- supervisor client

/// How to launch and supervise the worker. Defaults follow docs/05_AUDIO_INTELLIGENCE.md
/// section 1 and docs/04_CONTRACTS.md section 4.
struct WorkerOptions {
    /// Empty selects default_worker_executable(). A relative path is resolved against the current
    /// directory once, before the child changes its working directory.
    std::filesystem::path executable;
    /// Extra command-line flags. Each must be a single printable ASCII token without whitespace or
    /// quotes; nothing here passes through a shell.
    std::vector<std::string> arguments;
    /// Runs the synthetic analyzer (`--self-test`) instead of real DSP. Tests only: a handshake
    /// whose self-test flag differs from this option is refused, so a production supervisor never
    /// accepts a self-test worker and a test never gets real analysis by accident.
    bool self_test = false;
    /// Memory cap of the worker process (Windows: committed memory; POSIX: address space).
    std::uint64_t memory_limit_bytes = 2ull * 1024 * 1024 * 1024;
    /// Hard CPU rate cap in percent of all system cycles (Windows only); 0 disables the cap.
    unsigned cpu_rate_percent = 40;
    /// Time allowed from spawn to a valid handshake.
    std::chrono::milliseconds handshake_timeout{10'000};
    /// The worker is considered hung when no frame (heartbeats included) arrives for this long.
    /// At least two heartbeat intervals.
    std::chrono::milliseconds lease_deadline{15'000};
    /// A task is considered stalled when its work counter (carried by heartbeats) and its
    /// progress have not moved for this long, even though heartbeats keep arriving. At least two
    /// heartbeat intervals; one unit of work is one decoded block or one hashed MiB, so a healthy
    /// task advances many times per heartbeat even on a slow network share.
    std::chrono::milliseconds task_stall_deadline{60'000};
    /// After a cancel request (by the caller or for a stalled task), time allowed for the task's
    /// outcome before forced termination.
    std::chrono::milliseconds cancel_grace{5'000};
    /// Time allowed for a clean exit after Shutdown (and on POSIX after SIGTERM) before the
    /// process is killed.
    std::chrono::milliseconds shutdown_grace{2'000};
};

/// Which confinement actually took effect. A missing CPU cap is not fatal: the worker still
/// runs at lowered priority, and `detail` explains the fallback for the UI.
struct LimitsStatus {
    bool kill_on_job_close = false;    ///< Windows job object kills the worker with the supervisor
    bool parent_death_signal = false;  ///< Linux: the worker receives SIGKILL when the supervisor dies
    bool memory_limit_applied = false;
    std::uint64_t memory_limit_bytes = 0;  ///< the cap in force (POSIX: clamped to the hard limit)
    bool cpu_rate_cap_applied = false;
    unsigned cpu_rate_percent = 0;
    std::string detail;  ///< empty when every requested limit is in force
};

/// The worker process ended. Emitted once per spawned process, after the outcome of any task
/// that was still running (which is reported as TaskFailed with the same reason).
struct WorkerExited {
    /// Why the process ended: WORKER_CRASH (crash, hang, stalled task, protocol violation),
    /// RESOURCE_LIMIT (memory cap hit) or CANCELLED (killed after an unacknowledged cancel).
    /// nullopt for an expected exit after shutdown().
    std::optional<Error> failure;
    /// Exit status; on POSIX a death by signal N is reported as 128 + N.
    std::uint32_t exit_code = 0;
};

/// Everything the supervisor learns from the worker, in arrival order. Heartbeats are consumed
/// internally to renew the lease and track task progress, and are not delivered.
using WorkerEvent = std::variant<Progress, TaskResult, TaskFailed, WorkerExited>;

/// What the supervisor knows about the task in flight, for status displays and lease decisions.
struct TaskActivity {
    uuids::uuid job_id;
    std::uint64_t work_units = 0;                          ///< the task's work counter as last reported
    std::chrono::steady_clock::duration since_progress{};  ///< time since the counter last moved
    bool cancelling = false;                               ///< a cancel (requested, or for a stall) is pending
};

/// One supervised worker process. Thread-safe: any thread may submit, cancel or wait for events
/// concurrently, except that restart() must not race with other calls.
///
/// Fencing done here: an outcome is delivered only when its fence equals that of the task in
/// flight; a successful result that arrives after cancel() becomes TaskFailed(CANCELLED), and
/// after a stall cancel TaskFailed(WORKER_CRASH, retryable). Every task submitted while the
/// worker runs ends in exactly one TaskResult or TaskFailed, at the latest task_stall_deadline +
/// cancel_grace after its work counter stopped moving. The caller still checks the delivered
/// fence against the catalog before publishing anything.
class WorkerProcess {
public:
    /// Spawns the worker, confines it and completes the handshake. Fails with WORKER_CRASH when
    /// the child cannot start or never completes a valid handshake (wrong process, protocol or
    /// algorithm versions, self-test mode not as requested), RESOURCE_LIMIT when the mandatory
    /// confinement cannot be applied and INVALID_ARGUMENT for bad options.
    static Result<std::unique_ptr<WorkerProcess>> start(WorkerOptions options);

    /// Shuts the worker down within shutdown_grace, then kills it.
    ~WorkerProcess();
    WorkerProcess(const WorkerProcess&) = delete;
    WorkerProcess& operator=(const WorkerProcess&) = delete;

    /// The worker's self-description from the current process's handshake.
    Handshake handshake() const;
    /// The confinement in force for the current process.
    LimitsStatus limits_status() const;
    /// Operating-system id of the current process; 0 when none runs.
    std::uint32_t process_id() const;
    /// True while the current process is alive and accepts tasks.
    bool is_running() const;
    /// True while a submitted task has no delivered outcome yet.
    bool is_busy() const;
    /// Heartbeats received from the current process.
    std::uint64_t heartbeats_received() const;
    /// The task in flight and how recently it made progress; nullopt when idle.
    std::optional<TaskActivity> task_activity() const;

    /// Sends one task. Fails with INVALID_ARGUMENT for an invalid task or while another task is
    /// in flight, and WORKER_CRASH when the worker is not running.
    Result<void> submit(const AnalyzeTask& task);

    /// Requests cooperative cancellation of the task in flight. If its outcome does not arrive
    /// within cancel_grace the worker is terminated and the task reported as CANCELLED.
    /// NOT_FOUND when `job_id` is not the task in flight.
    Result<void> cancel(const uuids::uuid& job_id, std::int64_t cancel_epoch);

    /// Waits up to `timeout` for the next event; nullopt on timeout.
    std::optional<WorkerEvent> next_event(std::chrono::milliseconds timeout);

    /// Graceful stop: sends Shutdown and waits up to shutdown_grace before terminating. A task
    /// in flight is reported as CANCELLED. Idempotent.
    void shutdown();

    /// Stops the current process (if any) and spawns a fresh one with the same options.
    Result<void> restart();

private:
    class Session;     ///< one spawned child process and its I/O threads
    class EventQueue;  ///< bounded, thread-safe queue of WorkerEvent

    explicit WorkerProcess(WorkerOptions options);

    WorkerOptions options_;
    std::unique_ptr<EventQueue> events_;
    std::unique_ptr<Session> session_;
};

}  // namespace mso::worker
