# WiFi hub and several accounts: working notes

Notes kept for future AI-assisted sessions on this fork. They record what was
decided, what was measured, and the traps that cost time. Nothing here is a
secret: credentials live in a password manager, never in the repository.

## Architecture

The display no longer depends on a PC. A small always-on hub polls the usage API
and the display long-polls the hub over the LAN.

| | |
| --- | --- |
| Hub | `daemon/hub.py`, run by `clawdhub.service` in an LXC container |
| Display port | 47394 (`/device/poll`, long-poll), discovered over mDNS as `_clawdmeter._tcp` |
| Firmware env | PlatformIO `waveshare_amoled_216_wifi` (S3) and `waveshare_amoled_216_c6_wifi` (C6) |
| Accounts | one Claude Code config dir per account, listed in `config_dirs` |
| Switching | swipe right on the display; the label is shown in the title |

Why a hub: several computers at home, two Anthropic accounts, possibly several
displays. BLE tied a display to one machine; a hub makes displays
interchangeable and keeps secrets off the device.

## Porting to the S3 needed no new C++

`link_wifi.cpp` reimplements the whole `ble.h` interface under
`CLAWD_LINK_WIFI`. All that was needed was a PlatformIO env that excludes
`ble.cpp` and ignores NimBLE. The S3 has PSRAM, so unlike the C6 there is no
need to trim `LV_MEM_SIZE`.

## Several accounts

The slot logic (`slots.h`, `slots.cpp`) is adapted from the `dual-account`
branch of `egorshar/Clawdmeter`. It is **indexed by label, not by BLE peer**, so
it carried over unchanged. The hub publishes one payload per account under
`acc`, with the active account still at the root, so a firmware that ignores
`acc` keeps working.

Adding an account: run `claude auth login` against a new config dir and add
`path:label` to `config_dirs`. No code changes. `SLOT_MAX` is 3.

## The central trap: refresh tokens rotate

**Two machines cannot share one OAuth session.** Copying `.credentials.json`
creates two holders, and whichever renews first revokes the other. This bit
twice during the build.

Each machine needs its own session (`claude auth login` from inside the
container). `hub-extras/refresh-token.py` plus a 30-minute systemd timer (45
minute margin) then keeps them alive. It reads `config_dirs`, so no account can
be forgotten. The OAuth client ID and endpoint are undocumented (taken from the
`iBrewerRoot` fork): if they change, a fresh `claude auth login` is the way out.

## Silent failure to know about: the refresher

An account whose token expires **disappears from the display without any
message**. The hub skips it (`Token in <dir> expired/invalid; skipping`),
publishes a single account, so there is no `acc` key, so no pages at all. The
swipe looks broken while the firmware is fine. Read
`journalctl -u clawdhub-token` before suspecting the board.

Seen once: `refresh-token.py` treated `~/.claude:work` as a path because the
`:label` suffix of `config_dirs` was not stripped, so it maintained no token at
all for 24 hours and nothing said so. Fixed, but the class of failure remains:
it fails silently and only the journal speaks.

## Other traps met

- A stale Windows `.credentials.json` shadows the WSL one: the daemon probes
  `%USERPROFILE%\.claude\` first, so the result is a `401` although the WSL file
  is valid.
- Long press is unusable as a gesture: its release also fires the tap, which
  jumps to the home screen. Hence the swipe towards the side that had no page and
  used to only bounce.
- Every open of the serial port resets the board (native USB on the S3), so a
  running state cannot be tested that way. That is why the `account` serial
  command exists.
- `AXP2101 init failed` shows up intermittently (I2C error). That chip carries
  the middle button, which explains long presses with no effect.
- Stats are empty on a hub: they come from scanning local `projects/*.jsonl`
  transcripts, and a hub has none. Not a bug.

## Safety nets

Keep known-good firmware images aside and reflash them in about 40 s. Do not
commit them: their build paths embed the developer's home directory.
