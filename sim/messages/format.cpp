// This file was generated with the help of Claude Code.

#include "sim/messages/format.h"

#include <cstdio>
#include <sstream>

#include "sim/messages/execution_report.h"
#include "sim/messages/market_data.h"
#include "sim/messages/order_entry.h"
#include "sim/messages/wire.h"

namespace sim {

namespace {

const char* SideName(Side side) { return side == Side::Buy ? "BUY" : "SELL"; }

const char* TifName(oe::TimeInForce tif) {
    return tif == oe::TimeInForce::Day ? "DAY" : "IOC";
}

std::string Unknown(std::span<const std::byte> bytes) {
    std::ostringstream os;
    os << "UNKNOWN type='" << MessageTypeOf(bytes) << "' size=" << bytes.size();
    return os.str();
}

}  // namespace

std::string FormatPrice(Price price) {
    char buf[32];
    const char* sign = price < 0 ? "-" : "";
    const Price abs = price < 0 ? -price : price;
    std::snprintf(buf, sizeof(buf), "%s%lld.%04lld", sign,
                  static_cast<long long>(abs / kPriceScale),
                  static_cast<long long>(abs % kPriceScale));
    return buf;
}

std::string FormatMarketData(std::span<const std::byte> bytes) {
    std::ostringstream os;
    try {
        switch (static_cast<md::MsgType>(MessageTypeOf(bytes))) {
            case md::MsgType::AddOrder: {
                const auto m = Decode<md::AddOrder>(bytes);
                os << "seq=" << m.header.sequence << " ADD ref=" << m.order_ref << ' '
                   << SideName(m.side) << ' ' << m.shares << " @ " << FormatPrice(m.price);
                break;
            }
            case md::MsgType::OrderExecuted: {
                const auto m = Decode<md::OrderExecuted>(bytes);
                os << "seq=" << m.header.sequence << " EXECUTED ref=" << m.order_ref << ' '
                   << m.executed_shares << " match=" << m.match_number;
                break;
            }
            case md::MsgType::OrderCancel: {
                const auto m = Decode<md::OrderCancel>(bytes);
                os << "seq=" << m.header.sequence << " CANCEL ref=" << m.order_ref << " -"
                   << m.canceled_shares;
                break;
            }
            case md::MsgType::OrderDelete: {
                const auto m = Decode<md::OrderDelete>(bytes);
                os << "seq=" << m.header.sequence << " DELETE ref=" << m.order_ref;
                break;
            }
            case md::MsgType::OrderReplace: {
                const auto m = Decode<md::OrderReplace>(bytes);
                os << "seq=" << m.header.sequence << " REPLACE ref=" << m.original_order_ref
                   << " -> ref=" << m.new_order_ref << ' ' << m.shares << " @ "
                   << FormatPrice(m.price);
                break;
            }
            default:
                return Unknown(bytes);
        }
    } catch (const std::invalid_argument&) {
        return Unknown(bytes);
    }
    return os.str();
}

std::string FormatInbound(std::span<const std::byte> bytes) {
    std::ostringstream os;
    try {
        switch (static_cast<oe::InboundType>(MessageTypeOf(bytes))) {
            case oe::InboundType::EnterOrder: {
                const auto m = Decode<oe::EnterOrder>(bytes);
                os << "ENTER token=" << m.token << ' ' << SideName(m.side) << ' ' << m.shares
                   << " @ " << FormatPrice(m.price) << ' ' << TifName(m.time_in_force);
                break;
            }
            case oe::InboundType::ReplaceOrder: {
                const auto m = Decode<oe::ReplaceOrder>(bytes);
                os << "REPLACE token=" << m.existing_token << " -> token=" << m.replacement_token
                   << ' ' << m.shares << " @ " << FormatPrice(m.price);
                break;
            }
            case oe::InboundType::CancelOrder: {
                const auto m = Decode<oe::CancelOrder>(bytes);
                os << "CANCEL token=" << m.token << " to " << m.shares;
                break;
            }
            default:
                return Unknown(bytes);
        }
    } catch (const std::invalid_argument&) {
        return Unknown(bytes);
    }
    return os.str();
}

std::string FormatExecutionReport(std::span<const std::byte> bytes) {
    std::ostringstream os;
    try {
        switch (static_cast<oe::OutboundType>(MessageTypeOf(bytes))) {
            case oe::OutboundType::Accepted: {
                const auto m = Decode<oe::Accepted>(bytes);
                os << "seq=" << m.header.sequence << " ACCEPTED token=" << m.token
                   << " ref=" << m.order_ref << ' ' << SideName(m.side) << ' ' << m.shares
                   << " @ " << FormatPrice(m.price);
                break;
            }
            case oe::OutboundType::Replaced: {
                const auto m = Decode<oe::Replaced>(bytes);
                os << "seq=" << m.header.sequence << " REPLACED token=" << m.previous_token
                   << " -> token=" << m.replacement_token << " ref=" << m.order_ref << ' '
                   << m.shares << " @ " << FormatPrice(m.price);
                break;
            }
            case oe::OutboundType::Executed: {
                const auto m = Decode<oe::Executed>(bytes);
                os << "seq=" << m.header.sequence << " EXECUTED token=" << m.token << ' '
                   << m.executed_shares << " @ " << FormatPrice(m.price)
                   << " match=" << m.match_number
                   << (m.liquidity == oe::Liquidity::Added ? " ADDED" : " REMOVED");
                break;
            }
            case oe::OutboundType::Rejected: {
                const auto m = Decode<oe::Rejected>(bytes);
                os << "seq=" << m.header.sequence << " REJECTED token=" << m.token
                   << " reason=" << static_cast<char>(m.reason);
                break;
            }
            case oe::OutboundType::Canceled: {
                const auto m = Decode<oe::Canceled>(bytes);
                os << "seq=" << m.header.sequence << " CANCELED token=" << m.token << " -"
                   << m.decrement_shares << " reason=" << static_cast<char>(m.reason);
                break;
            }
            default:
                return Unknown(bytes);
        }
    } catch (const std::invalid_argument&) {
        return Unknown(bytes);
    }
    return os.str();
}

}  // namespace sim
