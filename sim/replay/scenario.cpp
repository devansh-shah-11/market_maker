// This file was generated with the help of Claude Code.

#include "sim/replay/scenario.h"

#include <algorithm>
#include <charconv>
#include <fstream>
#include <sstream>
#include <stdexcept>

#include "sim/messages/order_entry.h"

namespace sim {

namespace {

std::vector<std::string> SplitWords(std::string_view line) {
    std::vector<std::string> words;
    std::istringstream is{std::string(line)};
    std::string word;
    while (is >> word) {
        words.push_back(word);
    }
    return words;
}

std::uint64_t ParseUnsigned(std::string_view text) {
    std::string digits;
    for (char c : text) {
        if (c != '_') {
            digits.push_back(c);
        }
    }
    std::uint64_t value = 0;
    const auto [end, ec] = std::from_chars(digits.data(), digits.data() + digits.size(), value);
    if (ec != std::errc() || end != digits.data() + digits.size() || digits.empty()) {
        throw std::runtime_error("expected a non-negative integer, got '" + std::string(text) +
                                 "'");
    }
    return value;
}

std::uint32_t ParseShares(std::string_view text) {
    const std::uint64_t value = ParseUnsigned(text);
    if (value > UINT32_MAX) {
        throw std::runtime_error("share count too large: '" + std::string(text) + "'");
    }
    return static_cast<std::uint32_t>(value);
}

Side ParseSide(std::string_view text) {
    if (text == "B") return Side::Buy;
    if (text == "S") return Side::Sell;
    throw std::runtime_error("expected side B or S, got '" + std::string(text) + "'");
}

void ExpectArgs(const std::vector<std::string>& words, std::size_t min, std::size_t max) {
    if (words.size() < min || words.size() > max) {
        throw std::runtime_error("wrong number of arguments");
    }
}

void ParseConfigDirective(const std::vector<std::string>& w, SimConfig& config) {
    const std::string& name = w[0];
    if (name == "latency") {
        ExpectArgs(w, 3, 3);
        if (w[1] == "market_data") {
            config.market_data_latency_ns = ParseUnsigned(w[2]);
        } else if (w[1] == "order_entry") {
            config.order_entry_latency_ns = ParseUnsigned(w[2]);
        } else {
            throw std::runtime_error("unknown latency kind '" + w[1] + "'");
        }
    } else if (name == "timer") {
        ExpectArgs(w, 2, 2);
        config.timer_interval_ns = ParseUnsigned(w[1]);
    } else if (name == "end") {
        ExpectArgs(w, 2, 2);
        config.end_time_ns = ParseUnsigned(w[1]);
    } else if (name == "drop_market_data") {
        ExpectArgs(w, 2, 2);
        config.dropped_market_data.insert(ParseUnsigned(w[1]));
    } else {
        throw std::runtime_error("unknown directive '" + name + "'");
    }
}

ScheduledMessage ParseOrderLine(const std::vector<std::string>& w) {
    ExpectArgs(w, 4, 8);
    ScheduledMessage scheduled{};
    scheduled.time = ParseUnsigned(w[0]);
    const std::uint64_t participant = ParseUnsigned(w[1]);
    if (participant == kAppParticipant || participant > UINT16_MAX) {
        throw std::runtime_error("participant must be between 2 and 65535");
    }
    scheduled.participant = static_cast<ParticipantId>(participant);

    const std::string& action = w[2];
    if (action == "enter") {
        ExpectArgs(w, 7, 8);
        oe::EnterOrder msg{};
        msg.token = ParseUnsigned(w[3]);
        msg.side = ParseSide(w[4]);
        msg.shares = ParseShares(w[5]);
        msg.instrument = kInstrument;
        msg.price = ParsePrice(w[6]);
        msg.time_in_force = oe::TimeInForce::Day;
        if (w.size() == 8) {
            if (w[7] == "ioc") {
                msg.time_in_force = oe::TimeInForce::ImmediateOrCancel;
            } else if (w[7] != "day") {
                throw std::runtime_error("expected day or ioc, got '" + w[7] + "'");
            }
        }
        scheduled.packet = Encode(msg);
    } else if (action == "replace") {
        ExpectArgs(w, 7, 7);
        oe::ReplaceOrder msg{};
        msg.existing_token = ParseUnsigned(w[3]);
        msg.replacement_token = ParseUnsigned(w[4]);
        msg.shares = ParseShares(w[5]);
        msg.price = ParsePrice(w[6]);
        scheduled.packet = Encode(msg);
    } else if (action == "cancel") {
        ExpectArgs(w, 4, 5);
        oe::CancelOrder msg{};
        msg.token = ParseUnsigned(w[3]);
        msg.shares = w.size() == 5 ? ParseShares(w[4]) : 0;
        scheduled.packet = Encode(msg);
    } else {
        throw std::runtime_error("unknown action '" + action + "'");
    }
    return scheduled;
}

}  // namespace

Price ParsePrice(std::string_view text) {
    const auto dot = text.find('.');
    const std::string_view whole = text.substr(0, dot);
    const std::string_view frac = dot == std::string_view::npos ? "" : text.substr(dot + 1);
    const auto all_digits = [](std::string_view s) {
        return std::all_of(s.begin(), s.end(), [](char c) { return c >= '0' && c <= '9'; });
    };
    if (whole.empty() || !all_digits(whole) || !all_digits(frac) || frac.size() > 4) {
        throw std::runtime_error("expected a price like 100.25, got '" + std::string(text) + "'");
    }
    Price value = static_cast<Price>(ParseUnsigned(whole)) * kPriceScale;
    Price scale = kPriceScale / 10;
    for (char c : frac) {
        value += (c - '0') * scale;
        scale /= 10;
    }
    return value;
}

Scenario ParseScenario(std::string_view text, std::string_view source_name) {
    Scenario scenario;
    std::istringstream input{std::string(text)};
    std::string line;
    int line_number = 0;
    while (std::getline(input, line)) {
        ++line_number;
        if (const auto hash = line.find('#'); hash != std::string::npos) {
            line.erase(hash);
        }
        const std::vector<std::string> words = SplitWords(line);
        if (words.empty()) {
            continue;
        }
        try {
            const bool is_order = words[0][0] >= '0' && words[0][0] <= '9';
            if (is_order) {
                scenario.messages.push_back(ParseOrderLine(words));
            } else {
                ParseConfigDirective(words, scenario.config);
            }
        } catch (const std::runtime_error& e) {
            std::ostringstream msg;
            msg << source_name << ':' << line_number << ": " << e.what();
            throw std::runtime_error(msg.str());
        }
    }

    // Messages at the same timestamp keep their file order.
    std::stable_sort(scenario.messages.begin(), scenario.messages.end(),
                     [](const ScheduledMessage& a, const ScheduledMessage& b) {
                         return a.time < b.time;
                     });

    if (scenario.config.timer_interval_ns > 0 && scenario.config.end_time_ns == 0) {
        throw std::runtime_error(std::string(source_name) +
                                 ": a scenario with a timer must also set 'end'");
    }
    return scenario;
}

Scenario LoadScenario(const std::string& path) {
    std::ifstream file(path);
    if (!file) {
        throw std::runtime_error("cannot open scenario file '" + path + "'");
    }
    std::ostringstream contents;
    contents << file.rdbuf();
    return ParseScenario(contents.str(), path);
}

}  // namespace sim
