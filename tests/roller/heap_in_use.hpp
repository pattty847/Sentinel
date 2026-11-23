#pragma once
// Process-wide heap accounting for retained-memory tests and benches. Values are
// whole-process totals: measure deltas around single-threaded work only.
#include <cstdint>
#include <optional>
#if defined(__APPLE__)
#include <malloc/malloc.h>
#include <mach/mach.h>
#elif defined(__GLIBC__)
#include <malloc.h>
#endif

namespace sentinel::test {
struct HeapUse {
    uint64_t bytes = 0;
    uint64_t blocks = 0; // live allocations; 0 where the platform does not report it
};
inline std::optional<HeapUse> heapInUse() {
#if defined(__APPLE__)
    malloc_statistics_t s{};
    malloc_zone_statistics(nullptr, &s);
    return HeapUse{s.size_in_use, s.blocks_in_use};
#elif defined(__GLIBC__) && (__GLIBC__ > 2 || (__GLIBC__ == 2 && __GLIBC_MINOR__ >= 33))
    const auto m = mallinfo2();
    return HeapUse{uint64_t(m.uordblks) + uint64_t(m.hblkhd), 0};
#else
    return std::nullopt;
#endif
}
// Physical footprint as Activity Monitor and vmmap report it (macOS only).
inline std::optional<uint64_t> physFootprint() {
#if defined(__APPLE__)
    task_vm_info_data_t info{};
    mach_msg_type_number_t count = TASK_VM_INFO_COUNT;
    if (task_info(mach_task_self(), TASK_VM_INFO, reinterpret_cast<task_info_t>(&info), &count) != KERN_SUCCESS)
        return std::nullopt;
    return info.phys_footprint;
#else
    return std::nullopt;
#endif
}
} // namespace sentinel::test
