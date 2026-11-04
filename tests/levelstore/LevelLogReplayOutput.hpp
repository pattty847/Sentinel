#pragma once
// Shared offline replay output and its measurement evidence.
#include "levelstore/LevelLog.hpp"
#include <nlohmann/json.hpp>
#include <chrono>
#include <fstream>

namespace levelstore_test {
using namespace sentinel::levelstore;
namespace fs = std::filesystem;
using nlohmann::json;
struct LogReplayOutput final : LevelOutput {
    LevelLog log;
    std::ofstream expected;
    std::vector<double> costs;
    size_t count = 0;
    json imageCosts = json::array();
    LogReplayOutput(const fs::path &root, const std::string &product)
        : log(root / "log", product), expected(root / "expected.bin", std::ios::binary) {
        expected.exceptions(std::ios::badbit | std::ios::failbit);
        log.open();
    }
    static ByteWriter canonical(const LevelLogRecord &r) {
        ByteWriter w;
        w.str(r.binding.symbol); w.svarint(r.binding.floorMs); w.svarint(r.binding.ceilingMs);
        w.svarint(r.binding.priceDigits);
        if (const auto *i = std::get_if<BookImage>(&r.value)) { w.u8(1); encodeImage(w, *i); }
        else { w.u8(2); encodeBlock(w, std::get<MinuteBlock>(r.value)); }
        return w;
    }
    void save(const LevelLogRecord &r) {
        const auto w = canonical(r);
        ByteWriter length; length.fixed64(w.bytes.size());
        expected.write(reinterpret_cast<const char *>(length.bytes.data()), 8);
        expected.write(reinterpret_cast<const char *>(w.bytes.data()), static_cast<std::streamsize>(w.bytes.size()));
        ++count;
    }
    void onDayStart(const DayBinding &b, const BookImage &i) override {
        const auto start = std::chrono::steady_clock::now();
        log.onDayStart(b, i); log.sync();
        const auto ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count();
        imageCosts.push_back({{"symbol", b.symbol}, {"atMs", i.atMs}, {"appendSyncMs", ms}});
        save({b, i});
    }
    void onBlock(const DayBinding &b, const MinuteBlock &m) override {
        const auto start = std::chrono::steady_clock::now();
        log.onBlock(b, m); log.sync();
        costs.push_back(std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count());
        save({b, m});
    }
};
}
