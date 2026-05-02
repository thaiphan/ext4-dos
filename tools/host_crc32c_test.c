/* Host-side test for util/crc32c.
 *
 * Reference values were computed against the same algorithm lwext4 uses
 * (table-driven Castagnoli, no internal final XOR). A spot-check vector
 * "123456789" is the canonical CRC test string and is well-documented;
 * the rest are reproducible from the same tab generator. */
#include <stdio.h>
#include <stdint.h>
#include <string.h>

#include "util/crc32c.h"

static int failures = 0;

#define CHECK_CRC(input, len, expect) do { \
    uint32_t got = crc32c(CRC32C_INIT, (input), (len)); \
    if (got != (expect)) { \
        fprintf(stderr, "FAIL: %s:%d: crc32c('%s' len=%u) expected 0x%08x, got 0x%08x\n", \
                __FILE__, __LINE__, #input, (unsigned)(len), \
                (unsigned)(expect), (unsigned)got); \
        failures++; \
    } \
} while (0)

int main(void) {
    static const uint8_t zeros16[16] = {0};
    /* Empty input: nothing to mix in, returns the seed unchanged. */
    CHECK_CRC("",          0u, 0xFFFFFFFFu);
    /* Single-byte vector. */
    CHECK_CRC("a",         1u, 0x3E2FBCCFu);
    /* Canonical CRC test vector "123456789".
     * Standard CRC32C with final XOR = 0xE3069283. We don't apply the
     * final XOR (lwext4 convention), so 0xE3069283 ^ 0xFFFFFFFF = 0x1CF96D7C. */
    CHECK_CRC("123456789", 9u, 0x1CF96D7Cu);
    /* 16 zero bytes — used as a stand-in for "uuid in a zero-uuid FS". */
    CHECK_CRC(zeros16,    16u, 0xBD8F6515u);

    /* Chaining: crc32c(seed=A, "ab") should equal crc32c(crc32c(seed=A, "a"), "b"). */
    {
        uint32_t one_shot = crc32c(CRC32C_INIT, "ab", 2u);
        uint32_t step     = crc32c(CRC32C_INIT, "a",  1u);
        step              = crc32c(step,        "b",  1u);
        if (one_shot != step) {
            fprintf(stderr, "FAIL: chained crc32c diverges (one_shot=0x%08x step=0x%08x)\n",
                    (unsigned)one_shot, (unsigned)step);
            failures++;
        }
    }

    if (failures == 0) {
        printf("host_crc32c_test: all asserts passed\n");
        return 0;
    }
    fprintf(stderr, "host_crc32c_test: %d FAILURE(S)\n", failures);
    return 1;
}
