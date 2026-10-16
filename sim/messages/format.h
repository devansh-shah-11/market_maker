// This file was generated with the help of Claude Code.
//
// Human-readable rendering of wire messages, used for traces and debugging.
// Not intended for use on any latency-sensitive path.
#pragma once

#include <cstddef>
#include <span>
#include <string>

#include "sim/messages/types.h"

namespace sim {

std::string FormatPrice(Price price);  // 1'002'500 -> "100.2500"

std::string FormatMarketData(std::span<const std::byte> bytes);
std::string FormatInbound(std::span<const std::byte> bytes);
std::string FormatExecutionReport(std::span<const std::byte> bytes);

}  // namespace sim
