// libFuzzer target: the supervisor <-> mso-worker wire protocol (include/mso/worker/worker_protocol.hpp)
// on malformed bytes.
//
// Each peer must survive anything on the pipe: a crashed or misbehaving worker can write garbage to its
// pipe. The input is decoded exactly as a peer reads its pipe (decode_frames: magic, version, length
// bound, truncation), and every payload is decoded both as a supervisor message and as a worker
// message (JSON schema and range checks). Inputs that are not a sequence of whole frames are also
// decoded as a single payload, so the JSON message decoders are fuzzed without the frame header in
// the way. A decoded message is encoded again, which must succeed: the decoders only accept what the
// encoders can write.
#include <cstddef>
#include <cstdint>
#include <span>
#include <string_view>

#include "mso/worker/worker_protocol.hpp"

namespace {

/// Decodes `payload` as both message directions; a message that decodes must encode again.
void decode_payload(std::string_view payload) {
    if (const auto supervisor = mso::worker::decode_supervisor_message(payload)) {
        if (!mso::worker::encode_message(*supervisor)) __builtin_trap();
    }
    if (const auto worker = mso::worker::decode_worker_message(payload)) {
        if (!mso::worker::encode_message(*worker)) __builtin_trap();
    }
}

}  // namespace

/// libFuzzer entry point: decodes `size` bytes at `data` as pipe contents, then as one payload.
extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t* data, std::size_t size) {
    const std::span<const std::uint8_t> bytes(data, size);
    if (const auto frames = mso::worker::decode_frames(bytes)) {
        for (const auto& frame : *frames)
            decode_payload(frame.payload);
    }
    decode_payload(std::string_view(reinterpret_cast<const char*>(data), size));
    return 0;
}
