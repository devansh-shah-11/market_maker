// This file was generated with the help of Claude Code.
//
// Latency report.
//
// Measures end-to-end latency the way the exchange sees it: from the moment
// a message reaches us (the capture tap's timestamp, taken outside the
// application) to the moment we finished handling it (our own latency log).
// Every message counts, so the number reflects the system's real workload,
// and the average summarizes it in one comparable figure.
#pragma once

#include <cstdint>
#include <map>
#include <ostream>
#include <utility>

#include "app/evaluation/latency_log.h"
#include "sim/messages/execution_report.h"
#include "sim/messages/market_data.h"
#include "sim/runtime/capture.h"

namespace app {

inline void PrintLatencyReport(const sim::Capture& capture, const LatencyLog& log,
                               std::ostream& out) {
    std::map<std::pair<sim::DeliveryKind, std::uint64_t>, sim::WallTime> completed;
    for (const LatencyRecord& r : log.records()) {
        completed[{r.kind, r.key}] = r.completed;
    }

    double total_ns = 0;
    std::uint64_t count = 0;
    for (const sim::DeliveryRecord& d : capture.deliveries) {
        std::uint64_t key = d.sim_time;
        const auto bytes = d.packet.view();
        if (d.kind == sim::DeliveryKind::MarketData) {
            key = sim::Decode<sim::md::Header>(bytes.first(sizeof(sim::md::Header))).sequence;
        } else if (d.kind == sim::DeliveryKind::ExecutionReport) {
            key = sim::Decode<sim::oe::ReportHeader>(bytes.first(sizeof(sim::oe::ReportHeader)))
                      .sequence;
        }
        const auto it = completed.find({d.kind, key});
        if (it != completed.end()) {
            total_ns += static_cast<double>(it->second - d.entry);
            ++count;
        }
    }

    out << "messages: " << count << '\n';
    out << "average latency: " << (count > 0 ? total_ns / static_cast<double>(count) : 0.0)
        << " ns\n";
}

}  // namespace app
