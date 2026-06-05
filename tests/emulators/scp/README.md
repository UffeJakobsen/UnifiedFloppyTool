# SCP Firmware-Realistic Emulator + Synthetic Flux Generator

**Status:** SIMULATED (FIRMWARE-REALISTIC, ~80% coverage vs. real-HW spec).
Real-hardware bench session still required for Tier-3 PASS — see
`docs/MASTER_PLAN.md` §M3.1 and `tests/hil/`.

## Scope

This emulator goes *beyond* the existing Tier-2.5 byte-mock in
`tests/test_scp_direct_usb_mock.c`. The byte-mock scripts the bytes a
test author EXPECTS the C-HAL to send and verifies them; THIS emulator
models WHY those bytes are what they are — by enforcing the firmware's
documented prerequisite ordering:

```
DRIVE_SELECT  ->  MOTOR_ON  ->  SEEK  ->  READ_FLUX
                                              |
                                              v
                                       GET_FLUX_INFO  ->  SENDRAM_USB (per rev)
```

Sequencing violations (e.g. `CMD_STEPTO` before `CMD_MTRAON`) make the
firmware emulator enter `SCP_FW_STATE_ERROR` with sticky
`PR_COMMAND_ERR` — exactly as the real firmware would.

Three layers, per `.claude/agents/hardware-emulation-author.md`:

| Layer | File | Purpose |
|---|---|---|
| Wire protocol | `firmware_state_machine.c::scp_fw_build_packet` | Emits `[CMD,LEN,params,CHECKSUM]` packets identical to the production C-HAL |
| Firmware state machine | `firmware_state_machine.{h,c}` | 8 states + per-command transitions matching samdisk's documented call order |
| Flux generation | `tests/flux_gen/scp/flux_gen.{h,c}` + `defects/*.c` | Synthetic SCP capture-RAM bytes with opt-in defect injection |

## Non-scope

- **Write path** — `CMD_WRITE_FLUX` is ALWAYS refused (`PR_WP_ENABLED`).
  Forensic-safety guard: we never emulate writes because emulated-but-
  buggy writes would mask real-HW write bugs the bench session needs
  to catch. This matches the production C-HAL which also refuses writes
  until bench verification.
- **Magnetic medium physics** — no aging, no fluxometric noise, no
  cross-track bleed. The flux generator emits clean intervals + injected
  defects, nothing more.
- **`.scp` file format** — distinct from the USB capture-RAM byte stream
  the firmware produces. The file format is a separate audit subject.
- **Other controllers** — XUM1541, Applesauce etc. each get their own
  invocation per the agent contract.

## Reference

- Wire protocol + opcode constants: [`include/uft/hal/uft_scp_direct.h`](../../../include/uft/hal/uft_scp_direct.h)
- Production C-HAL: [`src/hal/uft_scp_direct.c`](../../../src/hal/uft_scp_direct.c)
- Open-source cross-reference: [simonowen/samdisk SuperCardPro.{h,cpp}](https://github.com/simonowen/samdisk)
  — the de-facto open implementation of the SCP USB protocol (active
  use by forensic disk practitioners since 2017).
- Audit: [`audit/scp/REPORT.md`](../../../audit/scp/REPORT.md) (D1..D5 findings).

## Build + run

The test binary `test_scp_emulator` is wired into the main
`tests/CMakeLists.txt`. Build the C-test set:

```bash
cmake -B build
cmake --build build --target test_scp_emulator
./build/tests/test_scp_emulator       # on Windows: .exe
```

Or via ctest:

```bash
cd build
ctest -R test_scp_emulator -V
```

## Forensic honesty

- Every byte this emulator emits is traceable to the synthetic-flux
  generator (deterministic seed) OR a documented packet builder. No
  silent fabrication.
- The flux generator REFUSES to emit out-of-spec intervals (<2 µs or
  >10 ms) — this guard exists so that if a future path ever fed
  generated flux back to real hardware, the medium would not be
  damaged.
- Sim-vs-real divergences are listed in [`DIVERGENCES.md`](DIVERGENCES.md).
  Coverage breakdown in [`coverage_matrix.md`](coverage_matrix.md).
