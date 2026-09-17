#ifndef MODBUS_CRC_H
#define MODBUS_CRC_H

#include <stdint.h>

/* CRC-16/MODBUS: poly 0xA001 (reflected 0x8005), init 0xFFFF, no final XOR.
 * Transmitted low byte first -- the only little-endian field in an RTU frame. */
uint16_t mb_crc16(const uint8_t *buf, uint16_t len);

#endif /* MODBUS_CRC_H */
