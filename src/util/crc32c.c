#include "crc32c.h"

/* Reflected Castagnoli polynomial: 0x82F63B78 = bitrev(0x1EDC6F41). */
#define CRC32C_POLY_REFLECTED 0x82F63B78u

static uint32_t crc32c_tab[256];
static int      crc32c_tab_built;

static void crc32c_build_table(void) {
    uint32_t i;
    for (i = 0; i < 256u; i++) {
        uint32_t c = i;
        int      j;
        for (j = 0; j < 8; j++) {
            c = (c & 1u) ? ((c >> 1) ^ CRC32C_POLY_REFLECTED) : (c >> 1);
        }
        crc32c_tab[i] = c;
    }
    crc32c_tab_built = 1;
}

uint32_t crc32c(uint32_t crc, const void *buf, uint32_t size) {
    const uint8_t *p = (const uint8_t *)buf;
    if (!crc32c_tab_built) crc32c_build_table();
    while (size--) {
        crc = crc32c_tab[(crc ^ *p++) & 0xFFu] ^ (crc >> 8);
    }
    return crc;
}
