# Clawdmeter

<img src="assets/readme/waving.gif" width="120" align="right" alt="">

A small ESP32 dashboard I made for my desk to keep an eye on Claude Code usage.

It runs on a [Waveshare ESP32-S3-Touch-AMOLED-2.16](https://www.waveshare.com/esp32-s3-touch-amoled-2.16.htm?&aff_id=149786) as well as a few other alternative boards and pairs over Bluetooth, the splash screen plays pixel-art Clawd animations that get
busier when your usage rate climbs. The two side buttons send Space and
Shift+Tab over BLE HID for Claude Code's voice mode and mode-toggle shortcuts.

<img width="1179" height="994" alt="Usage meter" src="https://github.com/user-attachments/assets/83e54aea-0932-428f-94aa-b3ede3a360aa" />

## This fork: WiFi on the S3 2.16, and several accounts

[![CI](https://github.com/OWNER/REPO/actions/workflows/ci.yml/badge.svg)](https://github.com/OWNER/REPO/actions/workflows/ci.yml)

This branch of the fork chain (HermannBjorgvin, then TheOriUHD) adds two things.

- **WiFi build for the ESP32-S3 AMOLED 2.16.** Upstream only ships a WiFi build
  for the C6 variant. `waveshare_amoled_216_wifi` reuses the S3 board port with
  the same WiFi link (`link_wifi.cpp`) and drops NimBLE. The S3 has PSRAM, so
  unlike the C6 the WiFi stack does not have to fight LVGL for memory.
- **Several Claude accounts on one display.** Label your config dirs
  (`config_dirs = ~/.claude:work, ~/.claude-personal:personal`) and the hub
  publishes one payload per account. The display keeps a page per account,
  **swipe right** to move to the next one with a slide animation, and the title
  shows the account's label. Single-account setups are unchanged, pixel for
  pixel. The page model follows the `slots` idea of
  [egorshar's dual-account fork](https://github.com/egorshar/Clawdmeter).
- **Headless hub kit** in [`hub-extras/`](hub-extras/README.md): systemd units and
  a token refresher, because on a box where nobody runs Claude Code, nothing
  renews the OAuth token and the display goes blank after about 8 hours.

Tested on hardware with a Waveshare ESP32-S3-Touch-AMOLED-2.16 (no battery)
talking to a hub in a Proxmox LXC, two accounts. Other boards are untested.

```bash
git clone https://github.com/OWNER/REPO && cd REPO
./flash.sh waveshare_amoled_216_wifi          # or grab a prebuilt image from Releases
python3 daemon/hub.py                          # or install hub-extras/systemd/* on a server
```

CI runs the daemon tests, builds the WiFi firmware for both boards, and scans for
leaked secrets. Tagging `vX.Y.Z` publishes the images as a GitHub release. Read
the [licensing warning](#licensing-gray-area-warning) before you fork this: it
bundles Anthropic's fonts and mascot art, inherited from upstream.

## This fork

[TheOriUHD/Clawdmeter](https://github.com/TheOriUHD/Clawdmeter) is a fork of
[HermannBjorgvin/Clawdmeter](https://github.com/HermannBjorgvin/Clawdmeter) that
makes the device more functional while keeping its design language intact:

- **Fable limit.** Plans with a per-model weekly allowance (the "Fable" row in
  Claude's own usage settings) get it as a second **face** of the Weekly card:
  the identical full-size card — same number, pill, bar and reset line — for
  the scoped model. Two dots on the reset row show the faces; the card flips on
  its own at a configurable interval (default 7 s) and **tapping the card
  flips it at once**. The label is the
  API's display name, so a future scoped model rides along without a firmware
  change. Plans without one have a single face and the classic card, pixel for
  pixel. (A third full card doesn't fit a 480 px panel, and a shrunken row
  looked like an afterthought — faces keep every limit at full size.)
- **Touch-first settings with real controls.** Swipe **up** on the Usage
  screen — Settings follows your finger in from below — for four pages,
  every control sized for a finger on the 39 mm panel and persisted across
  reboots. Swipe **down** from any Settings page and you're home; swipe
  left/right to page (the pages track your finger and snap on release):
  page 1 — **Clock** as a segmented picker (Off / Auto / 12h / 24h, the accent
  highlight glides to your choice, with a live preview of what the title will
  show) plus **Battery icon** and **Mascot** as animated toggles;
  page 2 — **Brightness** as a draggable slider (the panel follows your finger
  live), **Sleep after** as a stepped slider (5 min … Never), **Status line**
  toggle and the **Pairing** button (two taps forget the bonded host and
  re-advertise); page 3 — **Weekly card**: **Default face** (a picker built
  from your plan's limits, e.g. *Weekly* / *Fable*) and **Flip every**
  (Off · 3 s … 30 s; with Off the card stays on the default face and a tap
  picks — and keeps — a face); page 4 — **About** (board, firmware, Bluetooth
  state and address, battery, free RAM, uptime). Dots show the page. A tap on
  the Usage screen still toggles the splash.
- **Token-free daemon.** The macOS and Windows daemons read the official
  `/api/oauth/usage` endpoint — the same data `claude /usage` renders — instead
  of spending a 1-token message per poll. A `429` benches the endpoint for 15
  minutes and the old rate-limit-header method is the automatic fallback, so
  behaviour can never be worse than before.
- **Clock is a device setting.** The daemon now always sends the time
  (`clock = auto` by default; `12`/`24` force a format, `off` never sends it),
  and the device decides whether to show it.
- **A battery that means something.** The header battery is drawn by the
  firmware — a wider outline with a proportional fill and the **percentage
  inside** (red fill at ≤10%, accent while plugged in). When it's on: a board
  with a LiPo shows its own cell; a USB-powered board (the PMU reports no cell)
  shows the *host's* battery instead, which the daemon sends with every payload
  (`pmset -g batt` on macOS, `Win32_Battery` on Windows; `host_battery = off`
  disables it). Plugging in or out shows on the device within ~2 s — the
  daemon watches the battery between usage polls and pushes changes at once.
  About names the source. No cell and no host battery → nothing, instead of a
  permanently empty glyph.
- **Claude Code companion.** Claude Code's own hooks post every session event
  (a prompt went in, a tool is running, a permission is pending, the turn
  ended) to the daemon, which relays one compact state to the device. The
  **status line stops making words up** and says what Claude is actually doing
  (`Editing ui.cpp…`, `Bash: pio run…`, `Your turn…`); the whimsical spinner
  words stay for plain thinking — they are Claude Code's own. When a session
  on another machine holds the line, its host name sits above it as a small
  dim caption, and two equally busy sessions take turns every eight seconds
  instead of flipping on every event; a session that needs you always wins. When a session
  is waiting for you the line turns into a calm green **Ready** with a
  breathing dot; with no session open anywhere it reads a dim **Idle**.
- **"Needs you" alerts.** A permission prompt, a question, a plan to approve
  — or the end of a long turn — wakes the panel, starts a calm orange **glow**
  breathing along the screen
  edge (nested rounded rings that follow the glass's real corner radius, 70 px
  on the 2.16, so nothing is cut off by the bezel), has the corner Clawd
  **jump, wave and point**, and plays a **two-note chime** through the speaker
  (the C6 2.16's ES8311 is now wired up; upstream left it silent). One tap
  acknowledges — and settles everything else waiting with it, so an alert that
  arrived while the splash was up leaves with the splash on that same press. Settings → **Alerts** picks the chime (Off / Needs you / All),
  has a **Preview** button, a **Volume** slider (linear in dB; the codec
  follows your finger and auditions on release) and the **Glow** toggle.
- **Stats page.** Left of Usage: the Claude app's stats card on the desk —
  **Sessions, Messages, Tokens, Active days, Streak, Best streak, Peak hour,
  Model** as eight tiles, and a 24-week **activity heatmap** in accent tones.
  The daemon computes it from the local Claude Code transcripts
  (`~/.claude/projects`), scanning only what was appended since the last pass,
  and sends it as its own small beat whenever the numbers move.
- **Serial QA hook.** `page splash|usage|stats|settings|settings2..4|about`,
  `flip`, `cc <state> [label]`, `demo stats`, `alert [0|1]`, `preview`, `volume N`,
  `corners on|off`, `radius N`, `render plain|swapped`, `dma on|off`, `swipe
  up|down` and `stats` over the USB serial console drive the screens on boards
  without the framebuffer screenshot (the C6 ports).

|      Weekly card, Fable face      |              Stats              |      Claude needs you (glow)      |
| :-------------------------------: | :-----------------------------: | :-------------------------------: |
| ![Fable](screenshots/usage_fable.png) | ![Stats](screenshots/stats.png) | ![Needs you](screenshots/waiting.png) |

|     Settings 1 — clock & toggles     |     Settings 2 — sliders & pairing    |     Settings 3 — Weekly card     |     Settings 4 — Alerts     |       Settings 5 — About      |
| :----------------------------------: | :-----------------------------------: | :------------------------------: | :-------------------------: | :---------------------------: |
| ![Settings](screenshots/settings.png) | ![Settings 2](screenshots/settings2.png) | ![Settings 3](screenshots/settings3.png) | ![Settings 4](screenshots/settings4.png) | ![About](screenshots/about.png) |

Wire format additions: `"ws":[{"n":"Fable","p":8}, …]` — one entry per weekly
scoped-model limit (`n` label ≤ 15 chars, `p` percent; absent key = no scoped
limits, `0` is a real reading); `"hb": 85, "hc": 1` — the host's battery
percent and plugged-in flag (absent = no battery to report);
`"cc": {"n": 2, "a": 1, "s": 5, "l": "Permission: Bash", "p": "Clawdmeter",
"e": 37, "m": "Fable 5.1", "g": 0, "h": "devbox"}` — the companion's headline
session (`s`: 0 none · 1 idle · 2 thinking · 3 tool · 4 done · 5 needs you ·
6 error · 7 compacting · 8 long turn done; `l` ≤ 24 chars, `e` seconds in that
state); and `"st": {"se", "me", "tk", "ad", "cs", "ls", "ph", "fm", "hm"}` — the
Stats page (sessions, messages, tokens, active days, current/best streak, peak
hour, favourite model, and the heatmap as one char per day, `0`-`4` or `x` for
days still ahead in the current week, whole Sunday-first weeks, oldest first),
sent as its own beat. A `"cc"`- or `"st"`-only beat is applied without touching
the usage numbers. Old firmware ignores the keys, old daemons simply never send
them.

## Screens

The device boots into the splash. Tap the screen anywhere to switch to the Usage view; tap again to flip back to the splash. Swipe right from Usage for **Stats** (your lifetime Claude Code numbers and activity heatmap); page dots appear while you swipe. Swipe up for the five Settings pages (clock & toggles, sliders & pairing, Weekly card, Alerts, About) — left/right pages between them — and swipe down from anywhere in Settings to get back. On plans with a Fable limit, tap the Weekly card to flip between its faces. When Claude needs you the device glows orange, chimes softly and the corner Clawd jumps and waves; the status line says what Claude is doing the rest of the time. A tap settles everything pending at once: an alert waiting to be acknowledged and the splash standing in front of the numbers both clear on the same press. A tap only *enters* the splash when there was nothing to clear.

|              Splash               |              Usage              |
| :-------------------------------: | :-----------------------------: |
| ![Splash](screenshots/splash.gif) | ![Usage](screenshots/usage.png) |
|   Splash; touch-toggle anytime    | Session and weekly utilization  |

While the splash is up, the middle (PWR) button cycles animations. **Hold the power button for 3 seconds, then release, to put the device into pairing mode** — this clears the saved Bluetooth bond and re-advertises. The firmware also auto-rotates animations every 20 s within the current usage-rate group, so a long stretch on the splash isn't just one Clawd on loop.

## Hardware

Boards supported out of the box:

- [Waveshare ESP32-S3-Touch-AMOLED-2.16](https://www.waveshare.com/esp32-s3-touch-amoled-2.16.htm?&aff_id=149786)
- [Waveshare ESP32-C6-Touch-AMOLED-2.16](https://www.waveshare.com/esp32-c6-touch-amoled-2.16.htm?&aff_id=149786)
- [Waveshare ESP32-S3-Touch-AMOLED-1.8](https://www.waveshare.com/esp32-s3-touch-amoled-1.8.htm?&aff_id=149786)
- [Waveshare ESP32-C6-Touch-AMOLED-1.8](https://www.waveshare.com/esp32-c6-touch-amoled-1.8.htm?&aff_id=149786)
- [Waveshare ESP32-S3-Touch-AMOLED-2.06](https://www.waveshare.com/esp32-s3-touch-amoled-2.06.htm?&aff_id=149786)
- [Waveshare ESP32-S3-Touch-LCD-1.54](https://www.waveshare.com/esp32-s3-lcd-1.54.htm?sku=33869&aff_id=149786)
- [Waveshare ESP32-S3-Touch-LCD-4](https://www.waveshare.com/esp32-s3-touch-lcd-4.htm)

> Please check if a pull request exists for your alternative hardware port before opening a new one, providing QA feedback and testing on the same hardware is more valuable than duplicate pull requests.

**Porting to another board:** the firmware is a thin HAL with per-board folders under `firmware/src/boards/`. Drop in a new folder and a new PlatformIO env — `main.cpp`, `ui.cpp`, and `splash.cpp` never need to change. See [`docs/porting/adding-a-board.md`](docs/porting/adding-a-board.md) for the walk-through and [`docs/porting/hal-contract.md`](docs/porting/hal-contract.md) for the interfaces a port must implement.

## Prerequisites

- Linux (tested on Ubuntu), macOS, or Windows 10/11
- [PlatformIO CLI](https://docs.platformio.org/en/latest/core/installation/index.html)
- Linux: `curl`, `bluetoothctl`, `busctl` (BlueZ Bluetooth stack)
- macOS: `python3` (the installer sets up a venv with `bleak` and `httpx`)
- Windows: `python3` 3.11+ (the installer sets up a venv with `bleak`, `httpx`, and `pystray`)
- Claude Code with an active subscription

## macOS installation

The macOS host pieces — Python daemon, LaunchAgent, and flash helper — were ported by [Chris Davidson (@lorddavidson)](https://github.com/lorddavidson). Thanks Chris!

### Flash the firmware

```bash
./flash-mac.sh waveshare_amoled_216                       # ESP32-S3 2.16" (auto-detects /dev/cu.usbmodem*)
./flash-mac.sh waveshare_amoled_216_c6                    # ESP32-C6 2.16" variant
./flash-mac.sh waveshare_amoled_18  /dev/cu.usbmodem1101  # ESP32-S3 1.8" (or pass an explicit USB serial port)
```

The board env name is required. Run `./flash-mac.sh` with no args to see the available envs (scraped from `firmware/platformio.ini`).

### Pair the device

After flashing, open **System Settings → Bluetooth** and click _Connect_ next to "Clawdmeter". The daemon only ever connects to the peripheral this Mac is paired/connected to — it never scans for a nearby device — so once it's connected here the daemon picks it up on its next poll (~60 s).

### Install the daemon

The daemon reads your Claude OAuth token from the macOS Keychain (service `Claude Code-credentials`), polls usage every 60 s, and pushes it to the display over BLE.

```bash
./install-mac.sh
```

The installer creates a Python venv in `daemon/.venv/`, installs `bleak` and `httpx`, renders a LaunchAgent into `~/Library/LaunchAgents/com.user.claude-usage-daemon.plist`, and loads it. The first run is launched interactively so macOS prompts for Bluetooth permission.

Useful commands:

```bash
launchctl list | grep claude-usage                                          # check it's running
tail -F ~/Library/Logs/claude-usage-daemon.out.log                          # live logs
launchctl unload ~/Library/LaunchAgents/com.user.claude-usage-daemon.plist  # stop
launchctl load -w ~/Library/LaunchAgents/com.user.claude-usage-daemon.plist # start
```

### Two modes: Bluetooth, or WiFi with a hub

A Clawdmeter can get its numbers two ways, and the choice is made **when you
flash it** — the ESP32-C6 cannot run both radios alongside this UI (measured:
73.7 KB of free heap on the Bluetooth build, 21.8 KB with the WiFi stack merely
linked in, at which point WiFi refuses to start).

**Bluetooth** (`waveshare_amoled_216_c6`) is the simple one: the device pairs
with one nearby Mac or PC running the daemon. Nothing else to set up, but the
numbers stop when that machine sleeps, walks out of range or shuts its lid.

**WiFi** (`waveshare_amoled_216_c6_wifi`) puts the device on your home network
instead, talking to a **hub** you run on anything always-on — a NAS, a Pi, a
VM. That decouples the display from any one laptop, and any number of
Clawdmeters can watch the same hub from different desks.

```bash
python3 daemon/hub.py
```

The hub polls the usage numbers, scans your transcripts for the Stats page and
listens for hook events exactly as the Bluetooth bridge does — a worker machine
cannot tell the two apart, so the join one-liner below is unchanged. Devices
find it over mDNS (`_clawdmeter._tcp`), so the only thing to set up on a
device is which WiFi to join — and it asks for that itself.

**Setting up a device.** With no network stored it raises its own open hotspot
and shows the name on its screen. Join it from a phone and the setup page opens
by itself, the way a hotel WiFi login does. Pick the house network from the
scanned list, type its password, and the device saves it and restarts onto your
network. Nothing is typed on the device and no cable is involved.

**Changing the network later**, two ways:

- **Settings → WiFi → Set up WiFi**, then tap again to confirm. (On a Bluetooth
  build the same tile is the pairing one, "Forget host".)
- **Hold both buttons for ten seconds.** Long enough that it cannot happen by
  accident, and it works when the screen cannot help you — wrong network, hub
  moved, no hub at all.

Either one forgets the stored network and restarts into the hotspot. There is
also `wifi <ssid> <password>` on the serial console if you prefer a cable;
`wifi` alone reports the link, address, hub and signal.

A hub needs no Bluetooth at all, so it runs happily on a headless box.

Devices hold a long-poll open rather than asking repeatedly, so an alert
reaches every desk in about 30 ms while an idle account costs one request per
25 seconds.

### Companion: this Mac and every other machine

Think of two roles. The **bridge** is the computer with the Bluetooth link
running the daemon (this Mac, or a Windows PC). **Workers** are wherever Claude
Code actually runs: the bridge itself, a dev box you ssh into, a VM or a
container that can only reach the bridge over the network. A worker gets
**only the hooks** — one pasted line, no daemon, no Bluetooth, no clone of this
repo. Installing the daemon on a worker does nothing useful there.

**The bridge itself.** `install-mac.sh` offers it; by hand:

```bash
python3 companion/install-hooks.py
```

**Any other machine.** On the bridge, print the join command:

```bash
companion/link
```

It lists one line per address the bridge has (LAN, Tailscale, `name.local`).
Paste the one that machine can reach into it — Linux or macOS:

```bash
curl -fsSL http://192.168.1.23:47393/install/<token> | sh
```

Windows (PowerShell):

```powershell
irm http://192.168.1.23:47393/install.ps1/<token> | iex
```

The installer is served by the daemon itself with the bridge's address and a
token baked in, merges 15 hooks into that machine's `~/.claude/settings.json`
(keeping everything else, idempotent, backup written), and needs only `curl`
plus `python3` or `node`; the PowerShell one needs nothing. It ends by
pinging the bridge and says whether it answered; the daemon's log then reads
`Companion: <host> joined`, and `first hook from <host>` when Claude Code
there fires its first event. Claude Code sessions on that machine report to
the bridge from then on (running ones from their next event) and the device
treats them like local ones. `... | sh -s -- --uninstall` removes them.
Check from the worker with `curl -s http://<bridge>:47393/state/<token>`.

**Run the line as the user Claude Code runs as.** Hooks live in that user's
`~/.claude/settings.json`; a remote session that logs in as `root` needs the
join line run as root (`sudo -i`, then paste it), not as your everyday user.
The installer prints which user it installed for, and the daemon log repeats
it in the `joined` line.

If nothing shows up, the daemon's log says why: a hook that arrives without
the right token is logged as `rejected a hook from <host>` (once per ten
minutes per machine), and a machine that joined but sent no hook within ten
minutes gets a reminder naming the user its hooks were installed for. Hooks installed another way — the plugin from the
marketplace, `install-hooks.py` without `--url`/`--token` — post to that
machine's own loopback, where nothing listens; point them at the bridge with
`CLAWDMETER_URL=http://<bridge>:47393` and `CLAWDMETER_TOKEN=<token>` in that
Claude Code's environment, or simply run the served installer instead.

How it stays safe: the daemon listens on every interface, but anything that is
not the bridge itself must present the token (generated on first start into
`~/.config/claude-usage-monitor/companion.token`; `companion_token = …` in the
config to pick your own). Prefer loopback only? `companion_bind = 127.0.0.1`,
and give ssh hosts `RemoteForward 47393 127.0.0.1:47393` instead.

A session counts as open for as long as it really is. Hooks on the bridge's
own machine report their parent process id (Claude Code runs each hook straight
from its own process), so the daemon watches that process: the line stays
**Ready** whether you type again in a minute or leave the window open
overnight, and drops the moment the window closes, SessionEnd or not. Sessions
on other machines cannot be watched and time out instead (30 min silent while
working, 12 h idle). The table is saved to `companion-state.json` and restored
when the daemon restarts, so a deploy or a reboot of the bridge never blanks
the state. With no session anywhere the line reads a dim **Idle**.

Two more things to know: the *usage numbers* still come from the bridge's own
Claude Code login — and that stored token lives about eight hours and is only
renewed by a CLI session, not by the desktop app. So the daemon keeps it fresh
itself: shortly before expiry (or right after a 401) it runs one tiny
print-mode CLI call (`claude -p … --model haiku`, one Haiku call per eight
hours, kept off the device) so Claude Code renews its own token
(`token_keeper = off` disables this). And each hook is one background `curl`
line with a 2-second cap that never fails a tool call if the bridge is
unreachable. The
daemon config has `companion = on|off`, `companion_port`, `companion_bind`,
`companion_token` and `trend = on|off`; see `daemon/config.example`.

## Linux installation

### Flash the firmware

```bash
./flash.sh waveshare_amoled_216                  # ESP32-S3 2.16" (defaults to /dev/ttyACM0)
./flash.sh waveshare_amoled_216_c6               # ESP32-C6 2.16" variant
./flash.sh waveshare_amoled_18  /dev/ttyACM1     # ESP32-S3 1.8" (or pass an explicit USB serial port)
```

The board env name is required. Run `./flash.sh` with no args to see the available envs (scraped from `firmware/platformio.ini`).

### Pair the device

After flashing, the device advertises as "Clawdmeter". Pair it once:

```bash
# Scan for the device
bluetoothctl scan le

# When "Clawdmeter" appears, pair and trust it
bluetoothctl pair F4:12:FA:C0:8F:E5    # use your device's MAC
bluetoothctl trust F4:12:FA:C0:8F:E5
```

To re-pair later, hold the power button for 3 seconds then release — the device clears its saved bond and re-advertises.

### Install the daemon

The daemon polls your Claude usage every 60 seconds and sends it to the display over BLE.

```bash
./install.sh
systemctl --user start claude-usage-daemon
```

Check status: `systemctl --user status claude-usage-daemon`

View logs: `journalctl --user -u claude-usage-daemon -f`

## Windows installation

Runs natively on Windows — no WSL required. A system-tray app polls your usage and pushes it over BLE, and starts automatically at login.

### Prerequisites

- **Native Windows** (not WSL).
- **Python 3.11+** from [python.org](https://www.python.org/downloads/) — check _"Add python.exe to PATH"_ during install.
- **Claude Code** installed, with `claude login` completed. The token is read from `%USERPROFILE%\.claude\.credentials.json` (falling back to `%LOCALAPPDATA%\Claude\` then `%APPDATA%\Claude\`).
- The repo on a **native Windows path** (e.g. `%USERPROFILE%\Clawdmeter`), **not** a `\\wsl$` share — the installer refuses a WSL path.

### Flash the firmware

```powershell
pio run -d firmware -e waveshare_amoled_216 -t upload --upload-port COM5   # use your device's COM port
```

Run `pio run -d firmware` with no env to see the available board envs.

### Pair the device

The device is a bonded BLE HID keyboard, so pair it once: **Settings → Bluetooth & devices → Add device → Bluetooth**, then select "Clawdmeter". Pairing is **required** — it enables the physical buttons and keeps a persistent connection (the device keeps showing your last-synced usage even after the daemon quits). To undo, use **Remove device** (this disables the buttons).

### Install the daemon (recommended)

From the repo root in PowerShell:

```powershell
powershell -ExecutionPolicy Bypass -File install-windows.ps1
```

This creates a venv, installs `bleak`/`httpx`/`pystray`/`Pillow` from the in-repo requirements (no internet downloads), registers a per-user login-autostart entry (`HKCU\…\Run`, no admin needed), and launches the tray app headlessly (no console window).

### Run manually instead (optional)

```powershell
python -m venv .venv
.venv\Scripts\Activate.ps1        # if blocked: Set-ExecutionPolicy -Scope CurrentUser RemoteSigned, then retry
pip install -r daemon\requirements-windows.txt
python daemon\claude_usage_daemon_windows.py        # runs in the foreground; Ctrl+C to stop
```

### Tray icon and menu

The icon's corner bubble shows state — **green** Connected, **amber** Scanning, **red** Error — and hovering shows the status (`Connected · last update HH:MM`). A notification fires once when it enters Error (e.g. an expired token). Right-click for the menu:

- **Status header** — live state + last sync time.
- **Start at login** — toggle autostart on/off.
- **Quit** — stops the daemon cleanly; leaves the Windows pairing intact (device keeps its last reading).

### Logs and troubleshooting

```powershell
Get-Content $env:LOCALAPPDATA\Clawdmeter\daemon.log -Tail 30        # view logs
reg delete "HKCU\Software\Microsoft\Windows\CurrentVersion\Run" /v Clawdmeter /f   # remove autostart
```

| Symptom                                | Fix                                                      |
| -------------------------------------- | -------------------------------------------------------- |
| `Device not found`                     | Power on the device; make sure it's in range and paired. |
| `token expired` toast / `API HTTP 401` | Re-run `claude login`, then restart the daemon.          |
| `Connection failed`                    | Toggle Windows Bluetooth off/on in Settings.             |
| `Warning: running under Linux/WSL`     | Run from a native PowerShell window, not a WSL shell.    |

## How it works

<img src="assets/readme/magnifier.gif" width="150" align="right" alt="">

1. The daemon reads your Claude Code OAuth token — from the macOS Keychain (service `Claude Code-credentials`) on macOS, or from `~/.claude/.credentials.json` on Linux (`%USERPROFILE%\.claude\.credentials.json` on Windows).
2. It makes a minimal API call to `api.anthropic.com/v1/messages` — one token of Haiku, basically free.
3. The usage numbers come straight out of the response headers (`anthropic-ratelimit-unified-5h-utilization` and friends).
4. The daemon connects to the ESP32 over BLE and writes a JSON payload to the GATT RX characteristic.
5. The firmware parses it and updates the LVGL dashboard.
6. The firmware also tracks the rate of change of session % over a 5-minute window and picks splash animations from the matching mood group.
7. The two side buttons are independent of all of this — they send Space and Shift+Tab as BLE HID keyboard input to the paired host directly.

## Physical buttons

The board has three side buttons. Left and right send HID keys; the middle (PWR) button cycles splash animations and, held for 3 seconds, triggers pairing mode.

| Button           | GPIO         | Function                                                     |
| ---------------- | ------------ | ------------------------------------------------------------ |
| **Left**         | GPIO 0       | Hold to send Space (Claude Code voice-mode push-to-talk)     |
| **Middle** (PWR) | AXP2101 PKEY | On splash: cycle animations. Hold 3s + release: pairing mode |
| **Right**        | GPIO 18      | Press to send Shift+Tab (Claude Code mode toggle)            |

Space and Shift+Tab go out as standard BLE HID keyboard reports, so they trigger in whatever window has focus on the paired host — not just Claude Code.

## BLE protocol

The device advertises a custom GATT service alongside the standard HID keyboard service:

|                            | UUID                                   |
| -------------------------- | -------------------------------------- |
| **Data Service**           | `4c41555a-4465-7669-6365-000000000001` |
| RX Characteristic (write)  | `4c41555a-4465-7669-6365-000000000002` |
| TX Characteristic (notify) | `4c41555a-4465-7669-6365-000000000003` |
| **HID Service**            | `00001812-0000-1000-8000-00805f9b34fb` |

JSON payload format (written to RX):

```json
{ "s": 45, "sr": 120, "w": 28, "wr": 7200, "st": "allowed", "ok": true }
```

Fields: `s` = session %, `sr` = session reset (minutes), `w` = weekly %, `wr` = weekly reset (minutes), `st` = status, `ok` = success flag.

## Development

<img src="assets/readme/crab.gif" width="120" align="right" alt="">

- **Desktop simulator** — iterate on the UI without hardware: an SDL2 window
  runs the full firmware loop with scenario playback (`pio run -d firmware -e
sim`, then `cd firmware && .pio/build/sim/program`). See
  [`SIM-USAGE.md`](SIM-USAGE.md) for controls, scenarios, and headless
  screenshots.
- **Splash animations** — Anthropic's official Clawd sprites, archived with
  provenance notes in [`research/clawd-official/`](research/clawd-official/);
  `node tools/convert_official_clawd.js` regenerates
  `firmware/src/splash_animations.h`. See [`tools/README.md`](tools/README.md).
- **Icons** — Lucide PNGs convert to LVGL C arrays with
  `tools/png_to_lvgl.js`. See [`tools/README.md`](tools/README.md).
- **Fonts** — the pre-compiled LVGL fonts and the LVGL-9 patching they need:
  [`docs/fonts.md`](docs/fonts.md).
- **Porting** — [`docs/porting/adding-a-board.md`](docs/porting/adding-a-board.md)
  and [`docs/porting/hal-contract.md`](docs/porting/hal-contract.md).

## Credits

- Pixel-art Clawd animations are Anthropic's official mascot art (claude.ai/code, Claude Code desktop), archived and converted by the tooling in `tools/` and `research/clawd-official/`.
- Lucide icon set ([lucide.dev](https://lucide.dev), MIT) for bluetooth and battery UI glyphs.
- Anthropic brand fonts (Tiempos Text, Styrene B) — see licensing warning below.

## Licensing gray area warning

The software in this repository uses and adheres to the Anthropic brand guidelines and uses the same proprietary fonts that Anthropic has a license for but this software uses without permission as well as using assets from Anthropic such as the copyrighted Clawd mascot so even though the code in this repo is non-proprietary I will not license it myself under a copyleft license since this repo includes proprietary fonts and copyrighted assets. Please be aware of this if you fork or copy the code from this repo. **You have been warned!**
