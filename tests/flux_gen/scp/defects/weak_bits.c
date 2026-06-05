/**
 * @file tests/flux_gen/scp/defects/weak_bits.c
 * @brief UFT_DEFECT_WEAK_BITS injection.
 *
 * Weak bits are flux transitions whose timing varies between reads —
 * the magnetic flux is too weak for the head to consistently detect
 * the edge, so the PLL sees a different position each pass. In real
 * hardware this manifests as small (10-30%) jitter around the nominal
 * cell time.
 *
 * Forensic invariant: the jitter is deterministic per RNG seed, so
 * "the weak bits" are the same on every CI run, but DIFFERENT from
 * the clean-MFM case. A test can assert that the same seed produces
 * the same out-of-spec pattern, and a different seed produces a
 * different one.
 */

#include "../flux_gen.h"

#include <stdint.h>

/* Same xorshift64* implementation as flux_gen.c. We don't share state
 * because the rng_t type is internal to the generator. We declare an
 * incomplete-ish type so we can take a pointer; the actual struct
 * shape is identical (a single uint64_t). */
typedef struct { uint64_t s; } rng_t;

static uint64_t rng_next_local(rng_t *r)
{
    uint64_t x = r->s;
    x ^= x >> 12;
    x ^= x << 25;
    x ^= x >> 27;
    r->s = x;
    return x * 0x2545F4914F6CDD1Dull;
}

uft_flux_gen_err_t uft_defect_weak_bits_apply(
    rng_t *rng, uint8_t weak_jitter_pct,
    uint32_t *cell_ns_inout)
{
    if (!rng || !cell_ns_inout) return UFT_FLUX_GEN_ERR_NULL;
    if (weak_jitter_pct > 50) return UFT_FLUX_GEN_ERR_INVALID;
    if (weak_jitter_pct == 0) return UFT_FLUX_GEN_OK;

    uint32_t cell = *cell_ns_inout;
    uint64_t r = rng_next_local(rng);

    /* Map random to [-jitter_pct, +jitter_pct] of cell. */
    int32_t jitter_max = (int32_t)((uint64_t)cell *
                                    (uint64_t)weak_jitter_pct / 100u);
    int32_t jitter     = (int32_t)((r % (uint64_t)(jitter_max * 2 + 1))) -
                          jitter_max;

    int64_t new_cell = (int64_t)cell + jitter;
    if (new_cell < (int64_t)UFT_SCP_FLUX_GEN_MIN_NS) {
        new_cell = (int64_t)UFT_SCP_FLUX_GEN_MIN_NS;
    }
    *cell_ns_inout = (uint32_t)new_cell;
    return UFT_FLUX_GEN_OK;
}
