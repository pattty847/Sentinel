#pragma once
#include <boost/multiprecision/cpp_int.hpp>
#include <cstddef>
#include <stdexcept>
#include <string>
#include <utility>

namespace sentinel::levelstore {
class EncodingError : public std::overflow_error {
  public:
    explicit EncodingError(const std::string &why) : std::overflow_error(why) {}
};
inline void encodingCheck(bool ok, const std::string &why) {
    if (!ok) throw EncodingError(why);
}
// Only wrap in-memory work. File operations must remain outside this boundary.
// bad_alloc is deliberately not a runtime_error or logic_error: it stays transient.
template<class F> decltype(auto) encoding(const std::string &context, F &&work) {
    try { return std::forward<F>(work)(); }
    catch (const std::runtime_error &e) { throw EncodingError(context + ": " + e.what()); }
    catch (const std::logic_error &e) { throw EncodingError(context + ": " + e.what()); }
}
// A valid in-memory value can exceed a format's bounded frame. Classify this
// separately from I/O and allocation failure so the product halts deterministically.
inline void requireEncodingSize(size_t bytes, size_t maximum) {
    if (bytes > maximum)
        throw std::overflow_error("encoding frame exceeds representation limit");
}

// Fixed storage, checked signed 128-bit range (no allocator). All aggregate
// additions and products throw on overflow; serialization never wraps a sum.
using Wide = boost::multiprecision::number<boost::multiprecision::cpp_int_backend<
    127, 127, boost::multiprecision::signed_magnitude, boost::multiprecision::checked, void>>;
} // namespace sentinel::levelstore
