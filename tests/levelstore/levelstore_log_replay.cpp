// Offline RAWL2 replay evidence. Input is read-only; all output must be a new directory.
#include "levelstore/LevelLog.hpp"
#include "roller/Roller.hpp"
#include <QCoreApplication>
#include <nlohmann/json.hpp>
#include <algorithm>
#include <chrono>
#include <fstream>
#include <iostream>

namespace fs = std::filesystem;
using namespace sentinel::levelstore;
using nlohmann::json;
namespace {
struct Output final : LevelOutput {
    LevelLog log;
    std::ofstream expected;
    std::vector<double> costs;
    size_t count = 0;
    double imageMs = 0;
    Output(const fs::path &root, const std::string &product)
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
        imageMs = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count();
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
int main(int argc, char **argv) {
    QCoreApplication app(argc, argv);
    if (argc != 6) { std::cerr << "RAW_ROOT NEW_OUT PRODUCT YYYY-MM-DD REPORT.json\n"; return 2; }
    json report{{"pass", false}};
    try {
        const fs::path raw = fs::canonical(argv[1]), out = fs::absolute(argv[2]).lexically_normal();
        const auto source = raw.string() + "/", dest = out.string() + "/";
        if (fs::exists(out) || dest.starts_with(source) || source.starts_with(dest))
            throw std::runtime_error("output must be new and disjoint from RAWL2");
        fs::create_directories(out);
        Output output(out, argv[3]);
        sentinel::roller::RollOptions o;
        o.journalRoot = raw; o.outputRoot = out / "hmc2"; o.product = argv[3];
        o.fromMs = sentinel::roller::parseTime(argv[4]); o.toMs = o.fromMs + 86'400'000;
        o.useAnchors = false; o.writeAnchors = false; o.levelOutput = &output;
        const auto result = sentinel::roller::roll(o);
        output.log.sync(); output.expected.close();
        std::ifstream expected(out / "expected.bin", std::ios::binary);
        expected.exceptions(std::ios::badbit | std::ios::failbit);
        size_t compared = 0;
        LevelLogReader(out / "log", argv[3]).read([&](const LevelLogRecord &r) {
            uint8_t header[8]; expected.read(reinterpret_cast<char *>(header), 8);
            ByteReader length(header); const auto n = length.fixed64();
            const auto actual = Output::canonical(r);
            if (n != actual.bytes.size()) throw std::runtime_error("record length differs");
            std::vector<uint8_t> wanted(n);
            expected.read(reinterpret_cast<char *>(wanted.data()), static_cast<std::streamsize>(n));
            if (wanted != actual.bytes) throw std::runtime_error("record bytes differ");
            ++compared;
        });
        uint64_t bytes = 0;
        for (const auto &e : fs::recursive_directory_iterator(out / "log"))
            if (e.is_regular_file()) bytes += e.file_size();
        std::sort(output.costs.begin(), output.costs.end());
        const auto q = [&](double f) { return output.costs.at(size_t(f * double(output.costs.size() - 1) + 0.5)); };
        report = {{"pass", compared == output.count && output.costs.size() == 1440},
                  {"product", argv[3]}, {"day", argv[4]}, {"recordsCompared", compared},
                  {"bytesPerProductDay", bytes}, {"minutes", output.costs.size()},
                  {"appendSyncMs", {{"p50", q(0.5)}, {"p99", q(0.99)}, {"max", output.costs.back()}}},
                  {"imageAppendSyncMs", output.imageMs}, {"roll", result}};
    } catch (const std::exception &e) { report["error"] = e.what(); }
    std::ofstream(argv[5]) << report.dump(2) << '\n';
    std::cout << report.dump() << '\n';
    return report["pass"].get<bool>() ? 0 : 1;
}
