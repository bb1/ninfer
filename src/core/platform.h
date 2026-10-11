#pragma once

#include <cstdint>

#if defined(_WIN32)
#include <process.h>
#else
#include <unistd.h>
#endif

namespace ninfer {

[[nodiscard]] inline std::uint32_t process_id() noexcept {
#if defined(_WIN32)
    return static_cast<std::uint32_t>(::_getpid());
#else
    return static_cast<std::uint32_t>(::getpid());
#endif
}

} // namespace ninfer
