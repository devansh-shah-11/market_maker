// This file was generated with the help of Claude Code.
//
// The simulated exchange: accepts order entry messages from participants,
// matches them against the book, and produces market data plus execution
// reports.
//
// The exchange has no notion of time or latency; it processes one inbound
// message at a time, stamping outputs with the `now` it is given. Delivery
// delays are the Simulator's job.
#pragma once

#include <cstdint>
#include <map>
#include <set>
#include <span>
#include <utility>
#include <vector>

#include "sim/exchange/order_book.h"
#include "sim/messages/execution_report.h"
#include "sim/messages/market_data.h"
#include "sim/messages/order_entry.h"
#include "sim/messages/wire.h"

namespace sim {

class Exchange {
public:
    struct Output {
        std::vector<Packet> market_data;
        std::vector<std::pair<ParticipantId, Packet>> reports;

        void clear() {
            market_data.clear();
            reports.clear();
        }
    };

    // Processes one inbound order entry message. Appends any resulting market
    // data and execution reports to `out`, in the order they were generated.
    void Process(ParticipantId from, std::span<const std::byte> message, Timestamp now,
                 Output& out);

    const OrderBook& book() const { return book_; }

private:
    using SessionToken = std::pair<ParticipantId, Token>;

    void HandleEnter(ParticipantId from, const oe::EnterOrder& msg, Output& out);
    void HandleReplace(ParticipantId from, const oe::ReplaceOrder& msg, Output& out);
    void HandleCancel(ParticipantId from, const oe::CancelOrder& msg, Output& out);

    // Matches an incoming order against the opposite side of the book.
    // Returns the unfilled quantity.
    std::uint32_t Match(ParticipantId from, Token token, Side side, Price price,
                        std::uint32_t shares, Output& out);
    void Rest(ParticipantId from, Token token, OrderRef ref, Side side, Price price,
              std::uint32_t shares, Output& out);

    bool IsValidPrice(Price price) const;
    bool IsValidShares(std::uint32_t shares) const;

    template <typename Msg>
    void PublishMarketData(Msg msg, md::MsgType type, Output& out);
    template <typename Msg>
    void Report(ParticipantId to, Msg msg, oe::OutboundType type, Output& out);
    void Reject(ParticipantId to, Token token, oe::RejectReason reason, Output& out);

    OrderBook book_;
    Timestamp now_ = 0;
    std::uint64_t next_md_sequence_ = 1;
    std::map<ParticipantId, std::uint64_t> next_report_sequence_;
    OrderRef next_order_ref_ = 1;
    std::uint64_t next_match_number_ = 1;

    std::set<SessionToken> used_tokens_;
    std::map<SessionToken, OrderRef> live_orders_;
};

}  // namespace sim
