# SCP Emulator — Sim-vs-Real-HW Divergences

This is the honest list of every place this emulator simplifies away
behaviour the real SuperCard Pro firmware exhibits. Forensic-honesty
rule (see `.claude/agents/hardware-emulation-author.md` §Hard rules):
no silent simplifications.

For each entry: **what real HW does → what sim does → why we accept →
how to detect the gap**.

---

## D-1: 40 MHz sample clock is exact in sim

- **Real HW:** crystal-controlled 40 MHz oscillator with ±0.005% (typical
  ±50 ppm) tolerance. Two captures of the same disk on two SCP units
  may disagree by a few ns per sample.
- **Sim:** every "tick" is exactly 25 ns. Two simulated captures are
  byte-identical.
- **Why accept:** the tolerance is orders of magnitude smaller than the
  decoder's PLL bandwidth (~10%); no realistic decoder failure is masked.
- **Detection:** a real-HW bench session reading the same disk twice
  on the same SCP will produce slightly different flux-sample values
  even within the noise floor — sim will not.

---

## D-2: Index pulse timing is exact in sim

- **Real HW:** index pulse fires once per revolution from a photo-
  interrupter; mechanical jitter ±a few µs is typical for 3.5" drives,
  more for old 5.25". The `index_time` recorded in the rev_index table
  varies revolution-to-revolution by ~0.1-0.5%.
- **Sim:** `scp_fw_set_index_period_ticks()` sets a constant, and every
  rev's recorded index_time is exactly that value.
- **Why accept:** the decoder uses index purely as a frame mark, not
  for timing recovery. Constant index in sim does not change any
  bit-decode outcome.
- **Detection:** a real-HW capture's rev_index table will show ~5
  slightly-different `index_time` values; sim shows 5 identical values.

---

## D-3: Drive-status bit semantics are partially faked

- **Real HW:** `CMD_STATUS` returns a 16-bit word whose bit layout is
  not fully documented in the samdisk reference (samdisk reads it as a
  big-endian word and exposes it raw). The disk-present / motor-on /
  WP-enabled bits are inferred from observable behaviour.
- **Sim:** uses an assumed bit layout:
    - bit 15 = disk_present
    - bit 14 = motor_on
    - bit 13 = write_protected
    - bit 12 = at_track0
  - bits 11..0 zeroed.
- **Why accept:** the production C-HAL does not currently parse the
  status word — it only checks the response byte. The bit layout matters
  only inside this emulator's tests, not for the C-HAL's behaviour.
- **Detection:** a real-HW `CMD_STATUS` byte-trace will likely show
  a different bit pattern. When the C-HAL gains a `CMD_STATUS` parser,
  this divergence becomes load-bearing and needs a real-HW capture to
  pin the layout.

---

## D-4: Sticky-error semantics are conservative

- **Real HW:** unclear whether the firmware enters a "sticky" error
  state after a sequencing violation, or simply rejects the offending
  command and allows recovery. samdisk's error handling implies
  sticky-ish (it abandons the operation), but it does not explicitly
  reset.
- **Sim:** sticky — once `SCP_FW_STATE_ERROR` is set, only `STATUS`
  and `SCPINFO` are answered; all state-mutating commands return the
  sticky error code until `scp_fw_reset()`.
- **Why accept:** this is the stricter forensic interpretation. A test
  that depends on sticky-state error handling will catch any future C-HAL
  attempt to recover silently from an error (which would be a
  forensic-honesty violation).
- **Detection:** real-HW recovery sequences (e.g. SELA → DSELA → SELA
  re-arming) may succeed where sim refuses. If a real-HW bench session
  shows commands succeeding after a previous error, this divergence
  becomes a sim-too-strict bug.

---

## D-5: Capture-RAM is caller-owned and not preserved across reset

- **Real HW:** firmware capture RAM persists across the
  `CMD_READ_FLUX → CMD_GET_FLUX_INFO → CMD_SENDRAM_USB` sequence. It is
  cleared on power cycle but not by intermediate commands.
- **Sim:** `scp_fw_reset()` zeroes everything including the loaded
  capture-RAM pointer. Tests must call `scp_fw_load_capture_ram()`
  AFTER `scp_fw_power_on_defaults()` and BEFORE the read pipeline.
- **Why accept:** simpler test setup; the caller-owns model matches the
  byte-mock's "test pre-scripts everything" idiom.
- **Detection:** a stress test running 10 consecutive READ_FLUX cycles
  on real HW without re-loading the RAM model would succeed; the same
  pattern in sim requires re-loading.

---

## D-6: No timeout modelling

- **Real HW:** `pr_Timeout (0x04)` fires when an expected index pulse
  doesn't arrive within ~250 ms.
- **Sim:** never returns `PR_TIMEOUT`. A `read_flux` with `disk_present=true`
  always proceeds; with `disk_present=false` it returns `PR_NO_DISK`.
- **Why accept:** timeout testing requires modelling time, which adds
  a clock abstraction the rest of the emulator does not need.
- **Detection:** disconnect the drive cable mid-read on real HW → real
  firmware returns `PR_TIMEOUT`. Sim does not have an equivalent.
- **Next step:** if a timeout-recovery test becomes needed, add a
  `scp_fw_force_timeout_next()` flag — v4.1.6+ candidate.

---

## D-7: Flux-generator omits low-level magnetic noise

- **Real-HW captured flux:** every interval has a small (~0.5%) Gaussian
  noise on the timing — a side-effect of the head amplifier and PLL
  preamp. Two consecutive reads of the same disk show different exact
  timing values within noise.
- **Sim:** clean MFM generator produces exact 4/8/12 µs intervals
  (or the weak-bits-jittered versions). No Gaussian noise.
- **Why accept:** clean intervals are what the test author wants — they
  match the decoder's NOMINAL input. Adding Gaussian noise would couple
  the test's pass/fail to the decoder's noise tolerance, which is a
  decoder test concern not a flux-gen concern.
- **Detection:** running the generator's output through the production
  PLL gives a confidence map that's flat-perfect; real flux gives a
  varying confidence map. The bench session will show this.

---

## D-8: V-MAX! signature is uniform-only, not sector-mixed

- **Real V-MAX!:** uses a mix of uniform-cell sectors and scrambled
  sectors. The protection check looks for the uniform sectors AND the
  scrambled-sector contrast.
- **Sim:** `uft_scp_flux_gen_vmax()` emits ONLY uniform cells across the
  whole revolution. A V-MAX! detector built on uniformity-only will
  classify; one that requires sector contrast will not.
- **Why accept:** uniformity alone is enough to exercise the protection-
  classifier's main code path. Mixed-sector V-MAX! is a richer signature
  that earns its own flux-gen function — v4.1.6+ candidate.
- **Detection:** running a real V-MAX!-protected C64 disk through UFT
  shows a per-track confidence pattern with peaks and dips; sim shows a
  flat peak.

---

## D-9: USB transport layer not exercised by this emulator

- **Real-HW path:** every CMD has to traverse libusb → FT240-X → SCP
  firmware → drive → back. Transport adds latency, can lose bytes
  on flaky cables, etc.
- **Sim:** the firmware-state-machine is invoked directly by the test
  — there is NO libusb path. The existing byte-mock at
  `tests/test_scp_direct_usb_mock.c` exercises the libusb path
  (against scripted bytes); THIS test exercises the firmware
  semantics (against direct calls).
- **Why accept:** separation of concerns. Wire-correctness lives in
  the byte-mock; firmware-correctness lives here. End-to-end "C-HAL
  drives firmware-realistic emulator over scripted libusb" is a richer
  integration that is a v4.1.6+ candidate (would require teaching
  `libusb_mock.c` to dispatch incoming TX bytes through `scp_fw_t`).
- **Detection:** none — this is an architectural split, not a wrong
  result.

---

## Coverage summary

| Layer | Coverage vs real-HW |
|---|---|
| Wire-protocol opcodes (read path) | 100% (CMD_SELA..CMD_SCPINFO 11 opcodes used by samdisk's ReadFlux) |
| Wire-protocol opcodes (write path) | 0% (intentionally refused — forensic safety) |
| Firmware state-machine | ~85% (8 states + transitions; missing: timeout, capture-RAM persistence, real status bits) |
| Flux generation (clean MFM) | ~75% (correct format, deterministic; missing: Gaussian noise) |
| Flux defect classes | 3 classes (weak_bits, crc_error, V-MAX!); coverage ~60% (no Rob Northen, Copylock, half-tracks yet) |
| Capability-flag claim | "SIMULATED (FIRMWARE-REALISTIC, ~80% aggregate)" |

These numbers are estimates from feature-by-feature review against
samdisk's reference; the real-HW bench will produce ground truth.
