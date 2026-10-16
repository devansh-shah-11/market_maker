// This file was generated with the help of Claude Code.
//
// Full-depth, price-time priority limit order book for one instrument.
//
// This is part of the simulated exchange, not the trading system under
// analysis. It is written for clarity, not speed.
#pragma once

#include <cstdint>
#include <functional>
#include <list>
#include <map>
#include <optional>
#include <unordered_map>

#include "sim/messages/types.h"

namespace sim {

struct RestingOrder {
    OrderRef ref;
    ParticipantId owner;
    Token token;
    Side side;
    Price price;
    std::uint32_t shares;
};

class OrderBook {
public:
    void Add(const RestingOrder& order);
    void Remove(OrderRef ref);
    void SetShares(OrderRef ref, std::uint32_t shares);  // shares must be > 0

    const RestingOrder* Find(OrderRef ref) const;

    // Oldest order at the best price on `side`, or nullptr if that side is empty.
    const RestingOrder* BestOrder(Side side) const;
    std::optional<Price> BestPrice(Side side) const;
    std::uint64_t SharesAt(Side side, Price price) const;
    std::size_t OrderCount() const { return index_.size(); }

private:
    using Queue = std::list<RestingOrder>;
    struct Locator {
        Side side;
        Price price;
        Queue::iterator it;
    };

    template <typename Fn>
    decltype(auto) WithSide(Side side, Fn&& fn) {
        return side == Side::Buy ? fn(bids_) : fn(asks_);
    }
    template <typename Fn>
    decltype(auto) WithSide(Side side, Fn&& fn) const {
        return side == Side::Buy ? fn(bids_) : fn(asks_);
    }

    std::map<Price, Queue, std::greater<Price>> bids_;  // best (highest) first
    std::map<Price, Queue, std::less<Price>> asks_;     // best (lowest) first
    std::unordered_map<OrderRef, Locator> index_;
};

}  // namespace sim
