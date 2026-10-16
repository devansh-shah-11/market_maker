// This file was generated with the help of Claude Code.

#include "sim/exchange/exchange.h"

#include <algorithm>

namespace sim {

void Exchange::Process(ParticipantId from, std::span<const std::byte> message, Timestamp now,
                       Output& out) {
    now_ = now;
    // Malformed messages have no token to reject against, so they are dropped.
    try {
        switch (static_cast<oe::InboundType>(MessageTypeOf(message))) {
            case oe::InboundType::EnterOrder:
                HandleEnter(from, Decode<oe::EnterOrder>(message), out);
                break;
            case oe::InboundType::ReplaceOrder:
                HandleReplace(from, Decode<oe::ReplaceOrder>(message), out);
                break;
            case oe::InboundType::CancelOrder:
                HandleCancel(from, Decode<oe::CancelOrder>(message), out);
                break;
        }
    } catch (const std::invalid_argument&) {
    }
}

void Exchange::HandleEnter(ParticipantId from, const oe::EnterOrder& msg, Output& out) {
    // A reused token is treated as a retransmission and ignored.
    if (!used_tokens_.insert({from, Token{msg.token}}).second) {
        return;
    }
    if (msg.instrument != kInstrument) {
        return Reject(from, msg.token, oe::RejectReason::UnknownInstrument, out);
    }
    if (!IsValidPrice(msg.price)) {
        return Reject(from, msg.token, oe::RejectReason::InvalidPrice, out);
    }
    if (!IsValidShares(msg.shares)) {
        return Reject(from, msg.token, oe::RejectReason::InvalidShares, out);
    }

    const OrderRef ref = next_order_ref_++;
    oe::Accepted accepted{};
    accepted.token = msg.token;
    accepted.side = msg.side;
    accepted.shares = msg.shares;
    accepted.instrument = msg.instrument;
    accepted.price = msg.price;
    accepted.time_in_force = msg.time_in_force;
    accepted.order_ref = ref;
    Report(from, accepted, oe::OutboundType::Accepted, out);

    const std::uint32_t remaining = Match(from, msg.token, msg.side, msg.price, msg.shares, out);
    if (remaining == 0) {
        return;
    }
    if (msg.time_in_force == oe::TimeInForce::ImmediateOrCancel) {
        oe::Canceled canceled{};
        canceled.token = msg.token;
        canceled.decrement_shares = remaining;
        canceled.reason = oe::CancelReason::ImmediateOrCancel;
        return Report(from, canceled, oe::OutboundType::Canceled, out);
    }
    Rest(from, msg.token, ref, msg.side, msg.price, remaining, out);
}

void Exchange::HandleReplace(ParticipantId from, const oe::ReplaceOrder& msg, Output& out) {
    if (!used_tokens_.insert({from, Token{msg.replacement_token}}).second) {
        return;
    }
    auto live = live_orders_.find({from, Token{msg.existing_token}});
    if (live == live_orders_.end()) {
        return Reject(from, msg.replacement_token, oe::RejectReason::UnknownOrder, out);
    }
    if (!IsValidPrice(msg.price)) {
        return Reject(from, msg.replacement_token, oe::RejectReason::InvalidPrice, out);
    }
    if (!IsValidShares(msg.shares)) {
        return Reject(from, msg.replacement_token, oe::RejectReason::InvalidShares, out);
    }

    const OrderRef old_ref = live->second;
    const Side side = book_.Find(old_ref)->side;
    live_orders_.erase(live);
    book_.Remove(old_ref);

    const OrderRef new_ref = next_order_ref_++;
    oe::Replaced replaced{};
    replaced.replacement_token = msg.replacement_token;
    replaced.previous_token = msg.existing_token;
    replaced.side = side;
    replaced.shares = msg.shares;
    replaced.price = msg.price;
    replaced.order_ref = new_ref;
    Report(from, replaced, oe::OutboundType::Replaced, out);

    const std::optional<Price> opposite = book_.BestPrice(Opposite(side));
    const bool crosses = opposite && (side == Side::Buy ? msg.price >= *opposite
                                                        : msg.price <= *opposite);
    if (!crosses) {
        md::OrderReplace update{};
        update.original_order_ref = old_ref;
        update.new_order_ref = new_ref;
        update.shares = msg.shares;
        update.price = msg.price;
        PublishMarketData(update, md::MsgType::OrderReplace, out);
        book_.Add({new_ref, from, msg.replacement_token, side, msg.price, msg.shares});
        live_orders_.emplace(SessionToken{from, Token{msg.replacement_token}}, new_ref);
        return;
    }

    // A crossing replace is published as delete + executions + add.
    md::OrderDelete removed{};
    removed.order_ref = old_ref;
    PublishMarketData(removed, md::MsgType::OrderDelete, out);
    const std::uint32_t remaining =
        Match(from, msg.replacement_token, side, msg.price, msg.shares, out);
    if (remaining > 0) {
        Rest(from, msg.replacement_token, new_ref, side, msg.price, remaining, out);
    }
}

void Exchange::HandleCancel(ParticipantId from, const oe::CancelOrder& msg, Output& out) {
    auto live = live_orders_.find({from, Token{msg.token}});
    if (live == live_orders_.end()) {
        return;
    }
    const OrderRef ref = live->second;
    const std::uint32_t open = book_.Find(ref)->shares;
    if (msg.shares >= open) {
        return;
    }

    const std::uint32_t decrement = open - msg.shares;
    if (msg.shares == 0) {
        live_orders_.erase(live);
        book_.Remove(ref);
        md::OrderDelete removed{};
        removed.order_ref = ref;
        PublishMarketData(removed, md::MsgType::OrderDelete, out);
    } else {
        book_.SetShares(ref, msg.shares);
        md::OrderCancel reduced{};
        reduced.order_ref = ref;
        reduced.canceled_shares = decrement;
        PublishMarketData(reduced, md::MsgType::OrderCancel, out);
    }

    oe::Canceled canceled{};
    canceled.token = msg.token;
    canceled.decrement_shares = decrement;
    canceled.reason = oe::CancelReason::UserRequested;
    Report(from, canceled, oe::OutboundType::Canceled, out);
}

std::uint32_t Exchange::Match(ParticipantId from, Token token, Side side, Price price,
                              std::uint32_t shares, Output& out) {
    const Side resting_side = Opposite(side);
    while (shares > 0) {
        const RestingOrder* best = book_.BestOrder(resting_side);
        if (best == nullptr) {
            break;
        }
        const bool crosses = side == Side::Buy ? price >= best->price : price <= best->price;
        if (!crosses) {
            break;
        }

        const RestingOrder resting = *best;
        const std::uint32_t fill = std::min(shares, resting.shares);
        const std::uint64_t match_number = next_match_number_++;

        md::OrderExecuted print{};
        print.order_ref = resting.ref;
        print.executed_shares = fill;
        print.match_number = match_number;
        PublishMarketData(print, md::MsgType::OrderExecuted, out);

        oe::Executed passive{};
        passive.token = resting.token;
        passive.executed_shares = fill;
        passive.price = resting.price;
        passive.match_number = match_number;
        passive.liquidity = oe::Liquidity::Added;
        Report(resting.owner, passive, oe::OutboundType::Executed, out);

        oe::Executed aggressive = passive;
        aggressive.token = token;
        aggressive.liquidity = oe::Liquidity::Removed;
        Report(from, aggressive, oe::OutboundType::Executed, out);

        if (fill == resting.shares) {
            book_.Remove(resting.ref);
            live_orders_.erase({resting.owner, resting.token});
        } else {
            book_.SetShares(resting.ref, resting.shares - fill);
        }
        shares -= fill;
    }
    return shares;
}

void Exchange::Rest(ParticipantId from, Token token, OrderRef ref, Side side, Price price,
                    std::uint32_t shares, Output& out) {
    book_.Add({ref, from, token, side, price, shares});
    live_orders_.emplace(SessionToken{from, token}, ref);

    md::AddOrder add{};
    add.order_ref = ref;
    add.side = side;
    add.shares = shares;
    add.price = price;
    PublishMarketData(add, md::MsgType::AddOrder, out);
}

bool Exchange::IsValidPrice(Price price) const {
    return price > 0 && price % kTickSize == 0;
}

bool Exchange::IsValidShares(std::uint32_t shares) const {
    return shares > 0 && shares <= kMaxOrderShares;
}

template <typename Msg>
void Exchange::PublishMarketData(Msg msg, md::MsgType type, Output& out) {
    msg.header.type = type;
    msg.header.instrument = kInstrument;
    msg.header.sequence = next_md_sequence_++;
    msg.header.timestamp = now_;
    out.market_data.push_back(Encode(msg));
}

template <typename Msg>
void Exchange::Report(ParticipantId to, Msg msg, oe::OutboundType type, Output& out) {
    auto [seq, inserted] = next_report_sequence_.try_emplace(to, 1);
    msg.header.type = type;
    msg.header.sequence = seq->second++;
    msg.header.timestamp = now_;
    out.reports.emplace_back(to, Encode(msg));
}

void Exchange::Reject(ParticipantId to, Token token, oe::RejectReason reason, Output& out) {
    oe::Rejected rejected{};
    rejected.token = token;
    rejected.reason = reason;
    Report(to, rejected, oe::OutboundType::Rejected, out);
}

}  // namespace sim
