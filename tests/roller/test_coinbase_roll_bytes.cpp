// SHA-256 pins every HMC2, checkpoint and anchor byte produced by the unmodified
// 6910fc11 roller on both checked-in Coinbase captures. See the manifest for ranges.
#include "roller/Roller.hpp"
#include <gtest/gtest.h>
#include <QCryptographicHash>
#include <QFile>
#include <QTemporaryDir>
#include <fstream>

namespace fs = std::filesystem;
using nlohmann::json;
using namespace sentinel::roller;

TEST(CoinbaseRollBytes, Base6910fc11FilesAndPolicyIdentity) {
    std::ifstream in(fs::path(KALSHI_FIXTURES) / "coinbase_roll_6910fc11.json");
    const auto golden = json::parse(in);
    ASSERT_EQ(golden.at("base"), "6910fc11");
    for (const auto& run : golden.at("runs")) {
        QTemporaryDir temp;
        ASSERT_TRUE(temp.isValid());
        const fs::path root = temp.path().toStdString();
        fs::create_directories(root / "raw");
        fs::copy_file(fs::path(KALSHI_FIXTURES) / run.at("fixture").get<std::string>(), root / "raw" / "input.rawl2");
        RollOptions o{root / "raw", root / "out", "BTC-USD",
                      parseTime(run.at("from")), parseTime(run.at("to"))};
        roll(o);
        json actual = json::object();
        for (const auto& entry : fs::recursive_directory_iterator(o.outputRoot)) {
            if (!entry.is_regular_file()) continue;
            const auto path = entry.path();
            if (path.extension() != ".hmc2" && path.extension() != ".anchor" && path.filename() != "roller.json") continue;
            QFile file(QString::fromStdString(path.string()));
            ASSERT_TRUE(file.open(QIODevice::ReadOnly));
            actual[fs::relative(path, o.outputRoot).generic_string()] =
                QCryptographicHash::hash(file.readAll(), QCryptographicHash::Sha256).toHex().toStdString();
        }
        EXPECT_EQ(actual, run.at("sha256")) << run.at("fixture");
        std::ifstream checkpoint(o.outputRoot / "BTC-USD" / "roller.json");
        EXPECT_EQ(json::parse(checkpoint).at("configHash"), run.at("configHash"));
    }
}
