// This file was generated with the help of Claude Code.
//
// Tests for the simulated exchange and runtime. No external framework:
// each TEST registers itself and `make test` runs them all.

#include <algorithm>
#include <cstddef>
#include <functional>
#include <iostream>
#include <sstream>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

#include "sim/messages/execution_report.h"
#include "sim/messages/market_data.h"
#include "sim/runtime/simulator.h"

namespace {

struct TestCase {
    const char* name;
    void (*fn)();
};

std::vector<TestCase>& Registry() {
    static std::vector<TestCase> tests;
    return tests;
}

struct Registrar {
    Registrar(const char* name, void (*fn)()) { Registry().push_back({name, fn}); }
};

int g_failures = 0;

#define TEST(name)                                  \
    void name();                                    \
    const Registrar name##_registrar(#name, name);  \
    void name()

#define CHECK(cond)                                                                  \
    do {                                                                             \
        if (!(cond)) {                                                               \
            ++g_failures;                                                            \
            std::cerr << "  " << __FILE__ << ':' << __LINE__ << ": CHECK(" #cond ")\n"; \
        }                                                                            \
    } while (0)

#define CHECK_EQ(a, b)                                                                  \
    do {                                                                                \
        const auto va = (a);                                                            \
        const auto vb = (b);                                                            \
        if (!(va == vb)) {                                                              \
            ++g_failures;                                                               \
            std::cerr << "  " << __FILE__ << ':' << __LINE__ << ": CHECK_EQ(" #a ", " #b \
                      << ") got " << va << " vs " << vb << '\n';                        \
        }                                                                               \
    } while (0)

using namespace sim;

struct Received {
    Timestamp time;
    Packet packet;
    char type() const { return MessageTypeOf(packet.view()); }
    template <typename Msg>
    Msg as() const { return Decode<Msg>(packet.view()); }
};

// Records everything it receives. Tests can hook callbacks to send orders.
struct RecordingApp {
    explicit RecordingApp(OrderEntrySession& s) : session(s) {}

    OrderEntrySession& session;
    std::vector<Received> market_data;
    std::vector<Received> reports;
    std::function<void(Timestamp)> on_timer;
    std::function<void(const Received&)> on_report;

    void OnMarketData(std::span<const std::byte> bytes) {
        market_data.push_back({session.Now(), CopyPacket(bytes)});
    }
    void OnExecutionReport(std::span<const std::byte> bytes) {
        reports.push_back({session.Now(), CopyPacket(bytes)});
        if (on_report) on_report(reports.back());
    }
    void OnTimer(Timestamp now) {
        if (on_timer) on_timer(now);
    }

    static Packet CopyPacket(std::span<const std::byte> bytes) {
        Packet p;
        p.size = static_cast<std::uint8_t>(bytes.size());
        std::copy(bytes.begin(), bytes.end(), p.data.begin());
        return p;
    }
};

oe::EnterOrder MakeEnter(Token token, Side side, std::uint32_t shares, const char* price,
                         oe::TimeInForce tif = oe::TimeInForce::Day) {
    oe::EnterOrder msg{};
    msg.token = token;
    msg.side = side;
    msg.shares = shares;
    msg.instrument = kInstrument;
    msg.price = ParsePrice(price);
    msg.time_in_force = tif;
    return msg;
}

TEST(ParsePriceHandlesDecimals) {
    CHECK_EQ(ParsePrice("100"), 1'000'000);
    CHECK_EQ(ParsePrice("100.25"), 1'002'500);
    CHECK_EQ(ParsePrice("0.0001"), 1);
    bool threw = false;
    try {
        ParsePrice("1.23456");
    } catch (const std::runtime_error&) {
        threw = true;
    }
    CHECK(threw);
}

TEST(ScenarioErrorsReportLineNumbers) {
    std::string message;
    try {
        ParseScenario("latency market_data 10\n0 2 enter 1 Q 100 1.00\n", "test.scn");
    } catch (const std::runtime_error& e) {
        message = e.what();
    }
    CHECK(message.find("test.scn:2:") != std::string::npos);
}

TEST(RestingOrderIsPublishedAfterFeedLatency) {
    Simulator sim(ParseScenario("latency market_data 7000\n"
                                "1000 2 enter 1 B 100 100.00\n"));
    RecordingApp app(sim.session());
    sim.Run(app);

    CHECK_EQ(app.market_data.size(), 1u);
    const auto add = app.market_data[0].as<md::AddOrder>();
    CHECK_EQ(app.market_data[0].time, 8000u);
    CHECK_EQ(add.header.sequence, 1u);
    CHECK_EQ(add.header.timestamp, 1000u);
    CHECK(add.side == Side::Buy);
    CHECK_EQ(add.shares, 100u);
    CHECK_EQ(add.price, ParsePrice("100.00"));
}

TEST(MatchingFollowsPriceTimePriority) {
    Simulator sim(ParseScenario("0 2 enter 1 S 100 100.02\n"
                                "0 3 enter 1 S 100 100.01\n"  // better price
                                "0 4 enter 1 S 100 100.01\n"  // same price, later
                                "10 5 enter 1 B 150 100.02\n"));
    RecordingApp app(sim.session());
    sim.Run(app);

    // 3 adds, then executions against ref 2 (100 shares) and ref 3 (50 shares).
    CHECK_EQ(app.market_data.size(), 5u);
    const auto first = app.market_data[3].as<md::OrderExecuted>();
    const auto second = app.market_data[4].as<md::OrderExecuted>();
    CHECK_EQ(first.order_ref, 2u);
    CHECK_EQ(first.executed_shares, 100u);
    CHECK_EQ(second.order_ref, 3u);
    CHECK_EQ(second.executed_shares, 50u);
    CHECK_EQ(sim.exchange().book().SharesAt(Side::Sell, ParsePrice("100.01")), 50u);
    CHECK_EQ(sim.exchange().book().SharesAt(Side::Sell, ParsePrice("100.02")), 100u);
}

TEST(OrderRefOnlyKnownAfterRoundTrip) {
    Simulator sim(ParseScenario("latency order_entry 10000\n"
                                "timer 1000\n"
                                "end 100000\n"));
    RecordingApp app(sim.session());
    bool sent = false;
    app.on_timer = [&](Timestamp) {
        if (!sent) {
            app.session.Send(MakeEnter(42, Side::Buy, 100, "50.00"));
            sent = true;
        }
    };
    sim.Run(app);

    CHECK_EQ(app.reports.size(), 1u);
    const auto accepted = app.reports[0].as<oe::Accepted>();
    CHECK(app.reports[0].type() == 'A');
    CHECK_EQ(accepted.token, 42u);
    CHECK_EQ(accepted.order_ref, 1u);
    CHECK_EQ(accepted.header.sequence, 1u);
    CHECK_EQ(accepted.header.timestamp, 11000u);  // sent at 1000, one-way 10000
    CHECK_EQ(app.reports[0].time, 21000u);        // full round trip
}

TEST(AppFillsAreReportedWithLiquidityFlag) {
    Simulator sim(ParseScenario("latency order_entry 1000\n"
                                "timer 1000\n"
                                "end 50000\n"
                                "20000 2 enter 1 S 60 10.00\n"));
    RecordingApp app(sim.session());
    app.on_timer = [&](Timestamp now) {
        if (now == 1000) app.session.Send(MakeEnter(7, Side::Buy, 100, "10.00"));
    };
    sim.Run(app);

    CHECK_EQ(app.reports.size(), 2u);
    const auto fill = app.reports[1].as<oe::Executed>();
    CHECK(app.reports[1].type() == 'E');
    CHECK_EQ(fill.token, 7u);
    CHECK_EQ(fill.executed_shares, 60u);
    CHECK_EQ(fill.price, ParsePrice("10.00"));
    CHECK(fill.liquidity == oe::Liquidity::Added);
    CHECK_EQ(sim.exchange().book().SharesAt(Side::Buy, ParsePrice("10.00")), 40u);
}

TEST(IocRemainderIsCanceled) {
    Simulator sim(ParseScenario("latency order_entry 1000\n"
                                "timer 1000\n"
                                "end 50000\n"
                                "0 2 enter 1 S 30 10.00\n"));
    RecordingApp app(sim.session());
    app.on_timer = [&](Timestamp now) {
        if (now == 1000) {
            app.session.Send(
                MakeEnter(1, Side::Buy, 100, "10.00", oe::TimeInForce::ImmediateOrCancel));
        }
    };
    sim.Run(app);

    CHECK_EQ(app.reports.size(), 3u);  // Accepted, Executed, Canceled
    const auto canceled = app.reports[2].as<oe::Canceled>();
    CHECK(app.reports[2].type() == 'C');
    CHECK_EQ(canceled.decrement_shares, 70u);
    CHECK(canceled.reason == oe::CancelReason::ImmediateOrCancel);
    CHECK_EQ(sim.exchange().book().OrderCount(), 0u);
}

TEST(CancelPartialAndFull) {
    Simulator sim(ParseScenario("0 2 enter 1 B 100 10.00\n"
                                "10 2 cancel 1 40\n"
                                "20 2 cancel 1 50\n"  // not a reduction: ignored
                                "30 2 cancel 1\n"
                                "40 2 cancel 1\n"));  // already dead: ignored
    RecordingApp app(sim.session());
    sim.Run(app);

    CHECK_EQ(app.market_data.size(), 3u);
    CHECK(app.market_data[1].type() == 'X');
    CHECK_EQ(app.market_data[1].as<md::OrderCancel>().canceled_shares, 60u);
    CHECK(app.market_data[2].type() == 'D');
    CHECK_EQ(sim.exchange().book().OrderCount(), 0u);
}

TEST(NonCrossingReplacePublishesReplace) {
    Simulator sim(ParseScenario("0 2 enter 1 B 100 10.00\n"
                                "10 2 replace 1 2 80 10.01\n"));
    RecordingApp app(sim.session());
    sim.Run(app);

    CHECK_EQ(app.market_data.size(), 2u);
    const auto replace = app.market_data[1].as<md::OrderReplace>();
    CHECK_EQ(replace.original_order_ref, 1u);
    CHECK_EQ(replace.new_order_ref, 2u);
    CHECK_EQ(replace.shares, 80u);
    CHECK_EQ(replace.price, ParsePrice("10.01"));
}

TEST(CrossingReplaceIsDeleteExecuteAdd) {
    Simulator sim(ParseScenario("0 2 enter 1 S 50 10.02\n"
                                "0 3 enter 1 B 100 10.00\n"
                                "10 3 replace 1 2 100 10.02\n"));
    RecordingApp app(sim.session());
    sim.Run(app);

    CHECK_EQ(app.market_data.size(), 5u);
    CHECK(app.market_data[2].type() == 'D');
    CHECK(app.market_data[3].type() == 'E');
    CHECK(app.market_data[4].type() == 'A');
    CHECK_EQ(app.market_data[4].as<md::AddOrder>().shares, 50u);
}

TEST(DuplicateTokenIsIgnored) {
    Simulator sim(ParseScenario("latency order_entry 1000\n"
                                "timer 1000\n"
                                "end 50000\n"));
    RecordingApp app(sim.session());
    app.on_timer = [&](Timestamp now) {
        if (now <= 2000) app.session.Send(MakeEnter(5, Side::Buy, 100, "10.00"));
    };
    sim.Run(app);

    CHECK_EQ(app.reports.size(), 1u);
    CHECK_EQ(sim.exchange().book().OrderCount(), 1u);
}

TEST(InvalidOrdersAreRejected) {
    Simulator sim(ParseScenario("latency order_entry 1000\n"
                                "timer 1000\n"
                                "end 50000\n"));
    RecordingApp app(sim.session());
    app.on_timer = [&](Timestamp now) {
        if (now != 1000) return;
        app.session.Send(MakeEnter(1, Side::Buy, 100, "10.005"));  // off tick
        app.session.Send(MakeEnter(2, Side::Buy, 0, "10.00"));     // zero shares
        oe::ReplaceOrder replace{};
        replace.existing_token = 99;  // never entered
        replace.replacement_token = 3;
        replace.shares = 10;
        replace.price = ParsePrice("10.00");
        app.session.Send(replace);
    };
    sim.Run(app);

    CHECK_EQ(app.reports.size(), 3u);
    CHECK(app.reports[0].as<oe::Rejected>().reason == oe::RejectReason::InvalidPrice);
    CHECK(app.reports[1].as<oe::Rejected>().reason == oe::RejectReason::InvalidShares);
    CHECK(app.reports[2].as<oe::Rejected>().reason == oe::RejectReason::UnknownOrder);
    CHECK_EQ(app.reports[2].as<oe::Rejected>().token, 3u);
}

TEST(DroppedMarketDataLeavesSequenceGap) {
    Simulator sim(ParseScenario("drop_market_data 2\n"
                                "0 2 enter 1 B 100 10.00\n"
                                "0 2 enter 2 B 100 10.01\n"
                                "0 2 enter 3 B 100 10.02\n"));
    RecordingApp app(sim.session());
    sim.Run(app);

    CHECK_EQ(app.market_data.size(), 2u);
    CHECK_EQ(app.market_data[0].as<md::AddOrder>().header.sequence, 1u);
    CHECK_EQ(app.market_data[1].as<md::AddOrder>().header.sequence, 3u);
    CHECK_EQ(sim.exchange().book().OrderCount(), 3u);  // the exchange still has it
}

TEST(TimerFiresAtIntervalUntilEnd) {
    Simulator sim(ParseScenario("timer 250\nend 1000\n"));
    RecordingApp app(sim.session());
    std::vector<Timestamp> fired;
    app.on_timer = [&](Timestamp now) { fired.push_back(now); };
    sim.Run(app);

    CHECK_EQ(fired.size(), 4u);
    CHECK_EQ(fired.front(), 250u);
    CHECK_EQ(fired.back(), 1000u);
}

TEST(RunsAreDeterministic) {
    const Scenario scenario = LoadScenario("sim/fixtures/basic.scn");
    std::string traces[2];
    for (std::string& trace : traces) {
        std::ostringstream os;
        Simulator sim(scenario);
        sim.set_trace(&os);
        RecordingApp app(sim.session());
        app.on_timer = [&](Timestamp now) {
            app.session.Send(MakeEnter(now, Side::Buy, 10, "100.00"));
        };
        sim.Run(app);
        trace = os.str();
    }
    CHECK(!traces[0].empty());
    CHECK(traces[0] == traces[1]);
}

struct CapturedRun {
    Capture capture;
    std::vector<Received> market_data;  // what the app received
    std::vector<Received> reports;
};

// Runs basic.scn with a capture, sending an order on every timer tick and a
// cancel on every Accepted.
CapturedRun CaptureBasicRun() {
    CapturedRun run;
    Simulator sim(LoadScenario("sim/fixtures/basic.scn"));
    sim.set_capture(&run.capture);
    RecordingApp app(sim.session());
    app.on_timer = [&](Timestamp now) { app.session.Send(MakeEnter(now, Side::Buy, 10, "99.00")); };
    app.on_report = [&](const Received& r) {
        if (r.type() == 'A') {
            oe::CancelOrder cancel{};
            cancel.token = r.as<oe::Accepted>().token;
            app.session.Send(cancel);
        }
    };
    sim.Run(app);
    run.market_data = std::move(app.market_data);
    run.reports = std::move(app.reports);
    return run;
}

TEST(CaptureRecordsEveryDelivery) {
    const CapturedRun run = CaptureBasicRun();
    const Capture& capture = run.capture;

    std::size_t md = 0, reports = 0, timers = 0;
    for (std::size_t i = 0; i < capture.deliveries.size(); ++i) {
        const DeliveryRecord& d = capture.deliveries[i];
        CHECK_EQ(d.index, i);
        if (i > 0) CHECK(d.entry >= capture.deliveries[i - 1].entry);
        if (i > 0) CHECK(d.sim_time >= capture.deliveries[i - 1].sim_time);
        switch (d.kind) {
            case DeliveryKind::MarketData:
                CHECK_EQ(d.sim_time, run.market_data[md].time);
                CHECK(d.packet.view().size() == run.market_data[md].packet.view().size());
                ++md;
                break;
            case DeliveryKind::ExecutionReport:
                CHECK_EQ(d.sim_time, run.reports[reports].time);
                ++reports;
                break;
            case DeliveryKind::Timer:
                CHECK_EQ(d.packet.size, 0u);
                ++timers;
                break;
        }
    }
    CHECK_EQ(md, run.market_data.size());
    CHECK_EQ(reports, run.reports.size());
    CHECK_EQ(timers, 10u);  // timer 1ms, end 10ms
}

TEST(CaptureAttributesSendsToTheirDelivery) {
    const Capture capture = CaptureBasicRun().capture;

    std::size_t enters = 0, cancels = 0;
    for (const SendRecord& s : capture.sends) {
        CHECK(s.delivery_index < capture.deliveries.size());
        const DeliveryRecord& d = capture.deliveries[s.delivery_index];
        CHECK(s.time >= d.entry);
        const char type = MessageTypeOf(s.packet.view());
        if (type == 'O') {
            CHECK(d.kind == DeliveryKind::Timer);
            ++enters;
        } else if (type == 'X') {
            CHECK(d.kind == DeliveryKind::ExecutionReport);
            CHECK_EQ(MessageTypeOf(d.packet.view()), 'A');
            ++cancels;
        }
    }
    CHECK_EQ(enters, 10u);
    CHECK(cancels > 0);
    CHECK_EQ(enters + cancels, capture.sends.size());
}

TEST(CaptureCsvHasOneRowPerRecord) {
    const Capture capture = CaptureBasicRun().capture;
    std::ostringstream os;
    WriteCaptureCsv(capture, os);

    std::istringstream lines(os.str());
    std::string line;
    std::getline(lines, line);
    CHECK_EQ(line, std::string("record,delivery,kind,sim_time_ns,wall_ns,msg_type,seq,bytes"));
    std::size_t rows = 0;
    std::string last_delivery_index;  // column 2 of the most recent delivery row
    while (std::getline(lines, line)) {
        ++rows;
        CHECK_EQ(std::count(line.begin(), line.end(), ','), 7);
        const auto first = line.find(',');
        const std::string index = line.substr(first + 1, line.find(',', first + 1) - first - 1);
        if (line.starts_with("delivery,")) {
            last_delivery_index = index;
        } else {
            // A send row comes right after its delivery (or its delivery's other sends).
            CHECK(line.starts_with("send,"));
            CHECK_EQ(index, last_delivery_index);
        }
    }
    CHECK_EQ(rows, capture.deliveries.size() + capture.sends.size());
}

TEST(CapturesMatchAcrossRunsExceptWallTime) {
    const Capture a = CaptureBasicRun().capture;
    const Capture b = CaptureBasicRun().capture;
    CHECK_EQ(a.deliveries.size(), b.deliveries.size());
    CHECK_EQ(a.sends.size(), b.sends.size());
    for (std::size_t i = 0; i < std::min(a.deliveries.size(), b.deliveries.size()); ++i) {
        CHECK(a.deliveries[i].kind == b.deliveries[i].kind);
        CHECK_EQ(a.deliveries[i].sim_time, b.deliveries[i].sim_time);
        CHECK(a.deliveries[i].packet.view().size() == b.deliveries[i].packet.view().size());
        CHECK(std::equal(a.deliveries[i].packet.view().begin(), a.deliveries[i].packet.view().end(),
                         b.deliveries[i].packet.view().begin()));
    }
    for (std::size_t i = 0; i < std::min(a.sends.size(), b.sends.size()); ++i) {
        CHECK_EQ(a.sends[i].delivery_index, b.sends[i].delivery_index);
    }
}

TEST(SendFromAnotherThreadThrows) {
    Simulator sim(ParseScenario("timer 1000\nend 1000\n"));
    std::ostringstream trace;
    sim.set_trace(&trace);
    RecordingApp app(sim.session());
    bool threw = false;
    app.on_timer = [&](Timestamp) {
        std::thread other([&] {
            try {
                app.session.Send(MakeEnter(1, Side::Buy, 10, "100.00"));
            } catch (const std::logic_error&) {
                threw = true;
            }
        });
        other.join();
    };
    sim.Run(app);

    CHECK(threw);
    CHECK(trace.str().find("APP>") == std::string::npos);
}

}  // namespace

int main() {
    for (const TestCase& test : Registry()) {
        const int before = g_failures;
        test.fn();
        std::cout << (g_failures == before ? "PASS " : "FAIL ") << test.name << '\n';
    }
    std::cout << (g_failures == 0 ? "all tests passed\n" : "some tests FAILED\n");
    return g_failures == 0 ? 0 : 1;
}
