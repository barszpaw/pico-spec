# Timex SCLD hi-res mode 6 downsamples 512px to 256px via OR-merge AND gets the colour wrong — fix already exists in drewpo28/pico-speccy

## Opis

`VIDEO::MainScreen()`'s `timex_mode==6` handler (`src/Video.cpp`) renders real Timex hi-res
(SCLD alternates byte-columns from screen bank 0 and bank 1 at the same address, 512 real
horizontal pixels) by OR-merging the two banks' bytes into one, then rendering only 256
output pixels — **and** does it with the wrong colour: ink on plain black paper.

```cpp
uint8_t combined = grmem[bmpOffset++] | grmem[attOffset++];
*lineptr32++ = AluByte[combined >> 4][hires_att];
*lineptr32++ = AluByte[combined & 0xF][hires_att];
```

Real hi-res software (`moon-rabbit-zx` zxuno-80 80-column build, `TimexHiResExample`'s
`PortFF.tap`) draws assuming the full, independent 512px resolution — OR-merging two
different columns into one produces overlapping/garbled characters. Confirmed side-by-side:
correct on **Fuse**, broken on **pico-spec**, same files.

Separately, real SCLD hardware (and Fuse) render mode 6 as **bright ink on its own bright
complement as paper** — not ink on black. Confirmed independently three ways: (1) the
`PortFF.asm` source comment itself — `ld a,%00'001'110 ; 001 = set bright blue on yellow`
(ink=blue=`001`, paper=yellow=`~001&7=110`); (2) a screenshot of that program running:
blue text on a yellow background/border; (3) `moon-rabbit-zx` with ink=white renders
white-on-black (white's complement is black) — a classic-looking terminal, which is
correct, not a coincidence of a black-paper bug.

## The fix already exists in this author's other repo

`drewpo28/pico-speccy` (same author, "ESPectrum port" for RP2350) implements this
correctly — verified to render **identically to Fuse** — via a mechanism called
`timex_hires_live`/`timexHiresRequest`/`timexHiresApplyPending` in
`src/speccy/video/Video.cpp` and `src/speccy/core/Ports.cpp`. We ported it into pico-spec
and confirmed it fixes both issues on real hardware (see below). Summary of how it works,
for anyone wanting to port it the same way:

- Reuses the **existing Profi "DS80" packed-pair driver infrastructure**
  (`profi_pair_lookup`, `profi_ds80_driver_set`, `Graphics8BitPalette::ds80_active` —
  already in pico-spec for the unrelated Profi DS80 mode) instead of inventing a new
  mechanism: one packed-pair driver slot already encodes **two independent physical
  pixels** per framebuffer byte. A 16-entry LUT (`timex_hr_lut[16]`) maps every 4-bit
  nibble to two pair slots (ink or its bright complement, chosen per bit), so one input
  byte → one `uint32_t` write → 8 real pixels, at the *same* byte/write budget as the old
  OR-merge code (2 writes per byte-pair) or any other video mode.
- **Mode on/off is deferred to the frame boundary** (`timex_hires_pending_on/off`, applied
  in `EndFrame()` — the same vblank-safe pattern already used for DS80's own
  activate/deactivate and `profiPaletteApplyPending()`), because the driver's colour
  tables may only be rewritten outside active scanout. The framebuffer is cleared on
  transition so a frame's worth of bytes is never reinterpreted under the wrong mode.
- An in-mode **ink-only change is NOT deferred** (`timexHiresColour()`) — it only rebuilds
  the 16-entry LUT and the border colour, no driver/conv_color rewrite, so it's safe and
  beam-exact mid-frame (matches how a border colour change via `OUT (#FE)` already works).
- **Border colour in mode 6 is the bright paper colour**, not whatever the ULA border
  register says — easy to miss (we did, initially): `updateBorderBrd()` needs a
  `timex_hires_live` branch ahead of its normal border-colour logic.
- A colour-corrected 256px OR-merge fallback is kept for builds with no packed-pair driver
  (SOFTTV/TFT, or a refused DS80 snapshot allocation).

## Confirmed on real hardware

Waveshare RP2350-PiZero, pico-spec v1.2.30, custom build, `VGA_HDMI` driver.
- `PortFF.tap`: border/paper bright yellow, ink bright blue — matches Fuse exactly.
- `moon-rabbit-zx` zxuno-80 (real 80-column content): correct mono 80-column text
  (white-on-black, since its ink is white), no more character overlap.
- No interaction with unrelated subsystems (file-browser dialogs, keyboard) — tested
  together on the same build.
