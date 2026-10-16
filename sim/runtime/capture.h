// This file was generated with the help of Claude Code.
//
// The capture tap: a record of everything that crossed the sim/app boundary,
// with wall-clock timestamps. It plays the role of a network capture taken at
// a switch: it sits outside the application and the application can't change
// what it records.
//
// For each delivery (a market data message, an execution report, or a timer
// tick), the tap records the wall time at which the application callback was
// entered. For each order the application sends, it records the wall time of
// the Send call and the delivery that was being handled at the time (orders
// can only be sent from inside a callback, so this attribution is exact).
//
// The tap does not record when callbacks return.
//
// The tap is not passive: its timestamps are taken on the application's
// thread, at a cost of roughly 20 ns each.
#pragma once

#include <cstdint>
#include <ostream>
#include <vector>

#include "sim/messages/types.h"
#include "sim/messages/wire.h"
#include "sim/runtime/clock.h"

namespace sim {

enum class DeliveryKind : std::uint8_t {
    MarketData,
    ExecutionReport,
    Timer,
};

struct DeliveryRecord {
    std::uint64_t index;  // position in Capture::deliveries
    DeliveryKind kind;
    Timestamp sim_time;   // simulated time of the delivery
    WallTime entry;       // wall time just before the callback was invoked
    Packet packet;        // message bytes; empty for timer ticks
};

struct SendRecord {
    std::uint64_t delivery_index;  // the delivery being handled when Send was called
    WallTime time;                 // wall time on entry to Send
    Packet packet;
};

struct Capture {
    std::vector<DeliveryRecord> deliveries;
    std::vector<SendRecord> sends;
};

// Writes the capture as CSV, one row per record, deliveries and sends
// interleaved in the order they happened:
//
//   record,delivery,kind,sim_time_ns,wall_ns,msg_type,seq,bytes
//
//   record       "delivery" or "send"
//   delivery     delivery index (for a send: the delivery it's attributed to)
//   kind         md | report | timer | order
//   sim_time_ns  simulated time
//   wall_ns      callback entry (delivery) or Send entry (send)
//   msg_type     first byte of the message, e.g. A, E, O; empty for timers
//   seq          feed sequence (md) or report sequence (report); empty otherwise
//   bytes        message bytes in hex
void WriteCaptureCsv(const Capture& capture, std::ostream& out);

}  // namespace sim
