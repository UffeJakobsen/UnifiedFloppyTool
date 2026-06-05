/**
 * @file tests/flux_gen/scp/defects/copy_protection/vmax.c
 * @brief V-MAX! copy-protection signature pattern.
 *
 * V-MAX! (used by SoftWare Toolworks et al. for Commodore C64 5.25")
 * encodes its protection by writing a long run of identical GCR cell
 * times — specifically a uniform 3-cell pattern (~12 µs) that a
 * standard MFM/GCR decoder cannot easily parse but a V-MAX! aware
 * protection-detector (src/protection/c64/uft_track_align.c et al.)
 * recognises as a signature.
 *
 * This generator emits a stream of UNIFORM 3-cell intervals
 * (3 × 4 µs = 12 µs = 480 ticks per sample at 25 ns/tick) for every
 * transition in the requested rev. The result, when fed to a V-MAX!
 * detector, should classify with high confidence.
 *
 * Forensic invariant: V-MAX! protection is read-only by UFT policy.
 * This generator produces a SIGNATURE for testing the DETECTOR. It
 * never produces flux that would write a V-MAX! pattern onto real
 * media (UFT has no write path through this code at all).
 *
 * Limitation: this is a simplified V-MAX! profile — real V-MAX! has
 * SECTOR-LEVEL variability (some sectors uniform, others scrambled).
 * This generator emits the uniform-sector signature only. Test
 * coverage of mixed-sector V-MAX! is a v4.1.6+ candidate.
 */

#include "../../flux_gen.h"
#include "../../../../../include/uft/hal/uft_scp_direct.h"

#include <stdint.h>

#define VMAX_CELL_NS  (3u * UFT_SCP_FLUX_GEN_CELL_NS_DD)   /* 12000 ns = 480 ticks */

uft_flux_gen_err_t uft_defect_vmax_emit(
    uint8_t *out, size_t out_cap, size_t *out_written,
    uint32_t transitions)
{
    if (!out || !out_written) return UFT_FLUX_GEN_ERR_NULL;
    *out_written = 0;
    if (transitions == 0) return UFT_FLUX_GEN_ERR_INVALID;

    uint32_t ticks = VMAX_CELL_NS / UFT_SCP_FLUX_NS_PER_SAMPLE;  /* 480 */
    /* 480 < 0x10000 so each sample is one 16-bit BE word, no
     * overflow markers needed. */

    size_t need = (size_t)transitions * 2u;
    if (need > out_cap) return UFT_FLUX_GEN_ERR_BUF_SMALL;

    for (uint32_t i = 0; i < transitions; i++) {
        out[i * 2 + 0] = (uint8_t)(ticks >> 8);
        out[i * 2 + 1] = (uint8_t)(ticks & 0xFF);
    }
    *out_written = need;
    return UFT_FLUX_GEN_OK;
}
