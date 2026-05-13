# Nano Banana prompts for TouchRTTY documentation diagrams

Three prompts, one per diagram. Paste each block into Gemini (with
Nano Banana / Gemini 2.5 Flash Image active) one at a time. Save the
resulting PNG with the suggested filename into `docs/images/`.

If the first generation isn't quite right, the most useful follow-up
prompts are usually one of:

- `make the two parallel blocks side-by-side, not stacked vertically`
- `move the side captions closer to the block they describe`
- `use a darker / lighter background`
- `make the lines bolder`
- `remove the corner sparkles / decorations`

---

## 1. Signal flow

**Filename:** `docs/images/signal_flow.png`
**Used by:** `README.md`, `README.ru.md`, `docs/DEVELOPMENT_CONTEXT.md`,
`docs/DEVELOPMENT_CONTEXT.ru.md`

```
Create a clean technical block diagram. Strict flat-design, dark navy background (#0E1525), light blocks (#F4F6FA) with thin borders, teal stroke (#2DD4BF) for main path, amber stroke (#F59E0B) for parallel branches. Sans-serif sentence-case labels. No icons, no sparkles, no decorations whatsoever.

CRITICAL: the diagram fans out twice and merges back. Two PARALLEL branches MUST be drawn SIDE-BY-SIDE HORIZONTALLY, not stacked vertically. Use Y-shaped split and merge lines.

Vertical flow from top to bottom:

1. Single block at top: "ADC0 @ 10 kHz, 1.65 V biased"
2. ↓ Single block: "63-tap FIR bandpass, centred on FREQ"
3. ↓ Single block: "Quadrature (I/Q) demod → biquad LPF"
4. ↓ Arrow SPLITS into a Y-fork going down-left and down-right
5. Two blocks ON THE SAME HORIZONTAL ROW, separated by space:
   LEFT (amber border): "Path A — narrow"
   RIGHT (amber border): "Path B — wide"
6. Both arrows MERGE back into single block: "LLR fusion (HYB)"
   Small italic gray caption beside it: "default — run this"
7. ↓ Single block: "DPLL with PI loop"
   Small italic gray caption beside it: "controlled by ALPHA"
8. ↓ Single block: "Bit slicing → 7 soft bits"
9. ↓ Arrow SPLITS again into Y-fork
10. Two blocks ON THE SAME HORIZONTAL ROW:
    LEFT (amber border): "Hard decision (sign)"
    RIGHT (amber border): "B264 gate — if data_min / sig < 0.20, NN gets a vote"
11. Both arrows MERGE into single block: "32 Baudot codes"
12. ↓ Final block: "ITA-2 → ASCII"

Title above block 1: "TouchRTTY signal flow" in bold sans-serif.

Output 1200 × 1800 portrait, high-resolution, no corner sparkles, no glow, no gradients, no 3D, strict engineering flat design.
```

---

## 2. NN architecture

**Filename:** `docs/images/nn_architecture.png`
**Used by:** `docs/NN_TRAINING.md`, `docs/NN_TRAINING.ru.md`

```
Create a clean neural-network architecture diagram. Strict flat design, dark navy background (#0E1525), light blocks (#F4F6FA), teal accent strokes (#2DD4BF), warm amber (#F59E0B) for the final argmax block. Sans-serif sentence-case labels.

Layout: a horizontal pipeline showing a small MLP classifier, left to right.

From left to right:

1. Input column: a small vertical stack of 7 small circles (the 7 soft-bit inputs), labeled "input · 7 soft bits" below.
2. Arrow → labeled "w1, b1" above the arrow.
3. Dense layer: a vertical stack of 12 small circles (representing a "wide" hidden layer). Label below: "ReLU · 128 units".
4. Arrow → labeled "w2, b2" above.
5. Dense layer: a vertical stack of 8 small circles. Label below: "ReLU · 64 units".
6. Arrow → labeled "w3, b3" above.
7. Output column (amber): a small vertical stack of 10 small circles (representing 32 classes, just show ~10 visually). Label below: "argmax · 32 classes".
8. Final arrow → text on the right: "Baudot code (0–31)".

Caption at the bottom center: "7 → 128 → 64 → 32 MLP · ~44 KB float32 weights"

Title above the whole diagram: "TouchRTTY NN classifier" in bold sans-serif.

Use light teal lines between every neuron of adjacent layers to suggest full connectivity, but keep them subtle (low opacity, 0.5 px). Don't connect every dot to every dot — just show enough lines to communicate "fully connected".

Output 1800 × 900 landscape, high-resolution. No icons, no decorations, no gradients, no 3D, no sparkles. Strict flat infographic style.
```

---

## 3. Screen layout

**Filename:** `docs/images/screen_layout.png`
**Used by:** `docs/MENU_GUIDE.md`, `docs/MENU_GUIDE.ru.md`

```
Create a UI mockup of an embedded device screen. Strict flat design, dark navy background outside the screen (#0E1525), the screen itself dark (#0A0F1A) with teal border (#2DD4BF). Sans-serif labels.

Draw a single rectangular screen, 480 wide by 320 tall (proportional), centered. The screen is divided into 4 horizontal zones, top to bottom:

1. TOP BAR (40 px tall) — fill with a slightly lighter navy. Inside: three thin horizontal bars stacked vertically on the left side (SIG / AGC / ERR — fill the SIG bar 65% with teal, AGC 30% with amber, ERR 5% with green). To the right of each bar, light gray metric text: "SNR 14.2  SIG -12  M:1490 S:1660", "AGC x1.50  C0 8%  C1 28%", "BD 50  SH 450  ERR 3%".

2. MAIN ZONE (160 px tall) — fill with a vertical gradient from dark blue to slightly lighter. Draw two thin vertical orange bands (the RTTY mark and space tones) about 1/4 and 1/2 across the width. Scatter a few faint speckles of noise. Label in the middle in small light text: "MAIN ZONE — waterfall / spectrum / scope".

3. TEXT ZONE (80 px tall) — fill with very dark background. Show 5 lines of green monospace text simulating decoded RTTY:
   "RYRYRY THE QUICK BROWN FOX JUMPS OVER 1234567890"
   "DWD WETTER DEUTSCHLAND * KONTINENTALER HOCH*RUCK"
   "ZZCZC NAVTEX BROADCAST 0512 UTC GALE WARNING NORTH"
   "SEA AREA DOGGER 8 NW VEERING WEST 6 LATER ROUGH BECO"
   "MING MODERATE OCCASIONAL RAIN VISIBILITY MODERATE OR_"
   Render the asterisks * in red bold (these are decoded-frame errors).

4. BOTTOM BAR (40 px tall) — fill with slightly lighter navy. Draw 8 small rounded-corner buttons evenly spaced. Labels left to right: "B 50", "S 450", "ST 1.5", "NOR", "AFC", "SRCH", "CLR", "MENU". Highlight the "AFC" button with a teal border and teal text (to show it's active). Others have plain dark gray border with light text.

To the right of the screen, place 4 side annotations with thin connector lines pointing at each zone:
- "TOP BAR · 40 px — SIG / AGC / ERR + live metrics"
- "MAIN ZONE · 160 px — Waterfall · spectrum · scope"
- "TEXT ZONE · 80 px — Decoded RTTY · red * = ERR"
- "BOTTOM BAR · 40 px — 8 touch buttons"

Title above the screen: "TouchRTTY — screen layout (480 × 320 px)" in bold sans-serif.

Output 1600 × 1200 landscape, high-resolution. No icons, no decorations, no gradients on text, no glow, no 3D effects, strict flat engineering mockup style.
```

---

## Once all three PNGs are in `docs/images/`

Tell me when they're ready and I'll:

1. Replace the ASCII diagrams in `README.md`, `README.ru.md`,
   `docs/NN_TRAINING.md`, `docs/NN_TRAINING.ru.md`,
   `docs/MENU_GUIDE.md`, `docs/MENU_GUIDE.ru.md`,
   `docs/DEVELOPMENT_CONTEXT.md`, `docs/DEVELOPMENT_CONTEXT.ru.md`
   with `<img>` tags pointing to the new files.

2. Delete the placeholder `docs/images/image.png` (the v1 signal-flow
   attempt that's currently committed).

3. Delete my standby `docs/images/screen_layout.svg` (a fallback I
   drafted before you asked for Nano Banana — not needed now).
