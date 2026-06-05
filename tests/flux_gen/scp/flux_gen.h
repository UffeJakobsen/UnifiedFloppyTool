/**
 * @file tests/flux_gen/scp/flux_gen.h
 * @brief Synthetic flux generator for SCP capture-RAM format.
 *
 * Produces 16-bit big-endian flux samples in the byte layout the
 * SCP firmware would write to its capture RAM (samdisk
 * SuperCardPro.cpp's ReadFlux loop reads these directly). The output
 * can be loaded into a `scp_fw_t` via `scp_fw_load_capture_ram()` so
 * the firmware-state-machine emulator dumps them via CMD_SENDRAM_USB
 * exactly like real firmware would.
 *
 * Tick clock: 40 MHz (25 ns per tick) — `UFT_SCP_FLUX_NS_PER_SAMPLE`.
 * Sample wire format:
 *   - Non-zero 16-bit BE word: emit a transition after that many ticks
 *   - 0x0000 word: accumulator overflow marker, adds 0x10000 ticks to
 *     the running accumulator (per samdisk).
 *
 * Defect classes (opt-in via flags):
 *   - UFT_DEFECT_WEAK_BITS   — randomised flux intervals in marked range
 *   - UFT_DEFECT_CRC_ERROR   — flip one bit so MFM CRC fails
 *   - UFT_DEFECT_VMAX_SIG    — V-MAX! copy-protection signature pattern
 *
 * Determinism: every generator takes a seed. Identical seed +
 * identical params => byte-identical output. CI-stable.
 *
 * Forensic-medium-safety: the generator REFUSES to emit intervals
 *   - < UFT_SCP_FLUX_GEN_MIN_NS  (4000 ns / 8 µs is the conservative
 *     short-cell floor; anything below ~2 µs would damage 5.25" media
 *     if fed back via a write path)
 *   - > UFT_SCP_FLUX_GEN_MAX_NS  (10 ms — longer is unphysical)
 *   Out-of-spec params return UFT_FLUX_GEN_ERR_OUT_OF_SPEC.
 */
#ifndef UFT_TESTS_SCP_FLUX_GEN_H
#define UFT_TESTS_SCP_FLUX_GEN_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ─── Constants ─────────────────────────────────────────────────────── */

/** Default MFM cell time in ns at 250 kbps (3.5" DD): 4 µs = 4000 ns.
 *  All "clean MFM" flux is generated at multiples of this base. */
#define UFT_SCP_FLUX_GEN_CELL_NS_DD       4000u

/** Forensic-safety guards. The generator REFUSES to emit timing
 *  outside this range — feeding such timing back to real media via
 *  a write path could damage the medium. Document any exception. */
#define UFT_SCP_FLUX_GEN_MIN_NS           2000u    /* 2 µs — below this damages media */
#define UFT_SCP_FLUX_GEN_MAX_NS           10000000u /* 10 ms — unphysical above this */

/* ─── Error codes ───────────────────────────────────────────────────── */
typedef enum {
    UFT_FLUX_GEN_OK              = 0,
    UFT_FLUX_GEN_ERR_NULL        = -1,
    UFT_FLUX_GEN_ERR_BUF_SMALL   = -2,
    UFT_FLUX_GEN_ERR_OUT_OF_SPEC = -3,
    UFT_FLUX_GEN_ERR_INVALID     = -4,
} uft_flux_gen_err_t;

/* ─── Defect-class flags (bitmask, opt-in per call) ─────────────────── */
typedef enum {
    UFT_DEFECT_NONE           = 0,
    UFT_DEFECT_WEAK_BITS      = 1u << 0,
    UFT_DEFECT_CRC_ERROR      = 1u << 1,
    UFT_DEFECT_VMAX_SIG       = 1u << 2,
} uft_defect_flags_t;

/* ─── Output container ─────────────────────────────────────────────
 *
 * The generator allocates the byte buffer; caller frees via
 * uft_flux_gen_free(). rev_meta is filled with per-rev metadata
 * suitable for scp_fw_load_capture_ram().
 */
typedef struct {
    uint8_t  *bytes;            /* 16-bit BE flux samples, concatenated */
    size_t    bytes_len;
    uint32_t  rev_index_ticks[8];  /* per-rev index period in ticks */
    uint32_t  rev_flux_count[8];   /* per-rev sample count */
    int       rev_count;
} uft_scp_flux_capture_t;

/* ─── Generator params ──────────────────────────────────────────── */
typedef struct {
    uint64_t            seed;          /* deterministic RNG seed */
    int                 revolutions;   /* 2..5 (firmware range) */
    uint32_t            index_period_ns;  /* e.g. 200_000_000 ns = 200 ms */
    uint32_t            transitions_per_rev;
    uft_defect_flags_t  defects;       /* bitmask of UFT_DEFECT_* */
    /* For DEFECT_WEAK_BITS: jitter as fraction of cell time (0..50%) */
    uint8_t             weak_jitter_pct;
} uft_scp_flux_params_t;

/* ─── Public API ───────────────────────────────────────────────────── */

/** Generate clean MFM-style flux (250 kbps DD, 4 µs cells, mix of
 *  2/3/4-cell intervals matching real MFM density). Honours params->
 *  defects flags. Allocates capture->bytes via malloc; caller frees
 *  via uft_flux_gen_free(). */
uft_flux_gen_err_t uft_scp_flux_gen_clean(
    const uft_scp_flux_params_t *params,
    uft_scp_flux_capture_t      *out_capture);

/** Generate flux carrying the V-MAX! copy-protection signature —
 *  a long string of identical cell times (3-cell GCR pattern) that
 *  V-MAX! protection detectors look for. */
uft_flux_gen_err_t uft_scp_flux_gen_vmax(
    const uft_scp_flux_params_t *params,
    uft_scp_flux_capture_t      *out_capture);

/** Free buffer allocated by any uft_scp_flux_gen_*. Safe on NULL. */
void uft_scp_flux_gen_free(uft_scp_flux_capture_t *capture);

/** Verify a generated capture is medium-safe — every emitted ns
 *  interval is within [UFT_SCP_FLUX_GEN_MIN_NS, MAX_NS]. Returns
 *  number of out-of-spec intervals found (0 = safe). */
size_t uft_scp_flux_gen_count_unsafe(const uft_scp_flux_capture_t *capture);

#ifdef __cplusplus
}
#endif

#endif /* UFT_TESTS_SCP_FLUX_GEN_H */
