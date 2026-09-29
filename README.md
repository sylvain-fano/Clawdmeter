# Clawdmeter, WiFi and multi-account

[![CI](https://github.com/sylvain-fano/Clawdmeter/actions/workflows/ci.yml/badge.svg)](https://github.com/sylvain-fano/Clawdmeter/actions/workflows/ci.yml)
[![Release](https://img.shields.io/github/v/release/sylvain-fano/Clawdmeter)](https://github.com/sylvain-fano/Clawdmeter/releases/latest)

<img src="assets/readme/waving.gif" width="110" align="right" alt="">

A desk display for Claude Code: your 5-hour and weekly usage, what Claude is
doing right now, and a glow plus a chime when a session needs you. It runs on a
Waveshare ESP32 AMOLED board and gets its data over **WiFi from a small
always-on hub**, so it does not depend on any one laptop being awake. One
display can follow **several Claude accounts**; swipe to switch.

This is a fork of [HermannBjorgvin/Clawdmeter](https://github.com/HermannBjorgvin/Clawdmeter)
(via [TheOriUHD/Clawdmeter](https://github.com/TheOriUHD/Clawdmeter)). Everything
upstream does, from the Bluetooth daemons to the other board ports, is documented
there.

|                 Usage                 |              Stats              |          Claude needs you           |
| :-----------------------------------: | :-----------------------------: | :---------------------------------: |
| ![Usage](screenshots/usage_fable.png) | ![Stats](screenshots/stats.png) | ![Needs you](screenshots/waiting.png) |

## What this fork adds

- **WiFi build for the ESP32-S3 AMOLED 2.16** (`waveshare_amoled_216_wifi`).
  Upstream ships WiFi only for the C6 variant. The S3 has PSRAM, so the WiFi
  stack and the UI no longer compete for memory.
- **Several accounts on one display.** Each Claude Code config directory gets a
  label and its own page; swipe right to move to the next one, the title shows
  whose numbers you are looking at. A single account looks exactly as before.
- **A headless hub kit** ([`hub-extras/`](hub-extras/README.md)): a sandboxed
  systemd unit for a Pi, a NAS or a Proxmox container, and token renewal that
  goes through the official Claude Code CLI.
- **CI and releases**: tests, secret scanning and firmware builds on every push;
  a tag publishes flashable images.

Tested on a Waveshare ESP32-S3-Touch-AMOLED-2.16 talking to a hub in a Proxmox
LXC, with two accounts. The C6 WiFi build compiles in CI but is not tested
here.

## How it fits together

```
 Claude Code (any machine) --hooks--> hub :47393
 Anthropic usage API <------ poll --- hub (daemon/hub.py, always on)
                                       |
                        long-poll :47394, found over mDNS
                                       |
                       Clawdmeter display(s) on the home WiFi
```

The hub polls the usage of every configured account once a minute and listens
for Claude Code hook events. Displays hold a long-poll open, so a "needs you"
alert reaches every desk in a few tens of milliseconds. The display holds no
secrets: it only knows your WiFi and finds the hub by itself.

## Quick start

**1. Flash the display.** Download the image for your board from the
[latest release](https://github.com/sylvain-fano/Clawdmeter/releases/latest)
and write it at offset 0:

```bash
pip install esptool
esptool.py --chip esp32s3 write_flash 0x0 waveshare_amoled_216_wifi.factory.bin
```

Or build it yourself with [PlatformIO](https://platformio.org):
`./flash.sh waveshare_amoled_216_wifi`.

**2. Put it on your WiFi.** On first boot the display raises its own hotspot and
shows its name on screen. Join it from a phone, the setup page opens by itself,
pick your network and type its password. The hotspot is open and only lives
for this minute of setup, so do it at home rather than in a crowded place, or
use the serial console instead (`wifi <ssid> <password>` over USB). To change
networks later: Settings, WiFi, Set up WiFi.

**3. Run the hub** on anything that stays on, logged in to Claude Code:

```bash
git clone https://github.com/sylvain-fano/Clawdmeter && cd Clawdmeter
python3 -m venv .venv && .venv/bin/pip install httpx zeroconf
.venv/bin/python daemon/hub.py
```

For a server install (dedicated user, systemd, several accounts), follow
[`hub-extras/README.md`](hub-extras/README.md).

**4. Connect your machines** so the display shows what Claude is doing. The
hub's log prints a one-line installer at startup; run it on each machine where
you use Claude Code:

```bash
curl -fsSL http://<hub>:47393/install/<token> | sh          # Linux, macOS
irm http://<hub>:47393/install.ps1/<token> | iex            # Windows
```

It merges Claude Code hooks into that machine's `~/.claude/settings.json`
(backup kept, `--uninstall` to remove). Hooks from other machines must carry
the token.

## Several accounts

Log in to each account in its own config directory, then label them in the hub's
config (`~/.config/claude-usage-monitor/config`):

```
config_dirs = ~/.claude:work, ~/.claude-personal:personal
```

Labels are ASCII, at most 11 characters, up to three accounts. On the display:
swipe right for the next account, left for Stats, up for Settings.

**One login per machine.** Refresh tokens rotate, so copying
`.credentials.json` between machines makes them lock each other out. Run
`claude auth login` on the hub itself.

## Your credentials

The hub reads the OAuth token that Claude Code stores on that machine and uses
it for one thing: reading your own usage, from the endpoint behind
`claude /usage` (or, if that endpoint is unavailable, from the rate-limit
headers of a one-token request). It never sends the token anywhere but
Anthropic, never puts it on the display, and never refreshes it itself: when a
token is about to expire the hub runs the unmodified `claude` CLI once so that
Claude Code renews it.

Be aware that
[Anthropic's rules on credential use](https://code.claude.com/docs/en/legal-and-compliance#authentication-and-credential-use)
reserve subscription logins to Anthropic's own apps. Reading your own usage
with your own token, as every Clawdmeter does, is a gray area under those rules;
it is your call. This is a personal tool: never run it for someone else's
account.

## Development

| Area              | Where                                                                    |
| ----------------- | ------------------------------------------------------------------------ |
| Firmware          | `firmware/` (PlatformIO, LVGL 9), one folder per board in `firmware/src/boards/` |
| Hub and daemons   | `daemon/`, tests in `daemon/tests/` (`python -m pytest daemon/tests -q`) |
| Desktop simulator | `pio run -d firmware -e sim`, see [`SIM-USAGE.md`](SIM-USAGE.md)         |
| Design notes      | [`docs/ai/wifi-hub-and-accounts.md`](docs/ai/wifi-hub-and-accounts.md)   |
| Board porting     | [`docs/porting/adding-a-board.md`](docs/porting/adding-a-board.md)       |

CI runs the daemon tests, a gitleaks scan of the whole history and the two WiFi
firmware builds on every push and pull request. Pushing a tag `vX.Y.Z` builds
the images with that version on the About page and publishes them, with
checksums, as a GitHub release.

## Credits

- [Hermann Björgvin](https://github.com/HermannBjorgvin/Clawdmeter) wrote
  Clawdmeter; [TheOriUHD](https://github.com/TheOriUHD/Clawdmeter) added the
  Claude Code companion, alerts and the WiFi hub this fork builds on.
- The account pages follow the `slots` idea of
  [egorshar's dual-account fork](https://github.com/egorshar/Clawdmeter).
- Icons from [Lucide](https://lucide.dev).

## License and trademarks

Only this fork's own changes are under the MIT License. Upstream code is
published without a license, and the Anthropic fonts and Clawd mascot art it
bundles are not licensed at all: see [`LICENSE`](LICENSE) before reusing
anything.

Claude, Claude Code and Anthropic are trademarks of Anthropic PBC. This is an
independent hobby project, not affiliated with or endorsed by Anthropic.
