// Retained memory of JournalReader file catalogs over a real journal root (read-only:
// it parses file headers and never decodes blocks or writes). Not a ctest.
//   journal_catalog_memory_bench <raw-l2 root> [PRODUCT[,PRODUCT...]]
// Reports heap bytes and live allocations kept by each catalog, then by one cursor
// copy (the JournalReader(catalog, start) constructor) of the last catalog.
#include "heap_in_use.hpp"
#include "roller/JournalReader.hpp"
#include <chrono>
#include <iostream>
#include <memory>
#include <sstream>
#include <vector>

using namespace sentinel;

int main(int argc, char** argv) try {
    if (argc < 2 || argc > 3) {
        std::cerr << "usage: journal_catalog_memory_bench <raw-l2 root> [PRODUCT[,PRODUCT...]]\n";
        return 2;
    }
    std::vector<std::string> products;
    std::stringstream list(argc == 3 ? argv[2] : "BTC-USD");
    for (std::string p; std::getline(list, p, ',');) if (!p.empty()) products.push_back(p);
    if (!test::heapInUse()) throw std::runtime_error("heap accounting unavailable on this platform");
    const auto footprint = [] { return test::physFootprint().value_or(0); };
    const auto startHeap = *test::heapInUse();
    const auto startFootprint = footprint();
    std::vector<std::unique_ptr<roller::JournalReader>> catalogs;
    size_t totalFiles = 0;
    for (const auto& product : products) {
        const auto before = *test::heapInUse();
        const auto started = std::chrono::steady_clock::now();
        catalogs.push_back(std::make_unique<roller::JournalReader>(argv[1], product));
        const auto ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - started).count();
        const auto after = *test::heapInUse();
        const auto files = catalogs.back()->files().size();
        totalFiles += files;
        const auto bytes = int64_t(after.bytes) - int64_t(before.bytes);
        std::cout << "{\"product\":\"" << product << "\",\"files\":" << files
                  << ",\"catalog_ms\":" << ms << ",\"retained_bytes\":" << bytes
                  << ",\"retained_blocks\":" << int64_t(after.blocks) - int64_t(before.blocks)
                  << ",\"bytes_per_file\":" << (files ? bytes / int64_t(files) : 0) << "}\n";
    }
    const auto catalogsHeap = *test::heapInUse();
    const auto catalogsFootprint = footprint();
    roller::JournalReader cursor(*catalogs.back(), std::nullopt);
    const auto copyHeap = *test::heapInUse();
    std::cout << "{\"total_files\":" << totalFiles
              << ",\"catalogs_retained_bytes\":" << int64_t(catalogsHeap.bytes) - int64_t(startHeap.bytes)
              << ",\"catalogs_retained_blocks\":" << int64_t(catalogsHeap.blocks) - int64_t(startHeap.blocks)
              << ",\"phys_footprint_delta_bytes\":" << int64_t(catalogsFootprint) - int64_t(startFootprint)
              << ",\"cursor_copy_bytes\":" << int64_t(copyHeap.bytes) - int64_t(catalogsHeap.bytes)
              << ",\"cursor_copy_blocks\":" << int64_t(copyHeap.blocks) - int64_t(catalogsHeap.blocks) << "}\n";
    return 0;
} catch (const std::exception& e) {
    std::cerr << "journal_catalog_memory_bench: " << e.what() << '\n';
    return 1;
}
