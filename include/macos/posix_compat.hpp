#ifndef DBMS_MACOS_POSIX_COMPAT_HPP
#define DBMS_MACOS_POSIX_COMPAT_HPP

#if !defined(_WIN32)

#include <ctime>

// logger.hpp использует localtime_s (MSVC/Windows); на macOS/POSIX — localtime_r.
inline void localtime_s(std::tm* tm, const std::time_t* time) {
    localtime_r(time, tm);
}

#endif

#endif
