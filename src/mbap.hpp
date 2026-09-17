#pragma once
#include <cstdint>
#include <vector>
#include <array>
#include <optional>
#include <string>

// ---------------------------------------------------------------------------
// Modbus TCP MBAP (Modbus Application Protocol) header.
//
//   0..1  transaction id   echoed back unchanged; how a master matches a
//                          response to its request
//   2..3  protocol id      always 0 for Modbus
//   4..5  length           byte count of everything AFTER this field,
//                          i.e. unit_id + PDU
//   6     unit id          becomes the RTU slave address at the gateway
//   7..   PDU              identical bytes to the RTU PDU
//
// Modbus TCP MBAP (Modbus Application Protocol) header.
//
//   0..1  transaction id   echoed back unchanged; how a master matches a
//                          response to its request
//   2..3  protocol id      always 0 for Modbus
//   4..5  length           byte count of unit_id + PDU
//   6     unit id          becomes the RTU slave address at the gateway
//   7..   PDU              identical bytes to the RTU PDU
//
// RTU carries a CRC and no transaction id; TCP carries a transaction id and no
// CRC. The gateway must therefore hold the transaction id in memory while the
// request is on the serial bus and reattach it to the response: there is
// nothing on the RTU wire to recover it from. That makes the routing table the
// safety-critical part of the gateway.
// transaction id. TCP carries a transaction id and no CRC (TCP already
// guarantees integrity). So the gateway must *hold* the transaction id in
// memory while the request is on the serial bus, and reattach it to the
// response. There is nothing on the wire to recover it from.
//
// That is why the routing table is the safety-critical part of a gateway, and
// why the invariant we assert is "no response ever reaches the wrong master".
// ---------------------------------------------------------------------------

namespace mb {

inline constexpr std::size_t MBAP_HEADER_LEN = 7;
inline constexpr std::size_t MAX_PDU         = 253;
inline constexpr std::size_t MAX_TCP_ADU     = MBAP_HEADER_LEN + MAX_PDU;

// Gateway-specific exception codes (Modbus Application Protocol spec).
inline constexpr std::uint8_t EX_GATEWAY_PATH_UNAVAILABLE   = 0x0A;
inline constexpr std::uint8_t EX_GATEWAY_TARGET_NO_RESPONSE = 0x0B;

struct MbapFrame {
    std::uint16_t            transaction_id = 0;
    std::uint16_t            protocol_id    = 0;
    std::uint8_t             unit_id        = 0;
    std::vector<std::uint8_t> pdu;

    bool operator==(const MbapFrame& o) const {
        return transaction_id == o.transaction_id &&
               protocol_id    == o.protocol_id &&
               unit_id        == o.unit_id &&
               pdu            == o.pdu;
    }
};

enum class DecodeError {
    NeedMoreData,      // not a failure: TCP is a stream, wait for the rest
    BadProtocolId,     // protocol id must be zero
    LengthTooSmall,    // length must cover at least unit_id + function code
    LengthTooLarge,    // PDU cannot exceed 253 bytes
};

struct DecodeResult {
    std::optional<MbapFrame> frame;
    DecodeError              error = DecodeError::NeedMoreData;
    std::size_t              consumed = 0;   // bytes to drop from the stream
};

// Decode one ADU from the front of a byte stream.
//
// Decode one ADU from the front of a byte stream.
//
// Written as a stream decoder because TCP does not preserve message
// boundaries: a master may send two requests in one segment or split one
// across three.
// does not preserve message boundaries. A master is entitled to send two
// requests in one segment, or split one request across three. Treating a
// recv() buffer as exactly one frame is the single most common bug in
// hand-written Modbus TCP code.
DecodeResult decode(const std::uint8_t* data, std::size_t len);

// Serialise a frame to the wire.
std::vector<std::uint8_t> encode(const MbapFrame& f);

// Build an exception response carrying the original transaction id.
MbapFrame make_exception(std::uint16_t transaction_id,
                         std::uint8_t  unit_id,
                         std::uint8_t  function_code,
                         std::uint8_t  exception_code);

const char* to_string(DecodeError e);

} // namespace mb
