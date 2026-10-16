// This file was generated with the help of Claude Code.
//
// Pre-trade risk checks.
//
// Safety first: a quote must never go out if anything is wrong, so every
// check throws on failure. An exception can't be ignored the way a return
// code can, and it unwinds straight out of the quoting code, so no path can
// accidentally carry on and send an order after a failed check. Exceptions
// are zero-cost when not thrown, so the checks cost nothing on the normal
// path.
#pragma once

#include <cstdint>
#include <stdexcept>

#include "app/config.h"
#include "app/strategy/pricing.h"

namespace app {

enum class RiskError : std::uint8_t {
    BookInvalid,        // per tick: a side is empty or the book is crossed
    SequenceGap,        // standing, latched: a feed message was missed
    PositionLimit,      // per order: this side would exceed the position limit
    LossLimitBreached,  // standing, latched: detected on the timer
};

inline bool IsStanding(RiskError error) {
    return error == RiskError::SequenceGap || error == RiskError::LossLimitBreached;
}

class RiskViolation : public std::runtime_error {
public:
    RiskViolation(RiskError error, const char* what) : std::runtime_error(what), error_(error) {}
    RiskError error() const { return error_; }

private:
    RiskError error_;
};

class RiskGate {
public:
    void LatchSequenceGap() { sequence_gap_ = true; }
    void LatchLossLimit() { loss_limit_breached_ = true; }
    bool Halted() const { return sequence_gap_ || loss_limit_breached_; }

    // Standing conditions: once latched, nothing is quoted for the rest of the run.
    void CheckStanding() const {
        if (sequence_gap_) {
            throw RiskViolation(RiskError::SequenceGap, "market data sequence gap");
        }
        if (loss_limit_breached_) {
            throw RiskViolation(RiskError::LossLimitBreached, "loss limit breached");
        }
    }

    // Also guards the division in CalcMicroprice.
    static void CheckBook(const BookTop& top) {
        if (!top.Valid()) {
            throw RiskViolation(RiskError::BookInvalid, "book side empty or crossed");
        }
    }

    // A new or replacement quote of kQuoteShares on `side` must not be able to
    // take the position past the limit. With one order per side, the order
    // being replaced is superseded, so it isn't counted.
    static void CheckPosition(sim::Side side, std::int64_t position) {
        const bool breach = side == sim::Side::Buy ? position + kQuoteShares > kPositionLimit
                                                   : position - kQuoteShares < -kPositionLimit;
        if (breach) {
            throw RiskViolation(RiskError::PositionLimit, "position limit");
        }
    }

private:
    bool sequence_gap_ = false;
    bool loss_limit_breached_ = false;
};

}  // namespace app
