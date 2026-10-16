// This file was generated with the help of Claude Code.
//
// The one wall clock shared by the simulator's capture tap and the
// application's own instrumentation. Both must use it so that timestamps
// taken on either side of the sim/app boundary are directly comparable.
#pragma once

#include <chrono>
#include <cstdint>

namespace sim {

// Nanoseconds on a monotonic clock with an arbitrary epoch. Only differences
// between two WallTimes are meaningful.
using WallTime = std::int64_t;

inline WallTime WallClockNow() {
    return std::chrono::duration_cast<std::chrono::nanoseconds>(
               std::chrono::steady_clock::now().time_since_epoch())
        .count();
}

}  // namespace sim
