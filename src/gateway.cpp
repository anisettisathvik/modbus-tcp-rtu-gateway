#include "gateway.hpp"

namespace mb {

std::vector<std::uint8_t> tcp_to_rtu(const MbapFrame& f)
{
    std::vector<std::uint8_t> rtu;
    rtu.reserve(1 + f.pdu.size() + 2);
    rtu.push_back(f.unit_id);
    rtu.insert(rtu.end(), f.pdu.begin(), f.pdu.end());

    const std::uint16_t crc =
        mb_crc16(rtu.data(), static_cast<std::uint16_t>(rtu.size()));
    rtu.push_back(static_cast<std::uint8_t>(crc & 0xFF));   // low byte first
    rtu.push_back(static_cast<std::uint8_t>(crc >> 8));
    return rtu;
}

std::optional<MbapFrame> rtu_to_tcp(const std::vector<std::uint8_t>& rtu,
                                    std::uint16_t transaction_id)
{
        // addr + fc + crc = 4 is the shortest legal RTU ADU.
    if (rtu.size() < 4) return std::nullopt;

        // CRC over a frame including its own CRC is zero when intact.
    if (mb_crc16(rtu.data(), static_cast<std::uint16_t>(rtu.size())) != 0)
        return std::nullopt;

    MbapFrame f;
    f.transaction_id = transaction_id;
    f.protocol_id    = 0;
    f.unit_id        = rtu[0];
    f.pdu.assign(rtu.begin() + 1, rtu.end() - 2);   // drop address and CRC
    return f;
}

MbapFrame Gateway::handle(const MbapFrame& req)
{
    stats_.requests++;

    const std::uint8_t fc = req.pdu.empty() ? 0 : req.pdu[0];

    if (req.pdu.empty()) {
        stats_.malformed++;
        return make_exception(req.transaction_id, req.unit_id, 0,
                              EX_GATEWAY_PATH_UNAVAILABLE);
    }

    const auto rtu_req = tcp_to_rtu(req);
    std::vector<std::uint8_t> rtu_rsp;

    if (!rtu_.transact(rtu_req, rtu_rsp)) {
                // No device answered within the bus timeout. Exception 0x0B lets the
        // master distinguish a dead field device from a dead gateway.
        // exactly this and most gateways never use it: 0x0B, gateway target
        // device failed to respond. Returning it lets the master distinguish
        // "the field device is dead" from "the gateway is dead", which is the
        // difference between one truck roll and two.
        stats_.timeouts++;
        return make_exception(req.transaction_id, req.unit_id, fc,
                              EX_GATEWAY_TARGET_NO_RESPONSE);
    }

    auto tcp_rsp = rtu_to_tcp(rtu_rsp, req.transaction_id);
    if (!tcp_rsp) {
                // The response arrived with a bad CRC. TCP carries no checksum, so a
        // corrupted payload forwarded upward would be accepted as valid data.
        // collision, or a device with a broken stack. Never forward it. TCP
        // has no CRC, so a corrupted payload passed upward would be accepted
        // by the master as valid data. This is the one place where dropping
        // the frame is mandatory rather than merely tidy.
        stats_.crc_failures++;
        return make_exception(req.transaction_id, req.unit_id, fc,
                              EX_GATEWAY_TARGET_NO_RESPONSE);
    }

    stats_.responses++;
    return *tcp_rsp;
}

} // namespace mb
