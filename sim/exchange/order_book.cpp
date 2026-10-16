// This file was generated with the help of Claude Code.

#include "sim/exchange/order_book.h"

#include <stdexcept>

namespace sim {

void OrderBook::Add(const RestingOrder& order) {
    if (index_.contains(order.ref)) {
        throw std::logic_error("OrderBook::Add: duplicate order ref");
    }
    WithSide(order.side, [&](auto& levels) {
        Queue& queue = levels[order.price];
        auto it = queue.insert(queue.end(), order);
        index_.emplace(order.ref, Locator{order.side, order.price, it});
    });
}

void OrderBook::Remove(OrderRef ref) {
    auto found = index_.find(ref);
    if (found == index_.end()) {
        throw std::logic_error("OrderBook::Remove: unknown order ref");
    }
    const Locator loc = found->second;
    index_.erase(found);
    WithSide(loc.side, [&](auto& levels) {
        auto level = levels.find(loc.price);
        level->second.erase(loc.it);
        if (level->second.empty()) {
            levels.erase(level);
        }
    });
}

void OrderBook::SetShares(OrderRef ref, std::uint32_t shares) {
    auto found = index_.find(ref);
    if (found == index_.end()) {
        throw std::logic_error("OrderBook::SetShares: unknown order ref");
    }
    if (shares == 0) {
        throw std::logic_error("OrderBook::SetShares: use Remove to delete an order");
    }
    found->second.it->shares = shares;
}

const RestingOrder* OrderBook::Find(OrderRef ref) const {
    auto found = index_.find(ref);
    return found == index_.end() ? nullptr : &*found->second.it;
}

const RestingOrder* OrderBook::BestOrder(Side side) const {
    return WithSide(side, [](const auto& levels) -> const RestingOrder* {
        return levels.empty() ? nullptr : &levels.begin()->second.front();
    });
}

std::optional<Price> OrderBook::BestPrice(Side side) const {
    const RestingOrder* best = BestOrder(side);
    return best ? std::optional<Price>(best->price) : std::nullopt;
}

std::uint64_t OrderBook::SharesAt(Side side, Price price) const {
    return WithSide(side, [&](const auto& levels) -> std::uint64_t {
        auto level = levels.find(price);
        if (level == levels.end()) {
            return 0;
        }
        std::uint64_t total = 0;
        for (const RestingOrder& order : level->second) {
            total += order.shares;
        }
        return total;
    });
}

}  // namespace sim
