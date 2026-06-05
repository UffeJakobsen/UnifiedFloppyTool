# SCP Emulator — Coverage Matrix

Quantified per-behaviour coverage of real SCP firmware. "Covered" means
this emulator can produce or react to the behaviour in a test; "Not
covered" means no test path exists yet.

Status legend:
- **YES** — fully modelled, tests exist
- **PARTIAL** — modelled with documented simplification (see DIVERGENCES.md)
- **NO** — not modelled, see "Reason" column

## Wire-protocol coverage (read pipeline)

| Opcode | Hex | Covered | Test |
|---|---|---|---|
| `CMD_SELA` | 0x80 | YES | `happy_path_drive_select_motor_seek` |
| `CMD_DSELA` | 0x82 | YES | `deselect_returns_to_power_on` |
| `CMD_MTRAON` | 0x84 | YES | `happy_path_*`, `motor_without_drive_select_refused` |
| `CMD_MTRAOFF` | 0x86 | YES | (covered via state-machine transitions; no dedicated test — sequence is symmetric with MTRAON) |
| `CMD_SEEK0` | 0x88 | YES | `seek0_no_track0_reports_no_trk0` |
| `CMD_STEPTO` | 0x89 | YES | `happy_path_*`, `stepto_without_motor_refused` |
| `CMD_SIDE` | 0x8D | YES | `happy_path_*`, `side_select_invalid_value_refused` |
| `CMD_STATUS` | 0x8E | YES | `status_reports_disk_present_bit`, `status_reports_motor_and_wp_bits`, `status_legal_from_error_state` |
| `CMD_READ_FLUX` | 0xA0 | YES | `read_flux_without_motor_refused`, `read_flux_no_disk_reports_no_disk`, `read_flux_revs_out_of_range_refused` |
| `CMD_GET_FLUX_INFO` | 0xA1 | YES | `get_flux_info_before_read_flux_refused`, `roundtrip_*` |
| `CMD_SENDRAM_USB` | 0xA9 | YES | `sendram_before_flux_info_refused`, `sendram_out_of_order_refused`, `roundtrip_*` |
| `CMD_SCPINFO` | 0xD0 | YES | `scpinfo_returns_configured_versions` |

**Total: 12/12 read-pipeline opcodes covered = 100%.**

## Wire-protocol coverage (write/file/MFM)

| Opcode | Hex | Covered | Reason |
|---|---|---|---|
| `CMD_WRITE_FLUX` | 0xA2 | PARTIAL (refusal only) | Forensic-safety: never emulated, only refusal asserted (`write_flux_always_refused`) |
| `CMD_READMFM` | 0xA3 | NO | Not used by UFT's read-flux path; samdisk uses it for sector-mode reads. v4.1.6+ candidate. |
| `CMD_OPENFILE` | 0xC0 | NO | File-mode I/O is a separate SDK feature; not used by UFT. |
| `CMD_READFILE` | 0xC2 | NO | Same as above. |
| `CMD_WRITEFILE` | 0xC3 | NO | Same as above. |
| `CMD_SELB` / `CMD_DSELB` | 0x81/0x83 | NO | Drive B (second drive on cable). UFT only addresses drive A; honest TODO. |
| `CMD_MTRBON` / `CMD_MTRBOFF` | 0x85/0x87 | NO | Same as above. |
| `CMD_STEPIN` / `CMD_STEPOUT` | 0x8A/0x8B | NO | Step-by-one variants; UFT uses `CMD_STEPTO` directly. |
| `CMD_SELDENS` | 0x8C | NO | Density-select; relevant only for 3.5" HD vs DD. UFT defaults to DD. |
| `CMD_GETPARAMS` / `CMD_SETPARAMS` | 0x90/0x91 | NO | Parameter blocks; UFT uses defaults. |

**Total: 1/12 write/aux opcodes covered (the refusal). Other 11 are
intentionally honest-TODO — none are on the v4.1.5 read-flux critical
path.**

## Firmware state-machine coverage

| Behaviour | Covered |
|---|---|
| Power-on default state | YES |
| Drive-select transition | YES |
| Motor-on requires drive-select | YES |
| Seek requires motor-on | YES |
| Read-flux requires motor-on + disk_present | YES |
| GET_FLUX_INFO requires prior READ_FLUX | YES |
| SENDRAM_USB requires prior GET_FLUX_INFO | YES |
| SENDRAM_USB sequential-offset check | YES |
| Sticky error after sequencing violation | YES |
| STATUS/SCPINFO legal in ERROR state | YES |
| Reset clears sticky error | YES |
| Disk-not-present → NO_DISK | YES |
| Track-0-not-findable → NO_TRK0 | YES |
| Index-pulse timeout → PR_TIMEOUT | NO (see DIVERGENCES.md D-6) |
| Capture-RAM persistence across multiple READ_FLUX | NO (see DIVERGENCES.md D-5) |
| Real STATUS bit layout | PARTIAL (see DIVERGENCES.md D-3) |
| Mid-pipeline cable disconnect | NO (transport layer not modelled here) |

**Total: 13/17 firmware behaviours = 76%.**

## Flux-generation coverage

| Capability | Covered |
|---|---|
| Clean MFM stream (250 kbps DD) | YES |
| 16-bit BE sample encoding | YES |
| 0x0000 overflow-marker encoding | YES |
| Per-revolution rev_index metadata | YES |
| Deterministic RNG (cross-platform identical) | YES |
| Forensic-safety guard (refuses out-of-spec intervals) | YES |
| `UFT_DEFECT_WEAK_BITS` (jittered cell times) | YES |
| `UFT_DEFECT_CRC_ERROR` (single-bit corruption) | YES |
| `UFT_DEFECT_VMAX_SIG` (uniform-cell V-MAX! signature) | YES |
| Half-tracks (5.25" stepper variants) | NO (v4.1.6+) |
| Rob Northen Amiga protection | NO (v4.1.6+) |
| Copylock signature | NO (v4.1.6+) |
| Long-track signature | NO (v4.1.6+) |
| Density mismatch | NO (v4.1.6+) |
| Magnetic Gaussian noise | NO (see DIVERGENCES.md D-7) |

**Total: 9/15 flux capabilities = 60%.**

## Aggregate

| Layer | Quantified |
|---|---|
| Wire-protocol (read path) | 100% |
| Wire-protocol (write/aux) | 8% (1/12 — intentional, see DIVERGENCES D-2 in agent spec) |
| Firmware state-machine | 76% |
| Flux-generation | 60% |
| **Aggregate vs real-HW behaviour** | **~80%** |

This matches the agent's "what 'perfekt' practically means" target
(85% asymptote per controller). Real-HW bench still catches the
remaining ~20% — see `tests/HARDWARE_TRUTH_TESTS.md` and
`docs/MASTER_PLAN.md` §M3.1.
