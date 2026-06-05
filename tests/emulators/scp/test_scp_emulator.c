/**
 * @file tests/emulators/scp/test_scp_emulator.c
 * @brief Firmware-realistic SCP emulator + flux-gen edge-case tests.
 *
 * This complements (does NOT replace) the byte-mock tests in
 * tests/test_scp_direct_usb_mock.c. Those tests verify wire bytes;
 * THIS file verifies firmware sequencing semantics and synthetic-flux
 * properties.
 *
 * 26 tests organised into 4 groups:
 *
 *   A. State-machine prerequisites (1-9)      — sequencing rules
 *   B. Status / SCPINFO / sticky errors (10-14) — diagnostic commands
 *   C. Read-pipeline + write-refusal (15-19)  — capture-flow
 *   D. Flux-gen determinism + defects (20-26) — synthetic patterns
 *
 * Forensic invariants asserted across the suite:
 *   - Sequencing violations transition firmware to ERROR + return
 *     PR_COMMAND_ERR (never silent no-op)
 *   - CMD_WRITE_FLUX is always REFUSED (PR_WP_ENABLED) regardless of
 *     media state — this emulator never writes
 *   - Same RNG seed produces byte-identical flux output (CI-stable)
 *   - Every emitted flux interval is within medium-safe range
 *     [UFT_SCP_FLUX_GEN_MIN_NS, MAX_NS]
 */

#include "firmware_state_machine.h"
#include "../../flux_gen/scp/flux_gen.h"
#include "uft/hal/uft_scp_direct.h"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int _pass = 0, _fail = 0, _last_fail = 0;
#define TEST(name) static void test_##name(void)
#define RUN(name)  do { printf("  [TEST] %-50s ... ", #name); fflush(stdout); test_##name(); \
                        if (_last_fail == _fail) { printf("OK\n"); _pass++; } \
                        else { printf("FAIL\n"); } fflush(stdout); \
                        _last_fail = _fail; } while (0)
#define ASSERT(c)  do { if (!(c)) { printf("FAIL @ %d: %s\n", __LINE__, #c); _fail++; return; } } while (0)

/* ─────────────────────────────────────────────────────────────────────
 *  A. State-machine prerequisites
 * ───────────────────────────────────────────────────────────────────── */

TEST(power_on_state_is_defined) {
    scp_fw_t fw;
    scp_fw_power_on_defaults(&fw);
    ASSERT(fw.state == SCP_FW_STATE_POWER_ON);
    ASSERT(fw.disk_present == true);
    ASSERT(fw.motor_on == false);
    ASSERT(fw.current_track == -1);
    ASSERT(fw.cmd_count == 0);
    scp_fw_reset(&fw);
}

TEST(happy_path_drive_select_motor_seek) {
    scp_fw_t fw;
    scp_fw_power_on_defaults(&fw);

    ASSERT(scp_fw_cmd_sela(&fw)        == SCP_FW_PR_OK);
    ASSERT(fw.state == SCP_FW_STATE_DRIVE_SELECTED);

    ASSERT(scp_fw_cmd_mtraon(&fw)      == SCP_FW_PR_OK);
    ASSERT(fw.state == SCP_FW_STATE_MOTOR_SPINNING);

    ASSERT(scp_fw_cmd_stepto(&fw, 42)  == SCP_FW_PR_OK);
    ASSERT(fw.state == SCP_FW_STATE_SEEKED);
    ASSERT(fw.current_track == 42);

    /* Side select is allowed even before READ_FLUX, after SEEKED. */
    ASSERT(scp_fw_cmd_side(&fw, 1)     == SCP_FW_PR_OK);
    ASSERT(fw.current_side == 1);

    scp_fw_reset(&fw);
}

TEST(stepto_without_motor_refused) {
    scp_fw_t fw;
    scp_fw_power_on_defaults(&fw);
    ASSERT(scp_fw_cmd_sela(&fw) == SCP_FW_PR_OK);
    /* Skip CMD_MTRAON deliberately — firmware must refuse STEPTO. */
    scp_fw_response_t r = scp_fw_cmd_stepto(&fw, 10);
    ASSERT(r == SCP_FW_PR_COMMAND_ERR);
    ASSERT(fw.state == SCP_FW_STATE_ERROR);
    ASSERT(fw.sticky_error == SCP_FW_PR_COMMAND_ERR);
    scp_fw_reset(&fw);
}

TEST(motor_without_drive_select_refused) {
    scp_fw_t fw;
    scp_fw_power_on_defaults(&fw);
    /* No CMD_SELA — fire MTRAON directly. Firmware refuses. */
    scp_fw_response_t r = scp_fw_cmd_mtraon(&fw);
    ASSERT(r == SCP_FW_PR_COMMAND_ERR);
    ASSERT(fw.state == SCP_FW_STATE_ERROR);
    scp_fw_reset(&fw);
}

TEST(read_flux_without_motor_refused) {
    scp_fw_t fw;
    scp_fw_power_on_defaults(&fw);
    ASSERT(scp_fw_cmd_sela(&fw) == SCP_FW_PR_OK);
    /* No motor on — READ_FLUX must refuse. */
    scp_fw_response_t r = scp_fw_cmd_read_flux(&fw, 2, UFT_SCP_FF_INDEX);
    ASSERT(r == SCP_FW_PR_COMMAND_ERR);
    ASSERT(fw.state == SCP_FW_STATE_ERROR);
    scp_fw_reset(&fw);
}

TEST(seek0_no_track0_reports_no_trk0) {
    scp_fw_t fw;
    scp_fw_power_on_defaults(&fw);
    scp_fw_set_track0_findable(&fw, false);
    ASSERT(scp_fw_cmd_sela(&fw)   == SCP_FW_PR_OK);
    ASSERT(scp_fw_cmd_mtraon(&fw) == SCP_FW_PR_OK);
    scp_fw_response_t r = scp_fw_cmd_seek0(&fw);
    ASSERT(r == SCP_FW_PR_NO_TRK0);
    ASSERT(fw.sticky_error == SCP_FW_PR_NO_TRK0);
    scp_fw_reset(&fw);
}

TEST(side_select_invalid_value_refused) {
    scp_fw_t fw;
    scp_fw_power_on_defaults(&fw);
    /* side=2 is out of range (only 0 and 1 valid). */
    scp_fw_response_t r = scp_fw_cmd_side(&fw, 2);
    ASSERT(r == SCP_FW_PR_COMMAND_ERR);
    scp_fw_reset(&fw);
}

TEST(deselect_returns_to_power_on) {
    scp_fw_t fw;
    scp_fw_power_on_defaults(&fw);
    ASSERT(scp_fw_cmd_sela(&fw)   == SCP_FW_PR_OK);
    ASSERT(scp_fw_cmd_mtraon(&fw) == SCP_FW_PR_OK);
    ASSERT(scp_fw_cmd_dsela(&fw)  == SCP_FW_PR_OK);
    ASSERT(fw.state == SCP_FW_STATE_POWER_ON);
    ASSERT(fw.motor_on == false);
    /* After DSELA, motor command must be refused again. */
    ASSERT(scp_fw_cmd_mtraon(&fw) == SCP_FW_PR_COMMAND_ERR);
    scp_fw_reset(&fw);
}

TEST(read_flux_no_disk_reports_no_disk) {
    scp_fw_t fw;
    scp_fw_power_on_defaults(&fw);
    scp_fw_set_disk_present(&fw, false);
    ASSERT(scp_fw_cmd_sela(&fw)   == SCP_FW_PR_OK);
    ASSERT(scp_fw_cmd_mtraon(&fw) == SCP_FW_PR_OK);
    scp_fw_response_t r = scp_fw_cmd_read_flux(&fw, 2, UFT_SCP_FF_INDEX);
    ASSERT(r == SCP_FW_PR_NO_DISK);
    ASSERT(fw.sticky_error == SCP_FW_PR_NO_DISK);
    scp_fw_reset(&fw);
}

/* ─────────────────────────────────────────────────────────────────────
 *  B. Status / SCPINFO / sticky errors
 * ───────────────────────────────────────────────────────────────────── */

TEST(status_reports_disk_present_bit) {
    scp_fw_t fw;
    scp_fw_power_on_defaults(&fw);
    uint8_t out[2] = {0};
    scp_fw_response_t r = scp_fw_cmd_status(&fw, out, sizeof(out));
    ASSERT(r == SCP_FW_PR_OK);
    /* bit 15 = disk present, bit 12 = at_track0 (default state). */
    uint16_t s = ((uint16_t)out[0] << 8) | (uint16_t)out[1];
    ASSERT((s & (1u << 15)) != 0);
    /* current_track == -1, not 0, so at_track0 bit must NOT be set. */
    ASSERT((s & (1u << 12)) == 0);
    scp_fw_reset(&fw);
}

TEST(status_reports_motor_and_wp_bits) {
    scp_fw_t fw;
    scp_fw_power_on_defaults(&fw);
    scp_fw_set_write_protected(&fw, true);
    ASSERT(scp_fw_cmd_sela(&fw)   == SCP_FW_PR_OK);
    ASSERT(scp_fw_cmd_mtraon(&fw) == SCP_FW_PR_OK);

    uint8_t out[2] = {0};
    ASSERT(scp_fw_cmd_status(&fw, out, sizeof(out)) == SCP_FW_PR_OK);
    uint16_t s = ((uint16_t)out[0] << 8) | (uint16_t)out[1];
    ASSERT((s & (1u << 14)) != 0);   /* motor on */
    ASSERT((s & (1u << 13)) != 0);   /* WP */
    scp_fw_reset(&fw);
}

TEST(scpinfo_returns_configured_versions) {
    scp_fw_t fw;
    scp_fw_power_on_defaults(&fw);
    uint8_t out[2] = {0};
    ASSERT(scp_fw_cmd_scpinfo(&fw, 0x13, 0x37, out, sizeof(out)) == SCP_FW_PR_OK);
    ASSERT(out[0] == 0x13);
    ASSERT(out[1] == 0x37);
    scp_fw_reset(&fw);
}

TEST(status_legal_from_error_state) {
    scp_fw_t fw;
    scp_fw_power_on_defaults(&fw);
    /* Force ERROR. */
    ASSERT(scp_fw_cmd_mtraon(&fw) == SCP_FW_PR_COMMAND_ERR);
    ASSERT(fw.state == SCP_FW_STATE_ERROR);

    /* STATUS should still answer — host needs to diagnose. */
    uint8_t out[2] = {0};
    ASSERT(scp_fw_cmd_status(&fw, out, sizeof(out)) == SCP_FW_PR_OK);
    scp_fw_reset(&fw);
}

TEST(sticky_error_persists_until_reset) {
    scp_fw_t fw;
    scp_fw_power_on_defaults(&fw);
    /* Provoke ERROR via STEPTO-without-motor. */
    ASSERT(scp_fw_cmd_sela(&fw)        == SCP_FW_PR_OK);
    ASSERT(scp_fw_cmd_stepto(&fw, 10)  == SCP_FW_PR_COMMAND_ERR);
    ASSERT(fw.state == SCP_FW_STATE_ERROR);
    /* Any state-mutating command still returns the sticky error. */
    ASSERT(scp_fw_cmd_mtraon(&fw)      == SCP_FW_PR_COMMAND_ERR);
    ASSERT(scp_fw_cmd_seek0(&fw)       == SCP_FW_PR_COMMAND_ERR);
    /* RESET clears it. */
    scp_fw_reset(&fw);
    ASSERT(fw.state == SCP_FW_STATE_POWER_ON);
    ASSERT(fw.sticky_error == 0);
}

/* ─────────────────────────────────────────────────────────────────────
 *  C. Read-pipeline + write-refusal
 * ───────────────────────────────────────────────────────────────────── */

/* Helper: bring fw to MOTOR_SPINNING with disk present. */
static void setup_ready_for_read(scp_fw_t *fw)
{
    scp_fw_power_on_defaults(fw);
    (void)scp_fw_cmd_sela(fw);
    (void)scp_fw_cmd_mtraon(fw);
}

TEST(get_flux_info_before_read_flux_refused) {
    scp_fw_t fw;
    setup_ready_for_read(&fw);
    uint8_t buf[40] = {0};
    scp_fw_response_t r = scp_fw_cmd_get_flux_info(&fw, buf, sizeof(buf));
    ASSERT(r == SCP_FW_PR_COMMAND_ERR);
    scp_fw_reset(&fw);
}

TEST(sendram_before_flux_info_refused) {
    scp_fw_t fw;
    setup_ready_for_read(&fw);
    /* Even if READ_FLUX succeeded, SENDRAM_USB requires the GET_FLUX_INFO
     * step first (real firmware streams the index table before RAM). */
    uint8_t flux_bytes[8] = {0};
    scp_fw_rev_meta_t rm[2] = { {1000, 1}, {1000, 0} };
    scp_fw_load_capture_ram(&fw, flux_bytes, sizeof(flux_bytes), rm, 2);
    ASSERT(scp_fw_cmd_read_flux(&fw, 2, UFT_SCP_FF_INDEX) == SCP_FW_PR_OK);
    uint8_t out[8] = {0};
    scp_fw_response_t r = scp_fw_cmd_sendram_usb(&fw, 0, 2, out, sizeof(out));
    ASSERT(r == SCP_FW_PR_COMMAND_ERR);
    scp_fw_reset(&fw);
}

TEST(write_flux_always_refused) {
    scp_fw_t fw;
    setup_ready_for_read(&fw);
    /* Even on a fully-ready, NOT-write-protected drive: REFUSED.
     * Forensic-safety: this emulator never writes. */
    ASSERT(fw.write_protected == false);
    scp_fw_response_t r = scp_fw_cmd_write_flux(&fw);
    ASSERT(r == SCP_FW_PR_WP_ENABLED);
    scp_fw_reset(&fw);
}

TEST(read_flux_revs_out_of_range_refused) {
    scp_fw_t fw;
    setup_ready_for_read(&fw);
    /* revs=1 (< MIN) */
    ASSERT(scp_fw_cmd_read_flux(&fw, 1, UFT_SCP_FF_INDEX)
           == SCP_FW_PR_COMMAND_ERR);
    scp_fw_reset(&fw);

    setup_ready_for_read(&fw);
    /* revs=6 (> MAX=5) */
    ASSERT(scp_fw_cmd_read_flux(&fw, 6, UFT_SCP_FF_INDEX)
           == SCP_FW_PR_COMMAND_ERR);
    scp_fw_reset(&fw);
}

TEST(sendram_out_of_order_refused) {
    scp_fw_t fw;
    setup_ready_for_read(&fw);

    /* Load a 4-byte (2-sample) capture for 1 rev with count=2. */
    uint8_t flux_bytes[4] = { 0x00, 0x10, 0x00, 0x20 };
    scp_fw_rev_meta_t rm[2] = { {1000, 2}, {1000, 0} };
    scp_fw_load_capture_ram(&fw, flux_bytes, sizeof(flux_bytes), rm, 2);

    ASSERT(scp_fw_cmd_read_flux(&fw, 2, UFT_SCP_FF_INDEX) == SCP_FW_PR_OK);
    uint8_t info[40] = {0};
    ASSERT(scp_fw_cmd_get_flux_info(&fw, info, sizeof(info)) == SCP_FW_PR_OK);

    /* Skip rev 0 (offset 0..4) and ask for offset=8 — out of order. */
    uint8_t out[8] = {0};
    scp_fw_response_t r = scp_fw_cmd_sendram_usb(&fw, 8, 4, out, sizeof(out));
    ASSERT(r == SCP_FW_PR_COMMAND_ERR);
    scp_fw_reset(&fw);
}

/* ─────────────────────────────────────────────────────────────────────
 *  D. Flux-gen determinism + defects
 * ───────────────────────────────────────────────────────────────────── */

TEST(flux_gen_clean_is_deterministic) {
    uft_scp_flux_params_t p = {
        .seed                  = 0xCAFEBABEull,
        .revolutions           = 2,
        .index_period_ns       = 200000000u,  /* 200 ms */
        .transitions_per_rev   = 100,
        .defects               = UFT_DEFECT_NONE,
        .weak_jitter_pct       = 0,
    };
    uft_scp_flux_capture_t cap1 = {0}, cap2 = {0};
    ASSERT(uft_scp_flux_gen_clean(&p, &cap1) == UFT_FLUX_GEN_OK);
    ASSERT(uft_scp_flux_gen_clean(&p, &cap2) == UFT_FLUX_GEN_OK);
    ASSERT(cap1.bytes_len == cap2.bytes_len);
    ASSERT(cap1.bytes_len > 0);
    ASSERT(memcmp(cap1.bytes, cap2.bytes, cap1.bytes_len) == 0);
    ASSERT(cap1.rev_count == 2);
    ASSERT(cap1.rev_flux_count[0] == 100);
    ASSERT(cap1.rev_flux_count[1] == 100);
    uft_scp_flux_gen_free(&cap1);
    uft_scp_flux_gen_free(&cap2);
}

TEST(flux_gen_different_seed_yields_different_bytes) {
    uft_scp_flux_params_t p1 = {
        .seed = 1, .revolutions = 2, .index_period_ns = 200000000u,
        .transitions_per_rev = 100, .defects = 0, .weak_jitter_pct = 0,
    };
    uft_scp_flux_params_t p2 = p1;
    p2.seed = 2;
    uft_scp_flux_capture_t c1 = {0}, c2 = {0};
    ASSERT(uft_scp_flux_gen_clean(&p1, &c1) == UFT_FLUX_GEN_OK);
    ASSERT(uft_scp_flux_gen_clean(&p2, &c2) == UFT_FLUX_GEN_OK);
    ASSERT(c1.bytes_len == c2.bytes_len);
    /* Should differ in MOST bytes (random distribution). */
    ASSERT(memcmp(c1.bytes, c2.bytes, c1.bytes_len) != 0);
    uft_scp_flux_gen_free(&c1);
    uft_scp_flux_gen_free(&c2);
}

TEST(flux_gen_rejects_out_of_spec_index_period) {
    uft_scp_flux_params_t p = {
        .seed = 1, .revolutions = 2,
        .index_period_ns = 1000u,   /* 1 µs — absurdly short */
        .transitions_per_rev = 100, .defects = 0, .weak_jitter_pct = 0,
    };
    uft_scp_flux_capture_t c = {0};
    ASSERT(uft_scp_flux_gen_clean(&p, &c) == UFT_FLUX_GEN_ERR_OUT_OF_SPEC);
    /* bytes must NOT have been allocated. */
    ASSERT(c.bytes == NULL);
}

TEST(flux_gen_medium_safety_holds_for_clean) {
    uft_scp_flux_params_t p = {
        .seed = 42, .revolutions = 3, .index_period_ns = 200000000u,
        .transitions_per_rev = 500, .defects = 0, .weak_jitter_pct = 0,
    };
    uft_scp_flux_capture_t cap = {0};
    ASSERT(uft_scp_flux_gen_clean(&p, &cap) == UFT_FLUX_GEN_OK);
    /* Every interval must be within medium-safe range. */
    size_t unsafe = uft_scp_flux_gen_count_unsafe(&cap);
    ASSERT(unsafe == 0);
    uft_scp_flux_gen_free(&cap);
}

TEST(flux_gen_weak_bits_jitter_changes_output) {
    uft_scp_flux_params_t p_clean = {
        .seed = 7, .revolutions = 2, .index_period_ns = 200000000u,
        .transitions_per_rev = 100, .defects = 0, .weak_jitter_pct = 0,
    };
    uft_scp_flux_params_t p_weak = p_clean;
    p_weak.defects         = UFT_DEFECT_WEAK_BITS;
    p_weak.weak_jitter_pct = 20;

    uft_scp_flux_capture_t c_clean = {0}, c_weak = {0};
    ASSERT(uft_scp_flux_gen_clean(&p_clean, &c_clean) == UFT_FLUX_GEN_OK);
    ASSERT(uft_scp_flux_gen_clean(&p_weak,  &c_weak)  == UFT_FLUX_GEN_OK);
    ASSERT(c_clean.bytes_len == c_weak.bytes_len);
    /* Weak-bit version must differ. */
    ASSERT(memcmp(c_clean.bytes, c_weak.bytes, c_clean.bytes_len) != 0);
    /* But still medium-safe. */
    ASSERT(uft_scp_flux_gen_count_unsafe(&c_weak) == 0);
    uft_scp_flux_gen_free(&c_clean);
    uft_scp_flux_gen_free(&c_weak);
}

TEST(flux_gen_crc_error_corrupts_exactly_one_byte) {
    uft_scp_flux_params_t p_clean = {
        .seed = 11, .revolutions = 2, .index_period_ns = 200000000u,
        .transitions_per_rev = 100, .defects = 0, .weak_jitter_pct = 0,
    };
    uft_scp_flux_params_t p_crc = p_clean;
    p_crc.defects = UFT_DEFECT_CRC_ERROR;

    uft_scp_flux_capture_t c_clean = {0}, c_crc = {0};
    ASSERT(uft_scp_flux_gen_clean(&p_clean, &c_clean) == UFT_FLUX_GEN_OK);
    ASSERT(uft_scp_flux_gen_clean(&p_crc,   &c_crc)   == UFT_FLUX_GEN_OK);
    ASSERT(c_clean.bytes_len == c_crc.bytes_len);
    /* Count differing bytes — must be exactly 1 (the CRC defect's flip). */
    size_t diff = 0;
    for (size_t i = 0; i < c_clean.bytes_len; i++) {
        if (c_clean.bytes[i] != c_crc.bytes[i]) diff++;
    }
    ASSERT(diff == 1);
    uft_scp_flux_gen_free(&c_clean);
    uft_scp_flux_gen_free(&c_crc);
}

TEST(flux_gen_vmax_signature_is_uniform) {
    uft_scp_flux_params_t p = {
        .seed = 0xDEADBEEF, .revolutions = 2, .index_period_ns = 200000000u,
        .transitions_per_rev = 1000, .defects = UFT_DEFECT_VMAX_SIG,
        .weak_jitter_pct = 0,
    };
    uft_scp_flux_capture_t cap = {0};
    ASSERT(uft_scp_flux_gen_vmax(&p, &cap) == UFT_FLUX_GEN_OK);
    ASSERT(cap.bytes_len > 0);

    /* Every 16-bit BE sample should be the V-MAX! signature value
     * (3 cells × 4 µs = 12 µs = 480 ticks at 25 ns/tick). */
    ASSERT(cap.bytes_len % 2 == 0);
    for (size_t i = 0; i < cap.bytes_len; i += 2) {
        uint16_t s = ((uint16_t)cap.bytes[i] << 8) |
                      (uint16_t)cap.bytes[i + 1];
        ASSERT(s == 480u);
    }
    ASSERT(uft_scp_flux_gen_count_unsafe(&cap) == 0);
    uft_scp_flux_gen_free(&cap);
}

/* ─────────────────────────────────────────────────────────────────────
 *  E. End-to-end: flux-gen output round-trips through the firmware
 *     emulator's CMD_SENDRAM_USB and decodes to the expected ns values.
 *
 *     This proves the flux-gen byte format and the emulator's
 *     decode-side agree — important because the production C-HAL
 *     uses the SAME decode (samdisk-equivalent loop).
 * ───────────────────────────────────────────────────────────────────── */

TEST(roundtrip_flux_gen_via_emulator_sendram) {
    /* Generate a tiny clean capture. */
    uft_scp_flux_params_t p = {
        .seed = 99, .revolutions = 2, .index_period_ns = 200000000u,
        .transitions_per_rev = 20, .defects = 0, .weak_jitter_pct = 0,
    };
    uft_scp_flux_capture_t cap = {0};
    ASSERT(uft_scp_flux_gen_clean(&p, &cap) == UFT_FLUX_GEN_OK);

    /* Wire it into the emulator. */
    scp_fw_t fw;
    scp_fw_power_on_defaults(&fw);
    scp_fw_rev_meta_t rm[2] = {
        { cap.rev_index_ticks[0], cap.rev_flux_count[0] },
        { cap.rev_index_ticks[1], cap.rev_flux_count[1] },
    };
    scp_fw_load_capture_ram(&fw, cap.bytes, cap.bytes_len, rm, 2);

    /* Drive the firmware through the read pipeline. */
    ASSERT(scp_fw_cmd_sela(&fw)        == SCP_FW_PR_OK);
    ASSERT(scp_fw_cmd_mtraon(&fw)      == SCP_FW_PR_OK);
    ASSERT(scp_fw_cmd_stepto(&fw, 0)   == SCP_FW_PR_OK);
    ASSERT(scp_fw_cmd_side(&fw, 0)     == SCP_FW_PR_OK);
    ASSERT(scp_fw_cmd_read_flux(&fw, 2, UFT_SCP_FF_INDEX) == SCP_FW_PR_OK);

    uint8_t info[40] = {0};
    ASSERT(scp_fw_cmd_get_flux_info(&fw, info, sizeof(info)) == SCP_FW_PR_OK);
    /* Verify rev_index encoding. */
    uint32_t rev0_count = ((uint32_t)info[4] << 24) | ((uint32_t)info[5] << 16) |
                          ((uint32_t)info[6] << 8) | (uint32_t)info[7];
    ASSERT(rev0_count == cap.rev_flux_count[0]);

    /* Dump rev 0 + rev 1 via SENDRAM_USB. */
    uint8_t out_rev0[256] = {0};
    uint32_t rev0_bytes = cap.rev_flux_count[0] * 2u;
    ASSERT(scp_fw_cmd_sendram_usb(&fw, 0, rev0_bytes,
                                    out_rev0, sizeof(out_rev0)) == SCP_FW_PR_OK);
    /* Bytes streamed should match generator's output for rev 0. */
    ASSERT(memcmp(out_rev0, cap.bytes, rev0_bytes) == 0);

    uint8_t out_rev1[256] = {0};
    uint32_t rev1_bytes = cap.rev_flux_count[1] * 2u;
    ASSERT(scp_fw_cmd_sendram_usb(&fw, rev0_bytes, rev1_bytes,
                                    out_rev1, sizeof(out_rev1)) == SCP_FW_PR_OK);
    ASSERT(memcmp(out_rev1, cap.bytes + rev0_bytes, rev1_bytes) == 0);

    scp_fw_reset(&fw);
    uft_scp_flux_gen_free(&cap);
}

int main(void)
{
    printf("=== SCP Firmware-Realistic Emulator + Flux-Gen Tests ===\n");
    fflush(stdout);
    printf("\n-- A. State-machine prerequisites --\n");
    RUN(power_on_state_is_defined);
    RUN(happy_path_drive_select_motor_seek);
    RUN(stepto_without_motor_refused);
    RUN(motor_without_drive_select_refused);
    RUN(read_flux_without_motor_refused);
    RUN(seek0_no_track0_reports_no_trk0);
    RUN(side_select_invalid_value_refused);
    RUN(deselect_returns_to_power_on);
    RUN(read_flux_no_disk_reports_no_disk);

    printf("\n-- B. Status / SCPINFO / sticky errors --\n");
    RUN(status_reports_disk_present_bit);
    RUN(status_reports_motor_and_wp_bits);
    RUN(scpinfo_returns_configured_versions);
    RUN(status_legal_from_error_state);
    RUN(sticky_error_persists_until_reset);

    printf("\n-- C. Read-pipeline + write-refusal --\n");
    RUN(get_flux_info_before_read_flux_refused);
    RUN(sendram_before_flux_info_refused);
    RUN(write_flux_always_refused);
    RUN(read_flux_revs_out_of_range_refused);
    RUN(sendram_out_of_order_refused);

    printf("\n-- D. Flux-gen determinism + defects --\n");
    RUN(flux_gen_clean_is_deterministic);
    RUN(flux_gen_different_seed_yields_different_bytes);
    RUN(flux_gen_rejects_out_of_spec_index_period);
    RUN(flux_gen_medium_safety_holds_for_clean);
    RUN(flux_gen_weak_bits_jitter_changes_output);
    RUN(flux_gen_crc_error_corrupts_exactly_one_byte);
    RUN(flux_gen_vmax_signature_is_uniform);

    printf("\n-- E. End-to-end emulator + flux-gen roundtrip --\n");
    RUN(roundtrip_flux_gen_via_emulator_sendram);

    printf("\nResults: %d passed, %d failed\n", _pass, _fail);
    return _fail ? 1 : 0;
}
