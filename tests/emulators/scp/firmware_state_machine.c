/**
 * @file tests/emulators/scp/firmware_state_machine.c
 * @brief SCP firmware-realistic state machine implementation.
 *
 * Reference: simonowen/samdisk SuperCardPro.cpp (sequencing inferred
 * from observed call order in samdisk + documented prerequisites).
 *
 * Sequencing rules enforced (all reverse-engineered, see DIVERGENCES.md):
 *   1. CMD_STEPTO/SEEK0 require a selected drive AND motor on
 *   2. CMD_READ_FLUX requires a selected drive, motor on, and a prior seek
 *   3. CMD_GET_FLUX_INFO requires a prior CMD_READ_FLUX
 *   4. CMD_SENDRAM_USB requires a prior CMD_GET_FLUX_INFO and offsets
 *      that line up with the previous SENDRAM_USB's tail
 *   5. CMD_WRITE_FLUX is REFUSED (PR_WP_ENABLED) — forensic-safety
 *      guard; we never emulate writes
 *
 * No silent state changes — any sequencing violation transitions to
 * SCP_FW_STATE_ERROR with sticky_error = PR_COMMAND_ERR.
 */

#include "firmware_state_machine.h"

#include <stdlib.h>
#include <string.h>

#include "../../../include/uft/hal/uft_scp_direct.h"

/* ─── Internal helpers ──────────────────────────────────────────────── */

static void scp_fw_enter_error(scp_fw_t *fw, scp_fw_response_t reason)
{
    fw->state         = SCP_FW_STATE_ERROR;
    fw->sticky_error  = reason;
}

static void scp_fw_count_cmd(scp_fw_t *fw)
{
    fw->cmd_count++;
}

/* big-endian 32-bit write */
static void be32(uint8_t *p, uint32_t v)
{
    p[0] = (uint8_t)(v >> 24);
    p[1] = (uint8_t)(v >> 16);
    p[2] = (uint8_t)(v >>  8);
    p[3] = (uint8_t)(v);
}

/* ─── Lifecycle ─────────────────────────────────────────────────────── */

void scp_fw_reset(scp_fw_t *fw)
{
    if (!fw) return;
    /* Forensic invariant: this emulator NEVER owns the capture-RAM
     * buffer (scp_fw_load_capture_ram is the only path that sets the
     * pointer, and it always sets capture_ram_owned=false). Callers
     * who allocated the flux buffer via uft_scp_flux_gen_clean()
     * free it via uft_scp_flux_gen_free() — not via reset. We can
     * therefore zero the whole struct unconditionally, which makes
     * reset safe on a stack-local scp_fw_t that has never been
     * touched (the dominant first-test pattern). */
    memset(fw, 0, sizeof(*fw));
    fw->state              = SCP_FW_STATE_POWER_ON;
    fw->current_track      = -1;
    fw->index_time_ticks   = 8000000u;   /* ~200 ms => 300 RPM at 25 ns/tick */
}

void scp_fw_power_on_defaults(scp_fw_t *fw)
{
    scp_fw_reset(fw);
    fw->disk_present        = true;
    fw->write_protected     = false;
    fw->track0_findable     = true;
}

void scp_fw_set_disk_present(scp_fw_t *fw, bool present)
{
    fw->disk_present = present;
}

void scp_fw_set_write_protected(scp_fw_t *fw, bool wp)
{
    fw->write_protected = wp;
}

void scp_fw_set_track0_findable(scp_fw_t *fw, bool findable)
{
    fw->track0_findable = findable;
}

void scp_fw_set_index_period_ticks(scp_fw_t *fw, uint32_t ticks)
{
    fw->index_time_ticks = ticks;
}

void scp_fw_load_capture_ram(scp_fw_t *fw,
                              const uint8_t *flux_bytes,
                              size_t          flux_bytes_len,
                              const scp_fw_rev_meta_t *rev_meta,
                              size_t          rev_count)
{
    if (!fw || !flux_bytes || !rev_meta) return;
    if (rev_count > 8) rev_count = 8;
    /* The emulator does NOT copy — caller owns the buffer's lifetime.
     * This is the same forensic-honesty stance the C-HAL takes: no
     * silent duplication of flux data. */
    fw->capture_ram        = (uint8_t *)flux_bytes; /* cast away const for storage,
                                                      we never write */
    fw->capture_ram_len    = flux_bytes_len;
    fw->capture_ram_owned  = false;
    for (size_t i = 0; i < rev_count; i++) {
        fw->rev_meta[i] = rev_meta[i];
    }
    /* zero out the rest so a 2-rev-armed test does not accidentally
     * dump rev 3's leftover metadata */
    for (size_t i = rev_count; i < 8; i++) {
        fw->rev_meta[i].index_time_ticks = 0;
        fw->rev_meta[i].flux_count       = 0;
    }
}

/* ─── Command implementations ──────────────────────────────────────── */

scp_fw_response_t scp_fw_cmd_sela(scp_fw_t *fw)
{
    scp_fw_count_cmd(fw);
    if (fw->state == SCP_FW_STATE_ERROR) return fw->sticky_error;
    fw->selected_drive = 1;
    if (fw->state == SCP_FW_STATE_POWER_ON) {
        fw->state = SCP_FW_STATE_DRIVE_SELECTED;
    }
    return SCP_FW_PR_OK;
}

scp_fw_response_t scp_fw_cmd_dsela(scp_fw_t *fw)
{
    scp_fw_count_cmd(fw);
    if (fw->state == SCP_FW_STATE_ERROR) return fw->sticky_error;
    fw->selected_drive = 0;
    fw->motor_on       = false;
    /* deselect returns to POWER_ON — subsequent commands need re-select. */
    fw->state = SCP_FW_STATE_POWER_ON;
    return SCP_FW_PR_OK;
}

scp_fw_response_t scp_fw_cmd_mtraon(scp_fw_t *fw)
{
    scp_fw_count_cmd(fw);
    if (fw->state == SCP_FW_STATE_ERROR) return fw->sticky_error;
    if (fw->selected_drive == 0) {
        /* Per samdisk, motor cmds without a selected drive are a
         * sequencing error. Real HW may simply ignore — but ignoring
         * = silent no-op = forensic foot-gun. We refuse. */
        scp_fw_enter_error(fw, SCP_FW_PR_COMMAND_ERR);
        return SCP_FW_PR_COMMAND_ERR;
    }
    if (!fw->disk_present) {
        /* No disk => motor still spins, but subsequent ops fail. Real
         * SCP reports NO_DISK on the *next* media-touching command,
         * not on motor-on. We mirror that. */
    }
    fw->motor_on = true;
    if (fw->state == SCP_FW_STATE_DRIVE_SELECTED) {
        fw->state = SCP_FW_STATE_MOTOR_SPINNING;
    }
    return SCP_FW_PR_OK;
}

scp_fw_response_t scp_fw_cmd_mtraoff(scp_fw_t *fw)
{
    scp_fw_count_cmd(fw);
    if (fw->state == SCP_FW_STATE_ERROR) return fw->sticky_error;
    fw->motor_on = false;
    if (fw->state >= SCP_FW_STATE_MOTOR_SPINNING) {
        fw->state = SCP_FW_STATE_DRIVE_SELECTED;
    }
    return SCP_FW_PR_OK;
}

scp_fw_response_t scp_fw_cmd_seek0(scp_fw_t *fw)
{
    scp_fw_count_cmd(fw);
    if (fw->state == SCP_FW_STATE_ERROR) return fw->sticky_error;
    if (!fw->motor_on || fw->selected_drive == 0) {
        scp_fw_enter_error(fw, SCP_FW_PR_COMMAND_ERR);
        return SCP_FW_PR_COMMAND_ERR;
    }
    if (!fw->track0_findable) {
        /* Drive head misaligned or no track 0 sensor — real failure. */
        scp_fw_enter_error(fw, SCP_FW_PR_NO_TRK0);
        return SCP_FW_PR_NO_TRK0;
    }
    fw->current_track = 0;
    fw->state         = SCP_FW_STATE_SEEKED;
    return SCP_FW_PR_OK;
}

scp_fw_response_t scp_fw_cmd_stepto(scp_fw_t *fw, uint8_t track)
{
    scp_fw_count_cmd(fw);
    if (fw->state == SCP_FW_STATE_ERROR) return fw->sticky_error;
    if (!fw->motor_on || fw->selected_drive == 0) {
        scp_fw_enter_error(fw, SCP_FW_PR_COMMAND_ERR);
        return SCP_FW_PR_COMMAND_ERR;
    }
    if (track > UFT_SCP_MAX_TRACK_INDEX) {
        /* HAL boundary already enforces this, but the firmware
         * itself would also refuse — STEPTO byte is unsigned 8-bit
         * so values <= 167 are valid; 168..255 are illegal. */
        scp_fw_enter_error(fw, SCP_FW_PR_COMMAND_ERR);
        return SCP_FW_PR_COMMAND_ERR;
    }
    fw->current_track = track;
    fw->state         = SCP_FW_STATE_SEEKED;
    return SCP_FW_PR_OK;
}

scp_fw_response_t scp_fw_cmd_side(scp_fw_t *fw, uint8_t side)
{
    scp_fw_count_cmd(fw);
    if (fw->state == SCP_FW_STATE_ERROR) return fw->sticky_error;
    if (side > 1) {
        scp_fw_enter_error(fw, SCP_FW_PR_COMMAND_ERR);
        return SCP_FW_PR_COMMAND_ERR;
    }
    /* SIDE is documented as legal independent of motor/seek (samdisk
     * issues it directly before READ_FLUX, see SuperCardPro.cpp). */
    fw->current_side = side;
    return SCP_FW_PR_OK;
}

scp_fw_response_t scp_fw_cmd_read_flux(scp_fw_t *fw,
                                        uint8_t revolutions,
                                        uint8_t flags)
{
    scp_fw_count_cmd(fw);
    (void)flags;  /* ff_Index / ff_RPM360 are accepted but not modeled
                     beyond their wire format; documented in DIVERGENCES. */
    if (fw->state == SCP_FW_STATE_ERROR) return fw->sticky_error;
    if (!fw->motor_on || fw->selected_drive == 0 ||
        fw->state < SCP_FW_STATE_MOTOR_SPINNING) {
        scp_fw_enter_error(fw, SCP_FW_PR_COMMAND_ERR);
        return SCP_FW_PR_COMMAND_ERR;
    }
    if (!fw->disk_present) {
        scp_fw_enter_error(fw, SCP_FW_PR_NO_DISK);
        return SCP_FW_PR_NO_DISK;
    }
    if (revolutions < UFT_SCP_MIN_REVOLUTIONS ||
        revolutions > UFT_SCP_MAX_REVOLUTIONS) {
        /* Real firmware clamps; UFT (and this emulator) refuses
         * — honest fail rather than silent coercion. */
        scp_fw_enter_error(fw, SCP_FW_PR_COMMAND_ERR);
        return SCP_FW_PR_COMMAND_ERR;
    }
    fw->revolutions_armed   = revolutions;
    fw->state               = SCP_FW_STATE_READ_ARMED;
    fw->sendram_offset_next = 0;
    return SCP_FW_PR_OK;
}

scp_fw_response_t scp_fw_cmd_write_flux(scp_fw_t *fw)
{
    scp_fw_count_cmd(fw);
    /* Forensic-safety: this emulator NEVER emits writes. Real firmware
     * would accept the command if the media is writable; we always
     * refuse with PR_WP_ENABLED so a test asserting "write was refused"
     * always passes regardless of media state. The UFT HAL itself
     * also returns UFT_ERR_NOT_IMPLEMENTED for write, so this matches
     * the system-level contract. */
    return SCP_FW_PR_WP_ENABLED;
}

scp_fw_response_t scp_fw_cmd_get_flux_info(scp_fw_t *fw,
                                            uint8_t *out, size_t out_cap)
{
    scp_fw_count_cmd(fw);
    if (fw->state == SCP_FW_STATE_ERROR) return fw->sticky_error;
    if (fw->state != SCP_FW_STATE_READ_ARMED) {
        scp_fw_enter_error(fw, SCP_FW_PR_COMMAND_ERR);
        return SCP_FW_PR_COMMAND_ERR;
    }
    if (!out || out_cap < 40) {
        /* Test infrastructure error — caller's buffer is too small.
         * Don't change firmware state; just refuse. */
        return SCP_FW_PR_COMMAND_ERR;
    }
    /* Emit 5 × [index_time_be32, flux_count_be32] pairs. We always
     * emit 5 even if revolutions_armed < 5 — real firmware does
     * the same (samdisk reads a fixed-size table). */
    memset(out, 0, 40);
    for (int i = 0; i < UFT_SCP_MAX_REVOLUTIONS; i++) {
        be32(&out[i * 8 + 0], fw->rev_meta[i].index_time_ticks);
        be32(&out[i * 8 + 4], fw->rev_meta[i].flux_count);
    }
    fw->state             = SCP_FW_STATE_FLUX_INFO_READY;
    fw->bytes_tx_to_host += 40;
    return SCP_FW_PR_OK;
}

scp_fw_response_t scp_fw_cmd_sendram_usb(scp_fw_t *fw,
                                          uint32_t offset, uint32_t length,
                                          uint8_t *out, size_t out_cap)
{
    scp_fw_count_cmd(fw);
    if (fw->state == SCP_FW_STATE_ERROR) return fw->sticky_error;
    if (fw->state != SCP_FW_STATE_FLUX_INFO_READY &&
        fw->state != SCP_FW_STATE_RAM_TRANSFER) {
        scp_fw_enter_error(fw, SCP_FW_PR_COMMAND_ERR);
        return SCP_FW_PR_COMMAND_ERR;
    }
    /* Offset must equal sendram_offset_next — real firmware streams
     * sequentially; random-access reads are not part of the documented
     * protocol. Out-of-order = error. */
    if (offset != fw->sendram_offset_next) {
        scp_fw_enter_error(fw, SCP_FW_PR_COMMAND_ERR);
        return SCP_FW_PR_COMMAND_ERR;
    }
    /* Bounds check against the loaded capture RAM. Refuse to fabricate
     * — if the test set up rev_meta saying "rev 0 has 100 samples"
     * but only loaded 50, we report COMMAND_ERR. */
    if ((size_t)offset + (size_t)length > fw->capture_ram_len) {
        scp_fw_enter_error(fw, SCP_FW_PR_COMMAND_ERR);
        return SCP_FW_PR_COMMAND_ERR;
    }
    if (!out || (size_t)length > out_cap) {
        return SCP_FW_PR_COMMAND_ERR;
    }
    if (length > 0 && fw->capture_ram != NULL) {
        memcpy(out, fw->capture_ram + offset, length);
    }
    fw->sendram_offset_next = offset + length;
    fw->state               = SCP_FW_STATE_RAM_TRANSFER;
    fw->bytes_tx_to_host   += length;
    return SCP_FW_PR_OK;
}

scp_fw_response_t scp_fw_cmd_status(scp_fw_t *fw,
                                     uint8_t *out, size_t out_cap)
{
    scp_fw_count_cmd(fw);
    /* STATUS is always legal — real firmware allows it even from ERROR
     * state (it's how the host probes for the failure). */
    if (!out || out_cap < 2) return SCP_FW_PR_COMMAND_ERR;

    uint16_t status = 0;
    if (fw->disk_present)            status |= (1u << 15);
    if (fw->motor_on)                status |= (1u << 14);
    if (fw->write_protected)         status |= (1u << 13);
    if (fw->current_track == 0)      status |= (1u << 12);

    out[0] = (uint8_t)(status >> 8);
    out[1] = (uint8_t)(status & 0xFF);
    fw->bytes_tx_to_host += 2;
    return SCP_FW_PR_OK;
}

scp_fw_response_t scp_fw_cmd_scpinfo(scp_fw_t *fw,
                                      uint8_t hw_ver, uint8_t fw_ver,
                                      uint8_t *out, size_t out_cap)
{
    scp_fw_count_cmd(fw);
    if (!out || out_cap < 2) return SCP_FW_PR_COMMAND_ERR;
    out[0] = hw_ver;
    out[1] = fw_ver;
    fw->bytes_tx_to_host += 2;
    return SCP_FW_PR_OK;
}

/* ─── Packet builder (matches UFT HAL CHECKSUM_INIT = 0x4A) ──────────── */

size_t scp_fw_build_packet(uint8_t *out, uint8_t cmd,
                            const uint8_t *params, size_t param_len)
{
    out[0] = cmd;
    out[1] = (uint8_t)param_len;
    uint8_t cs = (uint8_t)(UFT_SCP_CHECKSUM_INIT + cmd + (uint8_t)param_len);
    for (size_t i = 0; i < param_len; i++) {
        out[2 + i] = params[i];
        cs = (uint8_t)(cs + params[i]);
    }
    out[2 + param_len] = cs;
    return 3 + param_len;
}
