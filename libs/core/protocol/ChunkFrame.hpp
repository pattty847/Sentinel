#pragma once
// Shared SCK1 transport codec. Payload bytes are opaque here; channel decoders
// own their counts/semantics. SHE1 dispatch preserves SHC1 during migration.
#include "../flow/FlowChunk.hpp"
#include "../heatmap/ChunkCodec.hpp"
#include <variant>

namespace protocol::chunks {
inline constexpr uint16_t WireVersion = 1, PayloadVersion = 1;
inline constexpr size_t MaxFrameBytes = 16u * 1024u * 1024u;
using Key = sentinel::flow::chunks::FlowChunkKey;
using Kind = heatmap::ChunkKind;
using Error = heatmap::ChunkError;
struct Frame {
    Kind kind = Kind::Chunk;
    bool sealed = true;
    Key key;
    int64_t endMs = 0, committedThroughMs = 0;
    uint64_t revision = 0, epoch = 0, contentHash = 0;
    uint16_t payloadVersion = PayloadVersion;
    uint8_t codec = 0;
    std::vector<uint8_t> payload;
    Error error = Error::InvalidRequest;
    std::string message;
};
bool validInstrument(const std::string& instrument);
bool validKey(const Key& key);
uint64_t contentHash(const Frame& frame);
// Encoding is for sealed flow only. No payload decode/re-encode on the server.
std::vector<uint8_t> encode(const Frame& frame);
Frame decode(std::span<const uint8_t> bytes);
struct Envelope {
    uint64_t requestId = 0;
    std::variant<heatmap::ChunkFrame, Frame> frame;
};
Envelope decodeEnvelope(std::span<const uint8_t> bytes);
} // namespace protocol::chunks
