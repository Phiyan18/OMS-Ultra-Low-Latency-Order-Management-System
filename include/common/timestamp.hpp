#pragma once

#include "common/types.hpp"

#if defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#else
#include <time.h>
#endif

#include <chrono>

namespace oms {

class NanoClock {
public:
    static TimestampNs now() noexcept {
#if defined(_WIN32)
        static LARGE_INTEGER freq = [] {
            LARGE_INTEGER f{};
            QueryPerformanceFrequency(&f);
            return f;
        }();
        LARGE_INTEGER counter{};
        QueryPerformanceCounter(&counter);
        return static_cast<TimestampNs>((counter.QuadPart * 1'000'000'000LL) / freq.QuadPart);
#else
        timespec ts{};
        clock_gettime(CLOCK_MONOTONIC, &ts);
        return static_cast<TimestampNs>(ts.tv_sec) * 1'000'000'000LL +
               static_cast<TimestampNs>(ts.tv_nsec);
#endif
    }

    static TimestampNs elapsed_ns(TimestampNs start, TimestampNs end) noexcept {
        return end - start;
    }
};

}  // namespace oms
