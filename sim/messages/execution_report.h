// This file was generated with the help of Claude Code.
//
// Order entry (OUCH-like), outbound: exchange -> the participant that owns
// the order.
//
// Outbound messages are sequenced per participant session: the first report
// a participant receives has sequence 1.
#pragma once

#include "sim/messages/order_entry.h"
#include "sim/messages/types.h"

namespace sim::oe {

enum class OutboundType : char {
    Accepted = 'A',
    Replaced = 'U',
    Executed = 'E',
    Rejected = 'J',
    Canceled = 'C',
};

enum class RejectReason : char {
    UnknownInstrument = 'S',
    InvalidPrice = 'X',   // non-positive or not a multiple of kTickSize
    InvalidShares = 'Z',  // zero or above kMaxOrderShares
    UnknownOrder = 'D',   // replace of a token that is not live
};

enum class CancelReason : char {
    UserRequested = 'U',
    ImmediateOrCancel = 'I',  // unfilled remainder of an IOC order
};

enum class Liquidity : char {
    Added = 'A',    // our order was resting
    Removed = 'R',  // our order was the aggressor
};

#pragma pack(push, 1)

struct ReportHeader {
    OutboundType type;
    std::uint64_t sequence;
    Timestamp timestamp;  // time the exchange processed the event
};

struct Accepted {
    ReportHeader header;
    Token token;
    Side side;
    std::uint32_t shares;
    InstrumentId instrument;
    Price price;
    TimeInForce time_in_force;
    OrderRef order_ref;  // first point at which the participant learns this
};

struct Replaced {
    ReportHeader header;
    Token replacement_token;
    Token previous_token;
    Side side;
    std::uint32_t shares;
    Price price;
    OrderRef order_ref;  // ref of the new order
};

struct Executed {
    ReportHeader header;
    Token token;
    std::uint32_t executed_shares;
    Price price;
    std::uint64_t match_number;
    Liquidity liquidity;
};

struct Rejected {
    ReportHeader header;
    Token token;
    RejectReason reason;
};

struct Canceled {
    ReportHeader header;
    Token token;
    std::uint32_t decrement_shares;
    CancelReason reason;
};

#pragma pack(pop)

static_assert(sizeof(ReportHeader) == 17);
static_assert(sizeof(Accepted) == 49);
static_assert(sizeof(Replaced) == 54);
static_assert(sizeof(Executed) == 46);
static_assert(sizeof(Rejected) == 26);
static_assert(sizeof(Canceled) == 30);

}  // namespace sim::oe
