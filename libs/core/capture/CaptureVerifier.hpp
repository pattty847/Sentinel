#pragma once
#include "RawCapture.hpp"
#include <map>

namespace sentinel::capture {

// Decimal strings are converted to integer ticks/atoms without a double at any
// point. Extra trailing zeroes are accepted; off-grid values are errors.
class DecimalGrid {
public:
    explicit DecimalGrid(const std::string& increment);
    uint64_t atoms(const std::string& value) const;
private:
    struct Decimal { uint64_t mantissa; uint32_t places; };
    static Decimal parse(const std::string& value);
    Decimal m_increment;
};

struct VerificationReport {
    nlohmann::json json;
    bool ok = false;
};
// Optional synchronization point for deterministic live-rotation tests: the
// selected file list is fixed before this callback, archive inventory follows.
VerificationReport verify(const QString& fileOrDirectory, const std::function<void()>& afterDiscovery = {});

} // namespace sentinel::capture
