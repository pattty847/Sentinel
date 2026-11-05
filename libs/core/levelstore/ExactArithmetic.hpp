#pragma once
#include <boost/multiprecision/cpp_int.hpp>

namespace sentinel::levelstore {
// Fixed storage, checked signed 128-bit range (no allocator). All aggregate
// additions and products throw on overflow; serialization never wraps a sum.
using Wide = boost::multiprecision::number<boost::multiprecision::cpp_int_backend<
    127, 127, boost::multiprecision::signed_magnitude, boost::multiprecision::checked, void>>;
} // namespace sentinel::levelstore
