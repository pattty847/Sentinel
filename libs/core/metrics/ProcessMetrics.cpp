#include "ProcessMetrics.hpp"
#include "MetricsRegistry.hpp"
#include "Version.hpp"
#include <chrono>
#include <optional>

#if defined(__APPLE__)
#include <mach/mach.h>
#endif
#if defined(__APPLE__) || defined(__linux__)
#include <sys/resource.h>
#include <unistd.h>
#endif
#if defined(__linux__)
#include <cstdio>
#endif

namespace sentinel::metrics {

namespace {
std::optional<double> residentBytes() {
#if defined(__APPLE__)
    mach_task_basic_info_data_t info{};
    mach_msg_type_number_t count = MACH_TASK_BASIC_INFO_COUNT;
    if (task_info(mach_task_self(), MACH_TASK_BASIC_INFO, reinterpret_cast<task_info_t>(&info), &count) !=
        KERN_SUCCESS)
        return std::nullopt;
    return static_cast<double>(info.resident_size);
#elif defined(__linux__)
    std::FILE* f = std::fopen("/proc/self/statm", "r");
    if (!f) return std::nullopt;
    long pages = 0, resident = 0;
    const int n = std::fscanf(f, "%ld %ld", &pages, &resident);
    std::fclose(f);
    if (n != 2) return std::nullopt;
    return static_cast<double>(resident) * static_cast<double>(sysconf(_SC_PAGESIZE));
#else
    return std::nullopt;
#endif
}

std::optional<double> cpuSeconds() {
#if defined(__APPLE__) || defined(__linux__)
    rusage usage{};
    if (getrusage(RUSAGE_SELF, &usage) != 0) return std::nullopt;
    const auto seconds = [](const timeval& tv) { return tv.tv_sec + tv.tv_usec / 1e6; };
    return seconds(usage.ru_utime) + seconds(usage.ru_stime);
#else
    return std::nullopt;
#endif
}
} // namespace

void registerProcessMetrics(MetricsRegistry& registry) {
    registry.gaugeFn("process_resident_memory_bytes", "Resident memory size in bytes.", {}, residentBytes);
    registry.counterFn("process_cpu_seconds_total", "Total user and system CPU time spent in seconds.", {},
                       cpuSeconds);
    const double startSeconds =
        std::chrono::duration<double>(std::chrono::system_clock::now().time_since_epoch()).count();
    registry.gauge("process_start_time_seconds", "Start time of the process since unix epoch in seconds.")
        .set(startSeconds);
    registry.gauge("sentinel_build_info", "Always 1; the label carries the Sentinel version.",
                   {{"version", Sentinel::getVersionString()}})
        .set(1);
}

} // namespace sentinel::metrics
