#pragma once
#include <filesystem>
#include <memory>
#include <cstdint>
struct RestartJournal {
    static constexpr int64_t Epoch = 1'798'761'600'000;
    explicit RestartJournal(const std::filesystem::path&, bool warmup = true);
    ~RestartJournal();
    void grow(int minutes);
    void restart(int second);
    struct Impl;
    std::unique_ptr<Impl> impl;
};
