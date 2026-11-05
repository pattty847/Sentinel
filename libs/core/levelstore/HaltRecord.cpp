#include "HaltRecord.hpp"
#include "Crc32.hpp"
#include "servermodel/PersistenceIo.hpp"
#include <fstream>
#include <stdexcept>

namespace sentinel::levelstore {
namespace fs = std::filesystem;
namespace {
fs::path path(const fs::path &root, const std::string &product) { return root / product / "halt.json"; }
void sync(const fs::path &p, bool directory) {
    int error = 0;
    if (!(directory ? persistence::syncDirectory(p, error) : persistence::syncFilePath(p, error)))
        throw std::runtime_error("halt record sync failed: " + p.string());
}
} // namespace
std::optional<nlohmann::json> readHalt(const fs::path &root, const std::string &product) {
    const auto p = path(root, product);
    if (!fs::exists(p))
        return {};
    std::ifstream in(p);
    nlohmann::json envelope;
    in >> envelope;
    if (envelope.at("version") != 1)
        throw std::runtime_error("halt record version");
    const auto &record = envelope.at("record");
    const auto text = record.dump();
    if (record.at("symbol") != product || envelope.at("crc32") != sentinel::crc32(text.data(), text.size()))
        throw std::runtime_error("halt record identity/CRC mismatch");
    return record;
}
void writeHalt(const fs::path &root, const std::string &product, nlohmann::json record, const fs::path &checkpoint) {
    if (readHalt(root, product))
        return; // A halted root is immutable across restarts and representation versions.
    if (!checkpoint.empty() && fs::exists(checkpoint)) {
        std::ifstream in(checkpoint);
        in >> record["committedCheckpoint"];
    } else
        record["committedCheckpoint"] = nullptr;
    const auto p = path(root, product), tmp = fs::path(p.string() + ".tmp");
    fs::create_directories(p.parent_path());
    sync(p.parent_path().parent_path(), true);
    const auto body = record.dump();
    const nlohmann::json envelope = {
        {"version", 1}, {"record", record}, {"crc32", sentinel::crc32(body.data(), body.size())}};
    {
        std::ofstream out(tmp, std::ios::binary | std::ios::trunc);
        out.exceptions(std::ios::badbit | std::ios::failbit);
        out << envelope.dump() << '\n';
        out.close();
    }
    sync(tmp, false);
    fs::rename(tmp, p);
    sync(p.parent_path(), true);
    const auto verified = readHalt(root, product);
    if (!verified || *verified != record)
        throw std::runtime_error("halt record read-back mismatch");
}
} // namespace sentinel::levelstore
