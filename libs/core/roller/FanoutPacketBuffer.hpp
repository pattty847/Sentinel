#pragma once
// Client-side framing of capture fan-out packets (docs/RAW_CAPTURE.md "Live fan-out"):
// u32 body_bytes | u32 json_bytes | JSON | optional RAWL2 record, little-endian.
// The buffer holds at most Limit bytes, so a peer that never frames is bounded.
// Its allocation follows what is buffered, not the size of any read request, and a
// burst (a handshake ring replay, a multi-MiB snapshot record) is released once it
// is consumed: a long-lived source must not keep the peak for its whole life.
#include "capture/RawCapture.hpp"
#include <QByteArray>
#include <nlohmann/json.hpp>
#include <optional>
#include <string>
#include <utility>

class QIODevice;

namespace sentinel::roller {
class FanoutPacketBuffer {
public:
    static constexpr qsizetype Limit = capture::MaxRecordBytes + 4100;
    // Capacity kept between packets; above it, the buffer shrinks once at most
    // half of it is still buffered, so steady small packets never reallocate.
    static constexpr qsizetype RetainBytes = 1024 * 1024;
    using Packet = std::pair<nlohmann::json, std::string>; // header JSON, raw suffix
    // The next complete packet, or nullopt until more bytes arrive. Throws on
    // invalid lengths or JSON.
    std::optional<Packet> take();
    // One non-blocking read of what the device has, up to Limit buffered bytes.
    void readFrom(QIODevice& device);
    void clear() { bytes_.clear(); }
    qsizetype size() const { return bytes_.size(); }
    qsizetype capacity() const { return bytes_.capacity(); }

private:
    QByteArray bytes_;
};
} // namespace sentinel::roller
