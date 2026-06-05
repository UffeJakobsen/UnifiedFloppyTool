/**
 * @file tests/emulators/scp/firmware_state_machine.h
 * @brief Firmware-realistic state machine for the SuperCard Pro controller.
 *
 * This header models the documented prerequisite-order of SCP firmware
 * commands so the byte-level mock (tests/usb_mock/scp_direct_mocked.c)
 * can be extended with "did the host follow the sequencing rules a real
 * SCP would enforce?" semantics.
 *
 * Reference: simonowen/samdisk SuperCardPro.{h,cpp}
 *   - SelectDrive() precedes any read/write command
 *   - EnableMotor() precedes any seek/read/write
 *   - SeekTrack() (CMD_STEPTO) requires motor on
 *   - Side select (CMD_SIDE) is independent of the read pipeline
 *   - CMD_READFLUX -> CMD_GETFLUXINFO -> per-rev CMD_SENDRAM_USB
 *     is the documented capture pipeline
 *   - CMD_WRITEFLUX exists but is REFUSED by this emulator (forensic-
 *     safety guard; never emulated, matches UFT HAL policy)
 *
 * SPEC_STATUS: VENDOR-DOCUMENTED via samdisk reference. The state-
 * sequencing rules below are inferred from samdisk's command sequence
 * (not from a vendor state diagram, which is not publicly published).
 * Divergences are listed in DIVERGENCES.md.
 *
 * Forensic invariant: this state machine NEVER produces flux bytes
 * itself. Flux comes from the synthetic generator
 * (tests/flux_gen/scp/flux_gen.c) which the test wires in.
 */
#ifndef UFT_TESTS_SCP_FIRMWARE_STATE_MACHINE_H
#define UFT_TESTS_SCP_FIRMWARE_STATE_MACHINE_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ─── State enum (explicit numeric values for ABI-stability) ─────────── */
typedef enum {
    SCP_FW_STATE_POWER_ON         = 0, /* device just powered, no drive selected */
    SCP_FW_STATE_DRIVE_SELECTED   = 1, /* CMD_SELA/SELB issued, motor still OFF */
    SCP_FW_STATE_MOTOR_SPINNING   = 2, /* CMD_MTRAON/MTRBON, drive ready */
    SCP_FW_STATE_SEEKED           = 3, /* CMD_STEPTO/SEEK0 completed, head positioned */
    SCP_FW_STATE_READ_ARMED       = 4, /* CMD_READ_FLUX queued capture in RAM */
    SCP_FW_STATE_FLUX_INFO_READY  = 5, /* CMD_GET_FLUX_INFO returned rev_index table */
    SCP_FW_STATE_RAM_TRANSFER     = 6, /* CMD_SENDRAM_USB streaming flux RAM */
    SCP_FW_STATE_ERROR            = 7, /* sticky error — only RESET / status query allowed */
} scp_fw_state_t;

/* ─── Response codes (samdisk pr_*) ─────────────────────────────────── */
typedef enum {
    SCP_FW_PR_OK            = 0x4F,  /* successful completion */
    SCP_FW_PR_BAD_COMMAND   = 0x01,  /* command byte unknown */
    SCP_FW_PR_COMMAND_ERR   = 0x02,  /* command sequencing violation */
    SCP_FW_PR_CHECKSUM      = 0x03,  /* packet checksum invalid */
    SCP_FW_PR_TIMEOUT       = 0x04,  /* index pulse never arrived */
    SCP_FW_PR_NO_TRK0       = 0x05,  /* recalibration could not find track 0 */
    SCP_FW_PR_WP_ENABLED    = 0x0F,  /* write-protect tab — refuse writes */
    SCP_FW_PR_NO_DISK       = 0x11,  /* no media inserted */
} scp_fw_response_t;

/* ─── Per-revolution capture metadata ──────────────────────────────── */
typedef struct {
    uint32_t index_time_ticks;  /* time between index pulses (25 ns ticks) */
    uint32_t flux_count;        /* number of 16-bit flux samples in RAM */
} scp_fw_rev_meta_t;

/* ─── Emulator context ──────────────────────────────────────────────
 *
 * Holds the full simulated firmware state. Reset to a defined power-on
 * state by scp_fw_reset(). NEVER aliased — caller owns one context per
 * simulated device.
 *
 * Capacities pick the documented worst case: 5 revolutions × 200k
 * samples × 2 bytes per sample ≈ 2 MB. Real firmware uses 8 MB SRAM;
 * the emulator reserves only what tests need (each test allocates its
 * own).
 *
 * Forensic: every field's value is traceable to either a configure
 * call (scp_fw_set_*) or a synthetic-flux generator output. No silent
 * fabrication. */
typedef struct {
    scp_fw_state_t   state;
    scp_fw_response_t sticky_error;   /* set when state == ERROR */

    /* Drive geometry */
    uint8_t  selected_drive;          /* 0=none, 1=A, 2=B */
    bool     motor_on;
    int      current_track;           /* -1 until first seek */
    int      current_side;            /* 0 or 1 */

    /* Configured media properties (set per test) */
    bool     disk_present;
    bool     write_protected;
    bool     track0_findable;         /* SEEK0 success */
    uint32_t index_time_ticks;        /* 200000 = 5 ms = 200 RPM-equiv;
                                         8000000 = 200 ms = 300 RPM
                                         (40 MHz / 5 Hz). */

    /* Per-revolution metadata for the next CMD_READ_FLUX */
    int                revolutions_armed;        /* set by READ_FLUX */
    scp_fw_rev_meta_t  rev_meta[8];              /* up to MAX_REVOLUTIONS=5,
                                                     extras for tests */

    /* Capture RAM — fed by the synthetic flux generator. Each rev's
     * bytes are concatenated; offsets in rev_meta[].flux_count map
     * back to where SENDRAM_USB reads from. */
    uint8_t *capture_ram;
    size_t   capture_ram_len;
    bool     capture_ram_owned;       /* free() on reset if owned */

    /* CMD_SENDRAM_USB state */
    uint32_t sendram_offset_next;     /* next expected SENDRAM offset */

    /* Counters for assertions */
    uint64_t cmd_count;
    uint64_t bytes_tx_to_host;
} scp_fw_t;

/* ─── Lifecycle ─────────────────────────────────────────────────────── */

/** Reset to a defined power-on state. Frees previously-owned capture
 *  RAM. Safe to call repeatedly. */
void scp_fw_reset(scp_fw_t *fw);

/** Convenience: power-on with sensible defaults (disk present, motor
 *  off, track 0, side 0, index = 200ms = 300 RPM). */
void scp_fw_power_on_defaults(scp_fw_t *fw);

/** Configure media properties before running a sequence. */
void scp_fw_set_disk_present(scp_fw_t *fw, bool present);
void scp_fw_set_write_protected(scp_fw_t *fw, bool wp);
void scp_fw_set_track0_findable(scp_fw_t *fw, bool findable);
void scp_fw_set_index_period_ticks(scp_fw_t *fw, uint32_t ticks);

/** Load capture RAM from a buffer the test owns (e.g. from the synthetic
 *  flux generator). The emulator does NOT copy — the buffer must
 *  outlive the test sequence. */
void scp_fw_load_capture_ram(scp_fw_t *fw,
                              const uint8_t *flux_bytes,
                              size_t          flux_bytes_len,
                              const scp_fw_rev_meta_t *rev_meta,
                              size_t          rev_count);

/* ─── Command processing ────────────────────────────────────────────
 *
 * Each function takes the current command + params and returns the
 * response byte (PR_OK or an error code) the firmware would emit. Side
 * effects update fw->state.
 *
 * The split per-command (instead of one big switch) makes test
 * authors' intent obvious: "this test exercises the seek-without-motor
 * branch" reads `scp_fw_cmd_stepto(&fw, 42)` with motor_on=false. */

scp_fw_response_t scp_fw_cmd_sela(scp_fw_t *fw);
scp_fw_response_t scp_fw_cmd_dsela(scp_fw_t *fw);
scp_fw_response_t scp_fw_cmd_mtraon(scp_fw_t *fw);
scp_fw_response_t scp_fw_cmd_mtraoff(scp_fw_t *fw);
scp_fw_response_t scp_fw_cmd_seek0(scp_fw_t *fw);
scp_fw_response_t scp_fw_cmd_stepto(scp_fw_t *fw, uint8_t track);
scp_fw_response_t scp_fw_cmd_side(scp_fw_t *fw, uint8_t side);
scp_fw_response_t scp_fw_cmd_read_flux(scp_fw_t *fw,
                                        uint8_t revolutions,
                                        uint8_t flags);
scp_fw_response_t scp_fw_cmd_write_flux(scp_fw_t *fw);
/* CMD_GET_FLUX_INFO writes 40 bytes (5 × 2 × be32) to `out`. Caller
 * sizes out >= 40. */
scp_fw_response_t scp_fw_cmd_get_flux_info(scp_fw_t *fw,
                                            uint8_t *out, size_t out_cap);
/* CMD_SENDRAM_USB streams `length` bytes from offset `offset` in the
 * loaded capture RAM to `out` (caller sizes out >= length). */
scp_fw_response_t scp_fw_cmd_sendram_usb(scp_fw_t *fw,
                                          uint32_t offset, uint32_t length,
                                          uint8_t *out, size_t out_cap);
/* CMD_STATUS writes a 2-byte big-endian status word. Bits:
 *   bit 15: disk_present
 *   bit 14: motor_on
 *   bit 13: write_protect
 *   bit 12: at_track0
 *   bits 11..0: reserved (zero) */
scp_fw_response_t scp_fw_cmd_status(scp_fw_t *fw,
                                     uint8_t *out, size_t out_cap);
/* CMD_SCPINFO writes hw_version, fw_version (1 byte each). */
scp_fw_response_t scp_fw_cmd_scpinfo(scp_fw_t *fw,
                                      uint8_t hw_ver, uint8_t fw_ver,
                                      uint8_t *out, size_t out_cap);

/* ─── Packet-level helper for verifying wire-format ─────────────────
 *
 * Builds a [CMD, LEN, params..., CHECKSUM] packet so tests can compare
 * what the production C-HAL sends to what a samdisk-equivalent host
 * would send. Same algorithm as the production C-HAL (CHECKSUM_INIT =
 * 0x4A).
 *
 * Returns total packet length (= 3 + param_len). out must be sized
 * >= 3 + param_len.
 */
size_t scp_fw_build_packet(uint8_t *out, uint8_t cmd,
                            const uint8_t *params, size_t param_len);

#ifdef __cplusplus
}
#endif

#endif /* UFT_TESTS_SCP_FIRMWARE_STATE_MACHINE_H */
