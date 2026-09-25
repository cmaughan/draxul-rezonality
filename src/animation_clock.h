#pragma once

#include <algorithm>

namespace rezonality
{

struct AnimationClock
{
    double elapsed_seconds = 0.0;
    double last_seconds = -1.0;
    bool paused = false;

    void set_paused(bool value)
    {
        if (paused == value)
            return;
        paused = value;
        // Rendering can stop entirely while paused. The next frame must
        // establish a new anchor rather than account for the idle interval.
        last_seconds = -1.0;
    }

    double advance(double monotonic_seconds)
    {
        if (last_seconds >= 0.0 && !paused)
            elapsed_seconds += std::max(0.0,
                monotonic_seconds - last_seconds);
        last_seconds = monotonic_seconds;
        return elapsed_seconds;
    }
};

} // namespace rezonality
