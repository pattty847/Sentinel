#include <gtest/gtest.h>
#include "capture/CaptureRouting.hpp"
#include <QFile>
#include <cstdlib>
#include <new>

// Isolated executable: count C++ allocations only within the routing call.
// A DOM parser allocates for each update; SAX retains only routing tokens.
namespace { thread_local bool measuring = false; thread_local size_t allocated = 0; }
void* operator new(std::size_t n) {
    if (measuring) allocated += n;
    if (auto* p = std::malloc(n ? n : 1)) return p;
    throw std::bad_alloc();
}
void* operator new[](std::size_t n) { return ::operator new(n); }
void operator delete(void* p) noexcept { std::free(p); }
void operator delete[](void* p) noexcept { std::free(p); }

using namespace sentinel::capture;
TEST(CaptureRouting, FrozenGoldenIdentitiesAndRangeReceipts) {
    QFile file(CAPTURE_ROUTING_GOLDEN);
    ASSERT_TRUE(file.open(QIODevice::ReadOnly));
    const auto golden = nlohmann::json::parse(file.readAll().toStdString());
    EXPECT_EQ(golden.at("routing"), RoutingId);
    const auto products = golden.at("products").get<std::vector<std::string>>();
    RoutingBatch batch;
    size_t index = 0;
    for (const auto& vector : golden.at("vectors")) {
        const auto payload = vector.at("payload").get<std::string>();
        const auto identity = frameReceipt(payload, products);
        EXPECT_EQ(identity, vector.at("identity")) << index;
        if (index < 3) {
            Record r{Kind::Frame, {11 + int64_t(index), 22 + int64_t(index)}, 1, payload};
            EXPECT_EQ(frameIdentityLine(r, identity), golden.at("identity_lines")[index].get<std::string>());
            batch.add(r, identity);
        }
        ++index;
    }
    for (const auto& symbol : products) EXPECT_EQ(batch.receipt(symbol), golden.at("ranges").at(symbol));
}
TEST(CaptureRouting, MultiMegabyteSnapshotDoesNotMaterializeUpdates) {
    std::string payload = R"({"channel":"l2_data","sequence_num":0,"events":[{"product_id":"BTC-USD","updates":[)";
    const std::string update = R"({"side":"bid","price_level":"12345.670000","new_quantity":"0.12345678","product_id":"ETH-USD"})";
    for (int i = 0; i < 40000; ++i) { if (i) payload += ','; payload += update; }
    payload += "]}]}";
    ASSERT_GT(payload.size(), 3 * 1024 * 1024);
    const std::vector<std::string> products{"BTC-USD", "ETH-USD"};
    allocated = 0; measuring = true;
    const auto identity = frameReceipt(payload, products);
    measuring = false;
    EXPECT_EQ(identity.at("products"), nlohmann::json::array({"BTC-USD"}));
    EXPECT_LT(allocated, 64 * 1024) << "routing allocations=" << allocated;
}
TEST(CaptureRouting, HeaderPeekDoesNotParseOrHashTheL2Body) {
    // An incomplete body cannot pass a full routing/DOM parse. The header peek
    // intentionally stops before it: v1 L2 replay does the one validating DOM parse.
    const std::string payload = R"({"channel":"l2_data","padding":")" +
        std::string(1024 * 1024, 'x') + R"(","events":[)";
    allocated = 0; measuring = true;
    const auto channel = peekFrameChannel(payload);
    measuring = false;
    ASSERT_TRUE(channel); EXPECT_EQ(*channel, "l2_data"); EXPECT_LT(allocated, 4096);
}
