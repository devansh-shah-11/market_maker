// This file was generated with the help of Claude Code.
//
// A Packet is the unit of transfer between the exchange and a participant.
// Every message is a packed, fixed-layout struct, so its in-memory
// representation *is* its wire representation (little-endian).
#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <span>
#include <stdexcept>
#include <type_traits>

namespace sim {

inline constexpr std::size_t kMaxPacketSize = 64;

struct Packet {
    std::uint8_t size = 0;
    std::array<std::byte, kMaxPacketSize> data{};

    std::span<const std::byte> view() const { return {data.data(), size}; }
};

template <typename Msg>
concept WireMessage = std::is_trivially_copyable_v<Msg> && sizeof(Msg) <= kMaxPacketSize;

template <WireMessage Msg>
Packet Encode(const Msg& msg) {
    Packet packet;
    packet.size = static_cast<std::uint8_t>(sizeof(Msg));
    std::memcpy(packet.data.data(), &msg, sizeof(Msg));
    return packet;
}

// Copies bytes into a message struct. Throws if the size doesn't match.
template <WireMessage Msg>
Msg Decode(std::span<const std::byte> bytes) {
    if (bytes.size() != sizeof(Msg)) {
        throw std::invalid_argument("sim::Decode: packet size does not match message type");
    }
    Msg msg;
    std::memcpy(&msg, bytes.data(), sizeof(Msg));
    return msg;
}

// The first byte of every message identifies its type.
inline char MessageTypeOf(std::span<const std::byte> bytes) {
    return bytes.empty() ? '\0' : static_cast<char>(bytes[0]);
}

}  // namespace sim
