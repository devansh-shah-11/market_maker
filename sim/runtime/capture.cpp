// This file was generated with the help of Claude Code.

#include "sim/runtime/capture.h"

#include <cstddef>
#include <optional>

#include "sim/messages/execution_report.h"
#include "sim/messages/market_data.h"

namespace sim {
namespace {

const char* KindName(DeliveryKind kind) {
    switch (kind) {
        case DeliveryKind::MarketData:
            return "md";
        case DeliveryKind::ExecutionReport:
            return "report";
        case DeliveryKind::Timer:
            return "timer";
    }
    return "?";
}

std::optional<std::uint64_t> SequenceOf(const DeliveryRecord& record) {
    const auto bytes = record.packet.view();
    if (record.kind == DeliveryKind::MarketData && bytes.size() >= sizeof(md::Header)) {
        return std::uint64_t{Decode<md::Header>(bytes.first(sizeof(md::Header))).sequence};
    }
    if (record.kind == DeliveryKind::ExecutionReport && bytes.size() >= sizeof(oe::ReportHeader)) {
        return std::uint64_t{Decode<oe::ReportHeader>(bytes.first(sizeof(oe::ReportHeader))).sequence};
    }
    return std::nullopt;
}

// Writes the msg_type column and its trailing comma.
void WriteMsgType(const Packet& packet, std::ostream& out) {
    if (packet.size > 0) {
        out << MessageTypeOf(packet.view());
    }
    out << ',';
}

void WriteHex(const Packet& packet, std::ostream& out) {
    static constexpr char kHex[] = "0123456789abcdef";
    for (const std::byte b : packet.view()) {
        const auto v = static_cast<unsigned>(b);
        out << kHex[v >> 4] << kHex[v & 0xf];
    }
}

void WriteDelivery(const DeliveryRecord& d, std::ostream& out) {
    out << "delivery," << d.index << ',' << KindName(d.kind) << ',' << d.sim_time << ','
        << d.entry << ',';
    WriteMsgType(d.packet, out);
    if (const auto seq = SequenceOf(d)) {
        out << *seq;
    }
    out << ',';
    WriteHex(d.packet, out);
    out << '\n';
}

void WriteSend(const SendRecord& s, const DeliveryRecord* delivery, std::ostream& out) {
    out << "send," << s.delivery_index << ",order,";
    if (delivery != nullptr) {
        out << delivery->sim_time;
    }
    out << ',' << s.time << ',';
    WriteMsgType(s.packet, out);
    out << ',';
    WriteHex(s.packet, out);
    out << '\n';
}

}  // namespace

void WriteCaptureCsv(const Capture& capture, std::ostream& out) {
    out << "record,delivery,kind,sim_time_ns,wall_ns,msg_type,seq,bytes\n";
    // Sends are appended in order during their delivery's callback, so they
    // are already sorted by delivery index: merge them in after their delivery.
    std::size_t next_send = 0;
    for (const DeliveryRecord& d : capture.deliveries) {
        WriteDelivery(d, out);
        while (next_send < capture.sends.size() &&
               capture.sends[next_send].delivery_index == d.index) {
            WriteSend(capture.sends[next_send++], &d, out);
        }
    }
    // Only possible if a callback threw before its delivery was recorded.
    for (; next_send < capture.sends.size(); ++next_send) {
        WriteSend(capture.sends[next_send], nullptr, out);
    }
}

}  // namespace sim
