#pragma once
// The one rule for where the recording lives: config recording.dir, or
// recording.fallback_dir while dir is on an unmounted /Volumes/<name> volume.
// The server writes there; the lab and tests read the same place.
#include <filesystem>
#include <string>

namespace recording {
// "/Volumes/<name>/..." is usable only while that volume is mounted. Writing
// there otherwise would silently fill the boot disk on macOS, and on Windows
// (where the path has a root directory but no drive) create C:\Volumes\...
inline bool volumeMounted(const std::filesystem::path &dir) {
    auto it = dir.begin();
    if (dir.has_root_directory() && !dir.has_root_name() && it != dir.end() && ++it != dir.end() &&
        *it == "Volumes" && ++it != dir.end())
        return std::filesystem::is_directory(std::filesystem::path("/Volumes") / *it);
    return true;
}

struct RecordingDirChoice {
    std::filesystem::path dir;  // empty: nowhere to record
    bool fallback = false;      // dir is fallbackDir because the volume is not mounted
};

// Relative results are relative to the working directory, as the server resolves them.
inline RecordingDirChoice resolveRecordingDir(const std::string &dir, const std::string &fallbackDir) {
    if (volumeMounted(dir)) return {dir, false};
    if (fallbackDir.empty()) return {};
    return {fallbackDir, true};
}
} // namespace recording
