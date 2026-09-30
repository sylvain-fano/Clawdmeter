# Asset tools

## Splash animations

```bash
node convert_official_clawd.js --in <assets dir>
node convert_official_clawd.js --in <assets dir> --verify /tmp/verify   # + per-animation PNGs
```

Converts the official Anthropic Clawd animations (Anthropic's assets, not
shipped in this repository; GIFs decoded via ImageMagick, the Laptop and
Soccer Lottie exports read directly) into a single
`firmware/src/splash_animations.h`:

- frames as bounding-box crops on the shared 55×37 art stage, one byte per
  cell into a per-animation ≤16-color RGB565 palette (index 0 = background)
- per-frame hold in ms, with consecutive duplicate frames collapsed
- a detected loop region per animation (gait cycles, scene middles) that the
  engine can hold or release for walk-to-target and timed scenes
- the eyes — transparent holes in the source GIFs — inked as `#141413`
- contrast recolors (trumpet notes → ivory, magnifier fedora → gray) and the
  sailing-loop cross-match that defines the sailing scene's loop window

`--out` overrides the output path. Rebuild firmware after running.

## Icons

```bash
node png_to_lvgl.js input.png symbol_name [W_MACRO] [H_MACRO] [--tint=RRGGBB | --no-tint]
```

Converts an alpha PNG to an LVGL RGB565A8 C array. Default tint is white —
Lucide PNGs ship black-on-transparent and would render invisible without it.
Paste the output into `firmware/src/icons.h`.
