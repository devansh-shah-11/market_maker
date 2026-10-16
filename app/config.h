// This file was generated with the help of Claude Code.
//
// Strategy and risk parameters. These are part of the fixed behavior: changing
// any of them changes the order stream, and `make verify` will fail.
#pragma once

#include <cstdint>

#include "sim/messages/types.h"

namespace app {

// Size of each quote, in shares.
inline constexpr std::uint32_t kQuoteShares = 100;

// Minimum half-spread, in Price units ($0.01).
inline constexpr sim::Price kMinCredit = 100;

// Half-spread multiplier on volatility: credit = kCreditK * vol * mid.
inline constexpr double kCreditK = 2.0;

// Maximum absolute position, in shares.
inline constexpr std::int64_t kPositionLimit = 500;

// Maximum loss before quoting stops, in Price units x shares ($100).
inline constexpr std::int64_t kLossLimit = 1'000'000;

// The price ladder covers prices from $0 up to (not including) this.
inline constexpr sim::Price kMaxPrice = 2'000 * sim::kPriceScale;

// Capacity of the order table, indexed by exchange order ref.
inline constexpr std::uint64_t kMaxOrderRefs = 1 << 20;

}  // namespace app
