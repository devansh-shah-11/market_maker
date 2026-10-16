// This file was generated with the help of Claude Code.
//
// A Scenario is a deterministic script of order flow from other market
// participants, plus the simulator settings to run it under.
//
// Scenario file format (one directive per line, '#' starts a comment,
// underscores in numbers are ignored):
//
//   latency market_data <ns>        one-way exchange -> app feed delay
//   latency order_entry <ns>        one-way delay in each direction
//   timer <ns>                      call App::OnTimer every <ns>
//   end <ns>                        stop the simulation at this time
//   drop_market_data <seq>          drop this feed message before the app sees it
//
//   <time_ns> <participant> enter <token> <B|S> <shares> <price> [day|ioc]
//   <time_ns> <participant> replace <token> <new_token> <shares> <price>
//   <time_ns> <participant> cancel <token> [<remaining_shares>]
//
// Participants are numeric ids >= 2 (1 is reserved for the application).
// Prices are decimal dollars, e.g. 100.25.
#pragma once

#include <set>
#include <string>
#include <string_view>
#include <vector>

#include "sim/messages/types.h"
#include "sim/messages/wire.h"

namespace sim {

struct SimConfig {
    Timestamp market_data_latency_ns = 5'000;
    Timestamp order_entry_latency_ns = 10'000;
    Timestamp timer_interval_ns = 0;  // 0 disables the timer
    Timestamp end_time_ns = 0;        // 0 runs until no events remain
    std::set<std::uint64_t> dropped_market_data;
};

// An inbound order entry message that a scripted participant sends directly
// to the exchange at `time` (scripted participants are co-located: no delay).
struct ScheduledMessage {
    Timestamp time;
    ParticipantId participant;
    Packet packet;
};

struct Scenario {
    SimConfig config;
    std::vector<ScheduledMessage> messages;
};

// Throws std::runtime_error with the offending line number on bad input.
Scenario ParseScenario(std::string_view text, std::string_view source_name = "<string>");
Scenario LoadScenario(const std::string& path);

// Parses a decimal price like "100.25" into fixed point. Throws on bad input.
Price ParsePrice(std::string_view text);

}  // namespace sim
