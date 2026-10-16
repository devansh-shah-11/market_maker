// This file was generated with the help of Claude Code.

#include "sim/runtime/simulator.h"

#include <cstdio>
#include <thread>
#include <utility>

#include "sim/messages/format.h"
#include "sim/messages/market_data.h"

namespace sim {

Timestamp OrderEntrySession::Now() const { return sim_.now(); }

void OrderEntrySession::SendPacket(const Packet& packet) {
    // Capture tap: order-out time. Taken first, so nothing below counts
    // toward tick-to-trade.
    const WallTime sent = WallClockNow();

    // The simulator isn't thread-safe, and the capture tap attributes each
    // order to the delivery in progress, so only the thread running
    // Simulator::Run may send.
    if (std::this_thread::get_id() != sim_.run_thread_) [[unlikely]] {
        throw std::logic_error(
            "OrderEntrySession::Send called outside the simulator thread; orders may only be "
            "sent from inside an application callback");
    }
    if (sim_.capture_ != nullptr) {
        sim_.capture_->sends.push_back({sim_.current_delivery_, sent, packet});
    }
    if (sim_.trace_ != nullptr) {
        sim_.TraceLine("APP>", FormatInbound(packet.view()));
    }
    sim_.Schedule(sim_.now_ + sim_.config_.order_entry_latency_ns,
                  Simulator::EventKind::ArriveAtExchange, kAppParticipant, packet);
}

Simulator::Simulator(Scenario scenario)
    : config_(std::move(scenario.config)), script_(std::move(scenario.messages)), session_(*this) {
    if (config_.timer_interval_ns > 0 && config_.end_time_ns == 0) {
        throw std::invalid_argument("Simulator: a timer requires a nonzero end_time_ns");
    }
}

void Simulator::Schedule(Timestamp time, EventKind kind, ParticipantId participant,
                         const Packet& packet) {
    queue_.push(Event{time, next_event_order_++, kind, participant, packet});
}

void Simulator::BeginRun() {
    if (started_) {
        throw std::logic_error("Simulator::Run may only be called once");
    }
    started_ = true;
    run_thread_ = std::this_thread::get_id();
    for (const ScheduledMessage& msg : script_) {
        Schedule(msg.time, EventKind::ArriveAtExchange, msg.participant, msg.packet);
    }
    if (config_.timer_interval_ns > 0) {
        Schedule(config_.timer_interval_ns, EventKind::Timer, kAppParticipant, Packet{});
    }
    if (capture_ != nullptr) {
        // Preallocate so that growing the capture rarely happens mid-run. A
        // scripted message can produce several feed messages (e.g. a sweep),
        // hence the headroom; the vectors still grow if this is exceeded.
        const std::size_t timer_ticks =
            config_.timer_interval_ns > 0 ? config_.end_time_ns / config_.timer_interval_ns : 0;
        capture_->deliveries.reserve(capture_->deliveries.size() + 4 * script_.size() +
                                     timer_ticks + 1024);
        capture_->sends.reserve(capture_->sends.size() + script_.size() + 1024);
    }
}

void Simulator::ArriveAtExchange(const Event& event) {
    if (event.participant != kAppParticipant && trace_ != nullptr) {
        TraceLine("EXT>", "p" + std::to_string(event.participant) + ' ' +
                              FormatInbound(event.packet.view()));
    }

    exchange_output_.clear();
    exchange_.Process(event.participant, event.packet.view(), now_, exchange_output_);

    for (const Packet& packet : exchange_output_.market_data) {
        const auto header = Decode<md::Header>(packet.view().first(sizeof(md::Header)));
        if (config_.dropped_market_data.contains(std::uint64_t{header.sequence})) {
            if (trace_ != nullptr) {
                TraceLine("DROP", FormatMarketData(packet.view()));
            }
            continue;
        }
        Schedule(now_ + config_.market_data_latency_ns, EventKind::DeliverMarketData,
                 kAppParticipant, packet);
    }
    // Scripted participants don't consume execution reports.
    for (const auto& [owner, packet] : exchange_output_.reports) {
        if (owner == kAppParticipant) {
            Schedule(now_ + config_.order_entry_latency_ns, EventKind::DeliverExecutionReport,
                     kAppParticipant, packet);
        }
    }
}

void Simulator::TraceDelivery(const Event& event) {
    if (trace_ == nullptr) {
        return;
    }
    if (event.kind == EventKind::DeliverMarketData) {
        TraceLine("MD  ", FormatMarketData(event.packet.view()));
    } else if (event.kind == EventKind::DeliverExecutionReport) {
        TraceLine("APP<", FormatExecutionReport(event.packet.view()));
    } else if (event.kind == EventKind::Timer) {
        TraceLine("TIMR", "");
    }
}

void Simulator::TraceLine(const char* label, const std::string& text) {
    if (trace_ == nullptr) {
        return;
    }
    char stamp[32];
    std::snprintf(stamp, sizeof(stamp), "[%12llu] ", static_cast<unsigned long long>(now_));
    *trace_ << stamp << label << ' ' << text << '\n';
}

}  // namespace sim
