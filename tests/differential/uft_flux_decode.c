/*
 * uft_flux_decode.c — UFT-side decoder for the P3.2 differential
 * conformance harness (task #109).
 *
 * NOT a product CLI. This is a TEST FIXTURE: it links the UFT flux
 * decode engine directly and is invoked by tests/differential/
 * test_gw_parity.py. UFT itself is GUI-only — this helper is the
 * bridge between the Python differential orchestrator and the C
 * engine (see memory feedback_no_cli: tests target the core library).
 *
 *   usage: uft_flux_decode <in.scp> <encoding> <out.img>
 *                          <heads> <spt> <secsize> <first_sec> [bitcell_ns]
 *
 *   encoding   : mfm   (the only encoding wired so far — see below)
 *   heads      : 1 or 2
 *   spt        : sectors per track (uniform geometry only)
 *   secsize    : bytes per sector (e.g. 512)
 *   first_sec  : first sector number on the disk (IBM/AtariST = 1)
 *   bitcell_ns : MFM bit-cell time; 0 = decoder default (DD). HD ~1000.
 *
 * Pipeline: SCP file -> uft_scp_parser (flux intervals, ns) ->
 * flux_raw_data_t (cumulative ticks @ 1 GHz, so 1 tick = 1 ns) ->
 * flux_decode_mfm() -> decoded sectors -> flat sector image written in
 * the standard CHS layout gw also uses:
 *   offset = ((cyl*heads + head)*spt + (sec - first_sec)) * secsize
 *
 * SCOPE (honest): only the uniform-geometry IBM-style MFM family
 * (IBM-DD, IBM-HD, AtariST-DD) is wired. C64-GCR (zone geometry —
 * non-uniform spt), Apple2-GCR and Amiga-MFM each need their own
 * image-assembly path; flux_decode_gcr_c64/_apple exist in the engine
 * but the layout reconstruction is follow-up work. For any non-"mfm"
 * encoding this helper exits 3 (ENC_NOT_WIRED) so the differential
 * records an honest per-format gap rather than a false divergence.
 *
 * Exit codes: 0 ok | 1 usage | 2 scp/decode error | 3 encoding not wired
 */
#include "uft/flux/uft_scp_parser.h"
#include "uft/flux/uft_flux_decoder.h"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define EXIT_OK            0
#define EXIT_USAGE         1
#define EXIT_SCP_ERROR     2
#define EXIT_ENC_NOT_WIRED 3

int main(int argc, char **argv)
{
    if (argc < 8 || argc > 9) {
        fprintf(stderr,
            "usage: %s <in.scp> <encoding> <out.img> <heads> <spt> "
            "<secsize> <first_sec> [bitcell_ns]\n", argv[0]);
        return EXIT_USAGE;
    }
    const char  *scp_path = argv[1];
    const char  *encoding = argv[2];
    const char  *out_path = argv[3];
    const int    heads    = atoi(argv[4]);
    const int    spt      = atoi(argv[5]);
    const int    secsize  = atoi(argv[6]);
    const int    first_sec= atoi(argv[7]);
    const uint32_t bitcell_ns = (argc == 9) ? (uint32_t)strtoul(argv[8], NULL, 10) : 0u;

    if (strcmp(encoding, "mfm") != 0) {
        /* C64-GCR / Apple2-GCR / Amiga-MFM: the engine has decoders
         * (flux_decode_gcr_c64/_apple) but the image-layout
         * reconstruction is not wired here yet. Honest gap, not a lie. */
        fprintf(stderr, "uft_flux_decode: encoding '%s' not wired "
                "(only 'mfm' so far) — see file header\n", encoding);
        return EXIT_ENC_NOT_WIRED;
    }
    if (heads < 1 || heads > 2 || spt < 1 || secsize < 1 || first_sec < 0) {
        fprintf(stderr, "uft_flux_decode: bad geometry args\n");
        return EXIT_USAGE;
    }

    uft_scp_ctx_t *scp = uft_scp_create();
    if (!scp) {
        fprintf(stderr, "uft_flux_decode: uft_scp_create failed\n");
        return EXIT_SCP_ERROR;
    }
    if (uft_scp_open(scp, scp_path) != UFT_SCP_OK) {
        fprintf(stderr, "uft_flux_decode: cannot open SCP '%s'\n", scp_path);
        uft_scp_destroy(scp);
        return EXIT_SCP_ERROR;
    }

    /* Output image: a track of `spt` sectors, both heads, as many
     * cylinders as the SCP holds. Sized generously, trimmed on write. */
    size_t max_cyl    = UFT_SCP_MAX_TRACKS;  /* SCP track index upper bound */
    size_t image_size = max_cyl * (size_t)heads * (size_t)spt * (size_t)secsize;
    uint8_t *image = (uint8_t *)calloc(image_size, 1);
    if (!image) {
        fprintf(stderr, "uft_flux_decode: out of memory\n");
        uft_scp_destroy(scp);
        return EXIT_SCP_ERROR;
    }

    int    placed_sectors = 0;
    size_t highest_offset = 0;   /* track the real extent for trimming */

    for (int t = 0; t < UFT_SCP_MAX_TRACKS; ++t) {
        if (!uft_scp_has_track(scp, t)) continue;

        uft_scp_track_data_t td;
        memset(&td, 0, sizeof(td));
        if (uft_scp_read_track(scp, t, &td) != UFT_SCP_OK) continue;
        if (td.revolution_count == 0 || td.revolutions[0].flux_count == 0) {
            uft_scp_free_track(&td);
            continue;
        }

        /* SCP flux_data is ns intervals (uft_scp_parser converts cells
         * -> ns, with 0 entries as overflow placeholders). flux_decode_*
         * wants cumulative transition TIMES in sample ticks. Use a
         * 1 GHz sample rate so 1 tick == 1 ns, and skip the 0
         * placeholders (they are not real transitions). */
        const uft_scp_rev_data_t *rev = &td.revolutions[0];
        uint32_t *trans = (uint32_t *)malloc(rev->flux_count * sizeof(uint32_t));
        if (!trans) { uft_scp_free_track(&td); continue; }
        size_t   n = 0;
        uint64_t cum = 0;
        for (uint32_t i = 0; i < rev->flux_count; ++i) {
            uint32_t iv = rev->flux_data ? rev->flux_data[i] : 0u;
            if (iv == 0) continue;          /* SCP overflow placeholder */
            cum += iv;
            trans[n++] = (uint32_t)cum;
        }

        flux_raw_data_t flux;
        memset(&flux, 0, sizeof(flux));
        flux.transitions      = trans;
        flux.transition_count = n;
        flux.sample_rate      = 1000000000u;   /* 1 GHz -> 1 tick = 1 ns */

        flux_decoder_options_t opts;
        flux_decoder_options_init(&opts);
        opts.encoding   = FLUX_ENC_MFM;
        opts.bitcell_ns = bitcell_ns;          /* 0 -> decoder DD default */
        opts.use_pll    = true;

        flux_decoded_track_t dt;
        flux_decoded_track_init(&dt);
        flux_status_t st = flux_decode_mfm(&flux, &dt, &opts);

        if (st == FLUX_OK) {
            for (size_t s = 0; s < dt.sector_count; ++s) {
                const flux_decoded_sector_t *sec = &dt.sectors[s];
                if (!sec->data || sec->data_size == 0) continue;
                int rel = (int)sec->sector - first_sec;
                if (sec->head >= heads || rel < 0 || rel >= spt) continue;
                size_t off = (((size_t)sec->cylinder * heads + sec->head)
                              * spt + (size_t)rel) * (size_t)secsize;
                size_t len = sec->data_size < (size_t)secsize
                                 ? sec->data_size : (size_t)secsize;
                if (off + (size_t)secsize > image_size) continue;
                memcpy(image + off, sec->data, len);
                if (off + (size_t)secsize > highest_offset)
                    highest_offset = off + (size_t)secsize;
                placed_sectors++;
            }
        }

        flux_decoded_track_free(&dt);
        free(trans);
        uft_scp_free_track(&td);
    }

    uft_scp_destroy(scp);

    if (placed_sectors == 0) {
        fprintf(stderr, "uft_flux_decode: decoded 0 sectors from '%s'\n",
                scp_path);
        free(image);
        return EXIT_SCP_ERROR;
    }

    FILE *out = fopen(out_path, "wb");
    if (!out) {
        fprintf(stderr, "uft_flux_decode: cannot write '%s'\n", out_path);
        free(image);
        return EXIT_SCP_ERROR;
    }
    size_t wrote = fwrite(image, 1, highest_offset, out);
    fclose(out);
    free(image);

    if (wrote != highest_offset) {
        fprintf(stderr, "uft_flux_decode: short write to '%s'\n", out_path);
        return EXIT_SCP_ERROR;
    }

    fprintf(stderr, "uft_flux_decode: %d sectors -> %zu bytes -> %s\n",
            placed_sectors, highest_offset, out_path);
    return EXIT_OK;
}
