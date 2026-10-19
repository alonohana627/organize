/// @file
/// Peak-based chroma accumulation weighted by tonality and stationarity, with tuning estimation.
#include "mso/analysis/chroma_accumulator.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <complex>
#include <numbers>
#include <span>

namespace mso::analysis {
namespace {

constexpr double kFrameSeconds = 8192.0 / 44100.0;  ///< about 186 ms: ~5.4 Hz bins at 44.1 kHz
constexpr double kLowestFrequencyHz = 80.0;
constexpr double kHighestFrequencyHz = 2500.0;
constexpr double kFlatnessForZeroTonality = 0.35;  ///< white noise measures about 0.56
constexpr double kSilentFramePower = 1e-10;        ///< mean band power below -100 dBFS
constexpr float kPeakFloorRatio = 0.01F;           ///< peaks below -40 dB of the frame maximum are ignored
/// A partial must stand 10 dB above the median of the bins 3..6 away on both sides, i.e. just
/// outside a Hann main lobe. Leakage skirts and sidelobe ripple have neighbours at their own level.
constexpr float kPeakProminenceRatio = 3.16F;
constexpr std::size_t kProminenceInnerOffset = 3;
constexpr std::size_t kProminenceOuterOffset = 6;
constexpr double kReferenceFrequencyHz = 440.0;
constexpr double kReferenceMidiNote = 69.0;  ///< A4; MIDI note 60 = C4 so note % 12 == 0 is C

/// Chroma frame length at `sample_rate`: the power of two closest to kFrameSeconds.
std::size_t chroma_frame_size(int sample_rate) {
    return nearest_power_of_two(kFrameSeconds * static_cast<double>(sample_rate), 1024, 65536);
}

/// True when bin `peak` rises kPeakProminenceRatio above the median of its surrounding bins.
bool is_prominent_peak(std::span<const float> magnitudes, std::size_t peak) {
    std::array<float, 2 * (kProminenceOuterOffset - kProminenceInnerOffset + 1)> surroundings{};
    std::size_t count = 0;
    for (std::size_t offset = kProminenceInnerOffset; offset <= kProminenceOuterOffset; ++offset) {
        if (peak >= offset) surroundings[count++] = magnitudes[peak - offset];
        if (peak + offset < magnitudes.size()) surroundings[count++] = magnitudes[peak + offset];
    }
    if (count == 0) return false;
    const auto median = surroundings.begin() + static_cast<std::ptrdiff_t>(count / 2);
    std::nth_element(surroundings.begin(), median, surroundings.begin() + static_cast<std::ptrdiff_t>(count));
    return magnitudes[peak] >= kPeakProminenceRatio * *median;
}

/// Power statistics of the analysis band of one frame.
struct BandPower {
    double mean_power = 0.0;
    double flatness = 1.0;  ///< geometric / arithmetic mean of power: ~0 for tones, ~0.56 for white noise
};

/// Mean power and spectral flatness of bins [first, last).
BandPower measure_band(std::span<const float> magnitudes, std::size_t first, std::size_t last) {
    constexpr double kPowerEpsilon = 1e-20;
    double log_sum = 0.0;
    double power_sum = 0.0;
    for (std::size_t bin = first; bin < last; ++bin) {
        const double power = static_cast<double>(magnitudes[bin]) * magnitudes[bin];
        log_sum += std::log(power + kPowerEpsilon);
        power_sum += power;
    }
    const auto count = static_cast<double>(last - first);
    BandPower band;
    band.mean_power = power_sum / count;
    band.flatness = std::exp(log_sum / count) / (band.mean_power + kPowerEpsilon);
    return band;
}

/// Spectral similarity of two frames over bins [first, last): sum(min) / sum(max), 1 for identical
/// spectra and near 0 when one frame holds a transient the other lacks.
double spectral_similarity(std::span<const float> first_frame, std::span<const float> second_frame, std::size_t first,
                           std::size_t last) {
    double shared = 0.0;
    double total = 0.0;
    for (std::size_t bin = first; bin < last; ++bin) {
        shared += std::min(first_frame[bin], second_frame[bin]);
        total += std::max(first_frame[bin], second_frame[bin]);
    }
    return total > 0.0 ? shared / total : 0.0;
}

/// Offset in bins of the true peak relative to the centre bin, from a parabola through the log magnitudes.
double interpolated_peak_offset(float below, float peak, float above) {
    const double alpha = std::log(std::max(below, 1e-12F));
    const double beta = std::log(std::max(peak, 1e-12F));
    const double gamma = std::log(std::max(above, 1e-12F));
    const double curvature = alpha - 2.0 * beta + gamma;
    if (curvature >= 0.0) return 0.0;
    return std::clamp(0.5 * (alpha - gamma) / curvature, -0.5, 0.5);
}

}  // namespace

ChromaAccumulator::ChromaAccumulator(int sample_rate)
    : spectrogram_(chroma_frame_size(sample_rate), chroma_frame_size(sample_rate) / 2),
      bin_frequency_hz_(static_cast<double>(std::max(sample_rate, 1)) / static_cast<double>(spectrogram_.frame_size())),
      first_bin_(std::max<std::size_t>(1, static_cast<std::size_t>(std::ceil(kLowestFrequencyHz / bin_frequency_hz_)))),
      last_bin_(std::min(spectrogram_.bin_count() - 1,
                         static_cast<std::size_t>(std::floor(kHighestFrequencyHz / bin_frequency_hz_)) + 1)),
      pending_magnitudes_(spectrogram_.bin_count(), 0.0F) {}

void ChromaAccumulator::add(std::span<const float> mono_samples) {
    spectrogram_.add(mono_samples, [this](std::span<const float> magnitudes) { on_frame(magnitudes); });
}

void ChromaAccumulator::on_frame(std::span<const float> magnitudes) {
    if (last_bin_ <= first_bin_ + 2) return;  // sample rate too low for the analysis band

    // The pending frame is accumulated now that both of its neighbours are known.
    const double similarity_to_pending =
        has_pending_frame_ ? spectral_similarity(pending_magnitudes_, magnitudes, first_bin_, last_bin_) : 0.0;
    if (has_pending_frame_) accumulate_pending_frame(std::min(pending_similarity_to_previous_, similarity_to_pending));

    std::copy(magnitudes.begin(), magnitudes.end(), pending_magnitudes_.begin());
    pending_similarity_to_previous_ = similarity_to_pending;
    has_pending_frame_ = true;
}

void ChromaAccumulator::accumulate_pending_frame(double stationarity) {
    const std::span<const float> magnitudes = pending_magnitudes_;
    const BandPower band = measure_band(magnitudes, first_bin_, last_bin_);
    if (band.mean_power < kSilentFramePower) return;
    const double tonality = std::clamp(1.0 - band.flatness / kFlatnessForZeroTonality, 0.0, 1.0);
    const double frame_weight = tonality * stationarity;
    if (frame_weight <= 0.0) return;

    const float frame_maximum = *std::max_element(magnitudes.begin() + static_cast<std::ptrdiff_t>(first_bin_),
                                                  magnitudes.begin() + static_cast<std::ptrdiff_t>(last_bin_));
    const float peak_floor = frame_maximum * kPeakFloorRatio;
    bool found_partial = false;
    for (std::size_t bin = first_bin_; bin < last_bin_; ++bin) {
        const float magnitude = magnitudes[bin];
        if (magnitude < peak_floor || magnitude <= magnitudes[bin - 1] || magnitude < magnitudes[bin + 1]) continue;
        if (!is_prominent_peak(magnitudes, bin)) continue;

        const double offset = interpolated_peak_offset(magnitudes[bin - 1], magnitude, magnitudes[bin + 1]);
        const double frequency = (static_cast<double>(bin) + offset) * bin_frequency_hz_;
        const double midi_note = kReferenceMidiNote + 12.0 * std::log2(frequency / kReferenceFrequencyHz);
        const auto fine_position = std::llround(midi_note * kFineBinsPerSemitone);
        const auto bin_count = static_cast<long long>(kFineBinCount);
        const auto fine_bin = static_cast<std::size_t>(((fine_position % bin_count) + bin_count) % bin_count);
        fine_chroma_[fine_bin] += frame_weight * magnitude;
        found_partial = true;
    }
    if (found_partial) tonal_frame_weight_ += frame_weight;
}

ChromaProfile ChromaAccumulator::profile() const {
    ChromaProfile profile;
    profile.tonal_frame_weight = tonal_frame_weight_;

    // Circular mean of each fine bin's position within its semitone gives the tuning offset.
    std::complex<double> tuning_phasor{0.0, 0.0};
    for (std::size_t fine_bin = 0; fine_bin < kFineBinCount; ++fine_bin) {
        const double position = static_cast<double>(fine_bin % kFineBinsPerSemitone) / kFineBinsPerSemitone;
        tuning_phasor += std::polar(fine_chroma_[fine_bin], 2.0 * std::numbers::pi * position);
    }
    double tuning = std::abs(tuning_phasor) > 0.0 ? std::arg(tuning_phasor) / (2.0 * std::numbers::pi) : 0.0;
    if (tuning >= 0.5) tuning -= 1.0;
    profile.tuning_offset_semitones = tuning;

    for (std::size_t fine_bin = 0; fine_bin < kFineBinCount; ++fine_bin) {
        const double semitone = static_cast<double>(fine_bin) / kFineBinsPerSemitone - tuning;
        const auto pitch_class = static_cast<std::size_t>(((std::llround(semitone) % 12) + 12) % 12);
        profile.pitch_class_energy[pitch_class] += fine_chroma_[fine_bin];
    }
    return profile;
}

}  // namespace mso::analysis
