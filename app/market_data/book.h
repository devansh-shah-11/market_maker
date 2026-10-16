// This file was generated with the help of Claude Code.
//
// The application's view of the order book, rebuilt from the market data feed.
//
// The feed is order-by-order, and Executed/Cancel/Delete carry only an order
// ref, so every live order is kept in a table. Exchange order refs are
// assigned in increasing order, so the table is a flat array indexed by ref
// rather than a hash map: a lookup is a single indexed load.
//
// Aggregate size per price level is kept in a price ladder (one slot per
// tick) for each side, and the best bid and ask are cached so that reading
// the top of book never searches.
//
// Our own orders appear in the feed unmarked. They are tracked but excluded
// from the ladders and the top of book, so our quotes never move our own
// fair value. An order counts as ours once MarkOwn has been called with its
// ref (on Accepted/Replaced), or as soon as the feed shows it replacing one
// of our orders. A new order whose Add arrives before its Accepted counts as
// market liquidity until the Accepted arrives.
#pragma once

#include <cstdint>
#include <vector>

#include "app/strategy/pricing.h"
#include "sim/messages/market_data.h"

namespace app {

class Book {
public:
    Book();

    void Apply(const sim::md::AddOrder& msg);
    void Apply(const sim::md::OrderExecuted& msg);
    void Apply(const sim::md::OrderCancel& msg);
    void Apply(const sim::md::OrderDelete& msg);
    void Apply(const sim::md::OrderReplace& msg);

    void MarkOwn(sim::OrderRef ref);

    const BookTop& top() const { return top_; }

private:
    struct Order {
        sim::Price price = 0;
        std::uint32_t shares = 0;
        sim::Side side = sim::Side::Buy;
        bool live = false;
        bool own = false;
    };

    struct Ladder {
        std::vector<std::uint64_t> shares_at_tick;  // indexed by price / kTickSize
        std::uint64_t order_count = 0;              // live market (non-own) orders
    };

    Order& Lookup(sim::OrderRef ref);
    void Reduce(sim::OrderRef ref, std::uint32_t shares);
    void AddLiquidity(const Order& order);
    void RemoveLiquidity(const Order& order, std::uint32_t shares, bool order_gone);
    void RecomputeBest(sim::Side side);

    std::vector<Order> orders_;
    Ladder bids_;
    Ladder asks_;
    BookTop top_;
};

}  // namespace app
