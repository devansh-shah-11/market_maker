// This file was generated with the help of Claude Code.
//
// In-process latency log: each callback records when it finished, into a
// preallocated array (no allocation, no I/O), keyed so the record can be
// joined with the capture tap's delivery records afterwards.
//
// Join keys: market data -> feed sequence number, execution report -> report
// sequence number, timer -> simulated time.
#pragma once

#include <cstdint>
#include <ostream>
#include <vector>

#include "sim/runtime/capture.h"
#include "sim/runtime/clock.h"

namespace app {

struct LatencyRecord {
    sim::DeliveryKind kind;
    std::uint64_t key;
    sim::WallTime completed;
};

class LatencyLog {
public:
    explicit LatencyLog(std::size_t capacity) { records_.reserve(capacity); }

    void Record(sim::DeliveryKind kind, std::uint64_t key) {
        const sim::WallTime now = sim::WallClockNow();
        if (records_.size() < records_.capacity()) {
            records_.push_back({kind, key, now});
        } else {
            ++dropped_;
        }
    }

    const std::vector<LatencyRecord>& records() const { return records_; }
    std::uint64_t dropped() const { return dropped_; }

    // CSV: kind,key,completed_ns (kind is md, report or timer).
    void WriteCsv(std::ostream& out) const {
        out << "kind,key,completed_ns\n";
        for (const LatencyRecord& r : records_) {
            const char* kind = r.kind == sim::DeliveryKind::MarketData        ? "md"
                               : r.kind == sim::DeliveryKind::ExecutionReport ? "report"
                                                                              : "timer";
            out << kind << ',' << r.key << ',' << r.completed << '\n';
        }
    }

private:
    std::vector<LatencyRecord> records_;
    std::uint64_t dropped_ = 0;
};

}  // namespace app
