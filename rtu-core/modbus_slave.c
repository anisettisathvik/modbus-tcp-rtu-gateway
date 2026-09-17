#include "modbus_slave.h"
#include "modbus_crc.h"

/* ---- small helpers ------------------------------------------------------ */

static uint16_t rd_u16(const uint8_t *p)
{
    return (uint16_t)(((uint16_t)p[0] << 8) | (uint16_t)p[1]);
}

static void wr_u16(uint8_t *p, uint16_t v)
{
    p[0] = (uint8_t)(v >> 8);
    p[1] = (uint8_t)(v & 0xFFu);
}

/* Append CRC low-byte-first and return the total ADU length. */
static uint16_t seal(uint8_t *resp, uint16_t body_len)
{
    uint16_t crc = mb_crc16(resp, body_len);
    resp[body_len]     = (uint8_t)(crc & 0xFFu);
    resp[body_len + 1] = (uint8_t)(crc >> 8);
    return (uint16_t)(body_len + 2u);
}

static mb_result_t make_exception(mb_slave_t *s, uint8_t fc, uint8_t code,
                                  uint8_t *resp, uint16_t *resp_len)
{
    resp[0] = s->addr;
    resp[1] = (uint8_t)(fc | 0x80u);
    resp[2] = code;
    *resp_len = seal(resp, 3u);
    s->cnt_exceptions++;
    s->cnt_responses++;
    return MB_RESULT_RESPOND;
}

/* ---- public ------------------------------------------------------------- */

void mb_slave_init(mb_slave_t *s, uint8_t addr,
                   uint16_t *holding, uint16_t holding_count)
{
    uint32_t i;
    s->addr          = addr;
    s->holding       = holding;
    s->holding_count = holding_count;
    s->cnt_frames_ok = 0u;
    s->cnt_crc_err   = 0u;
    s->cnt_len_err   = 0u;
    s->cnt_exceptions = 0u;
    s->cnt_responses = 0u;
    s->cnt_broadcast = 0u;
    for (i = 0u; i < holding_count; i++) {
        holding[i] = 0u;
    }
}

mb_result_t mb_slave_handle(mb_slave_t *s,
                            const uint8_t *req, uint16_t req_len,
                            uint8_t *resp, uint16_t *resp_len)
{
    uint8_t  fc;
    uint8_t  is_broadcast;
    uint16_t rx_crc, calc_crc;
    uint16_t start, qty, value, reg;
    uint8_t  byte_count;
    uint32_t i;

    /* 1. structural length ------------------------------------------------ */
    if (req_len < MB_ADU_MIN || req_len > MB_ADU_MAX) {
        s->cnt_len_err++;
        return MB_RESULT_BAD_LENGTH;
    }

    /* CRC is checked ahead of the address filter: a corrupted address byte
     * cannot be trusted, and the spec's bus communication error counter is
     * bus-wide rather than per-slave. */
    calc_crc = mb_crc16(req, (uint16_t)(req_len - 2u));
    rx_crc   = (uint16_t)((uint16_t)req[req_len - 2u] |
                          ((uint16_t)req[req_len - 1u] << 8));
    if (calc_crc != rx_crc) {
        s->cnt_crc_err++;
        return MB_RESULT_BAD_CRC;
    }

    /* 3. address filter --------------------------------------------------- */
    is_broadcast = (req[0] == MB_ADDR_BROADCAST) ? 1u : 0u;
    if (!is_broadcast && req[0] != s->addr) {
        return MB_RESULT_SILENT;
    }

    s->cnt_frames_ok++;
    if (is_broadcast) {
        s->cnt_broadcast++;
    }

    fc = req[1];

    switch (fc) {

    /* ---- 0x03 Read Holding Registers -------------------------------- */
    case MB_FC_READ_HOLDING:
        if (req_len != 8u) {
            s->cnt_len_err++;
            return MB_RESULT_BAD_LENGTH;
        }
        /* A broadcast read has no valid response: every slave would answer. */
        if (is_broadcast) {
            return MB_RESULT_SILENT;
        }
        start = rd_u16(&req[2]);
        qty   = rd_u16(&req[4]);

        if (qty < 1u || qty > MB_READ_QTY_MAX) {
            return make_exception(s, fc, MB_EX_ILLEGAL_VALUE, resp, resp_len);
        }
        /* Widened to 32 bit: start=0xFFFF, qty=2 wraps in 16-bit arithmetic
         * and would pass a naive bounds check. */
        if (((uint32_t)start + (uint32_t)qty) > (uint32_t)s->holding_count) {
            return make_exception(s, fc, MB_EX_ILLEGAL_ADDRESS, resp, resp_len);
        }

        resp[0] = s->addr;
        resp[1] = fc;
        resp[2] = (uint8_t)(qty * 2u);
        for (i = 0u; i < qty; i++) {
            wr_u16(&resp[3u + (i * 2u)], s->holding[start + i]);
        }
        *resp_len = seal(resp, (uint16_t)(3u + (qty * 2u)));
        s->cnt_responses++;
        return MB_RESULT_RESPOND;

    /* ---- 0x06 Write Single Register --------------------------------- */
    case MB_FC_WRITE_SINGLE:
        if (req_len != 8u) {
            s->cnt_len_err++;
            return MB_RESULT_BAD_LENGTH;
        }
        reg   = rd_u16(&req[2]);
        value = rd_u16(&req[4]);

        if ((uint32_t)reg >= (uint32_t)s->holding_count) {
            if (is_broadcast) {
                return MB_RESULT_SILENT;
            }
            return make_exception(s, fc, MB_EX_ILLEGAL_ADDRESS, resp, resp_len);
        }

        s->holding[reg] = value;

        if (is_broadcast) {
            return MB_RESULT_SILENT;   /* Applied, but a broadcast is never answered. */
        }

        resp[0] = s->addr;
        resp[1] = fc;
        wr_u16(&resp[2], reg);
        wr_u16(&resp[4], value);
        *resp_len = seal(resp, 6u);
        s->cnt_responses++;
        return MB_RESULT_RESPOND;

    /* ---- 0x10 Write Multiple Registers ------------------------------ */
    case MB_FC_WRITE_MULTIPLE:
        if (req_len < 9u) {
            s->cnt_len_err++;
            return MB_RESULT_BAD_LENGTH;
        }
        start      = rd_u16(&req[2]);
        qty        = rd_u16(&req[4]);
        byte_count = req[6];

        /* A declared byte count inconsistent with the received length is a
         * malformed ADU, not a protocol error: drop it rather than answering. */
        if ((uint32_t)req_len != (9u + (uint32_t)byte_count)) {
            s->cnt_len_err++;
            return MB_RESULT_BAD_LENGTH;
        }
        if (qty < 1u || qty > MB_WRITE_MULTI_QTY_MAX ||
            (uint32_t)byte_count != ((uint32_t)qty * 2u)) {
            if (is_broadcast) {
                return MB_RESULT_SILENT;
            }
            return make_exception(s, fc, MB_EX_ILLEGAL_VALUE, resp, resp_len);
        }
        if (((uint32_t)start + (uint32_t)qty) > (uint32_t)s->holding_count) {
            if (is_broadcast) {
                return MB_RESULT_SILENT;
            }
            return make_exception(s, fc, MB_EX_ILLEGAL_ADDRESS, resp, resp_len);
        }

        for (i = 0u; i < qty; i++) {
            s->holding[start + i] = rd_u16(&req[7u + (i * 2u)]);
        }

        if (is_broadcast) {
            return MB_RESULT_SILENT;
        }
        resp[0] = s->addr;
        resp[1] = fc;
        wr_u16(&resp[2], start);
        wr_u16(&resp[4], qty);
        *resp_len = seal(resp, 6u);
        s->cnt_responses++;
        return MB_RESULT_RESPOND;

    /* ---- anything else ---------------------------------------------- */
    default:
        if (is_broadcast) {
            return MB_RESULT_SILENT;
        }
        return make_exception(s, fc, MB_EX_ILLEGAL_FUNCTION, resp, resp_len);
    }
}
