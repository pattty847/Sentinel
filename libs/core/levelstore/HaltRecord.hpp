#pragma once
#include <filesystem>
#include <nlohmann/json.hpp>
#include <optional>

namespace sentinel::levelstore {
inline constexpr uint64_t kMaxHaltBytes = 64ULL * 1024 * 1024;
std::optional<nlohmann::json> readHalt(const std::filesystem::path &root, const std::string &product);
void writeHalt(const std::filesystem::path &root, const std::string &product, nlohmann::json record,
               const std::filesystem::path &checkpoint);
} // namespace sentinel::levelstore
