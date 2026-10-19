#pragma once
/// @file
/// Cache-line size used to keep data written by different threads on separate lines.
#include <cstddef>
#include <new>

namespace mso::audio::detail {

/// Alignment that keeps two objects from sharing a cache line (false sharing). The value comes from
/// the standard library (std::hardware_destructive_interference_size, which the libc++ of the
/// verified MSYS2 CLANG64 toolchain ships); a standard library without it falls back to 64 bytes,
/// the line size of every x86-64 and the common ARM64 cores this application targets.
#ifdef __cpp_lib_hardware_interference_size
inline constexpr std::size_t kCacheLineBytes = std::hardware_destructive_interference_size;
#else
inline constexpr std::size_t kCacheLineBytes = 64;
#endif

}  // namespace mso::audio::detail
