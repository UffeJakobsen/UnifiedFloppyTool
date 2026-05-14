# Differential Conformance Report — gw vs UFT (P3.2 / task #109)

Byte-exact differential of the **UFT flux decode engine** against
**Greaseweazle `gw` 1.23**, on a shared synthetic flux corpus.

- Harness: `tests/differential/test_gw_parity.py`
- Corpus: `tests/differential/corpus/` (Hybrid Layer 1 — synthetic,
  deterministic; `gen_corpus.py`)
- UFT side: `tests/differential/uft_flux_decode.c` (test fixture
  linking the UFT flux engine — UFT itself is GUI-only)
- Method: the SAME gw-produced `.scp` flux stream is decoded by both
  `gw convert` and the UFT engine; the two decoded sector images are
  compared byte-for-byte.

Last run: gw 1.23, UFT @ MF-218, MinGW gcc 13.1.0 — `pytest
tests/differential/` → **3 passed, 3 skipped, exit 0**.

## Per-format result

| Format | Disk class | gw `--format` | UFT encoding | Result |
|--------|-----------|---------------|--------------|--------|
| `ibm_dd`     | IBM 3.5" DD 720K   | `ibm.720`             | mfm       | ✅ **GLEICH** — byte-exact (737280 B, 1440 sectors) |
| `ibm_hd`     | IBM 3.5" HD 1.44M  | `ibm.1440`            | mfm       | ✅ **GLEICH** — byte-exact (1474560 B, 2880 sectors) |
| `atarist_dd` | Atari ST DD 720K   | `atarist.720`         | mfm       | ✅ **GLEICH** — byte-exact (737280 B, 1440 sectors) |
| `c64_gcr`    | Commodore 1541 GCR | `commodore.1541`      | gcr_c64   | ⬜ **UFT-side pending** — see below |
| `apple2_525` | Apple II 5.25" GCR | `apple2.appledos.140` | gcr_apple | ⬜ **UFT-side pending** — see below |
| `amiga_dd`   | Amiga DD 880K      | `amiga.amigados`      | amiga     | ⬜ **UFT-side pending** — see below |

No **DIVERGENT** rows in this run. If a format ever diverges,
`test_gw_parity.py` fails that parametrised case and prints the first
differing offset with 32-byte hex context for both sides — that hex
context is the per-format diff-hex this report would carry.

## The 3 IBM-style MFM formats — real parity

`ibm_dd`, `ibm_hd`, `atarist_dd` all decode through UFT's
`flux_decode_mfm()`. They reach byte-exact parity with gw **only after
MF-218** — before that fix UFT's MFM decoder skipped two of the three
0xA1 sync words and decoded **zero** sectors from any standard IBM
flux. The differential harness surfaced that bug on its first real
file; `tests/test_flux_mfm_sync.c` is the regression guard.

Additional confidence: the round-trip `source image → gw convert →
.scp → UFT decode` is also byte-identical to the committed
`corpus/sources/*` for these three — UFT's decode is not just *equal to
gw*, it is *correct*.

## The 3 GCR / Amiga formats — UFT-side decode not wired yet

These are **skipped**, not divergent — an honest "not tested yet", not
a claim that divergence is expected.

- `c64_gcr` / `apple2_525`: the UFT engine HAS the decoders
  (`flux_decode_gcr_c64`, `flux_decode_gcr_apple` in
  `src/flux/uft_flux_decoder.c`), but `uft_flux_decode.c` only wires
  the **uniform-geometry MFM** image-assembly path. C64 needs the
  zone-based sector-count table (21/19/18/17 sectors per track band);
  Apple II GCR needs its own sector layout. Wiring those is the next
  increment.
- `amiga_dd`: UFT has **no** Amiga-track flux decoder
  (`FLUX_ENC_AMIGA` is an enum value with no implementation —
  `flux_decode_mfm` finds no IBM IDAM/DAM in Amiga's whole-track MFM).
  This needs a dedicated Amiga decoder before a differential is
  possible.

The harness, corpus and report already cover all six — only the
helper-side decode/assembly for these three is outstanding.

## Reproduce

```bash
# 1. install gw (not on PyPI): github.com/keirf/greaseweazle release
export GW=/path/to/gw            # or put gw on PATH
# 2. run — conftest.py generates the corpus + builds the helper
python -m pytest tests/differential/ -v
```
