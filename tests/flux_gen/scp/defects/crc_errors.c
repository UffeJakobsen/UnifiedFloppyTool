/**
 * @file tests/flux_gen/scp/defects/crc_errors.c
 * @brief UFT_DEFECT_CRC_ERROR injection.
 *
 * Corrupts one byte in a generated flux stream so a downstream MFM
 * decoder + CRC check fails. The corruption is deterministic (the same
 * generated capture always gets the same byte flipped) so regression
 * tests don't drift.
 *
 * Strategy: XOR the 7th sample word's low byte with 0x55. That sample
 * is chosen because:
 *   - it's well past the first few samples (which a decoder might
 *     ignore as sync gunk)
 *   - it's well before the end of the rev (so end-of-rev accumulator
 *     drop doesn't mask it)
 *   - 0x55 (alternating bits) creates a large enough timing delta
 *     to be clearly out-of-band, but stays within the medium-safe
 *     range for a single corrupted cell.
 *
 * Forensic note: a real-world CRC error usually corrupts MANY bits
 * (a magnetic-bloom-out is broad). This generator simulates the
 * minimal CRC-failure case, which is what the recovery pipeline's
 * CRC-correction code needs to be tested against. Broader corruption
 * patterns are a v4.1.6+ candidate.
 */

#include "../flux_gen.h"

#include <stdint.h>

uft_flux_gen_err_t uft_defect_crc_error_apply(
    uft_scp_flux_capture_t *cap, size_t target_rev)
{
    if (!cap || !cap->bytes) return UFT_FLUX_GEN_ERR_NULL;
    if ((int)target_rev >= cap->rev_count) return UFT_FLUX_GEN_ERR_INVALID;

    /* Find rev start offset. */
    size_t off = 0;
    for (size_t r = 0; r < target_rev; r++) {
        off += (size_t)cap->rev_flux_count[r] * 2u;
    }
    /* Corrupt the 7th sample's low byte. Each sample is 2 bytes. */
    size_t corrupt_off = off + 7 * 2 + 1;
    if (corrupt_off >= cap->bytes_len) {
        return UFT_FLUX_GEN_ERR_INVALID;
    }
    cap->bytes[corrupt_off] ^= 0x55;
    return UFT_FLUX_GEN_OK;
}
