// Analysis module tests: DSP estimators on synthesized audio, waveform/PCM-hash invariants, the
// shared decode pass against an in-memory decoder, and analyze_file() end to end on generated WAVs.
// Every signal is generated here from fixed parameters and a fixed-seed generator, so results are
// deterministic. Synthetic signals test mechanics only; they say nothing about real-music accuracy.
#include <doctest.h>

#include <picosha2.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <functional>
#include <limits>
#include <numbers>
#include <optional>
#include <random>
#include <span>
#include <stdexcept>
#include <stop_token>
#include <string>
#include <utility>
#include <vector>

#include "mso/analysis/analysis.hpp"
#include "mso/analysis/chroma_accumulator.hpp"
#include "mso/analysis/decoded_stream_analysis.hpp"
#include "mso/analysis/key_estimator.hpp"
#include "mso/analysis/limits.hpp"
#include "mso/analysis/loudness_meter.hpp"
#include "mso/analysis/musical_features.hpp"
#include "mso/analysis/onset_envelope.hpp"
#include "mso/analysis/pcm_hasher.hpp"
#include "mso/analysis/progress_reporter.hpp"
#include "mso/analysis/spectrogram_stream.hpp"
#include "mso/analysis/spectrum.hpp"
#include "mso/analysis/tempo_estimator.hpp"
#include "mso/analysis/waveform_builder.hpp"
#include "mso/analysis/waveform_preview.hpp"
#include "mso/domain/core.hpp"
#include "test_support.hpp"

using namespace mso;
using namespace mso::analysis;

namespace {

// ---------------------------------------------------------------- signal synthesis

/// Uniform white noise in [-1, 1) from a fixed-seed mt19937, whose output sequence the standard
/// specifies exactly (std distributions are not portable, so the mapping is done by hand).
class DeterministicNoise {
public:
    explicit DeterministicNoise(std::uint32_t seed) : engine_(seed) {}
    float next() { return static_cast<float>(static_cast<double>(engine_()) / 4294967296.0 * 2.0 - 1.0); }

private:
    std::mt19937 engine_;
};

std::size_t seconds_to_frames(double seconds, int sample_rate) {
    return static_cast<std::size_t>(std::llround(seconds * sample_rate));
}

/// Adds a synthetic kick drum at `start`: a sine sweeping 150 -> 50 Hz with a 120 ms decay plus
/// a 4 ms noise click on the attack.
void add_kick(std::vector<float>& audio, std::size_t start, int sample_rate, float amplitude,
              DeterministicNoise& noise) {
    const std::size_t length = seconds_to_frames(0.35, sample_rate);
    const std::size_t click_length = seconds_to_frames(0.004, sample_rate);
    double phase = 0.0;
    for (std::size_t offset = 0; offset < length && start + offset < audio.size(); ++offset) {
        const double time = static_cast<double>(offset) / sample_rate;
        const double frequency = 50.0 + 100.0 * std::exp(-time / 0.03);
        phase += 2.0 * std::numbers::pi * frequency / sample_rate;
        double sample = std::sin(phase) * std::exp(-time / 0.12);
        if (offset < click_length) sample += 0.5 * noise.next();
        audio[start + offset] += amplitude * static_cast<float>(sample);
    }
}

/// Adds a hi-hat at `start`: 40 ms of exponentially decaying white noise.
void add_hat(std::vector<float>& audio, std::size_t start, int sample_rate, float amplitude,
             DeterministicNoise& noise) {
    const std::size_t length = seconds_to_frames(0.04, sample_rate);
    for (std::size_t offset = 0; offset < length && start + offset < audio.size(); ++offset) {
        const double time = static_cast<double>(offset) / sample_rate;
        audio[start + offset] += amplitude * noise.next() * static_cast<float>(std::exp(-time / 0.01));
    }
}

/// Mono drum loop: a kick on every beat, optionally a quieter hat on every off-beat.
std::vector<float> drum_loop(double bpm, double seconds, int sample_rate, bool off_beat_hats) {
    std::vector<float> audio(seconds_to_frames(seconds, sample_rate), 0.0F);
    DeterministicNoise noise(1234);
    const double beat_seconds = 60.0 / bpm;
    for (double beat_time = 0.05; beat_time < seconds; beat_time += beat_seconds) {
        add_kick(audio, seconds_to_frames(beat_time, sample_rate), sample_rate, 0.8F, noise);
        if (off_beat_hats)
            add_hat(audio, seconds_to_frames(beat_time + beat_seconds / 2, sample_rate), sample_rate, 0.3F, noise);
    }
    return audio;
}

/// Adds a snare at `start`: a 190 Hz body (50 ms decay) under 60 ms of decaying noise.
void add_snare(std::vector<float>& audio, std::size_t start, int sample_rate, float amplitude,
               DeterministicNoise& noise) {
    const std::size_t length = seconds_to_frames(0.18, sample_rate);
    for (std::size_t offset = 0; offset < length && start + offset < audio.size(); ++offset) {
        const double time = static_cast<double>(offset) / sample_rate;
        const double body = std::sin(2.0 * std::numbers::pi * 190.0 * time) * std::exp(-time / 0.05);
        audio[start + offset] +=
            amplitude * static_cast<float>(0.5 * body + 0.7 * noise.next() * std::exp(-time / 0.06));
    }
}

/// Drum groove shapes for the metrical-level fixtures.
enum class Groove {
    SixteenthHats,  ///< a kick on every beat and a hat on every 16th note
    HalfTime,       ///< kick on beat 1, snare on beat 3 of each bar, a hat on every 8th note
};

/// Mono groove of `seconds` at `bpm`. Hats sit at 0.3 against a 0.8 kick, as in a typical mix.
std::vector<float> groove(Groove shape, double bpm, double seconds, int sample_rate) {
    std::vector<float> audio(seconds_to_frames(seconds, sample_rate), 0.0F);
    DeterministicNoise noise(1234);
    const double beat_seconds = 60.0 / bpm;
    const int hats_per_beat = shape == Groove::SixteenthHats ? 4 : 2;
    int beat = 0;
    for (double beat_time = 0.05; beat_time < seconds; beat_time += beat_seconds, ++beat) {
        const std::size_t start = seconds_to_frames(beat_time, sample_rate);
        if (shape == Groove::SixteenthHats || beat % 4 == 0) add_kick(audio, start, sample_rate, 0.8F, noise);
        if (shape == Groove::HalfTime && beat % 4 == 2) add_snare(audio, start, sample_rate, 0.7F, noise);
        for (int hat = 0; hat < hats_per_beat; ++hat)
            add_hat(audio, seconds_to_frames(beat_time + beat_seconds * hat / hats_per_beat, sample_rate), sample_rate,
                    0.3F, noise);
    }
    return audio;
}

std::vector<float> white_noise(double seconds, int sample_rate, float amplitude) {
    DeterministicNoise noise(99);
    std::vector<float> audio(seconds_to_frames(seconds, sample_rate));
    for (float& sample : audio)
        sample = amplitude * noise.next();
    return audio;
}

/// Adds a harmonic tone (six partials at 1/h amplitude, 10 ms attack, 60 ms release).
void add_tone(std::vector<float>& audio, double start_seconds, double length_seconds, double midi_note, float amplitude,
              int sample_rate) {
    const double fundamental = 440.0 * std::exp2((midi_note - 69.0) / 12.0);
    const std::size_t start = seconds_to_frames(start_seconds, sample_rate);
    const std::size_t length = seconds_to_frames(length_seconds, sample_rate);
    for (std::size_t offset = 0; offset < length && start + offset < audio.size(); ++offset) {
        const double time = static_cast<double>(offset) / sample_rate;
        const double envelope = std::min({1.0, time / 0.01, (length_seconds - time) / 0.06});
        double sample = 0.0;
        for (int harmonic = 1; harmonic <= 6; ++harmonic)
            sample += std::sin(2.0 * std::numbers::pi * fundamental * harmonic * time) / harmonic;
        audio[start + offset] += amplitude * static_cast<float>(envelope * sample);
    }
}

/// A chord progression, one chord per second, played `repeats` times; each chord is a list of MIDI
/// notes and `detune_cents` shifts everything off A440.
std::vector<float> chord_progression(const std::vector<std::vector<double>>& chords, int repeats, int sample_rate,
                                     double detune_cents = 0.0) {
    const double chord_seconds = 1.0;
    std::vector<float> audio(
        seconds_to_frames(chord_seconds * static_cast<double>(chords.size() * repeats), sample_rate), 0.0F);
    double time = 0.0;
    for (int repeat = 0; repeat < repeats; ++repeat) {
        for (const auto& chord : chords) {
            for (const double note : chord)
                add_tone(audio, time, chord_seconds, note + detune_cents / 100.0, 0.12F, sample_rate);
            time += chord_seconds;
        }
    }
    return audio;
}

// i - iv - V - i cadences with a bass note under each chord.
const std::vector<std::vector<double>> kAMinorCadence{
    {45, 57, 60, 64}, {50, 62, 65, 69}, {40, 52, 56, 59}, {45, 57, 60, 64}};
const std::vector<std::vector<double>> kCMajorCadence{
    {48, 60, 64, 67}, {53, 65, 69, 72}, {43, 55, 59, 62}, {48, 60, 64, 67}};
const std::vector<std::vector<double>> kFSharpMinorCadence{
    {42, 54, 57, 61}, {47, 59, 62, 66}, {49, 61, 65, 68}, {42, 54, 57, 61}};

/// `chords` moved by `semitones`.
std::vector<std::vector<double>> transposed(const std::vector<std::vector<double>>& chords, int semitones) {
    std::vector<std::vector<double>> moved = chords;
    for (auto& chord : moved)
        for (double& note : chord)
            note += semitones;
    return moved;
}

TempoResult tempo_of(const std::vector<float>& mono, int sample_rate) {
    OnsetEnvelopeBuilder envelope(sample_rate);
    envelope.add(mono);
    return estimate_tempo(envelope.envelope(), envelope.frame_rate());
}

KeyResult key_of(const std::vector<float>& mono, int sample_rate) {
    ChromaAccumulator chroma(sample_rate);
    chroma.add(mono);
    return estimate_key(chroma.profile());
}

/// The error code of a failed result, nullopt for a success (safe to compare in a CHECK).
template <class T>
std::optional<ErrorCode> error_code(const Result<T>& result) {
    if (result) return std::nullopt;
    return result.error().code;
}

std::string describe(const KeyResult& result) {
    return result.key ? format_key(*result.key) : std::string("unknown");
}

// ---------------------------------------------------------------- in-memory decoder

/// media::Decoder over interleaved samples held in memory, with optional hooks for tests.
class MemoryDecoder final : public media::Decoder {
public:
    MemoryDecoder(std::vector<float> samples, int channels, int sample_rate, bool announce_length = true)
        : samples_(std::move(samples)),
          channels_(channels),
          sample_rate_(sample_rate),
          announce_length_(announce_length) {}

    int channels() const override { return channels_; }
    int sample_rate() const override { return sample_rate_; }
    std::optional<std::int64_t> total_frames() const override {
        if (estimated_total_frames) return estimated_total_frames;
        if (!announce_length_) return std::nullopt;
        return static_cast<std::int64_t>(samples_.size() / static_cast<std::size_t>(channels_));
    }
    bool frames_exact() const override { return announce_length_ && !estimated_total_frames; }
    Result<std::size_t> read(float* out, std::size_t frames) override {
        ++read_calls;
        if (on_read) on_read();
        if (fail_after_reads && read_calls > *fail_after_reads) return fail(ErrorCode::CorruptAudio, "test corruption");
        const std::size_t channel_count = static_cast<std::size_t>(channels_);
        const std::size_t available = (samples_.size() - position_ * channel_count) / channel_count;
        const std::size_t count = std::min(frames, available);
        std::copy_n(samples_.begin() + static_cast<std::ptrdiff_t>(position_ * channel_count), count * channel_count,
                    out);
        position_ += count;
        return count;
    }
    Result<void> seek(std::int64_t frame) override {
        position_ = static_cast<std::size_t>(frame);
        return {};
    }
    std::int64_t position() const override { return static_cast<std::int64_t>(position_); }
    std::string codec() const override { return "memory_f32"; }

    int read_calls = 0;
    std::function<void()> on_read;
    std::optional<int> fail_after_reads;
    /// When set, announced as an inexact length estimate (like a VBR MP3 without a seek table).
    std::optional<std::int64_t> estimated_total_frames;

private:
    std::vector<float> samples_;
    int channels_;
    int sample_rate_;
    bool announce_length_;
    std::size_t position_ = 0;
};

AnalysisRequest request_for(std::vector<Stage> stages) {
    AnalysisRequest request;
    request.stages = std::move(stages);
    return request;
}

// ---------------------------------------------------------------- files for end-to-end tests

using test::ScratchDirectory;
using test::WavEncoding;
using test::WavFormat;

std::string sha256_of_file(const std::filesystem::path& file) {
    return picosha2::hash256_hex_string(test::read_file(file));
}

}  // namespace

TEST_SUITE("analysis") {
    // ---------------------------------------------------------------- stage names

    TEST_CASE("stage names round-trip and unknown names are rejected") {
        for (const Stage stage :
             {Stage::Waveform, Stage::Loudness, Stage::Tempo, Stage::Key, Stage::ByteHash, Stage::PcmHash})
            CHECK(parse_stage(to_string(stage)) == stage);
        CHECK(to_string(Stage::ByteHash) == "byte_hash");
        CHECK_FALSE(parse_stage("Tempo").has_value());
        CHECK_FALSE(parse_stage("").has_value());
    }

    // ---------------------------------------------------------------- tempo

    TEST_CASE("tempo of kick loops is within 1% at common tempi") {
        constexpr int kSampleRate = 22050;
        for (const double bpm : {90.0, 120.0, 128.0, 140.0, 174.0}) {
            CAPTURE(bpm);
            const TempoResult result = tempo_of(drum_loop(bpm, 20.0, kSampleRate, false), kSampleRate);
            REQUIRE(result.milli_bpm.has_value());
            const double measured = static_cast<double>(*result.milli_bpm) / 1000.0;
            const double relative_error = std::abs(measured - bpm) / bpm;
            MESSAGE("kick loop " << bpm << " BPM -> " << measured << " BPM (error " << relative_error * 100.0
                                 << "%, confidence " << result.confidence << ")");
            CHECK_MESSAGE(std::abs(measured * 2.0 - bpm) / bpm > 0.01, "half-tempo octave error");
            CHECK_MESSAGE(std::abs(measured / 2.0 - bpm) / bpm > 0.01, "double-tempo octave error");
            CHECK(relative_error < 0.01);
            CHECK(result.confidence > 0.25);
        }
    }

    TEST_CASE("tempo of a kick and off-beat hat groove keeps the kick tempo at 44.1 kHz") {
        constexpr int kSampleRate = 44100;
        const TempoResult result = tempo_of(drum_loop(128.0, 15.0, kSampleRate, true), kSampleRate);
        REQUIRE(result.milli_bpm.has_value());
        const double measured = static_cast<double>(*result.milli_bpm) / 1000.0;
        MESSAGE("kick+hat 128 BPM @44.1 kHz -> " << measured << " BPM (confidence " << result.confidence << ")");
        CHECK(std::abs(measured - 128.0) / 128.0 < 0.01);
    }

    TEST_CASE("tempo is unknown for a one-shot, too-short audio, white noise and silence") {
        constexpr int kSampleRate = 22050;
        DeterministicNoise noise(7);

        std::vector<float> one_shot(seconds_to_frames(2.0, kSampleRate), 0.0F);
        add_kick(one_shot, 100, kSampleRate, 0.9F, noise);
        const TempoResult one_shot_tempo = tempo_of(one_shot, kSampleRate);
        CHECK_FALSE(one_shot_tempo.milli_bpm.has_value());

        const TempoResult short_tempo = tempo_of(drum_loop(120.0, 1.0, kSampleRate, false), kSampleRate);
        CHECK_FALSE(short_tempo.milli_bpm.has_value());

        const TempoResult noise_tempo = tempo_of(white_noise(10.0, kSampleRate, 0.5F), kSampleRate);
        MESSAGE("white noise tempo confidence " << noise_tempo.confidence);
        CHECK_FALSE(noise_tempo.milli_bpm.has_value());

        const TempoResult silence_tempo =
            tempo_of(std::vector<float>(seconds_to_frames(5.0, kSampleRate), 0.0F), kSampleRate);
        CHECK_FALSE(silence_tempo.milli_bpm.has_value());
        CHECK(silence_tempo.confidence == 0.0);
    }

    TEST_CASE("onset envelope does not depend on how the audio is split into blocks") {
        constexpr int kSampleRate = 22050;
        const std::vector<float> audio = drum_loop(120.0, 3.0, kSampleRate, true);
        OnsetEnvelopeBuilder whole(kSampleRate);
        whole.add(audio);
        OnsetEnvelopeBuilder pieces(kSampleRate);
        for (std::size_t offset = 0; offset < audio.size(); offset += 777)
            pieces.add(std::span(audio).subspan(offset, std::min<std::size_t>(777, audio.size() - offset)));
        CHECK(std::ranges::equal(whole.envelope(), pieces.envelope()));
        CHECK(whole.frame_rate() == doctest::Approx(22050.0 / 256.0));
    }

    // ---------------------------------------------------------------- key

    TEST_CASE("key of synthesized cadences") {
        constexpr int kSampleRate = 44100;
        struct Case {
            std::string name;
            const std::vector<std::vector<double>>* chords;
            MusicalKey expected;
        };
        const Case cases[] = {
            {"A minor", &kAMinorCadence, {9, KeyMode::Minor}},
            {"C major", &kCMajorCadence, {0, KeyMode::Major}},
            {"F# minor", &kFSharpMinorCadence, {6, KeyMode::Minor}},
        };
        for (const Case& test_case : cases) {
            CAPTURE(test_case.name);
            const KeyResult result = key_of(chord_progression(*test_case.chords, 3, kSampleRate), kSampleRate);
            MESSAGE(test_case.name << " cadence -> " << describe(result) << " (confidence " << result.confidence
                                   << ")");
            REQUIRE(result.key.has_value());
            CHECK(*result.key == test_case.expected);
            CHECK(result.confidence > 0.0);
        }
    }

    TEST_CASE("key estimation compensates for a detuned recording") {
        constexpr int kSampleRate = 44100;
        ChromaAccumulator chroma(kSampleRate);
        chroma.add(chord_progression(kAMinorCadence, 3, kSampleRate, /*detune_cents=*/35.0));
        const ChromaProfile profile = chroma.profile();
        CHECK(profile.tuning_offset_semitones == doctest::Approx(0.35).epsilon(0.15));
        const KeyResult result = estimate_key(profile);
        MESSAGE("A minor +35 cents -> " << describe(result) << " (tuning " << profile.tuning_offset_semitones
                                        << " semitones)");
        REQUIRE(result.key.has_value());
        CHECK(*result.key == MusicalKey{9, KeyMode::Minor});
    }

    TEST_CASE("key is unknown for white noise, a click, a kick loop, a single note and silence") {
        constexpr int kSampleRate = 44100;
        CHECK_FALSE(key_of(white_noise(8.0, kSampleRate, 0.5F), kSampleRate).key.has_value());

        std::vector<float> click(seconds_to_frames(3.0, kSampleRate), 0.0F);
        click[seconds_to_frames(1.0, kSampleRate)] = 1.0F;
        CHECK_FALSE(key_of(click, kSampleRate).key.has_value());

        const KeyResult kick_key = key_of(drum_loop(120.0, 8.0, kSampleRate, true), kSampleRate);
        MESSAGE("kick loop key -> " << describe(kick_key));
        CHECK_FALSE(kick_key.key.has_value());

        std::vector<float> single_note(seconds_to_frames(4.0, kSampleRate), 0.0F);
        add_tone(single_note, 0.0, 4.0, 60.0, 0.3F, kSampleRate);
        const KeyResult note_key = key_of(single_note, kSampleRate);
        MESSAGE("single C4 note key -> " << describe(note_key));
        CHECK_FALSE(note_key.key.has_value());

        CHECK_FALSE(key_of(std::vector<float>(seconds_to_frames(3.0, kSampleRate), 0.0F), kSampleRate).key.has_value());
    }

    // ---------------------------------------------------------------- loudness

    TEST_CASE("loudness reports sample peak and RMS in dBFS") {
        LoudnessMeter sine_meter;
        std::vector<float> sine(48000);
        for (std::size_t index = 0; index < sine.size(); ++index)
            sine[index] =
                static_cast<float>(std::sin(2.0 * std::numbers::pi * 1000.0 * static_cast<double>(index) / 48000.0));
        sine_meter.add(sine);
        CHECK(sine_meter.result().peak_dbfs == doctest::Approx(0.0).epsilon(0.001));
        CHECK(sine_meter.result().rms_dbfs == doctest::Approx(-3.0103).epsilon(0.001));

        LoudnessMeter square_meter;
        const std::vector<float> square{0.5F, -0.5F, 0.5F, -0.5F};
        square_meter.add(square);
        CHECK(square_meter.result().peak_dbfs == doctest::Approx(-6.0206).epsilon(0.0001));
        CHECK(square_meter.result().rms_dbfs == doctest::Approx(-6.0206).epsilon(0.0001));

        LoudnessMeter silence_meter;
        silence_meter.add(std::vector<float>(100, 0.0F));
        CHECK(silence_meter.result().peak_dbfs == kSilenceDbfs);
        CHECK(silence_meter.result().rms_dbfs == kSilenceDbfs);
        CHECK(LoudnessMeter{}.result().rms_dbfs == kSilenceDbfs);
    }

    // ---------------------------------------------------------------- waveform

    TEST_CASE("waveform bins hold signed peaks and RMS across channels") {
        constexpr int kFrames = 800;
        std::vector<float> stereo;
        for (int frame = 0; frame < kFrames; ++frame) {
            const float level = static_cast<float>(frame) / kFrames;
            stereo.push_back(level);          // left rises from 0
            stereo.push_back(-0.5F * level);  // right falls at half the rate
        }
        WaveformBuilder builder(8, 1000, kFrames);
        builder.add(stereo, 2);
        const WaveformOverview overview = builder.overview(true);

        REQUIRE(overview.minimum.size() == 8);
        CHECK(overview.frames_per_bin == 100);
        CHECK(overview.frames_covered == kFrames);
        CHECK(overview.total_frames == kFrames);
        CHECK(overview.sample_rate == 1000);
        CHECK(overview.complete);
        for (int bin = 0; bin < 8; ++bin) {
            CAPTURE(bin);
            const int last_frame = bin * 100 + 99;
            CHECK(overview.maximum[static_cast<std::size_t>(bin)] ==
                  doctest::Approx(static_cast<double>(last_frame) / kFrames));
            CHECK(overview.minimum[static_cast<std::size_t>(bin)] == doctest::Approx(-0.5 * last_frame / kFrames));
            double sum_of_squares = 0.0;
            for (int frame = bin * 100; frame <= last_frame; ++frame) {
                const double level = static_cast<double>(frame) / kFrames;
                sum_of_squares += level * level + 0.25 * level * level;
            }
            CHECK(overview.rms[static_cast<std::size_t>(bin)] == doctest::Approx(std::sqrt(sum_of_squares / 200.0)));
        }
    }

    TEST_CASE("waveform without a known length coarsens to the same bins as a known length") {
        std::vector<float> mono(1000);
        DeterministicNoise noise(5);
        for (float& sample : mono)
            sample = noise.next();

        WaveformBuilder adaptive(8, 44100, std::nullopt);
        for (std::size_t offset = 0; offset < mono.size(); offset += 37)
            adaptive.add(std::span(mono).subspan(offset, std::min<std::size_t>(37, mono.size() - offset)), 1);
        const WaveformOverview grown = adaptive.overview(true);
        CHECK(grown.frames_per_bin == 128);
        CHECK(grown.minimum.size() == 8);
        CHECK(grown.frames_covered == 1000);
        CHECK(grown.total_frames == 1000);

        WaveformBuilder direct(8, 44100, 1024);  // ceil(1024 / 8) = 128 frames per bin from the start
        direct.add(mono, 1);
        const WaveformOverview reference = direct.overview(true);
        CHECK(grown.minimum == reference.minimum);
        CHECK(grown.maximum == reference.maximum);
        for (std::size_t bin = 0; bin < grown.rms.size(); ++bin)
            CHECK(grown.rms[bin] == doctest::Approx(reference.rms[bin]));

        // A partial overview of an underestimated stream reports the total as unknown.
        WaveformBuilder underestimated(8, 44100, 500);
        underestimated.add(mono, 1);
        const WaveformOverview partial = underestimated.overview(false);
        CHECK(partial.minimum.size() <= 8);
        CHECK(partial.total_frames == 0);
        CHECK_FALSE(partial.complete);
    }

    TEST_CASE("waveform encoding round-trips and rejects corrupted or inconsistent data") {
        std::vector<float> mono(5000);
        for (std::size_t index = 0; index < mono.size(); ++index)
            mono[index] = 0.9F * static_cast<float>(std::sin(static_cast<double>(index) * 0.01));
        WaveformBuilder builder(64, 48000, 5000);
        builder.add(mono, 1);
        const WaveformOverview overview = builder.overview(true);
        const std::vector<std::uint8_t> encoded = encode_waveform(overview);
        CHECK(encoded.size() == 48 + overview.minimum.size() * 12 + 32);

        const Result<WaveformOverview> decoded = decode_waveform(encoded);
        REQUIRE(decoded.has_value());
        CHECK(decoded->minimum == overview.minimum);
        CHECK(decoded->maximum == overview.maximum);
        CHECK(decoded->rms == overview.rms);
        CHECK(decoded->frames_per_bin == overview.frames_per_bin);
        CHECK(decoded->frames_covered == overview.frames_covered);
        CHECK(decoded->total_frames == overview.total_frames);
        CHECK(decoded->sample_rate == overview.sample_rate);
        CHECK(decoded->complete == overview.complete);

        SUBCASE("every single flipped bit is rejected") {
            for (std::size_t byte = 0; byte < encoded.size(); byte += 7) {
                std::vector<std::uint8_t> corrupted = encoded;
                corrupted[byte] ^= 0x10;
                CAPTURE(byte);
                CHECK_FALSE(decode_waveform(corrupted).has_value());
            }
        }
        SUBCASE("truncated and extended data is rejected") {
            CHECK_FALSE(decode_waveform(std::vector<std::uint8_t>(encoded.begin(), encoded.end() - 1)).has_value());
            std::vector<std::uint8_t> extended = encoded;
            extended.push_back(0);
            CHECK_FALSE(decode_waveform(extended).has_value());
            CHECK_FALSE(decode_waveform({}).has_value());
        }
        SUBCASE("inconsistent fields are rejected even with a valid checksum") {
            WaveformOverview wrong_coverage = overview;
            wrong_coverage.frames_covered += overview.frames_per_bin;  // one bin more than stored
            wrong_coverage.total_frames = wrong_coverage.frames_covered;
            CHECK(error_code(decode_waveform(encode_waveform(wrong_coverage))) == ErrorCode::ParseError);

            WaveformOverview incomplete_total = overview;
            incomplete_total.total_frames = overview.frames_covered - 1;
            CHECK_FALSE(decode_waveform(encode_waveform(incomplete_total)).has_value());

            WaveformOverview inverted = overview;
            std::swap(inverted.minimum[3], inverted.maximum[3]);
            CHECK_FALSE(decode_waveform(encode_waveform(inverted)).has_value());

            WaveformOverview loud_rms = overview;
            loud_rms.rms[0] = 1.5F;
            CHECK_FALSE(decode_waveform(encode_waveform(loud_rms)).has_value());

            WaveformOverview missing_rms = overview;
            missing_rms.rms.pop_back();
            CHECK_FALSE(decode_waveform(encode_waveform(missing_rms)).has_value());

            WaveformOverview no_rate = overview;
            no_rate.sample_rate = 0;
            CHECK_FALSE(decode_waveform(encode_waveform(no_rate)).has_value());
        }
        SUBCASE("an empty overview of an empty file is valid") {
            WaveformBuilder empty_builder(16, 44100, 0);
            const auto empty = decode_waveform(encode_waveform(empty_builder.overview(true)));
            REQUIRE(empty.has_value());
            CHECK(empty->minimum.empty());
            CHECK(empty->frames_covered == 0);
        }
    }

    // ---------------------------------------------------------------- PCM hash

    TEST_CASE("signed 24-bit quantization is exact for 16- and 24-bit sources and clamps the rest") {
        CHECK(quantize_to_s24(0.0F) == 0);
        CHECK(quantize_to_s24(0.5F) == 4194304);
        CHECK(quantize_to_s24(1.0F) == 8388607);
        CHECK(quantize_to_s24(-1.0F) == -8388608);
        CHECK(quantize_to_s24(3.0F) == 8388607);
        CHECK(quantize_to_s24(-std::numeric_limits<float>::infinity()) == -8388608);
        CHECK(quantize_to_s24(std::numeric_limits<float>::quiet_NaN()) == 0);
        for (const int value : {-32768, -12345, -1, 1, 777, 32767}) {
            CAPTURE(value);
            CHECK(quantize_to_s24(static_cast<float>(value) / 32768.0F) == value * 256);
            CHECK(quantize_to_s24(static_cast<float>(value * 256 + 17) / 8388608.0F) == value * 256 + 17);
        }
    }

    TEST_CASE("PCM hash depends on content, rate, channels and length but not on block size") {
        std::vector<float> samples(4000);
        DeterministicNoise noise(11);
        for (float& sample : samples)
            sample = std::round(noise.next() * 32767.0F) / 32768.0F;

        const auto hash = [](std::span<const float> audio, int channels, int sample_rate, std::size_t block_frames) {
            PcmHasher hasher(channels, sample_rate);
            const std::size_t block_samples = block_frames * static_cast<std::size_t>(channels);
            for (std::size_t offset = 0; offset < audio.size(); offset += block_samples)
                hasher.add(audio.subspan(offset, std::min(block_samples, audio.size() - offset)));
            return hasher.finish();
        };

        const std::string reference = hash(samples, 2, 44100, 4096);
        CHECK(reference.size() == 64);
        CHECK(hash(samples, 2, 44100, 7) == reference);
        CHECK(hash(samples, 2, 48000, 4096) != reference);
        CHECK(hash(samples, 1, 44100, 4096) != reference);

        std::vector<float> one_lsb_changed = samples;
        one_lsb_changed[1234] += 1.0F / 32768.0F;
        CHECK(hash(one_lsb_changed, 2, 44100, 4096) != reference);

        std::vector<float> one_more_frame = samples;
        one_more_frame.insert(one_more_frame.end(), {0.0F, 0.0F});
        CHECK(hash(one_more_frame, 2, 44100, 4096) != reference);
    }

    // ---------------------------------------------------------------- progress

    TEST_CASE("progress is throttled, monotonic and always ends at 1") {
        std::vector<double> reported;
        ProgressReporter reporter([&reported](double fraction) { reported.push_back(fraction); },
                                  std::chrono::milliseconds(60'000));
        const ProgressPhase second_half(reporter, 0.5, 1.0);
        for (int step = 0; step <= 100; ++step)
            second_half.report(step / 100.0);
        reporter.report(0.1);
        reporter.finish();
        reporter.finish();
        REQUIRE(reported.size() == 2);  // the first report, then the final 1.0
        CHECK(reported.front() == doctest::Approx(0.5));
        CHECK(reported.back() == 1.0);

        ProgressReporter silent{ProgressCallback{}};
        silent.report(0.5);
        silent.finish();  // no callback: nothing to call, nothing to crash
    }

    // ---------------------------------------------------------------- shared decode pass

    TEST_CASE("one decode pass fills every requested decode stage") {
        constexpr int kSampleRate = 22050;
        const std::vector<float> mono = drum_loop(120.0, 12.0, kSampleRate, false);
        std::vector<float> stereo;
        for (const float sample : mono)
            stereo.insert(stereo.end(), {sample, sample});

        MemoryDecoder decoder(stereo, 2, kSampleRate);
        ProgressReporter reporter{ProgressCallback{}};
        AnalysisRequest request = request_for({Stage::Waveform, Stage::Loudness, Stage::Tempo, Stage::Key,
                                               Stage::PcmHash, Stage::ByteHash, Stage::Waveform});
        request.waveform_bins = 100;
        const Result<AnalysisReport> report =
            analyze_decoded_stream(decoder, request, {}, ProgressPhase(reporter, 0, 1));
        REQUIRE(report.has_value());

        CHECK(report->frames_decoded == static_cast<std::int64_t>(mono.size()));
        REQUIRE(report->waveform.has_value());
        CHECK(report->waveform->complete);
        CHECK(report->waveform->minimum.size() <= 100);
        CHECK(report->waveform->total_frames == static_cast<std::int64_t>(mono.size()));
        REQUIRE(report->loudness.has_value());
        LoudnessMeter direct_meter;
        direct_meter.add(stereo);
        CHECK(report->loudness->peak_dbfs == doctest::Approx(direct_meter.result().peak_dbfs));
        CHECK(report->loudness->rms_dbfs == doctest::Approx(direct_meter.result().rms_dbfs));
        REQUIRE(report->tempo.has_value());
        REQUIRE(report->tempo->milli_bpm.has_value());
        CHECK(std::abs(*report->tempo->milli_bpm - 120000) < 1200);
        REQUIRE(report->key.has_value());
        CHECK_FALSE(report->key->key.has_value());
        CHECK_FALSE(report->hashes.byte_sha256.has_value());  // byte hashing is file-level, not decode-level

        PcmHasher direct(2, kSampleRate);
        direct.add(stereo);
        CHECK(report->hashes.pcm_sha256 == direct.finish());
        CHECK(report->warnings.empty());
    }

    TEST_CASE("tempo-only analysis stops decoding after the musical window") {
        constexpr int kSampleRate = 22050;
        MemoryDecoder decoder(drum_loop(120.0, 30.0, kSampleRate, false), 1, kSampleRate, /*announce_length=*/false);
        ProgressReporter reporter{ProgressCallback{}};
        AnalysisRequest request = request_for({Stage::Tempo});
        request.max_musical_seconds = 8.0;
        const auto report = analyze_decoded_stream(decoder, request, {}, ProgressPhase(reporter, 0, 1));
        REQUIRE(report.has_value());
        CHECK(report->frames_decoded <= 8 * kSampleRate + static_cast<std::int64_t>(kDecodeBlockFrames));
        CHECK(report->frames_decoded > 8 * kSampleRate);
        REQUIRE(report->tempo.has_value());
        CHECK(report->tempo->milli_bpm.has_value());
        CHECK_FALSE(report->waveform.has_value());
        CHECK(report->warnings.size() == 1);
    }

    TEST_CASE("cancellation is honoured before the next block") {
        constexpr int kSampleRate = 22050;
        std::vector<float> audio = white_noise(20.0, kSampleRate, 0.1F);
        ProgressReporter reporter{ProgressCallback{}};
        const AnalysisRequest request = request_for({Stage::Waveform, Stage::Tempo, Stage::Key, Stage::PcmHash});

        SUBCASE("stop requested before the pass starts") {
            MemoryDecoder decoder(audio, 1, kSampleRate);
            std::stop_source stop;
            stop.request_stop();
            const auto report =
                analyze_decoded_stream(decoder, request, stop.get_token(), ProgressPhase(reporter, 0, 1));
            REQUIRE_FALSE(report.has_value());
            CHECK(report.error().code == ErrorCode::Cancelled);
            CHECK(decoder.read_calls == 0);
        }
        SUBCASE("stop requested during the pass") {
            MemoryDecoder decoder(audio, 1, kSampleRate);
            std::stop_source stop;
            decoder.on_read = [&decoder, &stop] {
                if (decoder.read_calls == 3) stop.request_stop();
            };
            const auto report =
                analyze_decoded_stream(decoder, request, stop.get_token(), ProgressPhase(reporter, 0, 1));
            REQUIRE_FALSE(report.has_value());
            CHECK(report.error().code == ErrorCode::Cancelled);
            CHECK(decoder.read_calls == 3);
        }
    }

    TEST_CASE("decode pass propagates decoder errors and rejects unusable streams") {
        ProgressReporter reporter{ProgressCallback{}};
        const AnalysisRequest request = request_for({Stage::Loudness});

        MemoryDecoder failing(std::vector<float>(100'000, 0.1F), 1, 44100);
        failing.fail_after_reads = 2;
        const auto failed = analyze_decoded_stream(failing, request, {}, ProgressPhase(reporter, 0, 1));
        REQUIRE_FALSE(failed.has_value());
        CHECK(failed.error().code == ErrorCode::CorruptAudio);

        MemoryDecoder no_rate(std::vector<float>(100, 0.1F), 1, 0);
        CHECK(error_code(analyze_decoded_stream(no_rate, request, {}, ProgressPhase(reporter, 0, 1))) ==
              ErrorCode::CorruptAudio);

        AnalysisRequest bad_bins = request_for({Stage::Waveform});
        bad_bins.waveform_bins = 0;
        MemoryDecoder decoder(std::vector<float>(100, 0.1F), 1, 44100);
        CHECK(error_code(analyze_decoded_stream(decoder, bad_bins, {}, ProgressPhase(reporter, 0, 1))) ==
              ErrorCode::InvalidArgument);
    }

    TEST_CASE("non-finite samples are treated as silence and reported") {
        std::vector<float> samples(1000, 0.25F);
        samples[10] = std::numeric_limits<float>::quiet_NaN();
        samples[20] = std::numeric_limits<float>::infinity();
        MemoryDecoder decoder(samples, 1, 8000);
        ProgressReporter reporter{ProgressCallback{}};
        const auto report = analyze_decoded_stream(decoder, request_for({Stage::Loudness, Stage::Waveform}), {},
                                                   ProgressPhase(reporter, 0, 1));
        REQUIRE(report.has_value());
        CHECK(report->loudness->peak_dbfs == doctest::Approx(amplitude_to_dbfs(0.25)));
        CHECK(std::isfinite(report->loudness->rms_dbfs));
        REQUIRE(report->warnings.size() == 1);
        CHECK(report->warnings.front().find("2 non-finite") != std::string::npos);
    }

    // ---------------------------------------------------------------- analyze_file end to end

    TEST_CASE("analyze_file hashes the same audio equally across WAV sample formats") {
        constexpr int kSampleRate = 44100;
        ScratchDirectory directory("analysis");
        const std::vector<float> mono = chord_progression(kCMajorCadence, 2, kSampleRate);
        std::vector<float> stereo;
        for (const float sample : mono)
            stereo.insert(stereo.end(), {sample, 0.5F * sample});
        const std::vector<std::int16_t> pcm = test::to_int16(stereo);

        const AnalysisRequest base =
            request_for({Stage::PcmHash, Stage::ByteHash, Stage::Waveform, Stage::Loudness, Stage::Key});
        std::vector<AnalysisReport> reports;
        for (const auto& [name, encoding] : {std::pair{std::string("s16.wav"), WavEncoding::Int16},
                                             std::pair{std::string("s24.wav"), WavEncoding::Int24},
                                             std::pair{std::string("f32.wav"), WavEncoding::Float32}}) {
            CAPTURE(name);
            const auto file = directory.file(name);
            test::write_wav(file, pcm, WavFormat{2, kSampleRate, encoding});
            AnalysisRequest request = base;
            request.file = file;
            std::vector<double> progress;
            const Result<AnalysisReport> report =
                analyze_file(request, {}, [&progress](double fraction) { progress.push_back(fraction); });
            REQUIRE_MESSAGE(report.has_value(), report.error().message());
            CHECK(report->frames_decoded == static_cast<std::int64_t>(mono.size()));
            CHECK(report->hashes.byte_sha256 == sha256_of_file(file));
            CHECK(report->evidence.unchanged());
            CHECK(report->evidence.size_before == static_cast<std::int64_t>(std::filesystem::file_size(file)));
            REQUIRE(report->key.has_value());
            CHECK(report->key->key == MusicalKey{0, KeyMode::Major});
            REQUIRE_FALSE(progress.empty());
            CHECK(progress.back() == 1.0);
            CHECK(std::ranges::is_sorted(progress));
            reports.push_back(*report);
        }
        CHECK(reports[0].hashes.pcm_sha256 == reports[1].hashes.pcm_sha256);
        CHECK(reports[0].hashes.pcm_sha256 == reports[2].hashes.pcm_sha256);
        CHECK(reports[0].hashes.byte_sha256 != reports[1].hashes.byte_sha256);
        CHECK(reports[0].waveform->maximum == reports[2].waveform->maximum);

        // Same samples at another rate, or as mono, never collide.
        test::write_wav(directory.file("s16-48k.wav"), pcm, WavFormat{2, 48000, WavEncoding::Int16});
        test::write_wav(directory.file("s16-mono.wav"), pcm, WavFormat{1, kSampleRate, WavEncoding::Int16});
        for (const char* name : {"s16-48k.wav", "s16-mono.wav"}) {
            AnalysisRequest request = request_for({Stage::PcmHash});
            request.file = directory.file(name);
            const auto report = analyze_file(request, {});
            REQUIRE(report.has_value());
            CHECK(report->hashes.pcm_sha256 != reports[0].hashes.pcm_sha256);
        }
    }

    TEST_CASE("analyze_file finds the tempo of a generated loop") {
        constexpr int kSampleRate = 44100;
        ScratchDirectory directory("analysis");
        const auto file = directory.file("loop-140.wav");
        test::write_wav(file, test::to_int16(drum_loop(140.0, 12.0, kSampleRate, true)),
                        WavFormat{1, kSampleRate, WavEncoding::Int16});
        AnalysisRequest request = request_for({Stage::Tempo, Stage::Key});
        request.file = file;
        const auto report = analyze_file(request, {});
        REQUIRE_MESSAGE(report.has_value(), report.error().message());
        REQUIRE(report->tempo->milli_bpm.has_value());
        MESSAGE("analyze_file 140 BPM loop -> " << static_cast<double>(*report->tempo->milli_bpm) / 1000.0 << " BPM");
        CHECK(std::abs(*report->tempo->milli_bpm - 140000) < 1400);
        CHECK_FALSE(report->key->key.has_value());
    }

    TEST_CASE("analyze_file reports missing files, cancellation and changed input") {
        ScratchDirectory directory("analysis");

        AnalysisRequest missing = request_for({Stage::ByteHash});
        missing.file = directory.file("does-not-exist.wav");
        CHECK(error_code(analyze_file(missing, {})) == ErrorCode::FileMissing);

        const auto file = directory.file("bytes.bin");
        {
            std::ofstream stream(file, std::ios::binary);
            const std::vector<char> content(3 << 20, 'x');  // three hashing chunks
            stream.write(content.data(), static_cast<std::streamsize>(content.size()));
        }
        AnalysisRequest request = request_for({Stage::ByteHash});
        request.file = file;

        std::stop_source cancelled;
        cancelled.request_stop();
        CHECK(error_code(analyze_file(request, cancelled.get_token())) == ErrorCode::Cancelled);

        std::stop_source stop_during_hash;
        const auto cancelled_midway = analyze_file(request, stop_during_hash.get_token(),
                                                   [&stop_during_hash](double) { stop_during_hash.request_stop(); });
        CHECK(error_code(cancelled_midway) == ErrorCode::Cancelled);

        bool touched = false;
        const auto changed = analyze_file(request, {}, [&touched, &file](double) {
            if (touched) return;
            touched = true;
            std::filesystem::last_write_time(file, std::filesystem::last_write_time(file) + std::chrono::hours(1));
        });
        REQUIRE(touched);
        CHECK(error_code(changed) == ErrorCode::InputChanged);

        const auto unchanged = analyze_file(request, {});
        REQUIRE(unchanged.has_value());
        CHECK(unchanged->hashes.byte_sha256 == sha256_of_file(file));
        CHECK(unchanged->evidence.unchanged());
    }

    // ---------------------------------------------------------------- FFT building blocks

    TEST_CASE("magnitude spectrum reads a bin-centred full-scale sine as 1 and accepts split frames") {
        constexpr std::size_t kSize = 1024;
        MagnitudeSpectrum spectrum(kSize);
        CHECK(spectrum.frame_size() == kSize);
        CHECK(spectrum.bin_count() == kSize / 2 + 1);
        CHECK(MagnitudeSpectrum(0).frame_size() == 2);

        std::vector<float> sine(kSize);
        for (std::size_t index = 0; index < kSize; ++index)
            sine[index] =
                static_cast<float>(std::sin(2.0 * std::numbers::pi * 64.0 * static_cast<double>(index) / kSize));
        const auto whole_span = spectrum.compute(sine);
        const std::vector<float> whole(whole_span.begin(), whole_span.end());
        CHECK(whole[64] == doctest::Approx(1.0).epsilon(0.001));
        CHECK(whole[63] == doctest::Approx(0.5).epsilon(0.01));  // Hann main lobe: half at one bin off
        CHECK(whole[10] < 1e-4F);

        // The same frame handed over in two pieces (a wrapped ring buffer) gives the same spectrum.
        const std::span<const float> frame(sine);
        const auto split = spectrum.compute(frame.first(300), frame.subspan(300));
        CHECK(std::ranges::equal(whole, split));

        // A constant reads its level at DC (scaled like every bin), and a short frame is zero-padded.
        const std::vector<float> constant(kSize, 0.5F);
        CHECK(spectrum.compute(constant)[0] == doctest::Approx(1.0).epsilon(0.001));
        std::vector<float> padded(kSize, 0.0F);
        std::copy_n(sine.begin(), 100, padded.begin());
        const auto padded_span = spectrum.compute(padded);
        const std::vector<float> padded_spectrum(padded_span.begin(), padded_span.end());
        CHECK(std::ranges::equal(padded_spectrum, spectrum.compute(frame.first(100))));
    }

    TEST_CASE("autocorrelation matches the direct sum") {
        DeterministicNoise noise(3);
        std::vector<double> signal(257);
        for (double& value : signal)
            value = noise.next();
        const std::vector<double> fast = autocorrelation(signal);
        REQUIRE(fast.size() == signal.size());
        for (const std::size_t lag : {0, 1, 2, 17, 100, 256}) {
            CAPTURE(lag);
            double direct = 0.0;
            for (std::size_t index = 0; index + lag < signal.size(); ++index)
                direct += signal[index] * signal[index + lag];
            CHECK(std::abs(fast[lag] - direct) < 1e-9 * static_cast<double>(signal.size()));
        }
        CHECK(autocorrelation({}).empty());
    }

    TEST_CASE("nearest_power_of_two rounds in log terms and clamps") {
        CHECK(nearest_power_of_two(1000.0, 1, 1 << 20) == 1024);
        CHECK(nearest_power_of_two(1400.0, 1, 1 << 20) == 1024);
        CHECK(nearest_power_of_two(1500.0, 1, 1 << 20) == 2048);
        CHECK(nearest_power_of_two(10.0, 64, 4096) == 64);
        CHECK(nearest_power_of_two(1e12, 64, 4096) == 4096);
        CHECK(nearest_power_of_two(0.5, 64, 4096) == 64);
        CHECK(nearest_power_of_two(std::numeric_limits<double>::quiet_NaN(), 64, 4096) == 64);
    }

    TEST_CASE("spectrogram stream emits every full frame once, independent of block size") {
        constexpr std::size_t kFrame = 256;
        constexpr std::size_t kHop = 64;
        const std::vector<float> audio = white_noise(2000.0 / 22050.0, 22050, 0.5F);
        REQUIRE(audio.size() == 2000);
        const auto collect = [](SpectrogramStream& stream, std::span<const float> samples,
                                std::vector<std::vector<float>>& frames) {
            stream.add(samples, [&frames](std::span<const float> magnitudes) {
                frames.emplace_back(magnitudes.begin(), magnitudes.end());
            });
        };

        SpectrogramStream whole(kFrame, kHop);
        std::vector<std::vector<float>> whole_frames;
        collect(whole, audio, whole_frames);
        CHECK(whole_frames.size() == 1 + (audio.size() - kFrame) / kHop);

        SpectrogramStream pieces(kFrame, kHop);
        std::vector<std::vector<float>> piece_frames;
        const std::size_t block_sizes[] = {1, 7, 300, 64, 1000};
        std::size_t offset = 0;
        for (std::size_t block = 0; offset < audio.size(); ++block) {
            const std::size_t size = std::min(block_sizes[block % std::size(block_sizes)], audio.size() - offset);
            collect(pieces, std::span(audio).subspan(offset, size), piece_frames);
            offset += size;
        }
        CHECK(piece_frames == whole_frames);

        // Frame k is the spectrum of samples [k * hop, k * hop + frame).
        MagnitudeSpectrum direct(kFrame);
        for (const std::size_t frame : {std::size_t{0}, std::size_t{5}, whole_frames.size() - 1}) {
            CAPTURE(frame);
            const auto expected = direct.compute(std::span(audio).subspan(frame * kHop, kFrame));
            CHECK(std::ranges::equal(whole_frames[frame], expected));
        }

        SpectrogramStream short_input(kFrame, kHop);
        std::vector<std::vector<float>> none;
        collect(short_input, std::span(audio).first(kFrame - 1), none);
        CHECK(none.empty());
    }

    TEST_CASE("musical feature extractor downmixes, honours its window and runs only requested stages") {
        constexpr int kSampleRate = 22050;
        const std::vector<float> mono = drum_loop(120.0, 12.0, kSampleRate, false);
        std::vector<float> stereo;
        std::vector<float> opposed;
        for (const float sample : mono) {
            stereo.insert(stereo.end(), {sample, sample});
            opposed.insert(opposed.end(), {sample, -sample});
        }
        const std::int64_t window = 10 * kSampleRate;

        MusicalFeatureExtractor from_stereo(kSampleRate, 2, window, true, false);
        from_stereo.add(stereo);
        CHECK(from_stereo.saturated());
        CHECK(from_stereo.frames_analyzed() == window);
        CHECK_FALSE(from_stereo.key().has_value());
        const auto stereo_tempo = from_stereo.tempo();
        REQUIRE(stereo_tempo.has_value());
        REQUIRE(stereo_tempo->milli_bpm.has_value());

        MusicalFeatureExtractor from_mono(kSampleRate, 1, window, true, false);
        from_mono.add(mono);
        CHECK(from_mono.tempo()->milli_bpm == stereo_tempo->milli_bpm);  // (a + a) / 2 == a exactly

        // Opposite polarity cancels to silence: equal 1/N downmix weights.
        MusicalFeatureExtractor from_opposed(kSampleRate, 2, window, true, true);
        from_opposed.add(opposed);
        CHECK_FALSE(from_opposed.tempo()->milli_bpm.has_value());
        CHECK_FALSE(from_opposed.key()->key.has_value());

        MusicalFeatureExtractor key_only(kSampleRate, 1, window, false, true);
        key_only.add(mono);
        CHECK_FALSE(key_only.tempo().has_value());
        CHECK(key_only.key().has_value());
    }

    // ---------------------------------------------------------------- tempo: metrical levels and rates

    TEST_CASE("16th-note hats over a kick on every beat keep the kick tempo") {
        struct Case {
            double bpm;
            int sample_rate;
        };
        for (const Case test_case :
             {Case{85.0, 44100}, Case{90.0, 44100}, Case{95.0, 44100}, Case{90.0, 48000}, Case{90.0, 96000}}) {
            CAPTURE(test_case.bpm);
            CAPTURE(test_case.sample_rate);
            const TempoResult result = tempo_of(
                groove(Groove::SixteenthHats, test_case.bpm, 15.0, test_case.sample_rate), test_case.sample_rate);
            REQUIRE(result.milli_bpm.has_value());
            const double measured = static_cast<double>(*result.milli_bpm) / 1000.0;
            MESSAGE("16th hats " << test_case.bpm << " BPM @" << test_case.sample_rate << " -> " << measured);
            CHECK(std::abs(measured - test_case.bpm) / test_case.bpm < 0.01);
        }
    }

    TEST_CASE("a half-time groove reads at its conventional tempo, not an octave off") {
        // Kick on 1, snare on 3, 8th-note hats. At 85 BPM the hats must not double it to 170; at
        // 140 BPM the prior keeps the conventional 140 rather than the 70 BPM backbeat.
        for (const double bpm : {85.0, 140.0}) {
            CAPTURE(bpm);
            const TempoResult result = tempo_of(groove(Groove::HalfTime, bpm, 15.0, 44100), 44100);
            REQUIRE(result.milli_bpm.has_value());
            const double measured = static_cast<double>(*result.milli_bpm) / 1000.0;
            MESSAGE("half-time " << bpm << " BPM -> " << measured);
            CHECK(std::abs(measured - bpm) / bpm < 0.01);
        }
    }

    TEST_CASE("tempo of kick loops is within 1% at 48 and 96 kHz") {
        for (const int sample_rate : {48000, 96000}) {
            for (const double bpm : {90.0, 128.0, 174.0}) {
                CAPTURE(sample_rate);
                CAPTURE(bpm);
                const TempoResult result = tempo_of(drum_loop(bpm, 15.0, sample_rate, false), sample_rate);
                REQUIRE(result.milli_bpm.has_value());
                CHECK(std::abs(static_cast<double>(*result.milli_bpm) / 1000.0 - bpm) / bpm < 0.01);
            }
        }
    }

    // ---------------------------------------------------------------- key: every key and other rates

    TEST_CASE("key of all 24 major and minor keys") {
        constexpr int kSampleRate = 22050;
        for (int shift = -5; shift <= 6; ++shift) {
            for (const KeyMode mode : {KeyMode::Major, KeyMode::Minor}) {
                const bool major = mode == KeyMode::Major;
                const MusicalKey expected{((major ? 0 : 9) + shift + 12) % 12, mode};
                CAPTURE(format_key(expected));
                const auto chords = transposed(major ? kCMajorCadence : kAMinorCadence, shift);
                const KeyResult result = key_of(chord_progression(chords, 2, kSampleRate), kSampleRate);
                REQUIRE(result.key.has_value());
                CHECK(*result.key == expected);
                CHECK(result.confidence > 0.0);
            }
        }
    }

    TEST_CASE("key estimation at 48 and 96 kHz") {
        for (const int sample_rate : {48000, 96000}) {
            CAPTURE(sample_rate);
            const KeyResult a_minor = key_of(chord_progression(kAMinorCadence, 2, sample_rate), sample_rate);
            REQUIRE(a_minor.key.has_value());
            CHECK(*a_minor.key == MusicalKey{9, KeyMode::Minor});
            const KeyResult c_major = key_of(chord_progression(kCMajorCadence, 2, sample_rate), sample_rate);
            REQUIRE(c_major.key.has_value());
            CHECK(*c_major.key == MusicalKey{0, KeyMode::Major});
        }
    }

    // ---------------------------------------------------------------- waveform resolution, work, preview

    TEST_CASE("an inexact length estimate never leaves the waveform too coarse") {
        MemoryDecoder decoder(white_noise(10'000.0 / 8000.0, 8000, 0.5F), 1, 8000);
        decoder.estimated_total_frames = 1'000'000;  // a 100x overestimate, as from a VBR header
        ProgressReporter reporter{ProgressCallback{}};
        AnalysisRequest request = request_for({Stage::Waveform});
        request.waveform_bins = 100;
        const auto report = analyze_decoded_stream(decoder, request, {}, ProgressPhase(reporter, 0, 1));
        REQUIRE(report.has_value());
        REQUIRE(report->waveform.has_value());
        CHECK(report->waveform->minimum.size() > 50);
        CHECK(report->waveform->minimum.size() <= 100);
        CHECK(report->waveform->complete);
        CHECK(report->waveform->total_frames == 10'000);
    }

    TEST_CASE("analyze_file counts one unit of work per decoded block and hashed chunk") {
        constexpr int kSampleRate = 44100;
        ScratchDirectory directory("analysis-work");
        const auto file = directory.file("three-seconds.wav");
        test::write_sine_wav(file, 3.0, 440.0, kSampleRate);
        AnalysisRequest request = request_for({Stage::Loudness, Stage::ByteHash});
        request.file = file;
        WorkCounter work{0};
        const auto report = analyze_file(request, {}, {}, &work);
        REQUIRE(report.has_value());
        const auto blocks = (report->frames_decoded + static_cast<std::int64_t>(kDecodeBlockFrames) - 1) /
                            static_cast<std::int64_t>(kDecodeBlockFrames);
        const auto chunks = (report->evidence.size_before + (1 << 20) - 1) / (1 << 20);
        CHECK(work.load() == static_cast<std::uint64_t>(blocks + chunks));
    }

    TEST_CASE("the preview waveform equals the analysis waveform and fails cleanly") {
        ScratchDirectory directory("analysis-preview");
        const auto file = directory.file("preview.wav");
        test::write_sine_wav(file, 2.0, 220.0, 44100);

        std::vector<double> progress;
        const auto preview =
            compute_waveform_overview(file, 300, {}, [&progress](double fraction) { progress.push_back(fraction); });
        REQUIRE_MESSAGE(preview.has_value(), preview.error().message());
        AnalysisRequest request = request_for({Stage::Waveform});
        request.file = file;
        request.waveform_bins = 300;
        const auto analyzed = analyze_file(request, {});
        REQUIRE(analyzed.has_value());
        CHECK(preview->minimum == analyzed->waveform->minimum);
        CHECK(preview->maximum == analyzed->waveform->maximum);
        CHECK(preview->rms == analyzed->waveform->rms);
        CHECK(preview->complete);
        REQUIRE_FALSE(progress.empty());
        CHECK(progress.back() == 1.0);

        CHECK(error_code(compute_waveform_overview(file, 0, {})) == ErrorCode::InvalidArgument);
        CHECK(error_code(compute_waveform_overview(file, kMaximumWaveformBins + 1, {})) == ErrorCode::InvalidArgument);
        CHECK(error_code(compute_waveform_overview(directory.file("missing.wav"), 100, {})) == ErrorCode::FileMissing);
        std::stop_source stop;
        stop.request_stop();
        CHECK(error_code(compute_waveform_overview(file, 100, stop.get_token())) == ErrorCode::Cancelled);
        const auto throwing =
            compute_waveform_overview(file, 100, {}, [](double) { throw std::runtime_error("callback failure"); });
        CHECK(error_code(throwing) == ErrorCode::Internal);
    }

    TEST_CASE("analyze_file refuses a directory") {
        ScratchDirectory directory("analysis-directory");
        AnalysisRequest request = request_for({Stage::ByteHash});
        request.file = directory.path();
        CHECK(error_code(analyze_file(request, {})) == ErrorCode::InvalidArgument);
    }

}  // TEST_SUITE
