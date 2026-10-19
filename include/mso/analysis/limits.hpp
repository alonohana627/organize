#pragma once
/// @file
/// Numeric bounds shared by the analysis stages, the waveform codec and the worker protocol. Each
/// limit exists exactly once; every consumer (request validation, stored-file validation, the IPC
/// schema) refers to these constants.
#include <cstddef>

namespace mso::analysis {

/// Frames requested from the decoder per read. Bounds the memory of a decode pass and the latency
/// of cancellation, and is the unit of WorkCounter progress.
inline constexpr std::size_t kDecodeBlockFrames = 4096;

/// Largest channel count analyzed; more is reported as UNSUPPORTED_CODEC.
inline constexpr int kMaximumChannels = 64;

/// Largest sample rate accepted from a decoder or a stored waveform overview, in Hz. Generous on
/// purpose: it only rejects nonsense headers, not unusual but real rates such as 384 kHz or DSD.
inline constexpr int kMaximumSampleRate = 10'000'000;

/// Upper bound of AnalysisRequest::max_musical_seconds; keeps the onset envelope bounded (one
/// float per ~11.6 ms hop, so at most about 1.2 MiB).
inline constexpr double kMaximumMusicalSeconds = 3600.0;

/// Largest waveform overview resolution, for requests and for stored overviews alike. At 12 bytes
/// per bin an overview segment stays below 800 KiB.
inline constexpr int kMaximumWaveformBins = 65536;

/// Waveform resolution used when a request does not choose one.
inline constexpr int kDefaultWaveformBins = 2048;

/// Musical-window length used when a request does not choose one, in seconds.
inline constexpr double kDefaultMusicalSeconds = 120.0;

}  // namespace mso::analysis
