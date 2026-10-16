// This file was generated with the help of Claude Code.
//
// Order state and order entry: one quote per side, at most one request in
// flight per side.
//
// A side with a request in flight (Enter, Replace, or Cancel awaiting its
// response) sends nothing else until the response arrives. Tokens are
// assigned sequentially from 1; each Enter and Replace takes the next one.
#pragma once

#include <cstdint>
#include <vector>

#include "app/execution/decision_log.h"
#include "sim/messages/execution_report.h"
#include "sim/runtime/simulator.h"

namespace app {

struct Quote {
    bool live = false;           // a resting order exists on the exchange
    sim::Token token = 0;        // token of the resting order
    sim::Price price = 0;
    std::uint32_t open_shares = 0;

    bool pending = false;        // a request is in flight
    sim::Token pending_token = 0;
};

// Every quote we send, kept for the end-of-day reconciliation against the
// exchange's records.
struct QuoteRecord {
    sim::Timestamp time;
    sim::Side side;
    sim::Price price;
    std::uint32_t shares;
    sim::Token token;
};

class OrderManager {
public:
    OrderManager(sim::OrderEntrySession& session, DecisionLog& log)
        : session_(session), log_(log) {}

    Quote& quote(sim::Side side) { return side == sim::Side::Buy ? bid_ : ask_; }

    // Places a new quote, or replaces the resting one, at `price` for `shares`.
    // The side must have no request in flight.
    void SendQuote(sim::Side side, sim::Price price, std::uint32_t shares);

    // Cancels everything, including orders whose Enter or Replace is still in
    // flight, bid first, then ask.
    void CancelAll();

    // Updates order state from an execution report. Fills update the position
    // and cash.
    void OnAccepted(const sim::oe::Accepted& msg);
    void OnReplaced(const sim::oe::Replaced& msg);
    void OnExecuted(const sim::oe::Executed& msg);
    void OnRejected(const sim::oe::Rejected& msg);
    void OnCanceled(const sim::oe::Canceled& msg);

    std::int64_t position() const { return position_; }
    std::int64_t cash() const { return cash_; }
    const std::vector<QuoteRecord>& history() const { return history_; }

private:
    Quote* FindByToken(sim::Token token);
    void CancelToken(sim::Token token);

    sim::OrderEntrySession& session_;
    DecisionLog& log_;
    Quote bid_;
    Quote ask_;
    sim::Token next_token_ = 1;
    std::int64_t position_ = 0;  // shares, positive = long
    std::int64_t cash_ = 0;      // Price units x shares
    std::vector<QuoteRecord> history_;
};

}  // namespace app
