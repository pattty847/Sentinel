#include "LevelLogFormat.hpp"
#include "Crc32.hpp"
#include "SentinelLogging.hpp"
#include "servermodel/PersistenceIo.hpp"
#include <array>
#include <map>
#include <set>

namespace sentinel::levelstore {
using namespace log_detail;
void LevelLogFiles::createDirectory(const fs::path &p) { fs::create_directory(p); }
std::FILE *LevelLogFiles::openAppend(const fs::path &p) {
#ifdef _WIN32
    auto *f = _wfopen(p.c_str(), L"ab");
#else
    auto *f = std::fopen(p.c_str(), "ab");
#endif
    check(f != nullptr, "append open path=" + p.string() + " errno=" + std::to_string(errno));
    if (std::setvbuf(f, nullptr, _IONBF, 0) != 0) {
        std::fclose(f);
        throw std::runtime_error("LevelLog: setvbuf path=" + p.string());
    }
    return f;
}
size_t LevelLogFiles::write(std::FILE *f, std::span<const uint8_t> bytes) {
    return std::fwrite(bytes.data(), 1, bytes.size(), f);
}
void LevelLogFiles::close(std::FILE *f) {
    const auto result = std::fclose(f);
    check(result == 0, "close errno=" + std::to_string(errno));
}
void LevelLogFiles::sync(const fs::path &p, bool directory) {
    int error = 0;
    const bool ok = directory ? persistence::syncDirectory(p, error) : persistence::syncFilePath(p, error);
    check(ok, "sync path=" + p.string() + " directory=" + std::to_string(directory) + " error=" + std::to_string(error));
}
void LevelLogFiles::truncate(const fs::path &p, uint64_t size) { fs::resize_file(p, size); }

struct LevelLog::Impl {
    fs::path root;
    std::string product;
    std::shared_ptr<LevelLogFiles> files;
    bool poisoned = false, opened = false;
    ScanResult state;
    std::set<fs::path> dirty;

    template <class F> void guard(F action) {
        check(!poisoned, "writer poisoned product=" + product);
        try { action(); }
        catch (const std::exception &e) {
            poisoned = true;
            sLog_Error("LevelLog: writer poisoned product=" << product << " root=" << root.string()
                       << " reason=" << e.what());
            throw;
        }
        catch (...) {
            poisoned = true;
            sLog_Error("LevelLog: writer poisoned product=" << product << " root=" << root.string());
            throw;
        }
    }
    void initialize() {
        if (opened) return;
        validateProduct(product);
        check(bool(files), "missing file operations");
        root = fs::absolute(root).lexically_normal();
        const auto dir = root / product;
        std::vector<fs::path> missing;
        for (auto at = dir; at != at.parent_path() && !fs::is_directory(at); at = at.parent_path())
            missing.push_back(at);
        for (auto i = missing.rbegin(); i != missing.rend(); ++i) files->createDirectory(*i);
        // Existing entries may have been left unsynced by a failed former writer.
        for (auto at = dir; at != at.parent_path(); at = at.parent_path()) files->sync(at.parent_path(), true);
        state = scan(root, product, {});
        if (state.tail) {
            const auto tail = *state.tail;
            files->truncate(tail.path, tail.offset);
            files->sync(tail.path, false);
            sLog_Warning("LevelLog: truncated incomplete terminal frame path=" << tail.path.string()
                         << " offset=" << tail.offset);
            state.tail.reset();
        }
        // Re-establish durability of inherited files (including an earlier failed create).
        for (const auto &p : paths(root, product)) files->sync(p, false);
        files->sync(dir, true);
        opened = true;
        sLog_Data("LevelLog: opened product=" << product << " root=" << root.string());
    }
    void append(const LevelLogRecord &r) {
        initialize();
        check(r.binding.symbol == product, "foreign product");
        auto next = state;
        advance(next, r);
        const auto p = payload(r);
        // Validate caller-owned structures before any byte reaches the file.
        ByteReader validation(p.bytes);
        (void)validation.str(256);
        (void)validation.svarint();
        (void)validation.svarint();
        (void)validation.svarint();
        if (std::holds_alternative<BookImage>(r.value)) (void)decodeImage(validation);
        else (void)decodeBlock(validation);
        const auto h = header(std::holds_alternative<BookImage>(r.value) ? 1 : 2, p.bytes.size());
        ByteWriter trailer;
        trailer.fixed32(sentinel::crc32Update(sentinel::crc32(h.bytes.data(), h.bytes.size()), p.bytes.data(), p.bytes.size()));
        const auto path = LevelLogReader::hourPath(root, product, r.minute());
        dirty.insert(path); // allocation must precede the first write
        auto *f = files->openAppend(path);
        try {
            for (const auto *part : std::array<const std::vector<uint8_t> *, 3>{&h.bytes, &p.bytes, &trailer.bytes})
                check(files->write(f, *part) == part->size(), "short write path=" + path.string());
        } catch (...) {
            std::fclose(f); // never flush buffered bytes after an uncertain write
            throw;
        }
        files->close(f);
        state = std::move(next);
        sLog_Probe("levelstore.log.append", "product=" << product << " minute=" << r.minute()
                   << " bytes=" << h.bytes.size() + p.bytes.size() + trailer.bytes.size());
    }
    void commit() {
        initialize();
        for (const auto &p : dirty) files->sync(p, false);
        // File contents precede publication of newly created directory entries.
        files->sync(root / product, true);
        dirty.clear();
        sLog_Probe("levelstore.log.sync", "product=" << product);
    }
    void cut(int64_t minute) {
        initialize();
        check(minute >= 0 && minute % 60'000 == 0, "unaligned cut point");
        std::map<fs::path, uint64_t> cuts;
        scan(root, product, [&](const LevelLogRecord &r, const Position &p) {
            if (r.minute() >= minute) cuts.try_emplace(p.path, p.offset);
        });
        // Persist removal of later records before changing an earlier file.
        for (auto i = cuts.rbegin(); i != cuts.rend(); ++i) {
            files->truncate(i->first, i->second);
            files->sync(i->first, false);
        }
        commit();
        state = scan(root, product, {});
        sLog_Data("LevelLog: cut product=" << product << " minute=" << minute << " files=" << cuts.size());
    }
};
LevelLog::LevelLog(fs::path root, std::string product, std::shared_ptr<LevelLogFiles> files)
    : impl_(std::make_unique<Impl>()) {
    impl_->root = std::move(root);
    impl_->product = std::move(product);
    impl_->files = std::move(files);
}
LevelLog::~LevelLog() = default; // no implicit durability promise
void LevelLog::open() { impl_->guard([&] { impl_->initialize(); }); }
void LevelLog::onDayStart(const DayBinding &b, const BookImage &i) { impl_->guard([&] { impl_->append({b, i}); }); }
void LevelLog::onBlock(const DayBinding &b, const MinuteBlock &m) { impl_->guard([&] { impl_->append({b, m}); }); }
void LevelLog::sync() { impl_->guard([&] { impl_->commit(); }); }
void LevelLog::truncateAfter(int64_t m) { impl_->guard([&] { impl_->cut(m); }); }
bool LevelLog::failed() const { return impl_->poisoned; }
} // namespace sentinel::levelstore
