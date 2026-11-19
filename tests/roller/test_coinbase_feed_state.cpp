// Pins every exported boundary in both checked-in Coinbase captures against
// baa928cf, including the CBOR bytes used by anchors. Never regenerate on a port.
#include "roller/JournalFeed.hpp"
#include <gtest/gtest.h>
#include <QCryptographicHash>
#include <QTemporaryDir>
#include <filesystem>

using namespace sentinel::roller;
TEST(CoinbaseFeedState, CapturedFixturesMatchBaseBytes) {
    for (const auto* name : {"btc-prefix.rawl2", "btc-hour-boundary.rawl2"}) {
        QTemporaryDir temp; ASSERT_TRUE(temp.isValid());
        std::filesystem::copy_file(std::filesystem::path(COINBASE_STATE_FIXTURES) / name,
                                  std::filesystem::path(temp.path().toStdString()) / name);
        JournalReader reader(temp.path().toStdString(), "BTC-USD");
        JournalFeed feed("BTC-USD");
        feed.onTrade = [](const Trade&) {};
        QCryptographicHash hash(QCryptographicHash::Sha256);
        JournalRecord record;
        size_t count = 0;
        while (reader.next(record)) {
            feed.apply(record);
            const auto state = feed.exportState();
            const auto text = state.dump();
            const auto bytes = nlohmann::json::to_cbor(state);
            hash.addData(QByteArrayView(text.data(), text.size()));
            hash.addData(QByteArrayView(reinterpret_cast<const char*>(bytes.data()), bytes.size()));
            ++count;
        }
        std::cout << name << " records=" << count << " sha256=" << hash.result().toHex().constData() << '\n';
        const bool prefix = std::string_view(name) == "btc-prefix.rawl2";
        EXPECT_EQ(count, prefix ? 4143u : 44266u);
        EXPECT_EQ(hash.result().toHex().toStdString(), prefix
            ? "5665eb61ed4d778216e812e4ca624c354af2d38746f8b6e2d4293739ba1e5158"
            : "2c2d5adb2b574970e331f3f0ce96f0072a776e148c10fce4aaa47812908c9206");
    }
}
