#pragma once

#include <windows.h>

namespace lm {

// Monotonic high-resolution clock.
class Clock {
public:
    // Seconds since an arbitrary fixed point.
    static double Now() noexcept
    {
        LARGE_INTEGER counter;
        ::QueryPerformanceCounter(&counter);
        return static_cast<double>(counter.QuadPart) * SecondsPerTick();
    }

private:
    static double SecondsPerTick() noexcept
    {
        static const double secondsPerTick = [] {
            LARGE_INTEGER frequency;
            ::QueryPerformanceFrequency(&frequency);
            return 1.0 / static_cast<double>(frequency.QuadPart);
        }();
        return secondsPerTick;
    }
};

} // namespace lm
