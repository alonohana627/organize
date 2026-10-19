/// @file
/// Win32 implementation of the worker OS layer: CreateProcessW with an explicit inherited-handle
/// list, a job object for lifetime, memory and CPU confinement, and synchronous pipe I/O.
#ifdef _WIN32

#include <windows.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <format>
#include <mutex>
#include <utility>

#include "mso/analysis/worker_platform.hpp"

namespace mso::worker {

namespace {

/// Exit code of a worker the supervisor kills.
constexpr UINT kTerminatedExitCode = 0xDEAD;
/// Anonymous pipe buffer size hint.
constexpr DWORD kPipeBufferBytes = 64 * 1024;
/// Largest single ReadFile/WriteFile request.
constexpr std::size_t kMaxTransferBytes = 1u << 20;

std::unexpected<Error> win32_failure(ErrorCode code, std::string_view action, DWORD error, bool retryable = false) {
    return fail(code, std::format("{} failed (Win32 error {})", action, error), retryable);
}

DWORD wait_milliseconds(std::chrono::milliseconds duration) {
    return static_cast<DWORD>(std::clamp<std::int64_t>(duration.count(), 0, INFINITE - 1));
}

ErrorCode file_error_code(DWORD error) {
    switch (error) {
        case ERROR_DISK_FULL:
        case ERROR_HANDLE_DISK_FULL:
            return ErrorCode::DiskFull;
        case ERROR_ACCESS_DENIED:
        case ERROR_SHARING_VIOLATION:
            return ErrorCode::AccessDenied;
        case ERROR_FILE_NOT_FOUND:
        case ERROR_PATH_NOT_FOUND:
            return ErrorCode::FileMissing;
        default:
            return ErrorCode::Internal;
    }
}

std::unexpected<Error> file_failure(std::string_view action, DWORD error) {
    return fail(file_error_code(error), std::format("{} the file failed (Win32 error {})", action, error));
}

/// Creates `file`, writes every byte and flushes it to stable storage.
Result<void> write_and_flush(const std::filesystem::path& file, std::span<const std::uint8_t> bytes) {
    UniqueHandle handle(
        CreateFileW(file.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr));
    if (!handle) return file_failure("creating", GetLastError());
    while (!bytes.empty()) {
        const auto chunk = static_cast<DWORD>(std::min(bytes.size(), kMaxTransferBytes));
        DWORD written = 0;
        if (!WriteFile(handle.get(), bytes.data(), chunk, &written, nullptr))
            return file_failure("writing", GetLastError());
        bytes = bytes.subspan(written);
    }
    if (!FlushFileBuffers(handle.get())) return file_failure("flushing", GetLastError());
    return {};
}

// ---------------------------------------------------------------- job object

/// The job object a worker runs in, the completion port that reports its limit violations, and
/// which limits took effect.
struct ConfinedJob {
    UniqueHandle job;
    UniqueHandle completion_port;
    LimitsStatus status;
};

/// Applies the hard CPU rate cap and verifies it by reading it back. Failure is recorded in
/// `status`, not returned: the worker may still run, uncapped, at below-normal priority.
void apply_cpu_rate_cap(HANDLE job, unsigned percent, LimitsStatus& status) {
    if (percent == 0) return;
    JOBOBJECT_CPU_RATE_CONTROL_INFORMATION requested{};
    requested.ControlFlags = JOB_OBJECT_CPU_RATE_CONTROL_ENABLE | JOB_OBJECT_CPU_RATE_CONTROL_HARD_CAP;
    requested.CpuRate = percent * 100;  // in hundredths of a percent of all system cycles
    if (!SetInformationJobObject(job, JobObjectCpuRateControlInformation, &requested, sizeof requested)) {
        status.detail = std::format(
            "CPU rate cap of {}% unavailable (Win32 error {}); the worker runs uncapped at "
            "below-normal priority",
            percent, GetLastError());
        return;
    }
    JOBOBJECT_CPU_RATE_CONTROL_INFORMATION in_force{};
    const bool readable =
        QueryInformationJobObject(job, JobObjectCpuRateControlInformation, &in_force, sizeof in_force, nullptr);
    if (!readable || (in_force.ControlFlags & JOB_OBJECT_CPU_RATE_CONTROL_HARD_CAP) == 0 ||
        in_force.CpuRate != requested.CpuRate) {
        status.detail = std::format(
            "CPU rate cap of {}% was accepted but is not in force; the worker runs uncapped at "
            "below-normal priority",
            percent);
        return;
    }
    status.cpu_rate_cap_applied = true;
    status.cpu_rate_percent = percent;
}

/// Creates the job object: killed with its last handle, one process only (the worker never
/// spawns), below-normal priority, no error dialogs, a per-process memory cap and, where the
/// system supports it, a hard CPU rate cap.
Result<ConfinedJob> create_job(const WorkerOptions& options) {
    ConfinedJob confined;
    confined.job.reset(CreateJobObjectW(nullptr, nullptr));
    if (!confined.job) return win32_failure(ErrorCode::ResourceLimit, "creating the worker job object", GetLastError());

    confined.completion_port.reset(CreateIoCompletionPort(INVALID_HANDLE_VALUE, nullptr, 0, 1));
    if (!confined.completion_port) {
        return win32_failure(ErrorCode::ResourceLimit, "creating the job completion port", GetLastError());
    }
    JOBOBJECT_ASSOCIATE_COMPLETION_PORT association{};
    association.CompletionKey = confined.job.get();
    association.CompletionPort = confined.completion_port.get();
    if (!SetInformationJobObject(confined.job.get(), JobObjectAssociateCompletionPortInformation, &association,
                                 sizeof association)) {
        return win32_failure(ErrorCode::ResourceLimit, "attaching the job completion port", GetLastError());
    }

    JOBOBJECT_EXTENDED_LIMIT_INFORMATION limits{};
    limits.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE | JOB_OBJECT_LIMIT_PROCESS_MEMORY |
                                              JOB_OBJECT_LIMIT_ACTIVE_PROCESS | JOB_OBJECT_LIMIT_PRIORITY_CLASS |
                                              JOB_OBJECT_LIMIT_DIE_ON_UNHANDLED_EXCEPTION;
    limits.BasicLimitInformation.ActiveProcessLimit = 1;
    limits.BasicLimitInformation.PriorityClass = BELOW_NORMAL_PRIORITY_CLASS;
    limits.ProcessMemoryLimit = static_cast<SIZE_T>(options.memory_limit_bytes);
    if (!SetInformationJobObject(confined.job.get(), JobObjectExtendedLimitInformation, &limits, sizeof limits)) {
        return win32_failure(ErrorCode::ResourceLimit, "applying the worker job limits", GetLastError());
    }
    confined.status.kill_on_job_close = true;
    confined.status.memory_limit_applied = true;
    confined.status.memory_limit_bytes = options.memory_limit_bytes;

    apply_cpu_rate_cap(confined.job.get(), options.cpu_rate_percent, confined.status);
    return confined;
}

// ---------------------------------------------------------------- process creation

/// Both ends of an anonymous pipe.
struct Pipe {
    UniqueHandle read_end;
    UniqueHandle write_end;
};

/// Which end of a pipe the child receives.
enum class ChildEnd { Read, Write };

/// Creates a pipe whose child end is inheritable and whose supervisor end is not.
Result<Pipe> create_pipe(ChildEnd child_end) {
    SECURITY_ATTRIBUTES inheritable{sizeof(SECURITY_ATTRIBUTES), nullptr, TRUE};
    HANDLE read_end = nullptr;
    HANDLE write_end = nullptr;
    if (!CreatePipe(&read_end, &write_end, &inheritable, kPipeBufferBytes)) {
        return win32_failure(ErrorCode::WorkerCrash, "creating a worker pipe", GetLastError());
    }
    Pipe pipe{UniqueHandle(read_end), UniqueHandle(write_end)};
    HANDLE supervisor_end = child_end == ChildEnd::Read ? write_end : read_end;
    if (!SetHandleInformation(supervisor_end, HANDLE_FLAG_INHERIT, 0)) {
        return win32_failure(ErrorCode::WorkerCrash, "protecting the supervisor pipe end", GetLastError());
    }
    return pipe;
}

/// PROC_THREAD_ATTRIBUTE_HANDLE_LIST naming the only handles the child may inherit, so a
/// concurrently spawned process never receives another worker's pipes.
class InheritedHandleList {
public:
    InheritedHandleList() = default;
    InheritedHandleList(const InheritedHandleList&) = delete;
    InheritedHandleList& operator=(const InheritedHandleList&) = delete;
    ~InheritedHandleList() {
        if (initialized_) DeleteProcThreadAttributeList(list());
    }

    Result<void> initialize(std::span<const HANDLE> handles) {
        handles_.assign(handles.begin(), handles.end());
        SIZE_T size = 0;
        InitializeProcThreadAttributeList(nullptr, 1, 0, &size);
        storage_.resize(size);
        if (!InitializeProcThreadAttributeList(list(), 1, 0, &size)) {
            return win32_failure(ErrorCode::WorkerCrash, "preparing the inherited handle list", GetLastError());
        }
        initialized_ = true;
        // The attribute list keeps a pointer to handles_, which therefore lives as long as it.
        if (!UpdateProcThreadAttribute(list(), 0, PROC_THREAD_ATTRIBUTE_HANDLE_LIST, handles_.data(),
                                       handles_.size() * sizeof(HANDLE), nullptr, nullptr)) {
            return win32_failure(ErrorCode::WorkerCrash, "restricting inherited handles", GetLastError());
        }
        return {};
    }

    LPPROC_THREAD_ATTRIBUTE_LIST list() { return reinterpret_cast<LPPROC_THREAD_ATTRIBUTE_LIST>(storage_.data()); }

private:
    std::vector<HANDLE> handles_;
    std::vector<std::byte> storage_;
    bool initialized_ = false;
};

/// Quoted executable followed by the flags. Flags are single tokens, so no further quoting
/// rules apply, and the command line never passes through a shell.
std::wstring build_command_line(const std::filesystem::path& executable, const std::vector<std::string>& arguments) {
    std::wstring command_line = L"\"" + executable.native() + L"\"";
    for (const auto& argument : arguments) {
        command_line += L' ';
        command_line += path_from_utf8(argument).native();
    }
    return command_line;
}

/// The Win32 child: a process alone in its job object.
class Win32Child final : public platform::ChildProcess {
public:
    Win32Child(ConfinedJob job, UniqueHandle process, std::uint32_t process_id, UniqueHandle to_child,
               UniqueHandle from_child)
        : job_(std::move(job.job)),
          completion_port_(std::move(job.completion_port)),
          limits_(std::move(job.status)),
          process_(std::move(process)),
          process_id_(process_id),
          to_child_(std::move(to_child)),
          from_child_(std::move(from_child)) {}

    ~Win32Child() override { TerminateJobObject(job_.get(), kTerminatedExitCode); }

    NativeHandle to_child() const override { return to_child_.get(); }
    NativeHandle from_child() const override { return from_child_.get(); }
    std::uint32_t process_id() const override { return process_id_; }
    const LimitsStatus& limits() const override { return limits_; }

    void terminate() override { TerminateJobObject(job_.get(), kTerminatedExitCode); }

    void poll() override {
        DWORD message = 0;
        ULONG_PTR key = 0;
        LPOVERLAPPED overlapped = nullptr;
        while (GetQueuedCompletionStatus(completion_port_.get(), &message, &key, &overlapped, 0)) {
            if (message == JOB_OBJECT_MSG_PROCESS_MEMORY_LIMIT || message == JOB_OBJECT_MSG_JOB_MEMORY_LIMIT) {
                memory_limit_hit_ = true;
            }
        }
    }

    bool memory_limit_hit() const override { return memory_limit_hit_; }

    std::optional<std::uint32_t> wait_for_exit(std::chrono::milliseconds timeout) override {
        if (WaitForSingleObject(process_.get(), wait_milliseconds(timeout)) != WAIT_OBJECT_0) return std::nullopt;
        DWORD exit_code = 0;
        GetExitCodeProcess(process_.get(), &exit_code);
        return static_cast<std::uint32_t>(exit_code);
    }

    void register_reader_thread() override {
        HANDLE duplicate = nullptr;
        if (DuplicateHandle(GetCurrentProcess(), GetCurrentThread(), GetCurrentProcess(), &duplicate, 0, FALSE,
                            DUPLICATE_SAME_ACCESS)) {
            std::lock_guard lock(mutex_);
            reader_thread_.reset(duplicate);
        }
    }

    void cancel_blocked_read() override {
        std::lock_guard lock(mutex_);
        if (reader_thread_) CancelSynchronousIo(reader_thread_.get());
    }

private:
    UniqueHandle job_;
    UniqueHandle completion_port_;
    const LimitsStatus limits_;
    UniqueHandle process_;
    const std::uint32_t process_id_;
    UniqueHandle to_child_;
    UniqueHandle from_child_;
    std::atomic<bool> memory_limit_hit_{false};
    std::mutex mutex_;
    UniqueHandle reader_thread_;
};

}  // namespace

// ---------------------------------------------------------------- shared plumbing

bool UniqueHandle::valid() const noexcept {
    return handle_ != nullptr && handle_ != INVALID_HANDLE_VALUE;
}

void UniqueHandle::reset(NativeHandle handle) noexcept {
    if (valid()) CloseHandle(handle_);
    handle_ = handle;
}

Result<std::size_t> HandleStream::read_some(std::span<std::uint8_t> destination) {
    if (destination.empty()) return std::size_t{0};
    const auto request = static_cast<DWORD>(std::min(destination.size(), kMaxTransferBytes));
    DWORD received = 0;
    if (!ReadFile(handle_, destination.data(), request, &received, nullptr)) {
        const DWORD error = GetLastError();
        if (error == ERROR_BROKEN_PIPE || error == ERROR_HANDLE_EOF) return std::size_t{0};
        return win32_failure(ErrorCode::WorkerCrash, "reading the worker channel", error, true);
    }
    return std::size_t{received};
}

Result<void> HandleStream::write_all(std::span<const std::uint8_t> bytes) {
    while (!bytes.empty()) {
        const auto request = static_cast<DWORD>(std::min(bytes.size(), kMaxTransferBytes));
        DWORD written = 0;
        if (!WriteFile(handle_, bytes.data(), request, &written, nullptr)) {
            return win32_failure(ErrorCode::WorkerCrash, "writing the worker channel", GetLastError(), true);
        }
        bytes = bytes.subspan(written);
    }
    return {};
}

NativeHandle standard_input_handle() noexcept {
    return GetStdHandle(STD_INPUT_HANDLE);
}

NativeHandle standard_output_handle() noexcept {
    return GetStdHandle(STD_OUTPUT_HANDLE);
}

std::uint32_t current_process_id() noexcept {
    return GetCurrentProcessId();
}

std::filesystem::path default_worker_executable() {
    constexpr std::size_t kLongestPath = 32768;
    std::wstring module_path(MAX_PATH, L'\0');
    while (module_path.size() <= kLongestPath) {
        const DWORD length = GetModuleFileNameW(nullptr, module_path.data(), static_cast<DWORD>(module_path.size()));
        if (length == 0) break;
        if (length < module_path.size()) {
            module_path.resize(length);
            return std::filesystem::path(module_path).parent_path() / kWorkerExecutableName;
        }
        module_path.resize(module_path.size() * 2);
    }
    return std::filesystem::path(kWorkerExecutableName);
}

Result<void> write_file_atomically(const std::filesystem::path& destination, std::span<const std::uint8_t> bytes) {
    auto temporary = destination;
    temporary += L".partial";
    auto written = write_and_flush(temporary, bytes);
    if (written &&
        !MoveFileExW(temporary.c_str(), destination.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
        written = file_failure("publishing", GetLastError());
    }
    if (!written) DeleteFileW(temporary.c_str());
    return written;
}

// ---------------------------------------------------------------- ChildProcess::spawn

/// Starts the worker suspended, places it in its job before it runs a single instruction, then
/// resumes it. Only the two pipe ends and a NUL stderr are inherited.
Result<std::unique_ptr<platform::ChildProcess>> platform::ChildProcess::spawn(const std::filesystem::path& executable,
                                                                              const WorkerOptions& options) {
    auto job = create_job(options);
    if (!job) return std::unexpected(job.error());
    auto child_input = create_pipe(ChildEnd::Read);
    if (!child_input) return std::unexpected(child_input.error());
    auto child_output = create_pipe(ChildEnd::Write);
    if (!child_output) return std::unexpected(child_output.error());

    SECURITY_ATTRIBUTES inheritable{sizeof(SECURITY_ATTRIBUTES), nullptr, TRUE};
    UniqueHandle null_device(CreateFileW(L"NUL", GENERIC_READ | GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE,
                                         &inheritable, OPEN_EXISTING, 0, nullptr));
    if (!null_device) return win32_failure(ErrorCode::WorkerCrash, "opening the NUL device", GetLastError());

    const std::array<HANDLE, 3> inherited_handles{child_input->read_end.get(), child_output->write_end.get(),
                                                  null_device.get()};
    InheritedHandleList inherited;
    if (auto ready = inherited.initialize(inherited_handles); !ready) return std::unexpected(ready.error());

    STARTUPINFOEXW startup{};
    startup.StartupInfo.cb = sizeof startup;
    startup.StartupInfo.dwFlags = STARTF_USESTDHANDLES;
    startup.StartupInfo.hStdInput = child_input->read_end.get();
    startup.StartupInfo.hStdOutput = child_output->write_end.get();
    startup.StartupInfo.hStdError = null_device.get();
    startup.lpAttributeList = inherited.list();

    auto command_line = build_command_line(executable, options.arguments);
    const auto working_directory = executable.parent_path();
    PROCESS_INFORMATION created{};
    if (!CreateProcessW(executable.c_str(), command_line.data(), nullptr, nullptr, TRUE,
                        CREATE_SUSPENDED | CREATE_NO_WINDOW | EXTENDED_STARTUPINFO_PRESENT | CREATE_UNICODE_ENVIRONMENT,
                        nullptr, working_directory.c_str(), &startup.StartupInfo, &created)) {
        return win32_failure(ErrorCode::WorkerCrash, "starting the worker process", GetLastError());
    }
    UniqueHandle process(created.hProcess);
    const UniqueHandle main_thread(created.hThread);
    if (!AssignProcessToJobObject(job->job.get(), process.get())) {
        const DWORD error = GetLastError();
        TerminateProcess(process.get(), kTerminatedExitCode);
        return win32_failure(ErrorCode::ResourceLimit, "confining the worker to its job object", error);
    }
    if (ResumeThread(main_thread.get()) == static_cast<DWORD>(-1)) {
        return win32_failure(ErrorCode::WorkerCrash, "resuming the worker process", GetLastError());
    }
    // The child ends close when the pipes go out of scope, so end of stream on our read end
    // means the worker is gone.
    return std::make_unique<Win32Child>(std::move(*job), std::move(process), created.dwProcessId,
                                        std::move(child_input->write_end), std::move(child_output->read_end));
}

}  // namespace mso::worker

#endif  // _WIN32
