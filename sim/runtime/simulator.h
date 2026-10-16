// This file was generated with the help of Claude Code.
//
// Discrete-event simulator connecting one application to the exchange.
//
// Everything runs in-process on a single thread against a simulated clock.
// Each message is placed on a time-ordered event queue and delivered after
// the configured one-way latency:
//
//   scripted participants --(0 ns)--------------------> exchange
//   app --OrderEntrySession::Send--(order_entry)------> exchange
//   exchange --(market_data)--> App::OnMarketData
//   exchange --(order_entry)--> App::OnExecutionReport
//
// Simulated time does not advance while the application is running a
// callback. Events with equal timestamps are delivered in the order they
// were scheduled, so a given scenario always produces the same run.
#pragma once

#include <concepts>
#include <cstddef>
#include <ostream>
#include <queue>
#include <span>
#include <stdexcept>
#include <string>
#include <thread>
#include <tuple>
#include <vector>

#include "sim/exchange/exchange.h"
#include "sim/messages/order_entry.h"
#include "sim/messages/wire.h"
#include "sim/replay/scenario.h"
#include "sim/runtime/capture.h"
#include "sim/runtime/clock.h"

namespace sim {

// What the simulator requires of an application. Callbacks receive the raw
// wire bytes of one message; decoding them is the application's job.
template <typename App>
concept Application = requires(App& app, std::span<const std::byte> bytes, Timestamp now) {
    app.OnMarketData(bytes);
    app.OnExecutionReport(bytes);
    app.OnTimer(now);
};

class Simulator;

// The application's order entry connection to the exchange. Send may only be
// called on the thread running Simulator::Run (i.e. from inside an
// application callback); calling it from any other thread throws
// std::logic_error.
class OrderEntrySession {
public:
    void Send(const oe::EnterOrder& msg) { SendPacket(Encode(msg)); }
    void Send(const oe::ReplaceOrder& msg) { SendPacket(Encode(msg)); }
    void Send(const oe::CancelOrder& msg) { SendPacket(Encode(msg)); }

    // Current simulated time.
    Timestamp Now() const;

private:
    friend class Simulator;
    explicit OrderEntrySession(Simulator& sim) : sim_(sim) {}
    void SendPacket(const Packet& packet);

    Simulator& sim_;
};

class Simulator {
public:
    explicit Simulator(Scenario scenario);
    Simulator(const Simulator&) = delete;
    Simulator& operator=(const Simulator&) = delete;

    // Pass this to the application so it can send orders.
    OrderEntrySession& session() { return session_; }

    // If set, every message is logged to `trace` in a human-readable form.
    void set_trace(std::ostream* trace) { trace_ = trace; }

    // If set, the capture tap records every delivery and send into `capture`
    // (see capture.h). Must be set before Run.
    void set_capture(Capture* capture) { capture_ = capture; }

    // Runs the scenario to completion. May only be called once.
    template <Application App>
    void Run(App& app);

    Timestamp now() const { return now_; }
    const SimConfig& config() const { return config_; }
    const Exchange& exchange() const { return exchange_; }

private:
    friend class OrderEntrySession;

    enum class EventKind : std::uint8_t {
        ArriveAtExchange,
        DeliverMarketData,
        DeliverExecutionReport,
        Timer,
    };

    struct Event {
        Timestamp time;
        std::uint64_t order;  // tie-breaker: scheduling order
        EventKind kind;
        ParticipantId participant;
        Packet packet;
    };

    struct Later {
        bool operator()(const Event& a, const Event& b) const {
            return std::tie(a.time, a.order) > std::tie(b.time, b.order);
        }
    };

    void Schedule(Timestamp time, EventKind kind, ParticipantId participant, const Packet& packet);
    void BeginRun();
    void ArriveAtExchange(const Event& event);
    void TraceDelivery(const Event& event);
    void TraceLine(const char* label, const std::string& text);

    // Invokes an application callback, recording it in the capture if one is
    // set. The entry stamp is the last thing before the callback; the record
    // is written after it returns, so capture bookkeeping stays outside the
    // measured window.
    template <typename Callback>
    void Deliver(DeliveryKind kind, const Event& event, Callback&& callback);

    SimConfig config_;
    std::vector<ScheduledMessage> script_;
    Exchange exchange_;
    Exchange::Output exchange_output_;
    OrderEntrySession session_;
    std::priority_queue<Event, std::vector<Event>, Later> queue_;
    std::uint64_t next_event_order_ = 0;
    Timestamp now_ = 0;
    bool started_ = false;
    std::thread::id run_thread_;  // set by Run; Send is only allowed from this thread
    std::ostream* trace_ = nullptr;
    Capture* capture_ = nullptr;
    std::uint64_t current_delivery_ = 0;  // capture index of the callback in progress
};

template <typename Callback>
void Simulator::Deliver(DeliveryKind kind, const Event& event, Callback&& callback) {
    if (capture_ == nullptr) {
        callback();
        return;
    }
    current_delivery_ = capture_->deliveries.size();
    const WallTime entry = WallClockNow();
    callback();
    capture_->deliveries.push_back({current_delivery_, kind, event.time, entry, event.packet});
}

template <Application App>
void Simulator::Run(App& app) {
    BeginRun();
    while (!queue_.empty()) {
        const Event event = queue_.top();
        if (config_.end_time_ns != 0 && event.time > config_.end_time_ns) {
            break;
        }
        queue_.pop();
        now_ = event.time;
        TraceDelivery(event);

        switch (event.kind) {
            case EventKind::ArriveAtExchange:
                ArriveAtExchange(event);
                break;
            case EventKind::DeliverMarketData:
                Deliver(DeliveryKind::MarketData, event,
                        [&] { app.OnMarketData(event.packet.view()); });
                break;
            case EventKind::DeliverExecutionReport:
                Deliver(DeliveryKind::ExecutionReport, event,
                        [&] { app.OnExecutionReport(event.packet.view()); });
                break;
            case EventKind::Timer: {
                Deliver(DeliveryKind::Timer, event, [&] { app.OnTimer(now_); });
                const Timestamp next = now_ + config_.timer_interval_ns;
                if (next <= config_.end_time_ns) {
                    Schedule(next, EventKind::Timer, kAppParticipant, Packet{});
                }
                break;
            }
        }
    }
}

}  // namespace sim
