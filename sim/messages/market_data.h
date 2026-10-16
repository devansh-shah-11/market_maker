// This file was generated with the help of Claude Code.
//
// Market data feed (ITCH-like). Exchange -> all participants, one-way.
//
// The feed is order-by-order: each message refers to an individual resting
// order by its exchange-assigned OrderRef. Only AddOrder and OrderReplace
// carry a side and price; Executed/Cancel/Delete carry just the ref, so a
// consumer must track live orders itself to know which price level changed.
//
// Simplifications relative to real ITCH 5.0:
//   * the sequence number lives in each message header rather than in a
//     MoldUDP64 packet header;
//   * fields are little-endian;
//   * all orders are displayed, so there is no separate non-displayed Trade
//     message. OrderExecuted is the trade print.
#pragma once

#include "sim/messages/types.h"

namespace sim::md {

enum class MsgType : char {
    AddOrder = 'A',
    OrderExecuted = 'E',
    OrderCancel = 'X',   // partial cancel: shares reduced, order stays live
    OrderDelete = 'D',   // order removed from the book
    OrderReplace = 'U',  // order replaced by a new one (new ref, loses priority)
};

#pragma pack(push, 1)

struct Header {
    MsgType type;
    InstrumentId instrument;
    std::uint64_t sequence;  // starts at 1, increments by 1 per message
    Timestamp timestamp;     // time the exchange processed the event
};

struct AddOrder {
    Header header;
    OrderRef order_ref;
    Side side;
    std::uint32_t shares;
    Price price;
};

struct OrderExecuted {
    Header header;
    OrderRef order_ref;  // the *resting* order that was hit
    std::uint32_t executed_shares;
    std::uint64_t match_number;
};

struct OrderCancel {
    Header header;
    OrderRef order_ref;
    std::uint32_t canceled_shares;
};

struct OrderDelete {
    Header header;
    OrderRef order_ref;
};

struct OrderReplace {
    Header header;
    OrderRef original_order_ref;
    OrderRef new_order_ref;
    std::uint32_t shares;
    Price price;
};

#pragma pack(pop)

static_assert(sizeof(Header) == 19);
static_assert(sizeof(AddOrder) == 40);
static_assert(sizeof(OrderExecuted) == 39);
static_assert(sizeof(OrderCancel) == 31);
static_assert(sizeof(OrderDelete) == 27);
static_assert(sizeof(OrderReplace) == 47);

}  // namespace sim::md
