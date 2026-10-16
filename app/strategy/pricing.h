// This file was generated with the help of Claude Code.
//
// Fair value, volatility, and credit: the pricing half of the strategy.
#pragma once

#include <algorithm>
#include <cmath>
#include <cstdint>

#include "app/config.h"
#include "sim/messages/types.h"

namespace app {

// Best bid and ask of the market, excluding our own orders. A size of 0 means
// that side of the book is empty.
struct BookTop {
    sim::Price bid_price = 0;
    sim::Price ask_price = 0;
    std::uint64_t bid_size = 0;
    std::uint64_t ask_size = 0;

    bool Valid() const { return bid_size > 0 && ask_size > 0 && bid_price < ask_price; }
};

// Microprice: each side's price weighted by the *opposite* side's size, so the
// fair value leans toward the side more likely to trade next.
//
// Integer arithmetic only, so the result is exact. The division floors to
// $0.0001. Precondition: top.Valid().
inline sim::Price CalcMicroprice(const BookTop& top) {
    const auto total_size = static_cast<sim::Price>(top.bid_size + top.ask_size);
    return (top.bid_price * static_cast<sim::Price>(top.ask_size) +
            top.ask_price * static_cast<sim::Price>(top.bid_size)) /
           total_size;
}

// EWMA estimate of the volatility of log returns between timer samples.
class VolatilityEstimator {
public:
    void Update(double new_mid) {
        if (last_mid_ > 0.0) {
            const double log_return = std::log(new_mid / last_mid_);
            ema_variance_ = kLambda * ema_variance_ + (1.0 - kLambda) * log_return * log_return;
        }
        last_mid_ = new_mid;
    }
    double Variance() const { return ema_variance_; }

private:
    static constexpr double kLambda = 0.94;
    double ema_variance_ = 0.0;
    double last_mid_ = 0.0;
};

// Half-spread in Price units, rounded up.
inline sim::Price CalcCredit(double variance, sim::Price sampled_mid) {
    const double credit = kCreditK * std::sqrt(variance) * static_cast<double>(sampled_mid);
    return std::max(kMinCredit, static_cast<sim::Price>(std::ceil(credit)));
}

// Rounds a quote price to the tick grid, away from fair value: bids down,
// asks up, so rounding never narrows the spread.
inline sim::Price RoundToTick(sim::Price price, sim::Side side) {
    if (side == sim::Side::Buy) {
        return price / sim::kTickSize * sim::kTickSize;
    }
    return (price + sim::kTickSize - 1) / sim::kTickSize * sim::kTickSize;
}

}  // namespace app
