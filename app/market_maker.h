// This file was generated with the help of Claude Code.
//
// The market maker: decodes the feed, prices, checks risk, and quotes.
//
// Hot path: OnMarketData and OnExecutionReport. After every message, both
// sides are re-evaluated against the current fair value and requoted if the
// desired price differs from the resting one.
//
// Cold path: OnTimer samples the mid, updates volatility, checks the loss
// limit, and reconciles the quote history.
#pragma once

#include <cstddef>
#include <cstdint>
#include <span>

#include "app/evaluation/latency_log.h"
#include "app/execution/decision_log.h"
#include "app/execution/orders.h"
#include "app/market_data/book.h"
#include "app/risk/risk.h"
#include "app/strategy/pricing.h"
#include "sim/runtime/simulator.h"

namespace app {

class MarketMaker {
public:
    MarketMaker(sim::OrderEntrySession& session, DecisionLog& decisions, LatencyLog& latency)
        : session_(session), decisions_(decisions), latency_(latency), orders_(session, decisions) {}

    void OnMarketData(std::span<const std::byte> bytes);
    void OnExecutionReport(std::span<const std::byte> bytes);
    void OnTimer(sim::Timestamp now);

    const OrderManager& orders() const { return orders_; }

private:
    void Requote();
    void QuoteSide(sim::Side side, sim::Price desired);
    void Reconcile(sim::Timestamp now);

    // Rare: cancels everything and stops quoting for the rest of the run.
    // Kept out of line so it doesn't bloat the quoting code.
    [[gnu::noinline]] void Halt(const char* reason);

    sim::OrderEntrySession& session_;
    DecisionLog& decisions_;
    LatencyLog& latency_;
    OrderManager orders_;
    Book book_;
    RiskGate risk_;
    VolatilityEstimator vol_;
    sim::Price sampled_mid_ = 0;  // mid at the last timer with a valid book; 0 = none yet
    std::uint64_t next_sequence_ = 1;
    bool halted_ = false;
    std::size_t reconciled_ = 0;  // quote history entries already reconciled
};

}  // namespace app
