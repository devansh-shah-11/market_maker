// This file was generated with the help of Claude Code.
//
// Write-ahead decision log.
//
// Every order is recorded *before* it is sent, so that if the process dies
// mid-send, the log shows what we intended to have on the exchange and the
// state can be reconstructed on restart. Writing after the send would leave
// a window where an order is live on the exchange with no record of it.
#pragma once

#include <cstdint>
#include <cstdio>
#include <stdexcept>
#include <string>

#include "sim/messages/types.h"

namespace app {

class DecisionLog {
public:
    explicit DecisionLog(const std::string& path) : file_(std::fopen(path.c_str(), "w")) {
        if (file_ == nullptr) {
            throw std::runtime_error("DecisionLog: cannot open " + path);
        }
    }
    ~DecisionLog() { std::fclose(file_); }
    DecisionLog(const DecisionLog&) = delete;
    DecisionLog& operator=(const DecisionLog&) = delete;

    void Record(sim::Timestamp now, const char* action, sim::Side side, sim::Price price,
                std::uint32_t shares, sim::Token token) {
        std::fprintf(file_, "%llu %s %c %lld.%04lld x %u token=%llu\n",
                     static_cast<unsigned long long>(now), action, static_cast<char>(side),
                     static_cast<long long>(price / sim::kPriceScale),
                     static_cast<long long>(price % sim::kPriceScale), shares,
                     static_cast<unsigned long long>(token));
    }

    void Note(sim::Timestamp now, const char* text) {
        std::fprintf(file_, "%llu %s\n", static_cast<unsigned long long>(now), text);
    }

private:
    std::FILE* file_;
};

}  // namespace app
