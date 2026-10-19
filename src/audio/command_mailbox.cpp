#include "mso/audio/command_mailbox.hpp"

#include <algorithm>
#include <bit>
#include <cmath>

namespace mso::audio {
namespace {

/// Seek payloads: a plain non-negative frame, or a fraction in parts per billion tagged by the top bit.
constexpr std::uint64_t kFractionSeekFlag = std::uint64_t{1} << 63;
constexpr double kFractionScale = 1'000'000'000.0;

std::uint64_t encode_seek_frame(std::int64_t frame) noexcept {
    return static_cast<std::uint64_t>(std::max<std::int64_t>(frame, 0)) & ~kFractionSeekFlag;
}

std::uint64_t encode_seek_fraction(double fraction) noexcept {
    const double clamped = std::isfinite(fraction) ? std::clamp(fraction, 0.0, 1.0) : 0.0;
    return kFractionSeekFlag | static_cast<std::uint64_t>(std::llround(clamped * kFractionScale));
}

std::uint64_t encode_gain(float gain) noexcept {
    return std::bit_cast<std::uint32_t>(gain);
}

template <class... Handlers>
struct Overloaded : Handlers... {
    using Handlers::operator()...;
};

}  // namespace

CommandMailbox::CommandMailbox() : queue_(kGeneralCapacity), payload_cells_(queue_.max_capacity() + 1) {}

bool CommandMailbox::post(const TransportCommand& command) noexcept {
    return std::visit(Overloaded{
                          [this](const transport::Play&) { return push(EntryKind::Play, 0); },
                          [this](const transport::Pause&) { return push(EntryKind::Pause, 0); },
                          [this](const transport::TogglePlayPause&) { return push(EntryKind::TogglePlayPause, 0); },
                          [this](const transport::Stop&) { return post_stop(); },
                          [this](const transport::SeekToFrame& seek) {
                              return push(EntryKind::Seek, encode_seek_frame(seek.source_frame));
                          },
                          [this](const transport::SeekToFraction& seek) {
                              return push(EntryKind::Seek, encode_seek_fraction(seek.fraction));
                          },
                          [this](const transport::SetVolumeDecibels& volume) {
                              return push(EntryKind::Volume, encode_gain(linear_gain_from_decibels(volume.decibels)));
                          },
                          [this](const transport::SetVolumeLinear& volume) {
                              return push(EntryKind::Volume, encode_gain(clamped_linear_gain(volume.gain)));
                          },
                          [this](const transport::SetLoop& loop) { return push(EntryKind::Loop, 0, loop.region); },
                          [this](const transport::SetSpeed& speed) {
                              if (!std::isfinite(speed.ratio)) {
                                  return reject();
                              }
                              const double ratio = std::clamp(speed.ratio, kMinimumSpeedRatio, kMaximumSpeedRatio);
                              return push(EntryKind::Speed, std::bit_cast<std::uint64_t>(ratio));
                          },
                          [this](const transport::SetPitch& pitch) {
                              return push(EntryKind::Pitch,
                                          std::bit_cast<std::uint64_t>(clamped_pitch_semitones(pitch.semitones)));
                          },
                          [this](const transport::SetPlaybackMode& mode) {
                              return push(EntryKind::PlaybackMode, static_cast<std::uint64_t>(mode.mode));
                          },
                      },
                      command);
}

bool CommandMailbox::push(EntryKind kind, std::uint64_t payload, std::optional<LoopRegion> loop) noexcept {
    if (is_coalescing(kind) && try_replace_newest_value(kind, payload)) {
        return true;
    }
    // The cell's previous entry has been popped (see payload_cells_), so overwriting it is safe
    // even if the queue turns out to be full.
    const std::size_t cell = static_cast<std::size_t>(enqueued_count_ % payload_cells_.size());
    payload_cells_[cell].store(payload, std::memory_order_relaxed);
    if (!queue_.try_enqueue(Entry{kind, next_sequence_, cell, loop})) {
        return reject();
    }
    ++next_sequence_;
    ++enqueued_count_;
    newest_post_kind_ = kind;
    return true;
}

bool CommandMailbox::post_stop() noexcept {
    stop_sequence_.store(next_sequence_++, std::memory_order_release);
    newest_post_kind_ = EntryKind::Stop;
    return true;
}

bool CommandMailbox::try_replace_newest_value(EntryKind kind, std::uint64_t payload) noexcept {
    if (newest_post_kind_ != kind) {
        return false;  // something else was posted since: the new value needs its own place in order
    }
    std::atomic<std::uint64_t>& newest = payload_cells_[(enqueued_count_ - 1) % payload_cells_.size()];
    // A single atomic exchange decides the race with the consumer: if it has not taken the old
    // value yet, it will take ours; if it already has, the old payload reads as kTakenPayload and
    // the command needs an entry of its own.
    return newest.exchange(payload, std::memory_order_acq_rel) != kTakenPayload;
}

bool CommandMailbox::reject() noexcept {
    dropped_count_.fetch_add(1, std::memory_order_relaxed);
    return false;
}

bool CommandMailbox::is_coalescing(EntryKind kind) noexcept {
    return kind == EntryKind::Seek || kind == EntryKind::Volume || kind == EntryKind::Speed || kind == EntryKind::Pitch;
}

std::optional<TransportCommand> CommandMailbox::take() noexcept {
    const std::uint64_t stop = stop_sequence_.load(std::memory_order_acquire);
    const Entry* front = queue_.peek();
    // A Stop goes out once everything posted before it has been delivered. Of several Stops not
    // yet delivered only the newest remains; the result is the same, a stopped transport at 0.
    if (stop > delivered_stop_sequence_ && (front == nullptr || front->sequence > stop)) {
        delivered_stop_sequence_ = stop;
        return transport::Stop{};
    }
    if (front == nullptr) {
        return std::nullopt;
    }
    const Entry entry = *front;
    const std::uint64_t payload = payload_cells_[entry.cell].exchange(kTakenPayload, std::memory_order_acq_rel);
    queue_.pop();
    return decode(entry, payload);
}

TransportCommand CommandMailbox::decode(const Entry& entry, std::uint64_t payload) noexcept {
    switch (entry.kind) {
        case EntryKind::Play:
            return transport::Play{};
        case EntryKind::Pause:
            return transport::Pause{};
        case EntryKind::TogglePlayPause:
            return transport::TogglePlayPause{};
        case EntryKind::Stop:
            return transport::Stop{};
        case EntryKind::Seek:
            if ((payload & kFractionSeekFlag) != 0) {
                return transport::SeekToFraction{static_cast<double>(payload & ~kFractionSeekFlag) / kFractionScale};
            }
            return transport::SeekToFrame{static_cast<std::int64_t>(payload)};
        case EntryKind::Volume:
            return transport::SetVolumeLinear{std::bit_cast<float>(static_cast<std::uint32_t>(payload))};
        case EntryKind::Speed:
            return transport::SetSpeed{std::bit_cast<double>(payload)};
        case EntryKind::Pitch:
            return transport::SetPitch{std::bit_cast<double>(payload)};
        case EntryKind::PlaybackMode:
            return transport::SetPlaybackMode{static_cast<PlaybackMode>(payload)};
        case EntryKind::Loop:
            return transport::SetLoop{entry.loop};
    }
    return transport::Stop{};
}

}  // namespace mso::audio
