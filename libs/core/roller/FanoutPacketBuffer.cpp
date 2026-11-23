#include "FanoutPacketBuffer.hpp"
#include <QIODevice>
#include <stdexcept>

namespace sentinel::roller {
namespace {
uint64_t little(const char *p, size_t n) {
  uint64_t v = 0;
  for (size_t i = 0; i < n; ++i)
    v |= uint64_t(uint8_t(p[i])) << (8 * i);
  return v;
}
} // namespace

std::optional<FanoutPacketBuffer::Packet> FanoutPacketBuffer::take() {
  if (bytes_.size() < 4)
    return std::nullopt;
  const auto n = little(bytes_.data(), 4);
  if (n < 4 || n > capture::MaxRecordBytes + 4096)
    throw std::runtime_error("fanout packet length");
  if (uint64_t(bytes_.size()) < n + 4)
    return std::nullopt;
  const auto jn = little(bytes_.data() + 4, 4);
  if (jn > n - 4 || jn > 4096)
    throw std::runtime_error("fanout JSON length");
  auto j = nlohmann::json::parse(bytes_.data() + 8, bytes_.data() + 8 + jn);
  std::string raw(bytes_.data() + 8 + jn, n - 4 - jn);
  bytes_.remove(0, n + 4);
  if (bytes_.capacity() > RetainBytes && bytes_.size() <= RetainBytes / 2)
    bytes_.squeeze();
  return Packet{std::move(j), std::move(raw)};
}

void FanoutPacketBuffer::readFrom(QIODevice &device) {
  // Copy, never adopt: read(maxSize) allocates maxSize however little arrived.
  const auto chunk = device.read(Limit - bytes_.size());
  bytes_.append(chunk.constData(), chunk.size());
}
} // namespace sentinel::roller
