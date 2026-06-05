/**
 * @file tests/flux_gen/scp/flux_gen.c
 * @brief Synthetic flux generator core for the SCP capture-RAM format.
 *
 * All defect-specific code lives under defects/ and is invoked from
 * the generator entry points. The base generator emits clean MFM
 * (3.5"/5.25" DD, 250 kbps) and the defect modules mutate or extend
 * the stream.
 */

#include "flux_gen.h"
#include "../../../include/uft/hal/uft_scp_direct.h"

#include <stdint.h>
#include <stdlib.h>
#include <string.h>

/* ─── Deterministic RNG (xorshift64*) ────────────────────────────────
 *
 * We need byte-identical output across platforms, so we cannot use the
 * platform PRNG. xorshift64* is well-tested, public-domain, and produces
 * the same sequence on x86-64, ARM, and STM32. Seed = 0 is disallowed
 * (xorshift64* property) — we map seed=0 to seed=1.
 */
typedef struct { uint64_t s; } rng_t;

static uint64_t rng_next(rng_t *r)
{
    uint64_t x = r->s;
    x ^= x >> 12;
    x ^= x << 25;
    x ^= x >> 27;
    r->s = x;
    return x * 0x2545F4914F6CDD1Dull;
}

static void rng_seed(rng_t *r, uint64_t s)
{
    r->s = (s == 0) ? 1 : s;
}

/* ─── Helpers ───────────────────────────────────────────────────────── */

/** Encode one ns interval as one or more 16-bit BE samples per SCP
 *  format. Returns number of bytes appended. */
static size_t encode_interval(uint8_t *out, size_t out_cap, uint32_t ns)
{
    /* Convert to ticks (25 ns per tick). */
    uint32_t ticks = ns / UFT_SCP_FLUX_NS_PER_SAMPLE;
    size_t written = 0;
    while (ticks > 0xFFFFu) {
        if (written + 2 > out_cap) return written;
        out[written++] = 0x00;   /* 0x0000 overflow marker */
        out[written++] = 0x00;
        ticks -= 0x10000u;
    }
    if (ticks == 0) {
        /* Encoded entirely as overflow markers. The trailing non-zero
         * is required for the transition to actually be emitted —
         * append a tiny epsilon (1 tick = 25 ns). NOTE: this is below
         * UFT_SCP_FLUX_GEN_MIN_NS, but it's not a real transition
         * timing — it's the leftover after a precise multiple of
         * 0x10000 ticks. Such intervals are vanishingly rare in real
         * flux but cleanly representable. */
        if (written + 2 > out_cap) return written;
        out[written++] = 0x00;
        out[written++] = 0x01;
        return written;
    }
    if (written + 2 > out_cap) return written;
    out[written++] = (uint8_t)(ticks >> 8);
    out[written++] = (uint8_t)(ticks & 0xFF);
    return written;
}

/** Count flux *samples* (16-bit words) in `bytes_len` bytes. */
static uint32_t sample_count(size_t bytes_len) { return (uint32_t)(bytes_len / 2); }

/* ─── Common generator core ────────────────────────────────────────── */

static uft_flux_gen_err_t validate_params(const uft_scp_flux_params_t *p)
{
    if (!p) return UFT_FLUX_GEN_ERR_NULL;
    if (p->revolutions < UFT_SCP_MIN_REVOLUTIONS ||
        p->revolutions > UFT_SCP_MAX_REVOLUTIONS) {
        return UFT_FLUX_GEN_ERR_INVALID;
    }
    if (p->transitions_per_rev < 10 || p->transitions_per_rev > 200000) {
        return UFT_FLUX_GEN_ERR_INVALID;
    }
    if (p->weak_jitter_pct > 50) return UFT_FLUX_GEN_ERR_INVALID;
    if (p->index_period_ns < 50000000u || p->index_period_ns > 500000000u) {
        /* 50 ms..500 ms — covers 120..600 RPM, all real drives. */
        return UFT_FLUX_GEN_ERR_OUT_OF_SPEC;
    }
    return UFT_FLUX_GEN_OK;
}

/* Defect hook prototypes (defined in defects/ subdir .c files). */
extern uft_flux_gen_err_t uft_defect_weak_bits_apply(
    rng_t *rng, uint8_t weak_jitter_pct,
    uint32_t *cell_ns_inout);
extern uft_flux_gen_err_t uft_defect_crc_error_apply(
    uft_scp_flux_capture_t *cap, size_t target_rev);
extern uft_flux_gen_err_t uft_defect_vmax_emit(
    uint8_t *out, size_t out_cap, size_t *out_written,
    uint32_t transitions);

/* ─── Clean MFM generator ──────────────────────────────────────────── */

uft_flux_gen_err_t uft_scp_flux_gen_clean(
    const uft_scp_flux_params_t *params,
    uft_scp_flux_capture_t      *out_capture)
{
    if (!out_capture) return UFT_FLUX_GEN_ERR_NULL;
    memset(out_capture, 0, sizeof(*out_capture));

    uft_flux_gen_err_t v = validate_params(params);
    if (v != UFT_FLUX_GEN_OK) return v;

    rng_t rng;
    rng_seed(&rng, params->seed);

    /* Capacity: each transition is ~2 bytes (most are < 0x10000
     * ticks at DD). Reserve 4 bytes/transition for safety. */
    size_t cap_bytes = (size_t)params->transitions_per_rev *
                       (size_t)params->revolutions * 4u;
    uint8_t *buf = (uint8_t *)calloc(1, cap_bytes);
    if (!buf) return UFT_FLUX_GEN_ERR_NULL;

    size_t written_total = 0;

    /* index period in ticks: index_period_ns / 25 */
    uint32_t index_ticks = params->index_period_ns / UFT_SCP_FLUX_NS_PER_SAMPLE;

    for (int rev = 0; rev < params->revolutions; rev++) {
        size_t rev_start = written_total;

        for (uint32_t t = 0; t < params->transitions_per_rev; t++) {
            /* Pick a cell count: MFM uses 2/3/4-cell intervals
             * (with the usual bias toward shorter cells). */
            uint64_t r = rng_next(&rng);
            uint8_t cells = (uint8_t)((r % 3) + 2);   /* 2..4 */
            uint32_t cell_ns = UFT_SCP_FLUX_GEN_CELL_NS_DD * cells;

            /* Apply WEAK_BITS jitter if requested. */
            if (params->defects & UFT_DEFECT_WEAK_BITS) {
                uft_defect_weak_bits_apply(&rng,
                                            params->weak_jitter_pct,
                                            &cell_ns);
            }

            /* Forensic-medium-safety guard. */
            if (cell_ns < UFT_SCP_FLUX_GEN_MIN_NS ||
                cell_ns > UFT_SCP_FLUX_GEN_MAX_NS) {
                free(buf);
                return UFT_FLUX_GEN_ERR_OUT_OF_SPEC;
            }

            size_t n = encode_interval(buf + written_total,
                                        cap_bytes - written_total,
                                        cell_ns);
            if (n == 0) {
                free(buf);
                return UFT_FLUX_GEN_ERR_BUF_SMALL;
            }
            written_total += n;
        }

        out_capture->rev_index_ticks[rev] = index_ticks;
        out_capture->rev_flux_count[rev]  = sample_count(written_total - rev_start);
    }

    /* Apply CRC_ERROR defect if requested — flips a bit in rev 0's
     * stream, so a real MFM decoder fails the CRC check. */
    out_capture->bytes      = buf;
    out_capture->bytes_len  = written_total;
    out_capture->rev_count  = params->revolutions;

    if (params->defects & UFT_DEFECT_CRC_ERROR) {
        uft_defect_crc_error_apply(out_capture, 0);
    }

    return UFT_FLUX_GEN_OK;
}

/* ─── V-MAX! signature generator ───────────────────────────────────── */

uft_flux_gen_err_t uft_scp_flux_gen_vmax(
    const uft_scp_flux_params_t *params,
    uft_scp_flux_capture_t      *out_capture)
{
    if (!out_capture) return UFT_FLUX_GEN_ERR_NULL;
    memset(out_capture, 0, sizeof(*out_capture));

    uft_flux_gen_err_t v = validate_params(params);
    if (v != UFT_FLUX_GEN_OK) return v;

    size_t cap_bytes = (size_t)params->transitions_per_rev *
                       (size_t)params->revolutions * 4u;
    uint8_t *buf = (uint8_t *)calloc(1, cap_bytes);
    if (!buf) return UFT_FLUX_GEN_ERR_NULL;

    size_t written_total = 0;
    uint32_t index_ticks = params->index_period_ns / UFT_SCP_FLUX_NS_PER_SAMPLE;

    for (int rev = 0; rev < params->revolutions; rev++) {
        size_t rev_start = written_total;
        size_t this_rev_written = 0;
        uft_flux_gen_err_t e = uft_defect_vmax_emit(
            buf + written_total,
            cap_bytes - written_total,
            &this_rev_written,
            params->transitions_per_rev);
        if (e != UFT_FLUX_GEN_OK) {
            free(buf);
            return e;
        }
        written_total += this_rev_written;
        out_capture->rev_index_ticks[rev] = index_ticks;
        out_capture->rev_flux_count[rev]  = sample_count(written_total - rev_start);
    }
    out_capture->bytes      = buf;
    out_capture->bytes_len  = written_total;
    out_capture->rev_count  = params->revolutions;
    (void)params->defects;  /* V-MAX! is the defect; flags ignored here */
    return UFT_FLUX_GEN_OK;
}

void uft_scp_flux_gen_free(uft_scp_flux_capture_t *capture)
{
    if (!capture) return;
    if (capture->bytes) free(capture->bytes);
    capture->bytes     = NULL;
    capture->bytes_len = 0;
    capture->rev_count = 0;
}

size_t uft_scp_flux_gen_count_unsafe(const uft_scp_flux_capture_t *capture)
{
    if (!capture || !capture->bytes) return 0;
    size_t unsafe_count = 0;
    uint32_t accum_ticks = 0;
    size_t i = 0;
    /* Walk samples per-rev so end-of-rev accumulator drops match SCP
     * decode behaviour. */
    for (int rev = 0; rev < capture->rev_count; rev++) {
        uint32_t samples_this_rev = capture->rev_flux_count[rev];
        accum_ticks = 0;
        for (uint32_t s = 0; s < samples_this_rev; s++, i += 2) {
            if (i + 1 >= capture->bytes_len) break;
            uint16_t w = ((uint16_t)capture->bytes[i] << 8) |
                          (uint16_t)capture->bytes[i + 1];
            if (w == 0) {
                accum_ticks += 0x10000u;
            } else {
                uint32_t ns = (accum_ticks + (uint32_t)w) *
                              (uint32_t)UFT_SCP_FLUX_NS_PER_SAMPLE;
                if (ns < UFT_SCP_FLUX_GEN_MIN_NS ||
                    ns > UFT_SCP_FLUX_GEN_MAX_NS) {
                    unsafe_count++;
                }
                accum_ticks = 0;
            }
        }
    }
    return unsafe_count;
}
