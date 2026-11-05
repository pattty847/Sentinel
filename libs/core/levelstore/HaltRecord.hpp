#pragma once
#include <filesystem>
#include <nlohmann/json.hpp>
#include <optional>

namespace sentinel::levelstore {
std::optional<nlohmann::json> readHalt(const std::filesystem::path &root, const std::string &product);
void writeHalt(const std::filesystem::path &root, const std::string &product, nlohmann::json record,
               const std::filesystem::path &checkpoint);
} // namespace sentinel::levelstore
