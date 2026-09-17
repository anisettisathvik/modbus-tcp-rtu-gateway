#ifndef MODBUS_SLAVE_H
#define MODBUS_SLAVE_H

#include <stdint.h>

/* Modbus RTU slave, protocol layer.
 *
 * Turns a received byte buffer into a response byte buffer. Contains no
 * reference to UARTs, timers, RS-485 or any MCU; all hardware access lives
 * behind port/mb_port.h, so this file compiles unchanged for host and target.
 */

#define MB_ADDR_BROADCAST       0u
#define MB_ADU_MAX              256u   /* addr + PDU(253) + CRC(2) = 256 */
#define MB_ADU_MIN              4u     /* addr + fc + CRC */

/* function codes */
#define MB_FC_READ_HOLDING      0x03u
#define MB_FC_WRITE_SINGLE      0x06u
#define MB_FC_WRITE_MULTIPLE    0x10u

/* exception codes */
#define MB_EX_ILLEGAL_FUNCTION  0x01u
#define MB_EX_ILLEGAL_ADDRESS   0x02u
#define MB_EX_ILLEGAL_VALUE     0x03u
#define MB_EX_DEVICE_FAILURE    0x04u

/* protocol limits (Modbus application protocol spec v1.1b3) */
#define MB_READ_QTY_MAX         125u
#define MB_WRITE_MULTI_QTY_MAX  123u

typedef enum {
    MB_RESULT_RESPOND = 0,  /* resp[0..*resp_len-1] is valid, transmit it   */
    MB_RESULT_SILENT,       /* legal frame, no reply owed (broadcast / not us) */
    MB_RESULT_BAD_CRC,      /* discard, bus error                           */
    MB_RESULT_BAD_LENGTH    /* discard, malformed ADU                       */
} mb_result_t;

typedef struct {
    uint8_t   addr;            /* this slave's unit id, 1..247              */
    uint16_t *holding;         /* holding register file                     */
    uint16_t  holding_count;

    /* Diagnostic counters: the only visibility into a bus with no console. */
    uint32_t  cnt_frames_ok;
    uint32_t  cnt_crc_err;
    uint32_t  cnt_len_err;
    uint32_t  cnt_exceptions;
    uint32_t  cnt_responses;
    uint32_t  cnt_broadcast;
} mb_slave_t;

void mb_slave_init(mb_slave_t *s, uint8_t addr,
                   uint16_t *holding, uint16_t holding_count);

/* req      : complete received ADU, including the 2 CRC bytes
 * req_len  : its length
 * resp     : caller buffer, must be >= MB_ADU_MAX
 * resp_len : set only when the return value is MB_RESULT_RESPOND
 */
mb_result_t mb_slave_handle(mb_slave_t *s,
                            const uint8_t *req, uint16_t req_len,
                            uint8_t *resp, uint16_t *resp_len);

#endif /* MODBUS_SLAVE_H */
