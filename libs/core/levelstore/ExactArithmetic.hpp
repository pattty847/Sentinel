#pragma once
#include <boost/multiprecision/cpp_int.hpp>
#include <cstddef>
#include <stdexcept>

namespace sentinel::levelstore {
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
