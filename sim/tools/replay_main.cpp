// This file was generated with the help of Claude Code.
//
// Runs a scenario against an application that never trades and prints the
// full message trace. Useful for checking what a scenario file does.
//
//   build/sim_replay sim/fixtures/basic.scn

#include <cstddef>
#include <exception>
#include <iostream>
#include <span>

#include "sim/messages/format.h"
#include "sim/runtime/simulator.h"

namespace {

struct PassiveApp {
    void OnMarketData(std::span<const std::byte>) {}
    void OnExecutionReport(std::span<const std::byte>) {}
    void OnTimer(sim::Timestamp) {}
};

}  // namespace

int main(int argc, char** argv) {
    if (argc != 2) {
        std::cerr << "usage: " << argv[0] << " <scenario-file>\n";
        return 2;
    }
    try {
        sim::Simulator simulator(sim::LoadScenario(argv[1]));
        simulator.set_trace(&std::cout);
        PassiveApp app;
        simulator.Run(app);

        const auto& book = simulator.exchange().book();
        const auto bid = book.BestPrice(sim::Side::Buy);
        const auto ask = book.BestPrice(sim::Side::Sell);
        std::cout << "\nfinal book: " << book.OrderCount() << " orders, best "
                  << (bid ? sim::FormatPrice(*bid) : "-") << " x "
                  << (ask ? sim::FormatPrice(*ask) : "-") << '\n';
    } catch (const std::exception& e) {
        std::cerr << "error: " << e.what() << '\n';
        return 1;
    }
    return 0;
}
