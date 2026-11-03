#pragma once
// Shared helpers for the levelstore tests and evidence tools (no Qt headers).
#include "levelstore/LevelRecorder.hpp"
#include "levelstore/MinuteSweep.hpp"
#include "servermodel/BookRecorder.hpp"
#include <cstdlib>
#include <filesystem>
#include <optional>
#include <random>
#include <stdexcept>
#include <string>
#include <vector>

namespace levelstore_test {
using namespace sentinel::levelstore;
namespace fs = std::filesystem;

// A level as JournalFeed produces it: the double strtod gives plus the text's decimal.
inline recording::Level L(bool isBid, const char *text, double size) {
    return {isBid, std::strtod(text, nullptr), size, recording::parsePriceDecimal(text)};
}

struct TempDir {
    fs::path path;
    TempDir() {
        std::random_device rd;
        path = fs::temp_directory_path() / ("levelstore-test-" + std::to_string(rd()) + std::to_string(rd()));
        fs::create_directories(path);
    }
    ~TempDir() {
        std::error_code ec;
        fs::remove_all(path, ec);
    }
};

// Collects C's output. failNextBlock: the next onBlock throws (an output failure).
struct Capture final : LevelOutput {
    std::optional<BookImage> image;
    std::vector<MinuteBlock> blocks;
    int dayStarts = 0;
    bool failNextBlock = false;
    bool failDayStart = false, failAfterAccept = false;
    int blockCalls = 0;
    int failBlocks = 0; // the next N blocks throw
    void onDayStart(const DayBinding &, const BookImage &i) override {
        ++dayStarts;
        if (failDayStart && !failAfterAccept) throw std::runtime_error("injected image failure");
        image = i;
        if (failDayStart) throw std::runtime_error("injected image failure after accept");
    }
    void onBlock(const DayBinding &, const MinuteBlock &b) override {
        ++blockCalls;
        if (failNextBlock || failBlocks > 0) {
            if (failAfterAccept) blocks.push_back(b);
            failNextBlock = false;
            if (failBlocks > 0) --failBlocks;
            throw std::runtime_error("injected output failure");
        }
        blocks.push_back(b);
    }
    std::vector<uint8_t> bytes() const {
        ByteWriter w;
        w.u8(image ? 1 : 0);
        if (image) encodeImage(w, *image);
        for (const auto &b : blocks) encodeBlock(w, b);
        return std::move(w.bytes);
    }
};

// Native rows of every block, from the image (exact Data Lab semantics).
struct SweptMinute {
    int64_t bucket;
    int64_t covered;
    std::vector<BinRow> rows;
};
inline std::vector<SweptMinute> sweep(const Capture &c, int64_t binUnits = 1) {
    std::vector<SweptMinute> out;
    if (!c.image) return out;
    MinuteSweep s(binUnits);
    s.start(*c.image);
    for (const auto &b : c.blocks) {
        SweptMinute m{b.bucketStartMs, 0, {}};
        s.apply(b, m.rows, m.covered);
        out.push_back(std::move(m));
    }
    return out;
}

} // namespace levelstore_test
