#pragma once

#if defined(_MSC_VER)
#include <__msvc_int128.hpp>
#endif

namespace ninfer {

// Exact unsigned 128-bit arithmetic. MSVC has no __int128; its standard library's
// _Unsigned128 provides the same operators and constexpr semantics.
#if defined(_MSC_VER)
using UInt128 = std::_Unsigned128;
#else
using UInt128 = unsigned __int128;
#endif

} // namespace ninfer
