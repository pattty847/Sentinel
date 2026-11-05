#include "LevelLogFormat.hpp"
#include "HaltRecord.hpp"
#include "SealStore.hpp"
#include "Crc32.hpp"
#include "SentinelLogging.hpp"
#include "servermodel/PersistenceIo.hpp"
#include <array>
#include <chrono>
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
void LevelLogFiles::remove(const fs::path &p) { check(fs::remove(p), "remove path=" + p.string()); }

struct LevelLog::Impl {
    fs::path root, checkpoint;
    std::string product;
    std::shared_ptr<LevelLogFiles> files;
    bool poisoned = false, opened = false;
    ScanResult state;
    std::optional<LevelLogTail> durable;
    std::set<fs::path> dirty;
    std::unique_ptr<SealStore> seals;
    std::optional<BookImage> sealStart;
    std::vector<MinuteBlock> sealBlocks;
    int64_t retentionMs = 172800000;
    void sealRecord(const LevelLogRecord &record) {
        if (!seals)
            return;
        if (const auto *image = std::get_if<BookImage>(&record.value)) {
            sealStart = *image;
            sealBlocks.clear();
            return;
        }
        check(sealStart.has_value(), "seal requires starting image");
        const auto &block = std::get<MinuteBlock>(record.value);
        sealBlocks.push_back(block);
        if ((block.bucketStartMs + 60000) % kHour)
            return;
        D2Hour hour{*sealStart, std::move(sealBlocks)};
        auto next = endImage(hour);
        if (hour.start.atMs % kHour == 0 && hour.blocks.size() == 60)
            seals->publish(hour);
        sealStart = std::move(next);
        sealBlocks.clear();
    }
    void rebuildSeals() {
        if (!seals)
            return;
        sealStart.reset();
        sealBlocks.clear();
        scan(root, product, [&](const auto &record, const auto &) { sealRecord(record); });
    }

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
    void prepareDirectories() {
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
    }
    void initialize() {
        if (opened) return;
        prepareDirectories();
        if (seals)
            seals->recoverRetention();
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
        files->sync(root / product, true);
        durable = state.last;
        opened = true;
        rebuildSeals();
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
        durable = state.last;
        sLog_Probe("levelstore.log.sync", "product=" << product);
    }
    void cut(int64_t minute, CutImageAtBoundary image) {
        check(minute >= 0 && minute % 60'000 == 0, "unaligned cut point");
        if (!opened) prepareDirectories();
        if (seals) {
            seals->requireRetained(minute);
            seals->recoverRetention();
        }
        auto plan = planCut(root, product, minute, image);
        if (seals)
            seals->cut(minute);
        // Validate the entire retained prefix before the first destructive operation.
        // Persist removal of later files before changing an earlier file.
        for (auto i = plan.files.rbegin(); i != plan.files.rend(); ++i) {
            if (i->at.offset == 0) {
                files->remove(i->at.path);
                files->sync(root / product, true);
            } else {
                files->truncate(i->at.path, i->at.offset);
                files->sync(i->at.path, false);
            }
            sLog_Warning("LevelLog: discarded suffix path=" << i->at.path.string() << " offset=" << i->at.offset
                         << " bytes=" << i->oldSize - i->at.offset << " reason=anchor-cut minute=" << minute);
        }
        // Adopt the surviving prefix as durable, including previously unsynced entries.
        for (const auto &p : paths(root, product)) files->sync(p, false);
        files->sync(root / product, true);
        state = std::move(plan.retained);
        durable = state.last;
        dirty.clear();
        opened = true;
        rebuildSeals();
        sLog_Data("LevelLog: cut product=" << product << " minute=" << minute << " files=" << plan.files.size());
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
void LevelLog::onDayStart(const DayBinding &b, const BookImage &i) {
    impl_->guard([&] {
        impl_->append({b, i});
        impl_->sealRecord({b, i});
    });
}
void LevelLog::onBlock(const DayBinding &b, const MinuteBlock &m) {
    impl_->guard([&] {
        impl_->append({b, m});
        if (impl_->seals && (m.bucketStartMs + 60000) % kHour == 0)
            impl_->commit();
        impl_->sealRecord({b, m});
        if (impl_->seals && (m.bucketStartMs + 60000) % kHour == 0)
            impl_->seals->prune(m.bucketStartMs + 60000, impl_->retentionMs);
    });
}
void LevelLog::enableSeals(int64_t retentionMs, std::function<void(const char *)> point) {
    check(!impl_->opened && retentionMs >= 0, "seal configuration requires fresh writer");
    impl_->retentionMs = retentionMs;
    impl_->seals = std::make_unique<SealStore>(impl_->root, impl_->product, std::move(point));
}
void LevelLog::sync() { impl_->guard([&] { impl_->commit(); }); }
void LevelLog::truncateAfter(int64_t m, CutImageAtBoundary image) { impl_->guard([&] { impl_->cut(m, image); }); }
std::optional<LevelLogTail> LevelLog::durableTail() const {
    check(!impl_->poisoned && impl_->opened, "tail requires an open healthy writer");
    return impl_->durable;
}
bool LevelLog::failed() const { return impl_->poisoned; }
} // namespace sentinel::levelstore

namespace sentinel::levelstore {
void LevelLog::setCheckpointPath(std::filesystem::path path) { impl_->checkpoint = std::move(path); }
void LevelLog::discardRecorderTemps(const fs::path &anchors, const std::function<void(const char *)> &point) {
    impl_->guard([&] {
        const auto suffix = [](std::string_view name, std::string_view prefix) {
            return name.starts_with(prefix) && name.size() == prefix.size() + 6 &&
                   std::all_of(name.begin() + prefix.size(), name.end(), [](char c) {
                       return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9');
                   });
        };
        const auto discard = [&](const fs::directory_entry &entry) {
            if (!fs::is_regular_file(entry.symlink_status()))
                return;
            impl_->files->remove(entry.path());
            impl_->files->sync(entry.path().parent_path(), true);
            if (point)
                point("c-recovery-temp-removed");
        };
        if (fs::is_directory(impl_->checkpoint.parent_path()))
            for (const auto &entry : fs::directory_iterator(impl_->checkpoint.parent_path()))
                if (suffix(entry.path().filename().string(), "roller.json."))
                    discard(entry);
        if (!fs::is_directory(fs::symlink_status(anchors)))
            return;
        for (const auto &day : fs::directory_iterator(anchors)) {
            const auto date = day.path().filename().string();
            if (!fs::is_directory(day.symlink_status()) || date.size() != 10 || date[4] != '-' || date[7] != '-')
                continue;
            const auto number = [](std::string_view text) {
                int n = 0;
                for (const char c : text) {
                    if (c < '0' || c > '9')
                        return -1;
                    n = n * 10 + c - '0';
                }
                return n;
            };
            const auto view = std::string_view(date);
            const auto y = number(view.substr(0, 4)), m = number(view.substr(5, 2)), d = number(view.substr(8, 2));
            if (y < 1 || m < 1 || d < 1 ||
                !std::chrono::year_month_day{std::chrono::year{y}, std::chrono::month{unsigned(m)},
                                            std::chrono::day{unsigned(d)}}.ok())
                continue;
            for (const auto &entry : fs::directory_iterator(day.path())) {
                const auto name = entry.path().filename().string();
                if (name.size() != 18 || !suffix(name, name.substr(0, 4) + ".anchor."))
                    continue;
                const auto hour = number(std::string_view(name).substr(0, 2));
                const auto minute = number(std::string_view(name).substr(2, 2));
                if (hour >= 0 && hour < 24 && minute >= 0 && minute < 60 && minute % 15 == 0)
                    discard(entry);
            }
        }
    });
}
void LevelLog::onHalt(const nlohmann::json &record) {
    writeHalt(impl_->root, impl_->product, record, impl_->checkpoint);
}
} // namespace sentinel::levelstore
