// This file was generated with the help of Claude Code.
//
// Order entry (OUCH-like), inbound: participant -> exchange.
//
// Inbound messages are unsequenced. Each order is identified by a
// client-chosen Token, which makes them safe to retransmit: the exchange
// silently ignores an EnterOrder whose token has already been used.
//
// Note that none of these messages carry the exchange's OrderRef. The
// participant refers to its orders by token; the OrderRef is assigned by the
// exchange and reported back in Accepted/Replaced (see execution_report.h).
#pragma once

#include "sim/messages/types.h"

namespace sim::oe {

enum class InboundType : char {
    EnterOrder = 'O',
    ReplaceOrder = 'U',
    CancelOrder = 'X',
};

enum class TimeInForce : char {
    Day = 'D',                // rest on the book until filled or canceled
    ImmediateOrCancel = 'I',  // match what is possible, cancel the remainder
};

#pragma pack(push, 1)

struct EnterOrder {
    InboundType type = InboundType::EnterOrder;
    Token token;
    Side side;
    std::uint32_t shares;
    InstrumentId instrument;
    Price price;
    TimeInForce time_in_force;
};

// Replaces a live order with a new one at a new price and/or size. The
// replacement gets a new token and a new OrderRef and always loses time
// priority. `shares` is the new open quantity.
struct ReplaceOrder {
    InboundType type = InboundType::ReplaceOrder;
    Token existing_token;
    Token replacement_token;
    std::uint32_t shares;
    Price price;
};

// Reduces a live order to `shares` open quantity. 0 cancels it entirely.
// Requests that would not reduce the order, or that name an unknown or
// already-dead token, are ignored.
struct CancelOrder {
    InboundType type = InboundType::CancelOrder;
    Token token;
    std::uint32_t shares;
};

#pragma pack(pop)

static_assert(sizeof(EnterOrder) == 25);
static_assert(sizeof(ReplaceOrder) == 29);
static_assert(sizeof(CancelOrder) == 13);

}  // namespace sim::oe
