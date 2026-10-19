#pragma once
/// @file
/// Flush-to-zero / denormals-are-zero for the calling thread.

namespace mso::audio::detail {

/// While alive, the calling thread treats denormal floats as zero (x86-64: MXCSR FTZ+DAZ; ARM64:
/// FPCR.FZ), and restores the previous mode on destruction. Decaying filter and phase-vocoder
/// tails otherwise produce denormals that cost 10-100× per operation on the decoder thread.
/// miniaudio does the same for its device thread, but its helper is internal to its implementation
/// unit, so this thin wrapper over the compiler intrinsics stands in for the decoder thread.
/// On other architectures it does nothing.
class ScopedDenormalFlush {
public:
    ScopedDenormalFlush() noexcept;
    ~ScopedDenormalFlush();
    ScopedDenormalFlush(const ScopedDenormalFlush&) = delete;
    ScopedDenormalFlush& operator=(const ScopedDenormalFlush&) = delete;

private:
    unsigned long long previous_mode_ = 0;  ///< control register value to restore
};

}  // namespace mso::audio::detail
