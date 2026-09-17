#include "mbap.hpp"

namespace mb {

DecodeResult decode(const std::uint8_t* data, std::size_t len)
{
    DecodeResult r;

    if (len < MBAP_HEADER_LEN) {
        r.error = DecodeError::NeedMoreData;
        return r;
    }

    const std::uint16_t txn   = static_cast<std::uint16_t>((data[0] << 8) | data[1]);
    const std::uint16_t proto = static_cast<std::uint16_t>((data[2] << 8) | data[3]);
    const std::uint16_t length= static_cast<std::uint16_t>((data[4] << 8) | data[5]);

    if (proto != 0) {
                // Unrecoverable: the length field is how the next frame boundary is
        // found, so the stream cannot be resynchronised. The caller closes the
        // connection.
        // must drop the connection rather than try to resynchronise.
        r.error = DecodeError::BadProtocolId;
        return r;
    }
        // length covers unit_id + PDU; the minimum PDU is one function code byte.
    // code byte, so length >= 2.
    if (length < 2) {
        r.error = DecodeError::LengthTooSmall;
        return r;
    }
    if (length > 1 + MAX_PDU) {
        r.error = DecodeError::LengthTooLarge;
        return r;
    }

    const std::size_t total = 6u + length;   // 6 bytes precede the length field
    if (len < total) {
        r.error = DecodeError::NeedMoreData;
        return r;
    }

    MbapFrame f;
    f.transaction_id = txn;
    f.protocol_id    = proto;
    f.unit_id        = data[6];
    f.pdu.assign(data + MBAP_HEADER_LEN, data + total);

    r.frame    = std::move(f);
    r.consumed = total;
    r.error    = DecodeError::NeedMoreData;   // unused when frame is set
    return r;
}

std::vector<std::uint8_t> encode(const MbapFrame& f)
{
    const std::uint16_t length = static_cast<std::uint16_t>(f.pdu.size() + 1u);
    std::vector<std::uint8_t> out;
    out.reserve(MBAP_HEADER_LEN + f.pdu.size());
    out.push_back(static_cast<std::uint8_t>(f.transaction_id >> 8));
    out.push_back(static_cast<std::uint8_t>(f.transaction_id & 0xFF));
    out.push_back(static_cast<std::uint8_t>(f.protocol_id >> 8));
    out.push_back(static_cast<std::uint8_t>(f.protocol_id & 0xFF));
    out.push_back(static_cast<std::uint8_t>(length >> 8));
    out.push_back(static_cast<std::uint8_t>(length & 0xFF));
    out.push_back(f.unit_id);
    out.insert(out.end(), f.pdu.begin(), f.pdu.end());
    return out;
}

MbapFrame make_exception(std::uint16_t transaction_id,
                         std::uint8_t  unit_id,
                         std::uint8_t  function_code,
                         std::uint8_t  exception_code)
{
    MbapFrame f;
    f.transaction_id = transaction_id;
    f.protocol_id    = 0;
    f.unit_id        = unit_id;
    f.pdu = { static_cast<std::uint8_t>(function_code | 0x80), exception_code };
    return f;
}

const char* to_string(DecodeError e)
{
    switch (e) {
    case DecodeError::NeedMoreData:   return "need more data";
    case DecodeError::BadProtocolId:  return "protocol id not zero";
    case DecodeError::LengthTooSmall: return "length below minimum";
    case DecodeError::LengthTooLarge: return "length above maximum";
    }
    return "unknown";
}

} // namespace mb
