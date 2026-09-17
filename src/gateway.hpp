#pragma once
#include "mbap.hpp"
#include "rtu_transport.hpp"
#include <cstdint>
#include <vector>
#include <atomic>

namespace mb {

// ---------------------------------------------------------------------------
// The bridge.
//
// TCP request  -> strip MBAP, prepend unit id as the RTU slave address,
//                 append CRC-16, put it on the bus
// RTU response -> validate CRC, strip address and CRC, reattach the ORIGINAL
//                 transaction id, send back to the originating client
//
// TCP/RTU bridge.
//
//   TCP request  -> strip MBAP, prepend unit id as the RTU slave address,
//                   append CRC-16, put it on the bus
//   RTU response -> validate CRC, strip address and CRC, reattach the original
//                   transaction id, return to the originating client
//
// The transaction id is held only in the caller's stack frame between these two
// steps. If that association is crossed, a master receives another master's
// data and accepts it as its own.
// caller's stack frame between these two calls. If that association is ever
// crossed, master A receives master B's data and believes it. In a plant that
// means acting on another device's readings.
//
// Hence the invariant asserted in the tests: for every response delivered,
// (client, transaction_id, unit_id) must equal the request that produced it,
// under any amount of concurrency.
// ---------------------------------------------------------------------------

struct GatewayStats {
    std::atomic<std::uint64_t> requests{0};
    std::atomic<std::uint64_t> responses{0};
    std::atomic<std::uint64_t> timeouts{0};       // -> exception 0x0B
    std::atomic<std::uint64_t> crc_failures{0};   // -> exception 0x0B
    std::atomic<std::uint64_t> bad_unit{0};       // -> exception 0x0A
    std::atomic<std::uint64_t> malformed{0};
};

// TCP ADU -> RTU ADU. Returns the RTU frame including CRC.
std::vector<std::uint8_t> tcp_to_rtu(const MbapFrame& f);

// RTU ADU -> TCP ADU. Returns nullopt when the CRC is bad or the frame is
// too short; the caller turns that into a gateway exception.
std::optional<MbapFrame> rtu_to_tcp(const std::vector<std::uint8_t>& rtu,
                                    std::uint16_t transaction_id);

class Gateway {
public:
    Gateway(IRtuTransport& rtu, GatewayStats& stats)
        : rtu_(rtu), stats_(stats) {}

    // Handle one decoded request and produce exactly one response frame.
        // Always produces a response: a silent gateway leaves the master waiting
    // for its own timeout, which is indistinguishable from a dead gateway.
    // waiting for its own timeout, which is worse than an explicit exception.
    MbapFrame handle(const MbapFrame& req);

private:
    IRtuTransport& rtu_;
    GatewayStats&  stats_;
};

} // namespace mb
