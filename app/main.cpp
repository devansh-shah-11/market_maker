// This file was generated with the help of Claude Code.
//
// Runs the market maker on a scenario with the capture tap enabled.
//
//   build/market_maker <scenario.scn> [--out <dir>] [--trace]
//
// Writes into <dir> (default: build/out/<scenario name>):
//   capture.csv       the capture tap (see sim/runtime/capture.h)
//   latency_log.csv   the app's per-callback completion times
//   orders.txt        every order sent: simulated time + message (used by make verify)
//   decisions.log     the app's decision log
//   trace.txt         full human-readable message trace (only with --trace;
//                     slows the run down, so don't measure with it)
// and prints the latency report.

#include <exception>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>

#include "app/evaluation/latency_log.h"
#include "app/evaluation/analysis.h"
#include "app/execution/decision_log.h"
#include "app/market_maker.h"
#include "sim/messages/format.h"
#include "sim/runtime/capture.h"
#include "sim/runtime/simulator.h"

namespace {

void WriteOrders(const sim::Capture& capture, std::ostream& out) {
    for (const sim::SendRecord& s : capture.sends) {
        out << capture.deliveries[s.delivery_index].sim_time << ' '
            << sim::FormatInbound(s.packet.view()) << '\n';
    }
}

}  // namespace

int main(int argc, char** argv) {
    std::filesystem::path scenario_path;
    std::filesystem::path out_dir;
    bool trace = false;
    for (int i = 1; i < argc; ++i) {
        const std::string arg = argv[i];
        if (arg == "--out" && i + 1 < argc) {
            out_dir = argv[++i];
        } else if (arg == "--trace") {
            trace = true;
        } else if (scenario_path.empty() && !arg.starts_with("--")) {
            scenario_path = arg;
        } else {
            scenario_path.clear();
            break;
        }
    }
    if (scenario_path.empty()) {
        std::cerr << "usage: " << argv[0] << " <scenario.scn> [--out <dir>] [--trace]\n";
        return 2;
    }
    try {
        if (out_dir.empty()) {
            out_dir = std::filesystem::path("build/out") / scenario_path.stem();
        }
        std::filesystem::create_directories(out_dir);

        sim::Scenario scenario = sim::LoadScenario(scenario_path.string());
        const std::size_t capacity = 4 * scenario.messages.size() + 1024 +
                                     (scenario.config.timer_interval_ns > 0
                                          ? scenario.config.end_time_ns /
                                                scenario.config.timer_interval_ns
                                          : 0);

        sim::Simulator simulator(std::move(scenario));
        sim::Capture capture;
        simulator.set_capture(&capture);
        std::ofstream trace_file;
        if (trace) {
            trace_file.open(out_dir / "trace.txt");
            simulator.set_trace(&trace_file);
        }

        app::LatencyLog latency(capacity);
        {
            app::DecisionLog decisions((out_dir / "decisions.log").string());
            app::MarketMaker market_maker(simulator.session(), decisions, latency);
            simulator.Run(market_maker);
        }

        std::ofstream capture_csv(out_dir / "capture.csv");
        sim::WriteCaptureCsv(capture, capture_csv);
        std::ofstream latency_csv(out_dir / "latency_log.csv");
        latency.WriteCsv(latency_csv);
        std::ofstream orders(out_dir / "orders.txt");
        WriteOrders(capture, orders);

        app::PrintLatencyReport(capture, latency, std::cout);
        if (latency.dropped() > 0) {
            std::cerr << "warning: latency log full, " << latency.dropped()
                      << " records dropped\n";
        }
    } catch (const std::exception& e) {
        std::cerr << "error: " << e.what() << '\n';
        return 1;
    }
    return 0;
}
