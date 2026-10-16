// This file was generated with the help of Claude Code.
//
// Primitive types shared by every protocol in the simulator.
#pragma once

#include <cstdint>

namespace sim {

// Fixed-point price with 4 implied decimal places (ITCH-style).
// $100.25 is represented as 1'002'500.
using Price = std::int64_t;
inline constexpr Price kPriceScale = 10'000;

// Nanoseconds since the start of the simulation.
using Timestamp = std::uint64_t;

// Exchange-assigned identifier for a resting order. Only known to the
// submitting firm once the exchange's Accepted/Replaced message arrives.
using OrderRef = std::uint64_t;

// Client-assigned order identifier (OUCH "order token"). Must be unique per
// participant for the lifetime of the session.
using Token = std::uint64_t;

using InstrumentId = std::uint16_t;
using ParticipantId = std::uint16_t;

enum class Side : char {
    Buy = 'B',
    Sell = 'S',
};

inline constexpr Side Opposite(Side side) {
    return side == Side::Buy ? Side::Sell : Side::Buy;
}

// The simulator trades a single instrument.
inline constexpr InstrumentId kInstrument = 1;

// Minimum price increment: $0.01.
inline constexpr Price kTickSize = 100;

// Largest order the exchange accepts.
inline constexpr std::uint32_t kMaxOrderShares = 1'000'000;

// The application under test always connects as this participant. Scripted
// participants in replay scenarios use other ids.
inline constexpr ParticipantId kAppParticipant = 1;

}  // namespace sim
