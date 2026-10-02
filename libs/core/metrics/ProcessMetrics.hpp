#pragma once

namespace sentinel::metrics {
class MetricsRegistry;

// Standard process series (names as the Prometheus client libraries use them):
//   process_resident_memory_bytes  gauge   (macOS task_info, Linux /proc/self/statm)
//   process_cpu_seconds_total      counter (getrusage user + system)
//   process_start_time_seconds     gauge   (wall clock when this was called: call at startup)
//   sentinel_build_info{version}   gauge 1
// A sampler that cannot read its value on this platform omits the sample.
void registerProcessMetrics(MetricsRegistry& registry);

} // namespace sentinel::metrics
