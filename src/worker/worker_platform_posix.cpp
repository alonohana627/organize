/// @file
/// POSIX implementation of the worker OS layer: fork + exec with close-on-exec pipes, every
/// other descriptor closed in the child, signals reset, a parent-death signal on Linux, an
/// RLIMIT_AS soft limit and nice applied in the child, a close-on-exec error pipe that reports
/// any setup failure in the child back to spawn(), SIGTERM then SIGKILL for forced termination,
/// and waitpid for exit detection.
#ifndef _WIN32

#include <fcntl.h>
#include <pthread.h>
#include <signal.h>
#include <sys/resource.h>
#include <sys/syscall.h>
#include <sys/wait.h>
#include <unistd.h>
#ifdef __linux__
#include <sys/prctl.h>
#endif

#include <algorithm>
#include <array>
#include <cerrno>
#include <cstring>
#include <format>
#include <mutex>
#include <thread>
#include <utility>
#include <vector>

#include "mso/worker/detail/worker_platform.hpp"

// Sanitizer runtimes reserve terabytes of address space up front, so an address-space limit makes
// an instrumented worker fail at startup. Supervisor and worker are built with the same flags.
#if defined(__SANITIZE_ADDRESS__) || defined(__SANITIZE_THREAD__)
#define MSO_WORKER_SANITIZED 1
#elif defined(__has_feature)
#if __has_feature(address_sanitizer) || __has_feature(thread_sanitizer) || __has_feature(memory_sanitizer)
#define MSO_WORKER_SANITIZED 1
#endif
#endif

namespace mso::worker {

namespace {

using Clock = std::chrono::steady_clock;

/// Niceness of the worker: background analysis yields to the UI and audio threads.
constexpr int kWorkerNiceness = 10;
/// Exit status the child uses when it cannot set itself up or exec the worker.
constexpr int kExecFailedStatus = 127;
/// Polling period of a timed wait for the child's exit (waitpid has no timeout).
constexpr std::chrono::milliseconds kExitPollPeriod{10};
/// How long the destructor waits for a SIGKILLed child to be reaped before it gives up (a process
/// in uninterruptible sleep on a dead network share can take arbitrarily long).
constexpr std::chrono::milliseconds kReapWait{2000};
/// The descriptor the error pipe occupies in the child while it sets itself up.
constexpr int kChildErrorDescriptor = 3;

/// A failure naming `action` and the errno text.
std::unexpected<Error> errno_failure(ErrorCode code, std::string_view action, int error, bool retryable = false) {
    return fail(code, std::format("{} failed ({})", action, std::strerror(error)), retryable);
}

/// Maps a file-system errno to the error taxonomy.
ErrorCode file_error_code(int error) {
    switch (error) {
        case ENOSPC:
        case EDQUOT:
            return ErrorCode::DiskFull;
        case EACCES:
        case EPERM:
        case EROFS:
            return ErrorCode::AccessDenied;
        case ENOENT:
        case ENOTDIR:
            return ErrorCode::FileMissing;
        default:
            return ErrorCode::Internal;
    }
}

/// A file-system failure of `action`, classified by file_error_code().
std::unexpected<Error> file_failure(std::string_view action, int error) {
    return fail(file_error_code(error), std::format("{} the file failed ({})", action, std::strerror(error)));
}

/// Blocks SIGPIPE in the calling thread for its lifetime, so writing to a closed pipe fails with
/// EPIPE instead of killing the process, and swallows a SIGPIPE that the write left pending.
class ScopedSigpipeBlock {
public:
    ScopedSigpipeBlock() {
        sigemptyset(&sigpipe_);
        sigaddset(&sigpipe_, SIGPIPE);
        sigset_t pending;
        sigpending(&pending);
        already_pending_ = sigismember(&pending, SIGPIPE) == 1;
        pthread_sigmask(SIG_BLOCK, &sigpipe_, &previous_);
    }
    ScopedSigpipeBlock(const ScopedSigpipeBlock&) = delete;
    ScopedSigpipeBlock& operator=(const ScopedSigpipeBlock&) = delete;
    ~ScopedSigpipeBlock() {
        sigset_t pending;
        sigpending(&pending);
        if (!already_pending_ && sigismember(&pending, SIGPIPE) == 1) {
            const timespec no_wait{};
            sigtimedwait(&sigpipe_, nullptr, &no_wait);
        }
        pthread_sigmask(SIG_SETMASK, &previous_, nullptr);
    }

private:
    sigset_t sigpipe_{};
    sigset_t previous_{};
    bool already_pending_ = false;
};

/// Creates `file`, writes every byte and fsyncs it.
Result<void> write_and_sync(const std::filesystem::path& file, std::span<const std::uint8_t> bytes) {
    UniqueHandle descriptor(::open(file.c_str(), O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0644));
    if (!descriptor) return file_failure("creating", errno);
    while (!bytes.empty()) {
        const auto written = ::write(descriptor.get(), bytes.data(), bytes.size());
        if (written < 0 && errno == EINTR) continue;
        if (written < 0) return file_failure("writing", errno);
        bytes = bytes.subspan(static_cast<std::size_t>(written));
    }
    if (::fsync(descriptor.get()) != 0) return file_failure("flushing", errno);
    return {};
}

/// Moves `descriptor` above the standard streams (keeping close-on-exec). When the supervisor
/// runs with fd 0, 1 or 2 closed, a fresh pipe can land there, and the child's dup2 onto the
/// standard streams would then overwrite one pipe end with another.
Result<void> raise_above_standard_streams(UniqueHandle& descriptor) {
    if (descriptor.get() > STDERR_FILENO) return {};
    const int raised = ::fcntl(descriptor.get(), F_DUPFD_CLOEXEC, STDERR_FILENO + 1);
    if (raised < 0) return errno_failure(ErrorCode::WorkerCrash, "moving a worker pipe descriptor", errno);
    descriptor.reset(raised);
    return {};
}

/// Collects killed children whose exit the destructor could not wait for. A SIGKILLed process
/// that is stuck in uninterruptible I/O exits only when that I/O returns; until then it cannot be
/// reaped, and blocking a supervisor thread on it would hang the application. Every later spawn
/// retries without blocking, so such children do not stay zombies once they finally exit.
class AbandonedChildren {
public:
    /// Remembers a killed child that could not be reaped yet.
    static void add(pid_t process_id) {
        std::lock_guard lock(mutex());
        pending().push_back(process_id);
    }

    /// Reaps every remembered child that has exited, without blocking.
    static void reap() {
        std::lock_guard lock(mutex());
        std::erase_if(pending(), [](pid_t process_id) {
            int status = 0;
            const pid_t reaped = ::waitpid(process_id, &status, WNOHANG);
            return reaped == process_id || (reaped < 0 && errno == ECHILD);
        });
    }

private:
    /// Guards pending().
    static std::mutex& mutex() {
        static std::mutex instance;
        return instance;
    }
    /// Children still waiting to be reaped.
    static std::vector<pid_t>& pending() {
        static std::vector<pid_t> instance;
        return instance;
    }
};

/// Why a forked child could not become the worker, sent over the error pipe.
enum class SetupStep : int { Redirect = 1, ParentDeathSignal, MemoryLimit, WorkingDirectory, Exec };

/// What the child writes to the error pipe before giving up.
struct SetupFailure {
    SetupStep step = SetupStep::Exec;
    int error = 0;  ///< errno
};

/// In the forked child: reports `step` and errno over the error pipe `descriptor` and exits.
/// Async-signal-safe.
[[noreturn]] void fail_setup(int descriptor, SetupStep step) noexcept {
    const SetupFailure failure{step, errno};
    // A short or failed write leaves the parent with a partial report or EOF; it then reports a
    // generic start failure, or (EOF) sees the 127 exit status before any handshake.
    [[maybe_unused]] const auto written = ::write(descriptor, &failure, sizeof failure);
    ::_exit(kExecFailedStatus);
}

/// In the forked child: makes `source` the descriptor `target`, without close-on-exec.
/// Async-signal-safe.
bool redirect(int source, int target) noexcept {
    if (source == target) return ::fcntl(target, F_SETFD, 0) == 0;
    return ::dup2(source, target) == target;
}

/// In the forked child: closes every descriptor from `first` up. Async-signal-safe.
void close_descriptors_from(int first, long descriptor_limit) noexcept {
#ifdef SYS_close_range
    if (::syscall(SYS_close_range, static_cast<unsigned>(first), ~0u, 0u) == 0) return;
#endif
    for (long descriptor = first; descriptor < descriptor_limit; ++descriptor)
        ::close(static_cast<int>(descriptor));
}

/// The POSIX child: one forked and exec'd process, reaped through waitpid.
class PosixChild final : public platform::ChildProcess {
public:
    PosixChild(pid_t process_id, UniqueHandle to_child, UniqueHandle from_child, LimitsStatus limits,
               std::chrono::milliseconds kill_grace)
        : process_id_(process_id),
          to_child_(std::move(to_child)),
          from_child_(std::move(from_child)),
          limits_(std::move(limits)),
          kill_grace_(kill_grace) {}

    ~PosixChild() override {
        std::lock_guard lock(mutex_);
        reap();
        if (exit_status_) return;
        ::kill(process_id_, SIGKILL);
        const auto deadline = Clock::now() + kReapWait;
        while (!exit_status_ && Clock::now() < deadline) {
            std::this_thread::sleep_for(kExitPollPeriod);
            reap();
        }
        if (!exit_status_) AbandonedChildren::add(process_id_);
    }

    NativeHandle to_child() const override { return to_child_.get(); }
    NativeHandle from_child() const override { return from_child_.get(); }
    std::uint32_t process_id() const override { return static_cast<std::uint32_t>(process_id_); }
    const LimitsStatus& limits() const override { return limits_; }

    void terminate() override {
        std::lock_guard lock(mutex_);
        reap();
        if (exit_status_ || sigterm_sent_at_) return;
        ::kill(process_id_, SIGTERM);
        sigterm_sent_at_ = Clock::now();
    }

    void poll() override {
        std::lock_guard lock(mutex_);
        reap();
        escalate();
    }

    bool memory_limit_hit() const override { return false; }

    std::optional<std::uint32_t> wait_for_exit(std::chrono::milliseconds timeout) override {
        const auto deadline = Clock::now() + timeout;
        while (true) {
            {
                std::lock_guard lock(mutex_);
                reap();
                escalate();
                if (exit_status_) return exit_status_;
            }
            const auto now = Clock::now();
            if (now >= deadline) return std::nullopt;
            std::this_thread::sleep_for(std::min<Clock::duration>(kExitPollPeriod, deadline - now));
        }
    }

    void register_reader_thread() override {}
    void cancel_blocked_read() override {}

private:
    /// Collects the exit status if the child has ended. Requires mutex_.
    void reap() {
        if (exit_status_) return;
        int status = 0;
        const pid_t reaped = ::waitpid(process_id_, &status, WNOHANG);
        if (reaped == process_id_) {
            exit_status_ = WIFSIGNALED(status) ? 128u + static_cast<std::uint32_t>(WTERMSIG(status))
                                               : static_cast<std::uint32_t>(WEXITSTATUS(status));
        } else if (reaped < 0 && errno == ECHILD) {
            exit_status_ = 255u;  // reaped elsewhere; the status is lost
        }
    }

    /// Sends SIGKILL once SIGTERM has gone unanswered for kill_grace_. Requires mutex_.
    void escalate() {
        if (exit_status_ || !sigterm_sent_at_ || sigkill_sent_) return;
        if (Clock::now() - *sigterm_sent_at_ < kill_grace_) return;
        ::kill(process_id_, SIGKILL);
        sigkill_sent_ = true;
    }

    const pid_t process_id_;
    UniqueHandle to_child_;
    UniqueHandle from_child_;
    const LimitsStatus limits_;
    const std::chrono::milliseconds kill_grace_;
    std::mutex mutex_;
    std::optional<std::uint32_t> exit_status_;
    std::optional<Clock::time_point> sigterm_sent_at_;
    bool sigkill_sent_ = false;
};

/// The address-space limit to set in the child: the requested cap as the soft limit, clamped to
/// the current hard limit (which an unprivileged process cannot raise, and which a lower
/// `ulimit -v` may already have set). The hard limit itself is left alone.
struct MemoryLimitPlan {
    bool apply = false;
    rlimit limit{};
    std::string detail;  ///< why the limit differs from the request, if it does
};

/// Decides the RLIMIT_AS soft limit for a requested cap (see MemoryLimitPlan).
MemoryLimitPlan plan_memory_limit(std::uint64_t requested_bytes) {
    MemoryLimitPlan plan;
#ifdef MSO_WORKER_SANITIZED
    (void)requested_bytes;  // unused: sanitizer builds run without the cap
    plan.detail = "the address-space limit is disabled in sanitizer builds";
#else
    rlimit current{};
    if (::getrlimit(RLIMIT_AS, &current) != 0) {
        plan.detail = std::format("the address-space limit is unavailable ({})", std::strerror(errno));
        return plan;
    }
    rlim_t soft = static_cast<rlim_t>(requested_bytes);
    if (current.rlim_max != RLIM_INFINITY && soft > current.rlim_max) {
        soft = current.rlim_max;
        plan.detail = std::format("the memory cap was lowered to the inherited hard limit of {}",
                                  format_byte_size(static_cast<std::int64_t>(soft)));
    }
    plan.apply = true;
    plan.limit = rlimit{soft, current.rlim_max};
#endif
    return plan;
}

/// The confinement a POSIX worker runs under, with the reasons for any shortfall.
LimitsStatus posix_limits(const WorkerOptions& options, const MemoryLimitPlan& memory) {
    LimitsStatus status;
    status.memory_limit_applied = memory.apply;
    status.memory_limit_bytes = memory.apply ? static_cast<std::uint64_t>(memory.limit.rlim_cur) : 0;
#ifdef __linux__
    status.parent_death_signal = true;
#endif
    std::vector<std::string> notes;
    if (!memory.detail.empty()) notes.push_back(memory.detail);
    if (options.cpu_rate_percent > 0) {
        notes.push_back(std::format(
            "a hard CPU-rate cap is unavailable on this platform; the worker runs at nice {} instead of a {}% cap",
            kWorkerNiceness, options.cpu_rate_percent));
    }
    for (const auto& note : notes)
        status.detail += status.detail.empty() ? note : "; " + note;
    return status;
}

/// Turns a setup failure the child reported into the spawn error.
std::unexpected<Error> setup_failure(const SetupFailure& failure) {
    switch (failure.step) {
        case SetupStep::MemoryLimit:
            return errno_failure(ErrorCode::ResourceLimit, "applying the worker memory limit", failure.error);
        case SetupStep::ParentDeathSignal:
            return errno_failure(ErrorCode::ResourceLimit, "binding the worker to the supervisor's lifetime",
                                 failure.error);
        case SetupStep::Redirect:
            return errno_failure(ErrorCode::WorkerCrash, "connecting the worker's standard streams", failure.error);
        case SetupStep::WorkingDirectory:
            return errno_failure(ErrorCode::WorkerCrash, "entering the worker directory", failure.error);
        case SetupStep::Exec:
            return errno_failure(ErrorCode::WorkerCrash, "executing the worker", failure.error);
    }
    return errno_failure(ErrorCode::WorkerCrash, "starting the worker", failure.error);
}

/// Waits (bounded by the child's immediate _exit) for a child that reported a setup failure.
void reap_failed_child(pid_t process_id) {
    int status = 0;
    while (::waitpid(process_id, &status, 0) < 0 && errno == EINTR) {
    }
}

}  // namespace

// ---------------------------------------------------------------- shared plumbing

bool UniqueHandle::valid() const noexcept {
    return handle_ >= 0;
}

void UniqueHandle::reset(NativeHandle handle) noexcept {
    if (valid()) ::close(handle_);
    handle_ = handle;
}

Result<std::size_t> HandleStream::read_some(std::span<std::uint8_t> destination) {
    while (true) {
        const auto received = ::read(handle_, destination.data(), destination.size());
        if (received >= 0) return static_cast<std::size_t>(received);
        if (errno != EINTR) return errno_failure(ErrorCode::WorkerCrash, "reading the worker channel", errno, true);
    }
}

Result<void> HandleStream::write_all(std::span<const std::uint8_t> bytes) {
    const ScopedSigpipeBlock no_sigpipe;
    while (!bytes.empty()) {
        const auto written = ::write(handle_, bytes.data(), bytes.size());
        if (written < 0 && errno == EINTR) continue;
        if (written < 0) return errno_failure(ErrorCode::WorkerCrash, "writing the worker channel", errno, true);
        bytes = bytes.subspan(static_cast<std::size_t>(written));
    }
    return {};
}

NativeHandle standard_input_handle() noexcept {
    return STDIN_FILENO;
}

NativeHandle standard_output_handle() noexcept {
    return STDOUT_FILENO;
}

std::uint32_t current_process_id() noexcept {
    return static_cast<std::uint32_t>(::getpid());
}

std::filesystem::path default_worker_executable() {
    std::error_code error;
    const auto self = std::filesystem::read_symlink("/proc/self/exe", error);
    if (error) return std::filesystem::path(kWorkerExecutableName);
    return self.parent_path() / kWorkerExecutableName;
}

Result<void> write_file_atomically(const std::filesystem::path& destination, std::span<const std::uint8_t> bytes) {
    auto temporary = destination;
    temporary += ".partial";
    auto written = write_and_sync(temporary, bytes);
    if (written && ::rename(temporary.c_str(), destination.c_str()) != 0) written = file_failure("publishing", errno);
    if (!written) {
        ::unlink(temporary.c_str());
        return written;
    }
    // Make the rename itself durable. A failure here leaves a complete file that may not survive
    // a power loss; the caller's checksum validation catches that case on the next read.
    const auto directory = destination.has_parent_path() ? destination.parent_path() : std::filesystem::path(".");
    if (UniqueHandle directory_descriptor(::open(directory.c_str(), O_RDONLY | O_DIRECTORY | O_CLOEXEC));
        directory_descriptor) {
        ::fsync(directory_descriptor.get());
    }
    return {};
}

// ---------------------------------------------------------------- ChildProcess::spawn

Result<std::unique_ptr<platform::ChildProcess>> platform::ChildProcess::spawn(const std::filesystem::path& executable,
                                                                              const WorkerOptions& options) {
    AbandonedChildren::reap();

    std::array<int, 2> child_input{};
    std::array<int, 2> child_output{};
    std::array<int, 2> setup_errors{};
    if (::pipe2(child_input.data(), O_CLOEXEC) != 0)
        return errno_failure(ErrorCode::WorkerCrash, "creating a worker pipe", errno);
    UniqueHandle input_read(child_input[0]);
    UniqueHandle input_write(child_input[1]);
    if (::pipe2(child_output.data(), O_CLOEXEC) != 0)
        return errno_failure(ErrorCode::WorkerCrash, "creating a worker pipe", errno);
    UniqueHandle output_read(child_output[0]);
    UniqueHandle output_write(child_output[1]);
    if (::pipe2(setup_errors.data(), O_CLOEXEC) != 0)
        return errno_failure(ErrorCode::WorkerCrash, "creating the worker setup pipe", errno);
    UniqueHandle errors_read(setup_errors[0]);
    UniqueHandle errors_write(setup_errors[1]);
    UniqueHandle null_device(::open("/dev/null", O_WRONLY | O_CLOEXEC));
    if (!null_device) return errno_failure(ErrorCode::WorkerCrash, "opening /dev/null", errno);
    for (UniqueHandle* child_side : {&input_read, &output_write, &null_device, &errors_write}) {
        if (auto raised = raise_above_standard_streams(*child_side); !raised) return std::unexpected(raised.error());
    }

    // Everything the child needs is prepared before fork: after it only async-signal-safe
    // calls are allowed.
    std::vector<std::string> argument_storage{executable.string()};
    argument_storage.insert(argument_storage.end(), options.arguments.begin(), options.arguments.end());
    if (options.self_test) argument_storage.emplace_back("--self-test");
    std::vector<char*> argument_pointers;
    for (auto& argument : argument_storage)
        argument_pointers.push_back(argument.data());
    argument_pointers.push_back(nullptr);
    const std::string working_directory = executable.parent_path().string();
    const MemoryLimitPlan memory = plan_memory_limit(options.memory_limit_bytes);
    const long descriptor_limit = std::max(::sysconf(_SC_OPEN_MAX), 1024L);
    sigset_t no_signals;
    sigemptyset(&no_signals);
    struct sigaction default_action{};
    default_action.sa_handler = SIG_DFL;
    sigemptyset(&default_action.sa_mask);
#ifdef __linux__
    const pid_t supervisor_process_id = ::getpid();
#endif

    const pid_t process_id = ::fork();
    if (process_id < 0) return errno_failure(ErrorCode::WorkerCrash, "starting the worker process", errno);
    if (process_id == 0) {
        // A clean signal state: the supervisor's mask and ignored signals would otherwise survive
        // exec (Qt and the audio backend block or ignore several of them).
        ::sigprocmask(SIG_SETMASK, &no_signals, nullptr);
        for (int signal_number = 1; signal_number < NSIG; ++signal_number) {
            if (signal_number != SIGKILL && signal_number != SIGSTOP)
                ::sigaction(signal_number, &default_action, nullptr);  // EINVAL for reserved numbers is fine
        }
        // Every child-side descriptor is above 2 (raise_above_standard_streams), so these cannot
        // overwrite one another.
        if (!redirect(input_read.get(), STDIN_FILENO) || !redirect(output_write.get(), STDOUT_FILENO) ||
            !redirect(null_device.get(), STDERR_FILENO)) {
            fail_setup(errors_write.get(), SetupStep::Redirect);
        }
        // The error pipe moves to a fixed descriptor (still close-on-exec), so it survives closing
        // everything else and disappears exactly when exec succeeds.
        if (::dup2(errors_write.get(), kChildErrorDescriptor) != kChildErrorDescriptor ||
            ::fcntl(kChildErrorDescriptor, F_SETFD, FD_CLOEXEC) != 0) {
            fail_setup(errors_write.get(), SetupStep::Redirect);
        }
        close_descriptors_from(kChildErrorDescriptor + 1, descriptor_limit);
#ifdef __linux__
        // The worker dies with the supervisor even if the supervisor crashes. The signal follows
        // the forking thread, so supervisors spawn from a thread that outlives the worker; the
        // getppid check closes the race with a supervisor that died before the prctl.
        if (::prctl(PR_SET_PDEATHSIG, SIGKILL) != 0) fail_setup(kChildErrorDescriptor, SetupStep::ParentDeathSignal);
        if (::getppid() != supervisor_process_id) {
            errno = ESRCH;
            fail_setup(kChildErrorDescriptor, SetupStep::ParentDeathSignal);
        }
#endif
        if (memory.apply && ::setrlimit(RLIMIT_AS, &memory.limit) != 0)
            fail_setup(kChildErrorDescriptor, SetupStep::MemoryLimit);
        // Best effort: an unprivileged process can always lower its own priority, and a worker at
        // normal priority is still correct.
        ::setpriority(PRIO_PROCESS, 0, kWorkerNiceness);
        if (::chdir(working_directory.c_str()) != 0) fail_setup(kChildErrorDescriptor, SetupStep::WorkingDirectory);
        ::execv(argument_pointers[0], argument_pointers.data());
        fail_setup(kChildErrorDescriptor, SetupStep::Exec);
    }

    // The child ends close here (the parent keeps only its own ends), so end of stream on
    // output_read means the worker is gone and end of stream on errors_read means exec succeeded.
    input_read.reset();
    output_write.reset();
    null_device.reset();
    errors_write.reset();
    SetupFailure failure{};
    ssize_t received = 0;
    do {
        received = ::read(errors_read.get(), &failure, sizeof failure);
    } while (received < 0 && errno == EINTR);
    if (received != 0) {
        reap_failed_child(process_id);
        if (received == static_cast<ssize_t>(sizeof failure)) return setup_failure(failure);
        return errno_failure(ErrorCode::WorkerCrash, "starting the worker process", received < 0 ? errno : EIO);
    }
    return std::make_unique<PosixChild>(process_id, std::move(input_write), std::move(output_read),
                                        posix_limits(options, memory), options.shutdown_grace);
}

}  // namespace mso::worker

#endif  // !_WIN32
