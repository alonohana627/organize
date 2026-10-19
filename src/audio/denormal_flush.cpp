#include "mso/audio/detail/denormal_flush.hpp"

#if defined(__x86_64__) || defined(_M_X64) || defined(__i386__)
#include <pmmintrin.h>
#include <xmmintrin.h>
#endif

namespace mso::audio::detail {

#if defined(__x86_64__) || defined(_M_X64) || defined(__i386__)

ScopedDenormalFlush::ScopedDenormalFlush() noexcept : previous_mode_(_mm_getcsr()) {
    _MM_SET_FLUSH_ZERO_MODE(_MM_FLUSH_ZERO_ON);
    _MM_SET_DENORMALS_ZERO_MODE(_MM_DENORMALS_ZERO_ON);
}

ScopedDenormalFlush::~ScopedDenormalFlush() {
    _mm_setcsr(static_cast<unsigned int>(previous_mode_));
}

#elif defined(__aarch64__)

namespace {
/// FPCR.FZ: flush denormal inputs and outputs to zero.
constexpr unsigned long long kFlushToZeroBit = 1ull << 24;
}  // namespace

ScopedDenormalFlush::ScopedDenormalFlush() noexcept {
    unsigned long long control = 0;
    asm volatile("mrs %0, fpcr" : "=r"(control));
    previous_mode_ = control;
    control |= kFlushToZeroBit;
    asm volatile("msr fpcr, %0" : : "r"(control));
}

ScopedDenormalFlush::~ScopedDenormalFlush() {
    asm volatile("msr fpcr, %0" : : "r"(previous_mode_));
}

#else

ScopedDenormalFlush::ScopedDenormalFlush() noexcept = default;
ScopedDenormalFlush::~ScopedDenormalFlush() = default;

#endif

}  // namespace mso::audio::detail
