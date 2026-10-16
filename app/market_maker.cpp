// This file was generated with the help of Claude Code.

#include "app/market_maker.h"

#include "sim/messages/execution_report.h"
#include "sim/messages/market_data.h"
#include "sim/messages/wire.h"

namespace app {

namespace md = sim::md;
namespace oe = sim::oe;

void MarketMaker::OnMarketData(std::span<const std::byte> bytes) {
    const auto header = sim::Decode<md::Header>(bytes.first(sizeof(md::Header)));
    if (header.sequence != next_sequence_) {
        risk_.LatchSequenceGap();
    }
    next_sequence_ = header.sequence + 1;

    switch (header.type) {
        case md::MsgType::AddOrder:
            book_.Apply(sim::Decode<md::AddOrder>(bytes));
            break;
        case md::MsgType::OrderExecuted:
            book_.Apply(sim::Decode<md::OrderExecuted>(bytes));
            break;
        case md::MsgType::OrderCancel:
            book_.Apply(sim::Decode<md::OrderCancel>(bytes));
            break;
        case md::MsgType::OrderDelete:
            book_.Apply(sim::Decode<md::OrderDelete>(bytes));
            break;
        case md::MsgType::OrderReplace:
            book_.Apply(sim::Decode<md::OrderReplace>(bytes));
            break;
    }

    Requote();
    latency_.Record(sim::DeliveryKind::MarketData, header.sequence);
}

void MarketMaker::OnExecutionReport(std::span<const std::byte> bytes) {
    const auto header = sim::Decode<oe::ReportHeader>(bytes.first(sizeof(oe::ReportHeader)));
    switch (header.type) {
        case oe::OutboundType::Accepted: {
            const auto msg = sim::Decode<oe::Accepted>(bytes);
            orders_.OnAccepted(msg);
            book_.MarkOwn(msg.order_ref);
            break;
        }
        case oe::OutboundType::Replaced: {
            const auto msg = sim::Decode<oe::Replaced>(bytes);
            orders_.OnReplaced(msg);
            book_.MarkOwn(msg.order_ref);
            break;
        }
        case oe::OutboundType::Executed:
            orders_.OnExecuted(sim::Decode<oe::Executed>(bytes));
            break;
        case oe::OutboundType::Rejected:
            orders_.OnRejected(sim::Decode<oe::Rejected>(bytes));
            break;
        case oe::OutboundType::Canceled:
            orders_.OnCanceled(sim::Decode<oe::Canceled>(bytes));
            break;
    }

    Requote();
    latency_.Record(sim::DeliveryKind::ExecutionReport, header.sequence);
}

void MarketMaker::Requote() {
    BookTop top;
    try {
        risk_.CheckStanding();
        top = book_.top();
        RiskGate::CheckBook(top);
    } catch (const RiskViolation& e) {
        if (IsStanding(e.error()) && !halted_) {
            Halt(e.what());
        }
        // BookInvalid: skip this tick; resting quotes stay.
        return;
    }

    const sim::Price fair_value = CalcMicroprice(top);
    // Always price off the latest volatility estimate.
    const sim::Price credit = CalcCredit(vol_.Variance(), sampled_mid_);

    QuoteSide(sim::Side::Buy, RoundToTick(fair_value - credit, sim::Side::Buy));
    QuoteSide(sim::Side::Sell, RoundToTick(fair_value + credit, sim::Side::Sell));
}

void MarketMaker::QuoteSide(sim::Side side, sim::Price desired) {
    try {
        RiskGate::CheckPosition(side, orders_.position());
    } catch (const RiskViolation&) {
        // This side is blocked; the other side still quotes.
        return;
    }

    const Quote& q = orders_.quote(side);
    if (q.pending || (q.live && q.price == desired)) {
        return;
    }
    orders_.SendQuote(side, desired, kQuoteShares);
}

void MarketMaker::OnTimer(sim::Timestamp now) {
    const BookTop& top = book_.top();
    if (top.Valid()) {
        sampled_mid_ = (top.bid_price + top.ask_price) / 2;
        vol_.Update(static_cast<double>(sampled_mid_));
    }

    // Loss limit: marked to the sampled mid. Skipped until there is a mark.
    if (sampled_mid_ != 0) {
        const std::int64_t pnl = orders_.cash() + orders_.position() * sampled_mid_;
        if (pnl < -kLossLimit) {
            risk_.LatchLossLimit();
        }
    }
    if (risk_.Halted() && !halted_) {
        Halt("loss limit breached");
    }

    Reconcile(now);
    latency_.Record(sim::DeliveryKind::Timer, now);
}

// Checks every quote sent since the last timer: on the tick grid, standard
// size, positive price.
void MarketMaker::Reconcile(sim::Timestamp now) {
    const auto& history = orders_.history();
    for (; reconciled_ < history.size(); ++reconciled_) {
        const QuoteRecord& r = history[reconciled_];
        if (r.price <= 0 || r.price % sim::kTickSize != 0 || r.shares != kQuoteShares) {
            decisions_.Note(now, "RECONCILE mismatch");
        }
    }
}

void MarketMaker::Halt(const char* reason) {
    halted_ = true;
    decisions_.Note(session_.Now(), reason);
    orders_.CancelAll();
}

}  // namespace app
