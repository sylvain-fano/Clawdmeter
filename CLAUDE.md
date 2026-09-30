# Project context

ESP32-S3 / ESP32-C6 firmware for a desk-side Claude Code usage monitor. Each
supported board lives in its own `firmware/src/boards/<name>/` folder and is
selected via PlatformIO's `build_src_filter`. Adding a board means dropping in
a new folder + a new `[env:...]` block — `main.cpp`, `ui.cpp`, and `splash.cpp`
never see board-specific code. See [`docs/porting/adding-a-board.md`](docs/porting/adding-a-board.md).

Seven ports today (two SoC families, five panel sizes):

- `boards/waveshare_amoled_216/` — original Waveshare ESP32-S3-Touch-AMOLED-2.16 (CO5300, 480×480 square, CST9220 touch, IMU rotation). Build env: `waveshare_amoled_216`.
- `boards/waveshare_amoled_18/` — Waveshare ESP32-S3-Touch-AMOLED-1.8 (368×448 portrait, XCA9554 IO expander). Build env: `waveshare_amoled_18`. **Two panel revisions are auto-detected at boot** (`board_rev()` in `board_init.cpp`, enum in `board_rev.h`): original = SH8601 display + FT3168 touch (0x38); later = CO5300 display + CST816 touch (0x15). One binary drives both.
- `boards/waveshare_amoled_216_c6/` — Waveshare ESP32-C6-Touch-AMOLED-2.16 (SH8601, 480×480, CST9217 touch). Build env: `waveshare_amoled_216_c6`. ESP32-C6 SoC: single-core RISC-V, **no PSRAM**, BLE 5 only.
- `boards/waveshare_amoled_18_c6/` — Waveshare ESP32-C6-Touch-AMOLED-1.8 (368×448 portrait, SH8601, FT3168 touch, TCA9554 expander). Build env: `waveshare_amoled_18_c6`. Same panel as the S3 1.8 but on the C6 SoC. All subsystems (display, touch, BOOT + PWR buttons, battery, BLE) verified on hardware.
- `boards/waveshare_amoled_206/` — Waveshare ESP32-S3-Touch-AMOLED-2.06 (CO5300, 410×502 watch form factor, FT3168 touch, no IO expander, 32 MB flash, PCF85063 RTC, ES8311 codec). Build env: `waveshare_amoled_206`. Display, touch, battery, IMU init, and BLE verified on hardware; the ES8311 chime path is not wired up (`sound.cpp` no-ops).
- `boards/waveshare_lcd_154/` — Waveshare ESP32-S3-Touch-LCD-1.54 (ST7789, 240×240 square, CST816T touch @ 0x15). Build env: `waveshare_lcd_154`. **The first non-AMOLED port**: a plain 4-wire SPI TFT, not QSPI, and the panel has no brightness command — backlight is LEDC PWM on `LCD_BL`. **No PMU**: battery is an ADC divider on GPIO1 and `BAT_EN` (GPIO2) is a power-hold line that must be driven HIGH early in `board_init()` or the board browns out on battery. Three buttons (BOOT + GPIO5 + a PWR-role GPIO4); ES8311 chime wired up; QMI8658 populated but unused (fixed orientation, no rotation).
- `boards/waveshare_lcd_4/` — Waveshare ESP32-S3-Touch-LCD-4 (ST7701 RGB parallel, 480×480 square, GT911 touch). Build env: `waveshare_lcd_4`. **RGB-panel port**: Arduino_ESP32RGBPanel + bounce buffers (tearing fix). IO expander @ 0x24 (TCA9554 / CH32V003) must init before `gfx->begin()` or the panel stays dark; backlight is expander pin 2 (on/off only). No AXP2101 / IMU; KEY/PWR is hardware RST. Single BOOT button (GPIO 0 → Space/PTT).

Plus one non-hardware target: `boards/sim/` — **native desktop simulator** (SDL2 window, 480×480, `platform = native`). Build env: `sim`. See "Desktop simulator" below.

**C6 ports have no PSRAM** — shared code gates on `BOARD_HAS_PSRAM` (absent on C6) to use `MALLOC_CAP_INTERNAL` for LVGL/splash buffers, and the `screenshot` serial command is disabled (`LV_USE_SNAPSHOT=0`), so UI changes on a C6 board must be eyeballed on hardware, not auto-captured. The C6 2.16 also has an asynchronous DMA pixel path and renders big-endian (`BoardCaps.be_pixels`) — see "Frame rate on PSRAM-less boards".

The shared code calls a small HAL (`firmware/src/hal/`) that each board implements: display, touch, input, power, IMU. Optional features are guarded by `BoardCaps` (runtime) and `BOARD_HAS_*` (compile-time) rather than `#ifdef BOARD_*`.

Connects to a host daemon over BLE; daemon polls Anthropic API for usage data. This file is for future Claude Code sessions to bootstrap quickly. Read this first.

## Hardware (critical pins)

### AMOLED-2.16 (original)
- Display: **CO5300** AMOLED via QSPI (CS=12, SCLK=38, SDIO0..3=4..7, RST=2)
- Touch: **CST9220** via I2C (SDA=15, SCL=14, INT=11, addr=0x5A)
- PMU: **AXP2101** on same I2C bus (addr=0x34) — battery, USB VBUS, PWR button IRQ
- IMU: **QMI8658** on same I2C bus (addr=0x6B) — accelerometer for auto-rotation
- Buttons: GPIO 0 (left → Space/voice-mode), GPIO 18 (right → Shift+Tab/mode-toggle), AXP PKEY (middle → cycle screens; on splash → cycle animations)

### AMOLED-1.8 (newer port)
**Two hardware revisions ship under this name; the firmware probes I2C at boot and picks drivers automatically (`board_rev()`):**
- Display: **SH8601** (original) or **CO5300** (later rev) AMOLED via QSPI (CS=12, **SCLK=11** ← different!, SDIO0..3=4..7, RST routed via XCA9554 EXIO1). Both are `Arduino_OLED` subclasses held behind one base pointer in `display.cpp`. The CO5300's 368-wide active area starts at GRAM column 16, so it gets `CO5300_COL_OFFSET 16` to center; SH8601 needs none.
- Touch: **FT3168** @ 0x38 (original) or **CST816** @ 0x15 (later rev), via I2C (SDA=15, SCL=14, INT=21). Both expose the same FocalTech-style data layout at regs 0x02..0x06, so one inline reader in `touch.cpp` serves both — only the address differs. Avoids vendoring the GPLv3 `Arduino_DriveBus` library. Revision is detected by which touch address ACKs (CST816 present ⇒ CO5300 panel).
- PMU: AXP2101 @ 0x34 (same chip as 2.16 — `XPowersLib` reused; battery is an optional kit add-on but PMU + charging circuitry are populated)
- IMU: QMI8658 @ 0x6B (same chip — initialized for I2C bus health, rotation logic disabled)
- IO expander: **XCA9554 / PCA9554** @ I2C 0x20. Gates LCD_RST, TP_RST, audio amp enable, and reads the PWR button. **`io_expander_init()` MUST run before `gfx->begin()` or `ft3168_init()`** — otherwise display/touch stay in reset and silently fail. PWR button is on EXIO4, active HIGH (verified empirically with the deleted `iox` serial debug command).
- Orientation: **fixed at 0°**. IMU auto-rotation is disabled; `rotate_strip()` / `handle_rotation_change()` are excluded via `#ifndef BOARD_AMOLED_18`.
- Buttons: GPIO 0 (BOOT → Space/voice-mode), XCA9554 EXIO4 (PWR → cycle screens; on splash → cycle animations). **No third button** (GPIO 18 button doesn't exist on this board).

### AMOLED-1.8 (C6) — `waveshare_amoled_18_c6`
ESP32-C6 sibling of the S3 1.8: same 368×448 SH8601 panel + FocalTech touch, different SoC and GPIO map. **All pins/edges below verified on hardware via temporary GPIO/IRQ scans, since Waveshare's wiki publishes no pin table and the third-party BSP's numbers were partly wrong.**
- Display: **SH8601** AMOLED via QSPI (CS=5, SCLK=0, SDIO0..3=1..4, no MCU reset pin — internal POR; effective reset is the TCA9554 power-cycle). Stock `Arduino_SH8601` init (no vendor-register patch — that's only needed on the C6 2.16).
- Touch: **FT3168** (some units FT6146) @ I2C 0x38, INT=15. Same inline FocalTech reader as the S3 1.8 (regs 0x02..0x06); no reset pin (gated by TCA9554 touch power).
- I2C bus: SDA=8, SCL=7 (shared by TCA9554, AXP2101, FT3168, QMI8658, PCF85063 RTC, ES8311 codec).
- IO expander: **TCA9554 / PCA9554** @ 0x20 — here it gates **power**, not reset: **P4 = display power, P5 = touch power, P7 = audio amp**. `io_expander_init()` runs the documented power-on sequence (P4/P5 LOW → 200 ms → HIGH) and **MUST run before `display_hal_init()`** or the panel stays unpowered. Amp (P7) left off (no audio path).
- PMU: AXP2101 @ 0x34 (owned by `power.cpp`, not `board_init` — LCD isn't on an ALDO rail here).
- IMU: QMI8658 @ 0x6B (init'd for bus health, rotation disabled).
- Orientation: **fixed at 0°**, no rotation (no PSRAM headroom).
- Buttons: **GPIO 9** (BOOT → Space/voice-mode, active LOW — *not* the docs' GPIO 0/9 guess; confirmed by scan), **AXP2101 PKEY** (PWR → cycle screens; on splash → cycle animations). The PKEY **SHORT-press IRQ fires on release** — that's the edge `power.cpp` acts on. No secondary button.

### AMOLED-2.06 (watch form factor) — `waveshare_amoled_206`
- Display: **CO5300** AMOLED via QSPI (CS=12, **SCLK=11** ← same as 1.8, SDIO0..3=4..7, RST=8 direct GPIO). 410×502 portrait. Requires **`col_offset1 = 23`** in the `Arduino_CO5300` constructor — the panel's visible viewport sits at a 22–23 column offset inside the controller's internal RAM. Without it, a vertical strip of stale/garbage content shows through on the right edge (23 was picked empirically for centering; Waveshare's reference library uses 22). The 2.16 dodges this because its 480×480 viewport fills the controller's RAM.
- Touch: **FT3168** via I2C (SDA=15, SCL=14, **INT=38, RST=9** direct GPIO, addr=0x38). Same inline FocalTech reader as the 1.8 port (no GPLv3 `Arduino_DriveBus` dependency). Coordinates verified end-to-end with the BLE reset zone.
- PMU: AXP2101 @ 0x34 (same chip as 2.16/1.8 — `XPowersLib` reused). PWR button routes through AXP PKEY IRQs (short / long / positive), same path as the 2.16 — no IO expander.
- IMU: QMI8658 @ 0x6B (initialized for I2C bus health; rotation logic disabled — fixed watch enclosure orientation).
- RTC: **PCF85063** on the same I2C bus, powered through AXP2101 for retention. Not used by Clawdmeter but present for future features.
- Audio codec: **ES8311** + ES7210 ADC on the same I2C bus. The amp path is unverified on this board, so `sound.cpp` no-ops (same posture as the C6 1.8) — the shared `chime.cpp` engine is ready to wire up once it's tested on hardware.
- **No IO expander** despite the Waveshare wiki FAQ implying one. The schematic shows Key3/PWR wired directly to AXP2101 PWRON; touch reset and display reset are direct GPIOs. `board_init()` pulses LCD_RESET (GPIO 8) and TP_RESET (GPIO 9) before display/touch HAL init.
- Buttons: GPIO 0 (BOOT → Space/voice-mode), AXP PKEY (PWR → cycle screens; hold-to-pair). **No third button**.
- Flash: 32 MB. Uses `default_32MB.csv` partition table.

### LCD-4 — `waveshare_lcd_4`
- Display: **ST7701** 480×480 RGB parallel (DE=40, VSYNC=39, HSYNC=38, PCLK=41, R0-4=46/3/8/18/17, G0-5=14/13/12/11/10/9, B0-4=5/45/48/47/21); ST7701 init via SW SPI (CS=42, SCK=2, MOSI=1).
- Touch: **GT911** via I2C (SDA=15, SCL=7), polled (wiki INT=GPIO 16 unused). Probe 0x5D then 0x14.
- IO expander: **addr 0x24** (fallback 0x20) on the same I2C bus — must init before `gfx->begin()` (output 0xFF, config 0x3A). Backlight is expander pin 2.
- No PMU / IMU. Buttons: GPIO 0 only (BOOT → Space/PTT). KEY/PWR is EN/RST (hardware reset). GPIO 18 is display R3.
- RGB tearing fix: pass `bounce_buffer_size_px = LCD_WIDTH * 10` to `Arduino_ESP32RGBPanel`. Do not call `rgbpanel->getFrameBuffer()` after `gfx->begin()`.

## Architecture

```text
firmware/src/
  hal/                      — board-agnostic interfaces shared code calls into
    board_caps.h            — runtime BoardCaps struct (W, H, button_count, has_* flags)
    display_hal.h           — init / begin / set_brightness / draw_bitmap / tick / round_area
    touch_hal.h             — init / read(&x, &y, &pressed)
    input_hal.h             — init / is_held(PRIMARY|SECONDARY)
    power_hal.h             — init / tick / battery_pct / is_charging / pwr_pressed (edge)
    imu_hal.h               — init / tick / rotation_quadrant
  boards/
    waveshare_amoled_216/   — CO5300 + CST9220 + AXP PKEY + QMI8658 rotation
    waveshare_amoled_18/    — SH8601 + FT3168 + AXP + XCA9554 (PWR via EXIO4), no rotation
    waveshare_amoled_216_c6/— C6: SH8601 + CST9217 + AXP PKEY, no PSRAM
    waveshare_amoled_18_c6/ — C6: SH8601 + FT3168 + AXP PKEY + TCA9554 (gates power), no PSRAM
    waveshare_amoled_206/   — CO5300 + FT3168 + AXP PKEY, no IO expander, 32 MB, no rotation
    waveshare_lcd_154/      — ST7789 SPI TFT + CST816T + ADC battery (no PMU), PWM backlight
    waveshare_lcd_4/         — ST7701 RGB parallel + GT911 + expander backlight, no PMU/IMU
    sim/                    — native desktop simulator: SDL2 + Arduino shims + scenario playback
    template/               — copy this to bootstrap a new port
  main.cpp                  — setup() + loop(): HAL calls only, zero #ifdef BOARD_*
  ui.{h,cpp}                — UI. Header (title, corner mascot, battery widget) is static chrome; everything else lives in `body`, a clipping viewport. Inside it the USAGE LEVEL (`level`) slides vertically against `settings_container` (Settings lives below: swipe up from anywhere on the level, swipe down from any settings page = home). The level holds TWO horizontal pages — `page_stats` (the Claude app's stats card: 8 tiles + a 24-week heatmap painted by ONE draw callback, fed by the daemon's "st" beat) ◂ `usage_container` (pair/idle/live sub-views, Weekly card faces) — plus the ticker `lbl_anim` (live companion state; green "Ready" + breathing dot when a session waits for you) and two page dots that only appear around a horizontal gesture (`level_dots_show()`/fade). Settings has five pages (clock picker + battery/mascot toggles / brightness & sleep sliders + status toggle + pairing / Weekly card: default-face picker + flip slider / Alerts: chime picker Off·Needs you·All + Preview, Volume slider + Glow toggle / About). One swipe engine for both axes hooked on the LVGL indev (PRESSED/RELEASED + per-tick polling): surfaces follow the finger 1:1, ONE lv_anim snaps leaving+arriving surface in lockstep, the header title cross-fades (`render_title()` decides the text: Settings / the Working page's state word / Trend / clock or Usage — it rewrites only on change, so it is called freely). Alerts: `alert_start(kind)` wakes the panel, breathes the glow — ONE full-screen transparent object whose LV_EVENT_DRAW_MAIN callback paints GLOW_RINGS nested rounded borders (`lv_draw_border`) at the glass's corner radius (`BoardCaps.corner_radius` = 70 px on the 2.16, measured with the serial `corners on` overlay; `radius N` overrides live); the anim invalidates only the edge bands + corner squares (`glow_invalidate()`), never the whole screen — cycles jump/wave/point on the HEADER mascot (on PSRAM-less boards the corner Clawd is a `splash_actor_*` canvas, 84×90 px at 3 px/cell, still pose otherwise) and chimes per settings; a tap (`global_click_cb`) acknowledges. No page ever changes by itself. compute_layout() picks fonts/positions from board_caps() (breakpoints: H >= 460 large, H >= 300 compact, else small; `#ifndef BOARD_HAS_PSRAM` shrinks the actor cell and idle creature). Accent = the one "active" colour. Controls are ≥ 13 mm touch targets on the 39 mm panel; never go back to 46 px rows. Fonts: the Styrene bitmaps carry NO middle dot/bullet/ellipsis (mono_32 has · and …) — use commas and ASCII in labels.
  settings.{h,cpp}          — NVS-backed user prefs (clock mode, battery/mascot/status visibility, sleep timeout, Weekly-card flip interval + default face, companion Auto-switch / Glow / chime mode); pushes the sleep timeout into idle. brightness.cpp stores a continuous level ("brt_lvl", migrates the old preset index) in the same "clawdmeter" namespace; the slider previews live and persists on release, the PWR button steps presets.
  version.h                 — FW_VERSION shown on the About page
  splash.{h,cpp}            — pixel-art engine: full-screen splash (direct-to-panel on C6), the mini idle creature, the PSRAM corner mascot, and the ACTOR (`splash_actor_*`): a switchable RGB565 canvas sized once for a set of animations (the Working page mascot — laptop while working, dancing while compacting, waving once when done, jumping happy/waving/pointing cycled by an alert). Loop mode holds the loop region, once mode ends on the still pose (walking frame 0).
  ble.{h,cpp}               — NimBLE peripheral: custom data service + HID keyboard (rx buffer 512 B; the daemon uses a long write when a payload exceeds MTU-3)
  chime.{h,cpp}             — ES8311 + I2S engine shared by every board with a speaker: the embedded reset bell (bell_pcm.h) and `chime_play_alert(kind)`, a fixed-point additive synth (1024-entry Q15 sine table, 1 KB blocks, no FPU needed) — kind 0 two rising notes E5→A5, kind 1 one C5, at a lower codec volume than the bell
  data.h                    — UsageData (+ ScopedWeekly[] from "ws"; has_usage tells a companion-only beat apart), CompanionData ("cc": CompanionState 0 none · 1 idle · 2 thinking · 3 tool · 4 done · 5 attention · 6 error · 7 compacting · 8 turn done, label/project/model/host, sessions/attention/agents, elapsed), TrendData ("tr": 24 hourly maxima + 7 daily values, -1 = gap)
  icons.h                   — icon arrays. Battery (5×, RGB565A8) are no longer drawn — the header battery is an LVGL-drawn widget (make_battery_widget(): outline + fill + percentage); rest are raw RGB565.
  logo.h                    — 80×80 RGB565 logo
  font_*.c                  — pre-compiled LVGL 9 bitmap fonts (Tiempos 56/34, Styrene 48/28/24/20/16/14/12, Mono 32/18)
  splash_animations.h       — generated, do not hand-edit
docs/porting/               — adding-a-board.md, hal-contract.md, capability-flags.md
companion/                  — Claude Code hooks: hooks.json (15 events, one async curl line each), install-hooks.py (merges into ~/.claude/settings.json, idempotent, --url/--uninstall), install-hooks.sh (curl | sh for remote hosts), plugin/ (Claude Code plugin with the same hooks; .claude-plugin/marketplace.json at the repo root lists it)
```

Each board folder contains: `board.h` (pins, I2C addresses, `BOARD_HAS_*` flags),
`board_init.cpp` (Wire.begin + any IO expander), `display.cpp`, `touch.cpp`,
`input.cpp`, `power.cpp`, `imu.cpp`, `caps.cpp` (the `BoardCaps` instance), plus
any board-private hardware drivers (e.g. `io_expander.{h,cpp}` on AMOLED-1.8).
PlatformIO's `build_src_filter` includes shared code + one board's folder per env.

## Build / flash

```bash
pio run -d firmware -e waveshare_amoled_216                                     # build 2.16 (S3, default original)
pio run -d firmware -e waveshare_amoled_18                                      # build 1.8 (S3)
pio run -d firmware -e waveshare_amoled_216_c6                                  # build 2.16 (C6)
pio run -d firmware -e waveshare_amoled_18_c6                                   # build 1.8 (C6)
pio run -d firmware -e waveshare_amoled_206                                     # build 2.06 (S3, watch)
pio run -d firmware -e waveshare_lcd_154                                        # build 1.54 (S3, SPI TFT)
pio run -d firmware -e waveshare_lcd_4                                           # build LCD-4 (S3, RGB TFT)
pio run -d firmware -e waveshare_amoled_18 -t upload --upload-port /dev/cu.usbmodem101   # flash 1.8 on macOS
pio run -d firmware -e waveshare_amoled_216 -t upload --upload-port /dev/ttyACM0         # flash 2.16 on Linux
# C6 boards: same native USB-JTAG flashing; flag a chip mismatch ("This chip is ESP32-C6,
# not ESP32-S3") means you picked an S3 env — use a *_c6 env for C6 hardware.
```

If `pio` isn't on PATH: try `~/.platformio/penv/bin/pio` (Linux/macOS pio install) or `brew install platformio` on macOS.

Device path differs by OS: `/dev/cu.usbmodem*` on macOS, `/dev/ttyACM0` on Linux. Both expose the ESP32-S3 native USB-JTAG (no boot-mode dance needed).

## Desktop simulator (`-e sim`) — develop UI without hardware

```bash
sudo apt install libsdl2-dev   # once (macOS: brew install sdl2)
pio run -d firmware -e sim && (cd firmware && .pio/build/sim/program)
```

An SDL2 window stands in for the 480×480 panel; the **full firmware loop runs
unmodified** — `main.cpp`, `ui.cpp`, `splash.cpp`, idle fade, pair gesture,
JSON parsing, usage-rate/chime logic. Only `ble.cpp`/`chime.cpp` are swapped
for stubs. How it works: `boards/sim/` implements the HAL against SDL2, thin
Arduino shims live in `boards/sim/shim/` (`millis`/`Serial`→stdio,
`heap_caps`→malloc, in-memory `Preferences`), and `ble_sim.cpp` plays back
daemon payloads from `firmware/sim/scenario.jsonl` (one JSON line per state +
optional `name`/`hold_ms`; override with `SIM_SCENARIO=<path>`).

Controls (full map in `boards/sim/board.h`): mouse = touch · space =
play/pause scenario · ←/→ = step · 1-9 = jump · d = BLE link toggle ·
b/n = BOOT/secondary buttons · p = PWR · c/-/= = charging/battery ·
s = screenshot BMP · esc = quit.

Headless screenshots (works in CI, no display):
`SDL_VIDEODRIVER=dummy SIM_AUTOSHOT_MS=6000 .pio/build/sim/program` saves
`sim-autoshot.bmp` (or `SIM_AUTOSHOT_PATH`) after 6 s and exits. Combine with
the boot-screen swap trick below to capture any screen. **The sim renders with
desktop LVGL and fake data — always do a final check on real hardware before
merging panel-related changes** (col offsets, rotation, rounding live in the
hardware boards, not shared code).

## QA your own UI changes — don't ask the user

The firmware ships a `screenshot` serial command that dumps the LVGL framebuffer. `./screenshot.sh out.png [port]` captures a PNG sized to the active display (480×480 or 368×448). **Use this on every UI iteration** — Read the PNG with the Read tool, verify the change visually, iterate. Script auto-picks the macOS/Linux default port and falls back to pio's bundled Python if pyserial isn't on the system Python.

The boot screen is `SCREEN_SPLASH` and only advances on a tap/button, so a fresh flash will sit on the splash. Drive the screens without touching the device with the serial commands **`page splash|usage|settings|settings2|about`** and **`flip`** (next Weekly face) (works on every board, C6 included — the C6 has no framebuffer screenshot, but every parsed payload logs a `usage: s=…% w=…% scoped=…` line and the boot banner reports free heap + LVGL pool usage). In the simulator, `SIM_PAGE=settings` (env) or a `"_sim":{"page":"about"}` key on a scenario line does the same for headless captures:

Serial QA commands: `page splash|usage|working|trend|settings|settings2|settings3|settings4|about`, `flip`, `swipe up|down [ms]`, `stats` (frames / avg / max ms + free heap), `cc <state> [label]` (fake a companion payload: 3 = tool, 5 = needs you → glow + chime + slide, 8 = long turn done), `trend demo`, `alert 0|1` (chime only), `preview` (the whole needs-you alert), `buzz` (reset bell). Sim: `SIM_PAGE=splash|usage|working|trend|settings|settings2..4|about`; scenario lines may carry `"cc"` / `"tr"` (see firmware/sim/scenario.jsonl); with a working/attention `cc` state the sim auto-switches to the Working page like the device, so photograph other pages with `cc.s` = 1 or 4.

```bash
SDL_VIDEODRIVER=dummy SIM_AUTOSHOT_MS=2500 SIM_PAGE=settings SIM_SCENARIO=path/to/one-state.jsonl .pio/build/sim/program
magick sim-autoshot.bmp shot.png   # then Read the PNG
```

## Critical gotchas

1. **CO5300 cannot rotate.** Its MADCTL only supports axis flips, not column/row exchange. Rotation is done by **CPU pixel remapping inside `display_hal_draw_bitmap`** in `boards/waveshare_amoled_216/display.cpp`. We use **PARTIAL render mode with strip rotation** (small 480×40 strips, fast). On rotation change → AMOLED brightness flash → force redraw (handled inside `display_hal_tick`).
2. **OPI PSRAM** required: `board_build.arduino.memory_type = qio_opi` in platformio.ini. Without this, `MALLOC_CAP_SPIRAM` returns NULL and the screen is black.
3. **pioarduino platform required.** GFX Library for Arduino needs Arduino Core 3.x (`esp32-hal-periman.h`), not the 2.x that standard `espressif32` ships. We pin `pioarduino/platform-espressif32` 55.03.38-1.
4. **LVGL 9 font patching.** `lv_font_conv` outputs LVGL 8 format. Must remove `#if LVGL_VERSION_MAJOR >= 8` guards, drop `.cache` field, add `.release_glyph`, `.kerning`, `.static_bitmap`, `.fallback`, `.user_data`. Without patching, fonts render invisible. Full regeneration recipe: `docs/fonts.md`.
5. **Touch reading is centralized inside each board's `touch.cpp`.** The HAL `touch_hal_read()` is called once per loop from `my_touch_cb`; the board's implementation owns its latched `touch_pressed/x/y` state. Don't call the underlying controller from anywhere else — CST9220's `getPoint()` etc. do a full I2C transaction and concurrent callers consume each other's data.
6. **Even-aligned flush regions.** `display_hal_round_area` (called from `rounder_cb`) is what each board uses to enforce this. Required on CO5300, harmless on SH8601.
7. **Touch axis swap/mirror is per-board.** The 2.16's CST9220 needs `setSwapXY(true)` + `setMirrorXY(true, false)` — applied inside `boards/waveshare_amoled_216/touch.cpp::touch_hal_init()`. New ports apply their own.
8. **LVGL RGB565A8 is planar.** `w*h` RGB565 pixels followed by `w*h` alpha bytes; `data_size = w*h*3`, `stride = w*2`. Use `init_icon_dsc_rgb565a8()` for icons that overlap non-uniform backgrounds (e.g. battery over splash). Lucide source PNGs are black-on-transparent — converter must tint to white or icons render invisible. See `tools/png_to_lvgl.js`.
9. **Per-board pre-init is `board_init()`.** Each board's `board_init.cpp` brings up `Wire` and any reset-gating IO expander BEFORE `display_hal_init()`. Skipping the IO expander release on AMOLED-1.8 leaves SH8601 + FT3168 in reset and they silently fail to probe. Same for LCD-4: expander @ 0x24 must run before `gfx->begin()` or the ST7701 stays dark.
10. **No `#ifdef BOARD_*` in shared code.** The whole point of the refactor — if you're about to add one, you probably want a `BoardCaps` field or a per-board file instead. See `docs/porting/capability-flags.md`.
11. **LCD-4 RGB bounce buffers.** `Arduino_RGB_Display` DMA-scans PSRAM. Pass `bounce_buffer_size_px = LCD_WIDTH * 10` so ESP-IDF allocates SRAM bounce buffers. Do not call `rgbpanel->getFrameBuffer()` after `gfx->begin()` — it constructs a second RGB panel and crashes.
12. **LCD-4 has only one user button (GPIO 0 / BOOT).** GPIO 18 is display R3. KEY/PWR is EN/RST (hardware reset). Hold-to-pair and PWR-short animation/brightness cycling are unavailable; tap the panel to toggle splash ↔ usage.

## Frame rate on PSRAM-less boards (C6) — the transition playbook

Measured on the C6 2.16 with the `stats` / `swipe up|down [ms]` serial commands
(frame = LVGL render + flush; a screen transition repaints the 380-row body):

| step | frame | notes |
|---|---|---|
| baseline: 2×20-line strips, `-Os`, blocking `writePixels` | 95 ms (~10 fps) | 24 strips → 24 object-tree walks per frame |
| one 120-line strip, header static + body viewport | 63 ms | fewer strips + 21% fewer pixels |
| `-O2` (Arduino core defaults to `-Os`) | 53 ms (flush 31) | render 45 → 22 ms |
| in-place swap + raw `writeBytes` / 16 KB chunks | 50 ms | flush is wire-bound, not overhead |
| QSPI 80 MHz (`-DESP32QSPI_FREQUENCY=80000000`) | 39 ms (flush 17) | electrically fine; visual check pending |
| async DMA flush (2nd SPI device, queued 32 KB transactions) + 2×60-line buffers + LVGL `flush_wait_cb` | 36 ms (flush wait 9) | flush overlaps render |
| LVGL renders `RGB565_SWAPPED` (`BoardCaps.be_pixels`) + `-O3` | **30 ms (~33 fps)** | no per-pixel swap anywhere |

Rules that fall out of this: never call `lv_display_flush_ready()` in the flush
callback (LVGL clears the flag after `display_hal_wait()`), keep the C6 SPI
transactions ≤ 32 KB (hardware limit — the failure mode is a hang in the wait),
and treat `heap free` in the boot banner as the budget (2×60-line buffers leave
~108 KB with BLE up). The horizontal page drags and the finger-follow use the
same path, so they benefit equally.

## Icons

`tools/png_to_lvgl.js <input.png> <symbol> [W_MACRO] [H_MACRO] [--tint=RRGGBB | --no-tint]` converts an alpha PNG to RGB565A8. Default tint is white (`0xFFFFFF`) — necessary for Lucide PNGs. Splice output into `firmware/src/icons.h` and use `init_icon_dsc_rgb565a8()` in ui.cpp. Currently only the 5 battery icons use this format; the rest are still raw RGB565 baked over the panel background, fine because they live inside opaque zones.

## Splash animations

17 official Anthropic Clawd animations (core poses + persona scenes), already
converted into `firmware/src/splash_animations.h` (generated, do not hand-edit).
The source assets are Anthropic's and are **not shipped in this repository**;
to regenerate, point the converter at a local copy:

```bash
node tools/convert_official_clawd.js --in <assets dir>                 # -> firmware/src/splash_animations.h
node tools/convert_official_clawd.js --in <assets dir> --verify DIR    # + per-animation PNGs for eyeballing
```

Requires ImageMagick; Laptop and Soccer convert from their Lottie exports
(crisp) rather than GIFs. Frames are bounding-box crops on the official 55x37
art stage (ox/oy = stage offset; every animation shares one idle-Clawd
position, so transitions are seamless), one byte per cell into a per-animation
<=16-color RGB565 palette (index 0 = background, true black), per-frame hold ms
with duplicates collapsed (~400 KB total). The converter also detects each
animation's **loop region** (gait cycles, scene middles), synthesizes the
**eyes** (transparent holes in the source GIFs) as `#141413` ink via border
flood-fill, and applies two contrast recolors (trumpet notes -> ivory,
magnifier fedora -> gray) via component analysis.

The splash engine (`splash.cpp`) plays intro -> loop -> outro on a **60x60
stage** (`SPLASH_GRID`, cell = min(W,H)/60 -> 8 px on 480, 6 px on 368, 4 px on
240): loops hold until released (walk arrival, scene timer, rotation), so
switches always pass through the shared idle pose. Walkers translate with
foot-locked per-frame schedules and mirror when heading left. Usage-rate
groups pick animations by name; the same rate drives the **corner mascot** on
the usage screen (`splash_mascot_*`, PSRAM boards; C6 falls back to the static
`clawd_still.h` icon): idle stills, rate-scaled acts, and walk-off/lurk/
walk-back trips. Default boot screen.

## Companion (Claude Code hooks), alerts, stats

**Data path.** Claude Code hooks (`companion/hooks.json`: SessionStart,
UserPromptSubmit, PreToolUse, PostToolUse(+Failure), PermissionRequest/Denied,
Notification, Stop, StopFailure, SubagentStart/Stop, PreCompact/PostCompact,
SessionEnd) run one `curl` line each, `async: true`, that POSTs the hook's stdin
JSON to the listener `http://<bridge or hub>:47393/hook`. Headers:
`X-Clawdmeter-Host` (short hostname, blanked for the listener's own machine),
`X-Clawdmeter-Pid: $PPID` (a hook's parent is the Claude Code process itself),
`X-Clawdmeter-User`; the token goes in `X-Clawdmeter-Token` or the path
(`/hook/$CLAWDMETER_TOKEN`). The listener logs `rejected a hook from <host>`
(once per 10 min per peer), `<host> joined` for the installers' hello, and
`first hook from <host>`, and reminds after `JOIN_REMINDER_S` when a joined
host sent nothing: without these logs a mis-installed worker fails silently.
The usual cause is installing the hooks as a different user than the one
Claude Code runs as (hooks live in that user's `~/.claude/settings.json`).

`daemon/companion.py` keeps a session table keyed by `session_id` and maps
events to states (UserPromptSubmit -> thinking; PreToolUse -> tool with
`tool_label()`; AskUserQuestion / ExitPlanMode / PermissionRequest /
Notification permission_prompt -> attention; Stop -> turn done (>= LONG_TURN_S
20 s) or quiet done; StopFailure -> error; PreCompact -> compacting; subagent
events only count). It picks the headline session (attention > working > done >
idle; equal ones rotate every `HEADLINE_ROTATE_S` = 8 s) and emits `"cc"`;
`signature()` ignores elapsed seconds so only real changes push. A remote
headline's host shows as a small caption above the ticker. Liveness: a hook
from the listener's own machine carries a PID, and `watchable_pid()` watches it
if it is alive and Claude-like, so the session lives exactly as long as that
process. Other machines use timers (30 min silent while working, 12 h idle,
`IDLE_STALE_S`). `pid_alive()` never uses `os.kill` on Windows (it would
terminate the target). The table persists to `companion-state.json` and is
restored at start (dead PIDs dropped, all watched sessions dropped after a
reboot). With no session the ticker reads a dim "Idle"; the whimsical words
appear only when no bridge has ever reported.

`daemon/stats.py` scans `~/.claude/projects/*/*.jsonl` incrementally (per-file
byte offsets in `stats-scan.json`, regex rather than json.loads;
CACHE_VERSION bump = full rescan) into the Stats page: messages, sessions,
tokens (input + output, no cache), favourite model, streaks, active days and a
24-week heatmap, sent as an `"st"` beat when it changes. `daemon/trend.py`
records poll history (8 days); `"tr"` is no longer sent. `shrink_payload()`
drops `tr`, then `cc`, if a payload would exceed 500 B.

**Device behaviour.** `ui_companion_update()`: attention (5/8) entering ->
`alert_start()` (wake, glow if enabled, chime per `alert_chime`: kind 0 for 5,
kind 1 for 8, header mascot acts); leaving -> `alert_stop()`. Nothing else
moves pages: working states show in the ticker only. A tap acknowledges.

**Install / test.** `companion/link` (or the hub's startup log) prints
`curl -fsSL http://<addr>:47393/install/<token> | sh` and the PowerShell
equivalent; the listener serves self-contained installers
(`render_install_sh/ps1`) with its address and token baked in. It binds
0.0.0.0; non-loopback callers must present the token (`companion.token` next
to the config, `companion_token` overrides). `companion/hooks.json` and the
plugin copy are GENERATED from `companion.hooks_fragment()` (a test enforces
it). Quick checks: `curl -s http://127.0.0.1:47393/state`; serial
`cc 5 Permission: Bash`, `preview`, `stats` on the device.

**Pixel path on the C6 2.16.** LVGL renders plain RGB565 and `display.cpp`
swaps bytes on flush (`swap_on_flush`, 32-bit swaps, ~0.4 ms/strip): LVGL 9.5's
RGB565_SWAPPED target copies RGB565 canvases without swapping when there is no
alpha. `BoardCaps.be_pixels` is false; the splash writes LE and waits after
every row blit (`blit_cells` reuses one strip buffer while DMA is in flight).
Serial `render plain|swapped` / `dma on|off` A/B the path live.

**Sound on the C6 2.16.** ES8311 at 0x18 on the shared I2C bus; I2S MCLK 19 /
BCLK 20 / WS 22 / DIN 21 / DOUT 23; no GPIO amp enable. The driver's 0..100 is
a raw DAC register scale (0.5 dB/step, 0 dB at 191), so `chime_set_volume(pct)`
maps 0..100 linearly in dB (-30 ... 0 dB); the synth peaks at half scale per
note so overlapping notes never clip. Default 40 % (Settings -> Alerts ->
Volume, serial `volume N`).

**WiFi setup portal.** `src/setup_portal.cpp` (WiFi builds only): with no
network in NVS the device raises an **open** SoftAP `Clawdmeter-XXXX` named from
`ESP.getEfuseMac()` (not `WiFi.macAddress()`, which is zeros before the radio
is up) and shows it on screen (`ui_show_setup()`). Captive detection answers
every OS probe (`/hotspot-detect.html`, `/generate_204`, `/ncsi.txt`,
`/connecttest.txt`, `/canonical.html`) and `onNotFound` with the form, plus a
catch-all `DNSServer` pointing at 192.168.4.1. Mode is `WIFI_AP_STA` so the page
can list scanned networks; `WiFi.scanDelete()` runs as soon as one is chosen.
Serial `wifi <ssid> <password>` writes NVS directly; `wifi` alone reports the
link. The native USB-JTAG can wedge so the console prints nothing: replug
before suspecting the firmware.

**Two link modes: BLE and WiFi.** On the C6 the radios do not fit together
with this UI (BLE build ~74 KB free heap; WiFi merely linked ~22 KB and
`esp_wifi_init()` panics; WiFi without NimBLE ~47 KB with the radio up), so the
transport is a **build-time** choice (`*_wifi` envs). `src/link_wifi.cpp`
implements the same `ble.h` contract, so main.cpp, the parser, the UI and the
alerts are untouched; a FreeRTOS task owns the blocking long-poll and hands
payloads over under a mutex. **Gotcha:** PlatformIO's LDF reads `#include`
lines without evaluating the `#ifdef` around them, so `link_wifi.cpp` must be
excluded from BLE envs' `build_src_filter` or the WiFi stack gets linked in.
`daemon/hub.py` is the always-on half: it reuses `companion.py` (workers cannot
tell a hub from a BLE bridge), `stats.py`, `trend.py` and `poll_slots()`,
publishes one payload with a monotonic sequence, long-polls devices on
`/device/poll?seq=` (204 when nothing moved) and advertises `_clawdmeter._tcp`
over mDNS. `bleak` is imported softly so a headless hub needs no Bluetooth.

**One touch settles everything pending.** `global_click_cb()` clears each
pending thing it finds (alert or preview, and the splash) and only falls
through to the plain "enter the splash" toggle when it settled nothing. Serial
`tap` injects a click through the same handler and prints the before/after
state. The first touch on a dark panel is still swallowed as a wake
(`idle_consume_wake_press`).

**Power path, C6 2.16 without a battery.** VBUS is the only source, and the
AXP2101 powers up with VINDPM at 4.36 V, throttling the input as soon as VBUS
sags there (thin cable, weak charger): boot loops. `power_hal_init()` sets
VINDPM to 4.04 V and pins the input limit at 1500 mA. Serial `power` dumps the
path (the VBUS and SYS ADCs must be enabled first or they read 0 mV) plus a
per-boot counter in NVS namespace `clawdboot` and `esp_reset_reason()`.

**Token keeper.** Access tokens live about 8 h and only a real CLI call renews
them (`claude auth status` does not; the desktop app renews through its host).
The daemon never refreshes tokens itself: `daemon/tokenkeeper.py` spawns
`claude -p "..." --model haiku` (hooks pointed at a dead CLAWDMETER_URL,
CLAUDECODE unset, 15 min cooldown) when `read_token_expiry()` says the token
expires within 10 min, and once more after a 401. The hub runs one keeper per
config dir with `CLAUDE_CONFIG_DIR` set (`AccountKeepers`). Config
`token_keeper = on|off`.

**C6 memory.** Boot: free heap ~54 KB, LVGL pool 61 % of 80 KB
(`-DLV_MEM_SIZE=81920`; the default 64 KB ran to 78 %). Big consumers: two
57.6 KB LVGL strips, the actor canvas ~33 KB, the idle creature ~10 KB, the
splash band 23 KB. Watch `free=` in the `usage:` serial line and `stats`.

## Daemon / host side

**Companion + Trend (macOS daemon):** `companion.py` (hook listener on 127.0.0.1:47393, session state machine, `"cc"` summary) and `trend.py` (poll history → `"tr"`) are imported by `claude_usage_daemon.py`; config keys `companion`, `companion_port`, `companion_bind`, `trend`. Tests: `daemon/tests/test_companion.py`, `test_companion_bridge.py`, `test_companion_liveness.py`, `test_trend.py`, `test_hooks_install.py`, `test_tokenkeeper.py`, `test_stats.py` (run the suite with a venv holding pytest + bleak + httpx + pillow + zeroconf: `python -m pytest daemon/tests -q`). The Windows daemon has the companion too (same modules, `link` subcommand).

**Usage source (macOS + Windows Python daemons):** the official `GET /api/oauth/usage` endpoint — the same data `claude /usage` renders, zero token cost, and the only source of the per-model weekly limits (`limits[]` entries of kind `weekly_scoped`, e.g. Fable → payload `"ws":[{"n":"Fable","p":8}]`). Fixed 60s cadence; a 429 benches it for 15 min (`USAGE_ENDPOINT_COOLDOWN_S`); any failure or non-Pro/Max shape falls back to the 1-token `/v1/messages` header method (`poll_api`), which stays the sole authority on Enterprise detection and dead tokens. The bash (Linux) daemon still uses headers only and never sends `ws`. `clock` config defaults to `auto` — the daemon always sends `t`/`tf` and the device's Clock setting decides whether to show it. `hb`/`hc` carry the host's battery (percent, plugged-in — not strictly charging, since macOS pauses charging on AC; `host_battery = off` disables; the inner loop re-checks every HOST_BATT_CHECK_S and pushes the last payload re-stamped via `host_battery_beat()` the moment it changes, so plug/unplug lands within ~2 s) — the drawn header battery shows the board's own cell if fitted, else the host's while data is fresh, else nothing, with the percentage inside (`resolve_battery()` / `refresh_battery_glyph()` in ui.cpp). Tests: `daemon/tests/test_usage_endpoint.py`.


Bash daemon (`daemon/claude-usage-daemon.sh`) reads OAuth token, polls Anthropic API, sends JSON over BLE GATT. Run with `systemctl --user start claude-usage-daemon`. The unit file's `ExecStart` is the absolute path to the script — repoint it when switching between the worktree and the main checkout.

**Discovery & resilience:**

- Connects by name (`"Clawdmeter"`) on first run, caches resolved MAC at `~/.config/claude-usage-monitor/ble-address`. ESP32 BLE addresses are factory-burned per-chip, so swapping any board invalidates the cache.
- On connect failure: cache is dropped AND device is removed from bluez (`bluetoothctl remove`) so the next scan won't re-pick a dead MAC. Multi-candidate scans pick `head -1` and let the failure cycle converge.
- `POLL_INTERVAL=60`, `TICK=5`. Inner loop wakes every 5s to detect disconnects fast; polls Anthropic when 60s elapsed OR when ESP fires a refresh request.

**GATT characteristics on service `4c41555a-...0001`:**

- `...0002` RX — daemon writes JSON usage payload here.
- `...0003` TX — firmware notifies ack/nack (daemon doesn't subscribe).
- `...0004` REQ — firmware fires `0x01` notify in `onSubscribe` if `has_received_data` is false. Daemon subscribes via `setsid bash -c "stdbuf -oL dbus-monitor … | awk …"`; awk drops a flag file the inner loop picks up. Three traps there: pipe buffering (hence `stdbuf -oL`), a race when `busctl` exits early, and `wait` blocking on pipeline jobs.

## This fork: WiFi hub and several accounts

The display talks to an always-on hub over WiFi (`daemon/hub.py`, envs `waveshare_amoled_216_wifi` and `waveshare_amoled_216_c6_wifi`) instead of BLE, and can show up to three accounts, switched with a swipe (`firmware/src/slots.*`, `config_dirs = path:label` in the daemon config).

Read [`docs/ai/wifi-hub-and-accounts.md`](docs/ai/wifi-hub-and-accounts.md) before touching accounts or token renewal: OAuth refresh tokens rotate, so two machines can never share one session; the hub renews tokens only by running the unmodified `claude` CLI (never call the OAuth endpoints ourselves, Anthropic's terms forbid it); and an expired account vanishes from the display without any message. The earlier BLE mount and its hardware traps are in [`docs/ai/ble-setup-hardware-notes.md`](docs/ai/ble-setup-hardware-notes.md).

Never put credentials, tokens or firmware images built on a personal machine in the repository.
