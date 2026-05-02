#ifndef UTIL_CRC32C_H
#define UTIL_CRC32C_H

#include <stdint.h>
#include <stddef.h>

/* Castagnoli CRC32C (polynomial 0x1EDC6F41, reflected 0x82F63B78). Used
 * by ext4/jbd2 for metadata checksums. Table-driven: ~1 KB DGROUP, lazy
 * init on first call.
 *
 * Convention follows lwext4's ext4_crc32c — no internal final XOR. The
 * caller passes 0xFFFFFFFF on the first call, chains the result for
 * additional buffers, and compares directly against the on-disk
 * checksum (which the writer stored without a final XOR too). */
#define CRC32C_INIT 0xFFFFFFFFu

uint32_t crc32c(uint32_t crc, const void *buf, uint32_t size);

#endif
