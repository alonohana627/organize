/// @file
/// POSIX implementation of the worker OS layer: fork + exec with close-on-exec pipes, every
/// other descriptor closed in the child, RLIMIT_AS and nice applied in the child, SIGTERM then
/// SIGKILL for forced termination and waitpid for exit detection.
#ifndef _WIN32

#include <fcntl.h>
#include <pthread.h>
#include <signal.h>
#include <sys/resource.h>
#include <sys/syscall.h>
#include <sys/wait.h>
#include <unistd.h>

#include <algorithm>
#include <cerrno>
#include <cstring>
#include <format>
#include <mutex>
#include <thread>
#include <utility>

#include "mso/analysis/worker_platform.hpp"

namespace mso::worker {

namespace {

using Clock = std::chrono::steady_clock;

/// Niceness of the worker: background analysis yields to the UI and audio threads.
constexpr int kWorkerNiceness = 10;
/// Exit status the child uses when it cannot set itself up or exec the worker.
constexpr int kExecFailedStatus = 127;
/// Polling period of a timed wait for the child's exit (waitpid has no timeout).
constexpr std::chrono::milliseconds kExitPollPeriod{10};

std::unexpected<Error> errno_failure(ErrorCode code, std::string_view action, int error, bool retryable = false) {
    return fail(code, std::format("{} failed ({})", action, std::strerror(error)), retryable);
}

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

/// In the forked child: makes `source` the descriptor `target`, without close-on-exec.
/// Async-signal-safe.
bool redirect(int source, int target) noexcept {
    if (source == target) return ::fcntl(target, F_SETFD, 0) == 0;
    return ::dup2(source, target) == target;
}

/// In the forked child: closes every descriptor above the standard streams. Async-signal-safe.
void close_other_descriptors(long descriptor_limit) noexcept {
#ifdef SYS_close_range
    if (::syscall(SYS_close_range, 3u, ~0u, 0u) == 0) return;
#endif
    for (long descriptor = 3; descriptor < descriptor_limit; ++descriptor)
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
        int status = 0;
        while (::waitpid(process_id_, &status, 0) < 0 && errno == EINTR) {
        }
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

LimitsStatus posix_limits(const WorkerOptions& options) {
    LimitsStatus status;
    status.memory_limit_applied = true;
    status.memory_limit_bytes = options.memory_limit_bytes;
    if (options.cpu_rate_percent > 0) {
        status.detail = std::format(
            "a hard CPU-rate cap is unavailable on this platform; the worker runs at nice {} "
            "instead of a {}% cap",
            kWorkerNiceness, options.cpu_rate_percent);
    }
    return status;
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
    // Make the rename itself durable.
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
    int child_input[2];
    int child_output[2];
    if (::pipe2(child_input, O_CLOEXEC) != 0)
        return errno_failure(ErrorCode::WorkerCrash, "creating a worker pipe", errno);
    UniqueHandle input_read(child_input[0]);
    UniqueHandle input_write(child_input[1]);
    if (::pipe2(child_output, O_CLOEXEC) != 0)
        return errno_failure(ErrorCode::WorkerCrash, "creating a worker pipe", errno);
    UniqueHandle output_read(child_output[0]);
    UniqueHandle output_write(child_output[1]);
    UniqueHandle null_device(::open("/dev/null", O_WRONLY | O_CLOEXEC));
    if (!null_device) return errno_failure(ErrorCode::WorkerCrash, "opening /dev/null", errno);

    // Everything the child needs is prepared before fork: after it only async-signal-safe
    // calls are allowed.
    std::vector<std::string> argument_storage{executable.string()};
    argument_storage.insert(argument_storage.end(), options.arguments.begin(), options.arguments.end());
    std::vector<char*> argument_pointers;
    for (auto& argument : argument_storage)
        argument_pointers.push_back(argument.data());
    argument_pointers.push_back(nullptr);
    const std::string working_directory = executable.parent_path().string();
    const rlimit memory_limit{static_cast<rlim_t>(options.memory_limit_bytes),
                              static_cast<rlim_t>(options.memory_limit_bytes)};
    const long descriptor_limit = std::max(::sysconf(_SC_OPEN_MAX), 1024L);

    const pid_t process_id = ::fork();
    if (process_id < 0) return errno_failure(ErrorCode::WorkerCrash, "starting the worker process", errno);
    if (process_id == 0) {
        if (!redirect(input_read.get(), STDIN_FILENO) || !redirect(output_write.get(), STDOUT_FILENO) ||
            !redirect(null_device.get(), STDERR_FILENO)) {
            ::_exit(kExecFailedStatus);
        }
        close_other_descriptors(descriptor_limit);
        if (::setrlimit(RLIMIT_AS, &memory_limit) != 0) ::_exit(kExecFailedStatus);
        ::setpriority(PRIO_PROCESS, 0, kWorkerNiceness);
        if (::chdir(working_directory.c_str()) != 0) ::_exit(kExecFailedStatus);
        ::execv(argument_pointers[0], argument_pointers.data());
        ::_exit(kExecFailedStatus);
    }
    // The child ends close here, so end of stream on output_read means the worker is gone.
    return std::make_unique<PosixChild>(process_id, std::move(input_write), std::move(output_read),
                                        posix_limits(options), options.shutdown_grace);
}

}  // namespace mso::worker

#endif  // !_WIN32
