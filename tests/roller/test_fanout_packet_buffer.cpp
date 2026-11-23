// LiveSource's fan-out buffer lives as long as a product's shadow roller. A handshake
// ring replay or a multi-MiB snapshot record must not leave it at its peak size.
#include "roller/FanoutPacketBuffer.hpp"
#include <gtest/gtest.h>
#include <QIODevice>
#include <cstring>
#include <deque>

using namespace sentinel;
using namespace sentinel::roller;
using nlohmann::json;
namespace {
// A sequential fake socket that hands out one queued chunk at a time.
class ChunkedSource : public QIODevice {
public:
    explicit ChunkedSource(std::deque<QByteArray> chunks) : chunks_(std::move(chunks)) {
        open(QIODevice::ReadOnly | QIODevice::Unbuffered);
    }
    bool isSequential() const override { return true; }
    qint64 bytesAvailable() const override {
        return (chunks_.empty() ? 0 : chunks_.front().size()) + QIODevice::bytesAvailable();
    }
    bool drained() const { return chunks_.empty(); }

protected:
    qint64 readData(char* data, qint64 max) override {
        if (chunks_.empty()) return 0;
        auto& chunk = chunks_.front();
        const auto n = std::min<qint64>(max, chunk.size());
        std::memcpy(data, chunk.constData(), size_t(n));
        chunk.remove(0, n);
        if (chunk.isEmpty()) chunks_.pop_front();
        return n;
    }
    qint64 writeData(const char*, qint64) override { return -1; }

private:
    std::deque<QByteArray> chunks_;
};
void put32(QByteArray& out, uint32_t v) {
    for (int i = 0; i < 4; ++i) out.append(char(v >> (8 * i)));
}
QByteArray packet(const json& j, const std::string& raw) {
    const auto text = j.dump();
    QByteArray out;
    put32(out, uint32_t(4 + text.size() + raw.size()));
    put32(out, uint32_t(text.size()));
    out.append(text.data(), qsizetype(text.size()));
    out.append(raw.data(), qsizetype(raw.size()));
    return out;
}
json record(uint64_t block, uint32_t index) {
    return {{"type", "record"}, {"provisional", true},
            {"pos", {{"product", "BTC-USD"}, {"run_id", "run"}, {"block", block}, {"record", index}}}};
}
std::string raw(size_t payload, char fill) {
    std::string r(32, '\0');
    r.append(payload, fill);
    return r;
}
struct Expected {
    json header;
    std::string raw;
};
// Drive the buffer as LiveSource::packet does: take, else read once.
FanoutPacketBuffer::Packet next(FanoutPacketBuffer& buffer, ChunkedSource& source) {
    for (;;) {
        if (auto p = buffer.take()) return std::move(*p);
        if (source.drained()) throw std::runtime_error("source drained before a whole packet");
        buffer.readFrom(source);
    }
}
std::deque<QByteArray> split(const QByteArray& bytes, qsizetype chunk) {
    std::deque<QByteArray> out;
    for (qsizetype at = 0; at < bytes.size(); at += chunk) out.push_back(bytes.mid(at, chunk));
    return out;
}
} // namespace

TEST(FanoutPacketBuffer, LargePacketIsReleasedAndLaterPacketsParseUnchanged) {
    std::vector<Expected> expected;
    expected.push_back({record(7, 0), raw(3 * 1024 * 1024, 's')}); // a multi-MiB snapshot
    for (uint32_t i = 1; i <= 50; ++i) expected.push_back({record(7, i), raw(200 + i, char('a' + i % 26))});
    const json durable = {{"type", "durable"}, {"product", "BTC-USD"},
                          {"through", {{"product", "BTC-USD"}, {"run_id", "run"}, {"block", 7}, {"record", 50}}}};
    expected.push_back({durable, ""}); // a control packet has no raw suffix
    QByteArray wire;
    for (const auto& e : expected) wire += packet(e.header, e.raw);
    // Socket-sized reads that cut packets anywhere.
    for (const qsizetype chunk : {qsizetype(64 * 1024 + 3), wire.size()}) {
        ChunkedSource source(split(wire, chunk));
        FanoutPacketBuffer buffer;
        for (size_t i = 0; i < expected.size(); ++i) {
            const auto [header, suffix] = next(buffer, source);
            EXPECT_EQ(header, expected[i].header) << i;
            EXPECT_EQ(suffix, expected[i].raw) << i;
            // Retained capacity never stays above the threshold once the large
            // packet is consumed (it is never below 3 MiB while it is buffered).
            if (i == 0) EXPECT_LT(buffer.capacity(), FanoutPacketBuffer::RetainBytes) << "chunk=" << chunk;
            EXPECT_LE(buffer.capacity(), FanoutPacketBuffer::RetainBytes) << "chunk=" << chunk << " i=" << i;
        }
        EXPECT_EQ(buffer.size(), 0);
        EXPECT_TRUE(source.drained());
    }
}

TEST(FanoutPacketBuffer, SmallReadsKeepASmallBuffer) {
    // read(maxSize) allocates its whole request; the buffer must not keep that.
    std::deque<QByteArray> chunks;
    for (uint32_t i = 0; i < 100; ++i) chunks.push_back(packet(record(1, i), raw(100, 'x')));
    ChunkedSource source(chunks);
    FanoutPacketBuffer buffer;
    for (uint32_t i = 0; i < 100; ++i) {
        buffer.readFrom(source); // one whole packet per read, as a quiet socket delivers
        EXPECT_LT(buffer.capacity(), 64 * 1024) << i;
        const auto p = buffer.take();
        ASSERT_TRUE(p) << i;
        EXPECT_EQ(p->first, record(1, i));
        EXPECT_EQ(p->second, raw(100, 'x'));
        EXPECT_LT(buffer.capacity(), 64 * 1024) << i;
    }
}

TEST(FanoutPacketBuffer, InvalidLengthsThrowAsBefore) {
    const auto failure = [](QByteArray bytes) {
        ChunkedSource source({bytes});
        FanoutPacketBuffer buffer;
        buffer.readFrom(source);
        try {
            buffer.take();
        } catch (const std::runtime_error& e) {
            return std::string(e.what());
        }
        return std::string("no error");
    };
    QByteArray shortBody; put32(shortBody, 3); put32(shortBody, 0);
    EXPECT_EQ(failure(shortBody), "fanout packet length");
    QByteArray huge; put32(huge, capture::MaxRecordBytes + 4097);
    EXPECT_EQ(failure(huge), "fanout packet length");
    QByteArray longJson; put32(longJson, 4 + 4097); put32(longJson, 4097); longJson.append(4097, ' ');
    EXPECT_EQ(failure(longJson), "fanout JSON length");
    QByteArray overrun; put32(overrun, 8); put32(overrun, 5); overrun.append("{}  ");
    EXPECT_EQ(failure(overrun), "fanout JSON length");
}

TEST(FanoutPacketBuffer, OneByteReadsSplitEveryPrefix) {
    std::vector<Expected> expected;
    for (uint32_t i = 0; i < 4; ++i) expected.push_back({record(2, i), raw(40 + i, 'p')});
    QByteArray wire;
    for (const auto& e : expected) wire += packet(e.header, e.raw);
    ChunkedSource source(split(wire, 1));
    FanoutPacketBuffer buffer;
    for (const auto& e : expected) {
        const auto [header, suffix] = next(buffer, source);
        EXPECT_EQ(header, e.header);
        EXPECT_EQ(suffix, e.raw);
    }
    EXPECT_EQ(buffer.size(), 0);
}

TEST(FanoutPacketBuffer, ClearOnReconnectDropsAPartialPacket) {
    const auto first = packet(record(3, 0), raw(5000, 'q'));
    ChunkedSource dropped({first.left(first.size() / 2)});
    FanoutPacketBuffer buffer;
    buffer.readFrom(dropped);
    EXPECT_FALSE(buffer.take());
    buffer.clear(); // LiveSource::handshake on a new socket
    EXPECT_EQ(buffer.size(), 0);
    const auto again = packet(record(3, 0), raw(5000, 'q'));
    ChunkedSource reconnected({again});
    const auto [header, suffix] = next(buffer, reconnected);
    EXPECT_EQ(header, record(3, 0));
    EXPECT_EQ(suffix, raw(5000, 'q'));
}
