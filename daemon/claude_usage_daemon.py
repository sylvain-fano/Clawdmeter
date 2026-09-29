#!/usr/bin/env python3
"""Claude Usage Tracker Daemon (BLE) — macOS port of claude-usage-daemon.sh.

Polls the official OAuth usage endpoint (token-free; also carries the
per-model weekly limits such as Fable), falling back to the Claude API
rate-limit headers, and writes a JSON payload to the ESP32 "Clawdmeter"
peripheral over a custom GATT service. Uses bleak (CoreBluetooth backend
on macOS).
"""

import asyncio
import calendar
import datetime
import getpass
import json
import os
import re
import shutil
import signal
import subprocess
import sys
import time
from pathlib import Path

import httpx

# A WiFi hub (hub.py) reuses this module's usage polling on a headless box that
# has no Bluetooth at all, so the BLE transport is imported softly. Every use of
# BleakClient sits behind the BLE main loop, which such a host never runs.
try:
    from bleak import BleakClient
    from bleak.exc import BleakError
except ImportError:  # pragma: no cover - exercised on hosts without bleak
    BleakClient = None

    class BleakError(Exception):
        pass

# Companion (live Claude Code session state, see companion.py) and the usage
# history behind the device's Trend page (trend.py). Importable both as a plain
# script (`python claude_usage_daemon.py`) and as the daemon package (tests).
try:
    import companion as cc_mod
    import trend as trend_mod
    import tokenkeeper as tk_mod
    import stats as stats_mod
except ImportError:  # pragma: no cover - package import path
    from . import companion as cc_mod
    from . import trend as trend_mod
    from . import tokenkeeper as tk_mod
    from . import stats as stats_mod

DEVICE_NAME = "Clawdmeter"
SERVICE_UUID = "4c41555a-4465-7669-6365-000000000001"
RX_CHAR_UUID = "4c41555a-4465-7669-6365-000000000002"
REQ_CHAR_UUID = "4c41555a-4465-7669-6365-000000000004"

POLL_INTERVAL = 60
TICK = 2                 # inner-loop tick: link check + host-battery watch
HOST_BATT_CHECK_S = 2    # a plug/unplug reaches the device within about this long
COMPANION_PUSH_MIN_S = 0.25   # coalesce bursts of hook events into one BLE write
STATS_REFRESH_S = 300         # rescan the transcripts for the Stats page this often
BLE_PAYLOAD_MAX = 500    # firmware rx buffer is 512 bytes incl. NUL; keep a margin
CONNECT_TIMEOUT = 20.0

# macOS: token lives in Keychain (service "Claude Code-credentials").
# Linux: token lives in ~/.claude/.credentials.json.
KEYCHAIN_SERVICE = "Claude Code-credentials"
DEFAULT_CONFIG_DIR = Path.home() / ".claude"
SAVED_ADDR_FILE = Path.home() / ".config" / "claude-usage-monitor" / "ble-address"
CONFIG_FILE = Path.home() / ".config" / "claude-usage-monitor" / "config"

API_URL = "https://api.anthropic.com/v1/messages"
USAGE_URL = "https://api.anthropic.com/api/oauth/usage"
API_HEADERS_TEMPLATE = {
    "anthropic-version": "2023-06-01",
    "anthropic-beta": "oauth-2025-04-20",
    "Content-Type": "application/json",
    "User-Agent": "claude-code/2.1.5",
}
API_BODY = {
    "model": "claude-haiku-4-5-20251001",
    "max_tokens": 1,
    "messages": [{"role": "user", "content": "hi"}],
}


class TokenExpired(Exception):
    """Raised by poll_api on a 401/403 — the access token is dead. The daemon never
    refreshes (pure free-ride: Claude Code owns refreshing), so the caller just
    signals "No data" to the device until the CLI re-seeds the token."""


def log(msg: str) -> None:
    print(f"[{time.strftime('%H:%M:%S')}] {msg}", flush=True)


def _extract_access_token(blob: str) -> str | None:
    """Pull the accessToken out of a credentials blob.

    Claude Code stores credentials as a JSON object; the blob may also be
    nested ({"claudeAiOauth": {"accessToken": "..."}}). Fall back to a
    regex match so unexpected shapes still work, and finally treat the
    blob as a raw token if nothing else matches.
    """
    blob = blob.strip()
    if not blob:
        return None
    try:
        data = json.loads(blob)
    except json.JSONDecodeError:
        data = None
    if isinstance(data, dict):
        # direct: {"accessToken": "..."}
        tok = data.get("accessToken")
        if isinstance(tok, str) and tok.strip():
            return tok
        # nested: {"claudeAiOauth": {"accessToken": "..."}}
        for v in data.values():
            if isinstance(v, dict):
                tok = v.get("accessToken")
                if isinstance(tok, str) and tok.strip():
                    return tok
    m = re.search(r'"accessToken"\s*:\s*"([^"]+)"', blob)
    if m:
        return m.group(1)
    # Raw token (no JSON wrapper) — must look plausible (sk-ant-... etc.)
    if re.fullmatch(r"[A-Za-z0-9_\-.~+/=]{20,}", blob):
        return blob
    return None


def _decode_keychain_blob(raw: str) -> str:
    """Transparently decode a hex-dumped Keychain secret back to text.

    ``security … -w`` prints the password as a continuous hex string whenever
    the stored bytes aren't cleanly printable (e.g. an embedded newline). A
    normal credentials blob is JSON, which is never valid hex (it contains
    '{', '"', …), so all-hex detection is unambiguous and safe.
    """
    s = raw.strip()
    if s and len(s) % 2 == 0 and re.fullmatch(r"[0-9a-fA-F]+", s):
        try:
            return bytes.fromhex(s).decode("utf-8")
        except (ValueError, UnicodeDecodeError):
            return raw
    return raw


def _extract_expiry_s(blob: str) -> float | None:
    """The stored token's `expiresAt` (ms since the epoch in Claude Code's blob) as seconds, or None."""
    try:
        data = json.loads(blob.strip())
    except (ValueError, AttributeError):
        return None
    if not isinstance(data, dict):
        return None
    for obj in [data] + [v for v in data.values() if isinstance(v, dict)]:
        exp = obj.get("expiresAt")
        if isinstance(exp, (int, float)) and exp > 0:
            return exp / 1000.0 if exp > 1e11 else float(exp)
    return None


def _read_keychain_blob() -> str | None:
    try:
        out = subprocess.run(
            ["security", "find-generic-password", "-s", KEYCHAIN_SERVICE, "-a", getpass.getuser(), "-w"],
            check=True, capture_output=True, text=True, timeout=10)
    except (subprocess.CalledProcessError, FileNotFoundError, subprocess.TimeoutExpired):
        return None
    return _decode_keychain_blob(out.stdout)


def read_token_expiry(config_dir: Path = DEFAULT_CONFIG_DIR) -> float | None:
    """When the stored token expires (seconds since the epoch), or None if unknown."""
    cred = config_dir / ".credentials.json"
    try:
        if cred.exists():
            return _extract_expiry_s(cred.read_text())
    except OSError:
        return None
    if sys.platform == "darwin" and config_dir == DEFAULT_CONFIG_DIR:
        blob = _read_keychain_blob()
        return _extract_expiry_s(blob) if blob else None
    return None


def _read_token_keychain() -> str | None:
    """Read the OAuth access token from the macOS Keychain, or None.

    ``security … -w`` may hex-dump the stored secret (see _decode_keychain_blob),
    so decode before extracting the access token.
    """
    try:
        out = subprocess.run(
            [
                "security",
                "find-generic-password",
                "-s",
                KEYCHAIN_SERVICE,
                "-a",
                getpass.getuser(),
                "-w",
            ],
            check=True,
            capture_output=True,
            text=True,
            timeout=10,
        )
    except subprocess.CalledProcessError as e:
        log(f"Keychain read failed (rc={e.returncode}): {e.stderr.strip()}")
        return None
    except (FileNotFoundError, subprocess.TimeoutExpired) as e:
        log(f"Keychain access error: {e}")
        return None
    return _extract_access_token(_decode_keychain_blob(out.stdout))


# The firmware files its account pages by this label, held in a 12-byte buffer:
# short and pure ASCII are mandatory.
SLOT_ID_MAX = 11
DEFAULT_SLOT = "main"


def read_option(name: str) -> str:
    """Raw value of a `key = value` config line, or "" when absent."""
    raw = ""
    try:
        if CONFIG_FILE.exists():
            for line in CONFIG_FILE.read_text().splitlines():
                line = line.split("#", 1)[0].strip()
                if "=" not in line:
                    continue
                key, val = line.split("=", 1)
                if key.strip().lower() == name:
                    raw = val.strip()
    except OSError:
        pass
    return raw


def _slug(raw: str) -> str:
    """A label the firmware can hold and draw: ASCII, short."""
    return re.sub(r"[^A-Za-z0-9 _-]", "", raw).strip()[:SLOT_ID_MAX]


def read_label() -> str:
    """This machine's label: the name of its page on the display."""
    return _slug(read_option("label")) or DEFAULT_SLOT


def read_config_dir_specs() -> list[tuple[Path, str | None]]:
    """Directories to poll, each with an optional `:label` suffix.

    `config_dirs = ~/.claude:work, ~/.claude-personal:personal` gives one page
    per account on the display. Without labels the original behaviour applies:
    poll every directory and show whichever plan is active.
    """
    raw = read_option("config_dirs")
    if not raw:
        return [(DEFAULT_CONFIG_DIR, None)]
    out: list[tuple[Path, str | None]] = []
    for part in (s.strip() for s in raw.split(",")):
        if not part:
            continue
        label = None
        # A label after a colon, but not the colon of a Windows drive letter,
        # which can only sit at position 1.
        if ":" in part[2:]:
            head, _, tail = part.rpartition(":")
            if head.strip():
                part, label = head.strip(), _slug(tail)
        out.append((Path(part).expanduser(), label or None))
    return out or [(DEFAULT_CONFIG_DIR, None)]


def read_config_dirs() -> list[Path]:
    """Config directories to poll, without their labels."""
    return [d for d, _label in read_config_dir_specs()]


def read_token_for(config_dir: Path) -> str | None:
    """Read the OAuth token for one config dir.

    Linux: each dir keeps its own ``<dir>/.credentials.json``. macOS: the default
    install stores the token in Keychain with no file, so for the default dir we
    fall back to Keychain when no file is present — preserving existing
    single-plan macOS behavior. Additional macOS dirs are read from their files;
    a work plan whose token lives only in the single Keychain entry can't be told
    apart there (documented follow-up).
    """
    cred = config_dir / ".credentials.json"
    try:
        if cred.exists():
            return _extract_access_token(cred.read_text())
    except OSError as e:
        log(f"Error reading credentials in {config_dir}: {e}")
    if sys.platform == "darwin" and config_dir == DEFAULT_CONFIG_DIR:
        return _read_token_keychain()
    return None


def load_cached_address() -> str | None:
    if not SAVED_ADDR_FILE.exists():
        return None
    addr = SAVED_ADDR_FILE.read_text().strip()
    # Accept both Linux MAC (AA:BB:CC:DD:EE:FF) and macOS CoreBluetooth UUID
    # (E621E1F8-C36C-495A-93FC-0C247A3E6E5F).
    if re.fullmatch(r"(?:[0-9A-Fa-f]{2}:){5}[0-9A-Fa-f]{2}", addr) or re.fullmatch(
        r"[0-9A-Fa-f]{8}-(?:[0-9A-Fa-f]{4}-){3}[0-9A-Fa-f]{12}", addr
    ):
        return addr
    log("Cached address malformed, discarding")
    SAVED_ADDR_FILE.unlink(missing_ok=True)
    return None


# --- macOS: recover a device the OS already holds as an HID keyboard --------
#
# The firmware advertises as a BLE HID keyboard so its buttons type into the
# Mac. macOS auto-connects to that HID, and CoreBluetooth then EXCLUDES the
# peripheral from BleakScanner.discover() results (already-connected devices
# never appear in scans). bleak's connect-by-address path also scans
# internally, so a cached address can't help either. The documented escape
# hatch is retrieveConnectedPeripheralsWithServices_, which returns
# peripherals the system is already connected to. We wrap the result in a
# BLEDevice carrying the live (peripheral, manager) details so BleakClient
# connects to it directly without scanning. CoreBluetooth shares the single
# physical link, so this rides the existing HID connection — the keyboard
# keeps working.
_cb_manager = None  # reused CentralManagerDelegate (CoreBluetooth)


async def _get_cb_manager():
    """Lazily create and ready a shared CoreBluetooth central manager."""
    global _cb_manager
    if _cb_manager is None:
        from bleak.backends.corebluetooth.CentralManagerDelegate import (
            CentralManagerDelegate,
        )

        mgr = CentralManagerDelegate()
        await mgr.wait_until_ready()  # raises if Bluetooth is unauthorized/off
        _cb_manager = mgr
    return _cb_manager


async def retrieve_connected_macos(skip_addr: str | None = None):
    """Return a BLEDevice for a system-connected 'Clawdmeter', or None.

    Two-step lookup, strongest signal first:

    1. Peripherals connected under our CUSTOM service UUID. Membership in
       that service is unambiguous (no other device exposes it), so we accept
       by service alone — the peripheral's name can be None on macOS.
    2. Fall back to the generic HID service 0x1812, but ONLY trust a
       peripheral whose name matches DEVICE_NAME. 0x1812 also matches
       unrelated keyboards/mice, so picking blindly here could grab the
       wrong device.

    ``skip_addr`` skips a peripheral whose UUID just failed to connect, so a
    stale CoreBluetooth handle can't trap us into never trying a fresh scan.
    """
    from CoreBluetooth import CBUUID
    from bleak.backends.device import BLEDevice

    try:
        manager = await _get_cb_manager()
    except Exception as e:  # BleakBluetoothNotAvailableError etc.
        log(f"CoreBluetooth unavailable: {e}")
        return None

    cm = manager.central_manager

    def _wrap(p):
        addr = p.identifier().UUIDString()
        log(f"Found system-connected peripheral: {p.name()!r} [{addr}]")
        return BLEDevice(addr, p.name(), (p, manager))

    def _ok(p) -> bool:
        return not (skip_addr and p.identifier().UUIDString() == skip_addr)

    # 1. Custom service — accept by service membership alone.
    custom = cm.retrieveConnectedPeripheralsWithServices_(
        [CBUUID.UUIDWithString_(SERVICE_UUID)]
    )
    for p in custom or []:
        if _ok(p):
            return _wrap(p)

    # 2. Generic HID service — require an exact name match.
    hid = cm.retrieveConnectedPeripheralsWithServices_(
        [CBUUID.UUIDWithString_("1812")]
    )
    for p in hid or []:
        if _ok(p) and p.name() == DEVICE_NAME:
            return _wrap(p)

    return None


async def discover_target(skip_addr: str | None = None):
    """Return a connectable target, or None.

    The daemon only ever targets the device this system already holds — it
    never scans for a nearby device by name, so it can't grab a stranger's or
    the wrong nearby unit. On macOS that's the system-connected peripheral (the
    firmware advertises as an HID keyboard, so once paired the OS auto-connects
    and holds it — HID-grabbed devices are invisible to scans anyway). On other
    platforms it's a previously-pinned address in the cache file. If the device
    isn't held/pinned, we log and wait rather than scanning. ``skip_addr`` skips
    a peripheral whose handle just failed to connect.
    """
    if sys.platform == "darwin":
        dev = await retrieve_connected_macos(skip_addr=skip_addr)
        if dev is None:
            log("Device not held by OS; waiting (not scanning by name)")
        return dev

    address = load_cached_address()
    if not address:
        log("No pinned address cached; waiting (not scanning by name)")
    return address


def read_chime_setting() -> str:
    """Read the `chime` option from the config file. One of: off|on.

    Defaults to "off" (the device stays silent) so existing setups are
    unaffected until the user opts in.
    """
    try:
        if CONFIG_FILE.exists():
            for line in CONFIG_FILE.read_text().splitlines():
                line = line.split("#", 1)[0].strip()
                if "=" not in line:
                    continue
                key, val = line.split("=", 1)
                if key.strip().lower() == "chime":
                    val = val.strip().lower()
                    if val in ("off", "on"):
                        return val
    except OSError:
        pass
    return "off"


def read_clock_setting() -> str:
    """Read the `clock` option from the config file. One of: off|auto|12|24.

    Defaults to "auto": the daemon always ships the wall-clock time and the
    host's hour format, and the DEVICE decides whether to show it (Settings →
    Clock, on the touch screen). Set `clock = off` here to never send time at
    all — then the device can't show a clock regardless of its own setting.
    """
    try:
        if CONFIG_FILE.exists():
            for line in CONFIG_FILE.read_text().splitlines():
                line = line.split("#", 1)[0].strip()
                if "=" not in line:
                    continue
                key, val = line.split("=", 1)
                if key.strip().lower() == "clock":
                    val = val.strip().lower()
                    if val in ("off", "auto", "12", "24"):
                        return val
    except OSError:
        pass
    return "auto"


def read_host_battery_setting() -> str:
    """`host_battery` config option: on|off (default on)."""
    try:
        if CONFIG_FILE.exists():
            for line in CONFIG_FILE.read_text().splitlines():
                line = line.split("#", 1)[0].strip()
                if "=" not in line:
                    continue
                key, val = line.split("=", 1)
                if key.strip().lower() == "host_battery":
                    val = val.strip().lower()
                    if val in ("off", "on"):
                        return val
    except OSError:
        pass
    return "on"


def _config_value(key: str) -> str | None:
    """Raw value of `key` in the config file (last one wins), or None."""
    found = None
    try:
        if CONFIG_FILE.exists():
            for line in CONFIG_FILE.read_text().splitlines():
                line = line.split("#", 1)[0].strip()
                if "=" not in line:
                    continue
                k, v = line.split("=", 1)
                if k.strip().lower() == key:
                    found = v.strip()
    except OSError:
        pass
    return found


def read_companion_setting() -> str:
    """`companion` config option: on|off (default on) — the Claude Code hook listener."""
    v = (_config_value("companion") or "on").lower()
    return v if v in ("on", "off") else "on"


def read_companion_port() -> int:
    try:
        port = int(_config_value("companion_port") or cc_mod.COMPANION_PORT)
    except ValueError:
        return cc_mod.COMPANION_PORT
    return port if 1 <= port <= 65535 else cc_mod.COMPANION_PORT


def read_companion_bind() -> str:
    """`companion_bind`: 0.0.0.0 (default — other machines join over the network,
    token-protected) or 127.0.0.1 to accept only this machine and SSH tunnels."""
    return _config_value("companion_bind") or cc_mod.COMPANION_BIND


TOKEN_FILE = CONFIG_FILE.parent / "companion.token"


def companion_token() -> str:
    """The shared secret remote hooks present (config `companion_token`, else generated)."""
    return cc_mod.load_or_create_token(TOKEN_FILE, _config_value("companion_token"))


def read_trend_setting() -> str:
    """`trend` config option: on|off (default on) — record history for the Trend page."""
    v = (_config_value("trend") or "on").lower()
    return v if v in ("on", "off") else "on"


def read_stats_setting() -> str:
    """`stats` config option: on|off (default on) — the device's Stats page."""
    v = (_config_value("stats") or "on").lower()
    return v if v in ("on", "off") else "on"


def read_token_keeper_setting() -> str:
    """`token_keeper` config option: on|off (default on) — see tokenkeeper.py."""
    v = (_config_value("token_keeper") or "on").lower()
    return v if v in ("on", "off") else "on"


TOKEN_KEEPER = tk_mod.TokenKeeper(cwd=CONFIG_FILE.parent, log=log)


async def keep_token_fresh(reason: str) -> bool:
    """Let Claude Code renew its stored token (once per cooldown) when it is
    expiring or was just rejected. Off with `token_keeper = off`."""
    if read_token_keeper_setting() != "on":
        return False
    return await TOKEN_KEEPER.run(reason)


# --- Companion + Trend -------------------------------------------------------------
#
# The companion listener (companion.py) turns Claude Code hook events into one
# "cc" object; the history (trend.py) turns polls into the "tr" object. Both ride
# on every usage payload, and a companion change also triggers an immediate beat
# so the device reacts within a BLE write of the event.
COMPANION = cc_mod.Companion()
HISTORY: "trend_mod.History | None" = None
HISTORY_FILE = CONFIG_FILE.parent / "history.json"
_wake: "asyncio.Event | None" = None    # wakes the connected loop early (refresh / companion)
_cc_dirty = False


STATE_FILE = CONFIG_FILE.parent / "companion-state.json"   # the session table, across restarts
_state_save_handle: "asyncio.TimerHandle | None" = None


def _schedule_state_save(delay: float = 1.0) -> None:
    """Persist the companion's session table shortly after it changed (bursts of
    hook events coalesce into one write). Skipped under the test guard."""
    global _state_save_handle
    if os.environ.get("CLAWDMETER_NO_LISTENER") or _state_save_handle is not None:
        return
    try:
        loop = asyncio.get_running_loop()
    except RuntimeError:
        return

    def _write() -> None:
        global _state_save_handle
        _state_save_handle = None
        try:
            COMPANION.save(STATE_FILE)
        except OSError as e:
            log(f"Companion: could not save {STATE_FILE}: {e}")

    _state_save_handle = loop.call_later(delay, _write)


def _restore_companion_state() -> None:
    if os.environ.get("CLAWDMETER_NO_LISTENER"):
        return
    n = COMPANION.load(STATE_FILE)
    if n:
        log(f"Companion: restored {n} session(s) from the last run — {COMPANION.describe()}")


def _companion_changed() -> None:
    global _cc_dirty
    _cc_dirty = True
    if _wake is not None:
        _wake.set()
    _schedule_state_save()


def add_companion_fields(payload: dict) -> None:
    if read_companion_setting() != "on":
        return
    cc = COMPANION.summary()
    if cc is not None:
        payload["cc"] = cc


def record_history(payload: dict, now: float | None = None) -> None:
    """Remember this poll for the Trend page (Pro/Max payloads with data only)."""
    if HISTORY is None or not payload.get("ok") or payload.get("acct") == "ent":
        return
    HISTORY.add(time.time() if now is None else now, payload.get("s", 0), payload.get("w", 0))
    HISTORY.save()


def add_trend_fields(payload: dict, now: float | None = None) -> None:
    if HISTORY is None or read_trend_setting() != "on":
        return
    tr = HISTORY.payload(now)
    if tr is not None:
        payload["tr"] = tr


def finalize_payload(payload: dict) -> dict:
    """Everything a fresh usage payload carries beyond the numbers themselves.
    (History is still recorded; the Trend page gave way to the Stats page, so
    "tr" is no longer sent.)"""
    record_history(payload)
    add_companion_fields(payload)
    return payload


# --- Stats page ("st") --------------------------------------------------------------
# Lifetime Claude Code stats from the local transcripts (stats.py): sent as its
# own beat on connect and whenever a rescan changes them (every STATS_REFRESH_S),
# never inside the usage payload — the heatmap alone is ~170 bytes.
STATS: "stats_mod.ClaudeStats | None" = None
STATS_CACHE = CONFIG_FILE.parent / "stats-scan.json"
_st_dirty = False


def stats_project_dirs() -> list[Path]:
    return [d / "projects" for d in read_config_dirs()]


def stats_payload() -> dict | None:
    if STATS is None or read_stats_setting() != "on":
        return None
    return STATS.payload()


async def stats_loop() -> None:
    """Background rescan; wakes the device loop when the numbers moved."""
    global _st_dirty
    while True:
        try:
            changed = await asyncio.to_thread(STATS.refresh) if STATS is not None else False
        except Exception as e:  # noqa: BLE001 - never let a scan hiccup kill the daemon
            log(f"Stats scan failed: {e}")
            changed = False
        if changed:
            _st_dirty = True
            if _wake is not None:
                _wake.set()
        await asyncio.sleep(STATS_REFRESH_S)


def companion_beat(last_payload: dict | None) -> dict:
    """The last usage payload re-stamped with the current companion state.

    Before any usage data exists the beat is companion-only ({"cc": …}); the
    firmware applies "cc" independently of the usage numbers, so the Working
    page comes alive even while the token is still being looked up.
    """
    beat = dict(last_payload) if last_payload else {}
    beat.pop("cc", None)
    cc = COMPANION.summary()
    if cc is not None:
        beat["cc"] = cc
    if last_payload:
        add_clock_fields(beat)
    return beat


def shrink_payload(payload: dict, limit: int = BLE_PAYLOAD_MAX) -> bytes:
    """Encode compactly; drop the optional extras (trend, then companion) if it
    would overflow the firmware's receive buffer."""
    def enc(p: dict) -> bytes:
        # UTF-8 on the wire: "…" is 3 bytes, its \u escape would be 6 and the
        # firmware buffer is counted in bytes.
        return json.dumps(p, separators=(",", ":"), ensure_ascii=False).encode()
    data = enc(payload)
    for key in ("tr", "cc"):
        if len(data) <= limit:
            break
        if key in payload:
            payload = {k: v for k, v in payload.items() if k != key}
            data = enc(payload)
    return data


# --- Host battery ---------------------------------------------------------------
#
# The board's own battery glyph means nothing on a USB-powered Clawdmeter (the
# PMU reports "no cell"), so the daemon offers the HOST's battery instead —
# the MacBook's charge, on the desk display. Bluetooth's Battery Service only
# runs peripheral → central, hence it rides along in our own payload as
# "hb" (percent) + "hc" (1 = charging). Desktop Macs simply omit the fields.
def parse_pmset_batt(text: str) -> tuple[int, bool] | None:
    """(percent, plugged_in) from `pmset -g batt` output, or None when no battery.

    Sample: `Now drawing from 'AC Power'` then
    ` -InternalBattery-0 (id=…)\t85%; charging; 1:03 remaining present: true`.
    The flag is "plugged in", not "charging": macOS pauses charging while on
    AC ("AC attached; not charging", "charged" at 100%) and the device should
    still show the cable is in — that's what the user reacts to.
    """
    text = text or ""
    m = re.search(r"(\d{1,3})%;\s*([^;\n]+)", text)
    if not m:
        return None
    pct = max(0, min(100, int(m.group(1))))
    state = m.group(2).strip().lower()
    plugged = ("'ac power'" in text.lower()) or \
              state in ("charging", "finishing charge", "charged", "ac attached")
    return pct, plugged


def read_host_battery() -> tuple[int, bool] | None:
    if sys.platform != "darwin":
        return None
    try:
        out = subprocess.run(["pmset", "-g", "batt"], capture_output=True, text=True, timeout=3)
    except (OSError, subprocess.SubprocessError):
        return None
    return parse_pmset_batt(out.stdout)


def host_battery_state() -> tuple[int, bool] | None:
    """(percent, charging) as the payload would carry it; None = disabled / no battery."""
    if read_host_battery_setting() != "on":
        return None
    return read_host_battery()


def add_host_battery_fields(payload: dict) -> None:
    """Add "hb"/"hc" (host battery percent / charging) unless disabled or absent."""
    hb = host_battery_state()
    if hb is None:
        return
    payload["hb"] = hb[0]
    payload["hc"] = 1 if hb[1] else 0


def payload_battery_state(payload: dict) -> tuple[int, bool] | None:
    """The host battery a payload carries, in host_battery_state() shape."""
    if "hb" not in payload:
        return None
    return int(payload["hb"]), bool(payload.get("hc"))


def host_battery_beat(last_payload: dict, state: tuple[int, bool] | None) -> dict:
    """The last usage payload re-stamped with the current host battery.

    Battery changes (plugging in, a percent step) must not wait for the 60 s
    usage poll: the inner loop watches the host battery every
    HOST_BATT_CHECK_S and, on a change, re-sends the last usage numbers with
    fresh "hb"/"hc" (and a fresh clock) so the device updates within seconds.
    The numbers are unchanged, so the device just re-renders them.
    """
    beat = dict(last_payload)
    beat.pop("hb", None)
    beat.pop("hc", None)
    if state is not None:
        beat["hb"] = state[0]
        beat["hc"] = 1 if state[1] else 0
    add_clock_fields(beat)
    return beat


def add_chime_field(payload: dict) -> None:
    """Add "c":1 to the payload when the config opts in, so the firmware may
    sound the session-reset chime. Omitted entirely when chime is off."""
    if read_chime_setting() == "on":
        payload["c"] = 1


def detect_hour_format() -> int:
    """Best-effort 12h/24h detection for the host. Returns 12 or 24 (default 24)."""
    # macOS: the explicit System Settings toggle lives in NSGlobalDomain.
    for key, result in (("AppleICUForce24HourTime", 24), ("AppleICUForce12HourTime", 12)):
        try:
            out = subprocess.run(["defaults", "read", "-g", key],
                                 capture_output=True, text=True, timeout=3)
            if out.stdout.strip() == "1":
                return result
        except (OSError, subprocess.SubprocessError):
            pass
    # Fallback to the C locale's time format (may be C/24h under launchd).
    try:
        import locale
        locale.setlocale(locale.LC_TIME, "")
        fmt = locale.nl_langinfo(locale.T_FMT)
        if "%p" in fmt or "%r" in fmt or "%I" in fmt:
            return 12
    except (ImportError, locale.Error, AttributeError):
        pass
    return 24


def add_clock_fields(payload: dict) -> None:
    """Add wall-clock fields to the payload when the config opts in.

    "t"  = local wall-clock epoch (UTC epoch shifted by the tz offset) so the
           device can show the time without an RTC.
    "tf" = 12 or 24, the hour format the device should render.
    """
    clock = read_clock_setting()
    if clock == "off":
        return
    tf = 24 if clock == "24" else 12 if clock == "12" else detect_hour_format()
    payload["t"] = int(time.time()) + time.localtime().tm_gmtoff
    payload["tf"] = tf


# --- Official usage endpoint ---------------------------------------------------
#
# GET /api/oauth/usage is what Claude Code's own `/usage` screen reads. It
# returns the same 5h/7d windows the rate-limit headers carry PLUS the
# per-model weekly limits (limits[] entries of kind "weekly_scoped" with a
# model scope — e.g. "Fable" on Max plans), and it costs zero tokens, whereas
# the header method spends a 1-token Haiku message per poll (~1,440/day).
#
# It is undocumented and was rate-limited once before (#29 → reverted in #37,
# under a 5s retry loop), so: fixed 60s cadence, a 15-minute bench after any
# 429, and the header method as the automatic fallback for every failure or
# unrecognised shape. Behaviour can never be worse than the header method.
USAGE_ENDPOINT_COOLDOWN_S = 900
SCOPED_NAME_MAX = 15                    # firmware ScopedWeekly.name is char[16]
_usage_endpoint_cooldown_until = 0.0
_usage_source: str | None = None        # "endpoint" | "headers" — logged on change


def _note_usage_source(src: str) -> None:
    global _usage_source
    if src != _usage_source:
        _usage_source = src
        log("Usage source: " + ("OAuth usage endpoint" if src == "endpoint"
                                else "rate-limit headers (fallback)"))


def _iso_reset_minutes(iso, now: float) -> int:
    """Minutes from `now` until an ISO-8601 reset timestamp; 0 if past/invalid."""
    if not isinstance(iso, str) or not iso:
        return 0
    try:
        ts = datetime.datetime.fromisoformat(iso.replace("Z", "+00:00"))
    except ValueError:
        return 0
    if ts.tzinfo is None:
        ts = ts.replace(tzinfo=datetime.timezone.utc)
    try:
        mins = (ts.timestamp() - now) / 60.0
    except (OSError, OverflowError):
        return 0
    return int(round(mins)) if mins > 0 else 0


def _clamp_pct(value) -> int | None:
    try:
        return max(0, min(100, int(round(float(value)))))
    except (TypeError, ValueError):
        return None


def scoped_weekly_limits(limits) -> list[dict]:
    """[{"n": <label>, "p": <0-100>}, ...] for every weekly scoped-model limit.

    The label is the API's display name (falling back to the model id), cut to
    the firmware's buffer. Accounts without scoped limits yield [] and the
    "ws" key is omitted — key absence, never 0%, is the "no such limit" signal.
    """
    out: list[dict] = []
    if not isinstance(limits, list):
        return out
    for lim in limits:
        if not isinstance(lim, dict) or lim.get("kind") != "weekly_scoped":
            continue
        scope = lim.get("scope")
        model = scope.get("model") if isinstance(scope, dict) else None
        if not isinstance(model, dict):
            continue
        name = model.get("display_name") or model.get("id")
        pct = _clamp_pct(lim.get("percent"))
        if not isinstance(name, str) or not name or pct is None:
            continue
        out.append({"n": name[:SCOPED_NAME_MAX], "p": pct})
    return out


def parse_usage_response(data, now: float) -> dict | None:
    """Build the Pro/Max device payload from the usage endpoint's JSON.

    None when the response isn't the full Pro/Max shape (both the 5h and 7d
    windows present): Enterprise spending-limit accounts have no weekly window
    and stay on the header method, which remains the sole authority on
    Enterprise detection. Utilization here is already a 0-100 percentage (the
    headers use a 0-1 fraction).
    """
    if not isinstance(data, dict):
        return None
    five = data.get("five_hour")
    seven = data.get("seven_day")
    s = _clamp_pct(five.get("utilization")) if isinstance(five, dict) else None
    w = _clamp_pct(seven.get("utilization")) if isinstance(seven, dict) else None
    if s is None or w is None:
        return None
    payload = {
        "s": s,
        "sr": _iso_reset_minutes(five.get("resets_at"), now),
        "w": w,
        "wr": _iso_reset_minutes(seven.get("resets_at"), now),
        "st": "allowed" if s < 100 else "rejected",
        "acct": "pro",
        "ok": True,
    }
    ws = scoped_weekly_limits(data.get("limits"))
    if ws:
        payload["ws"] = ws
    return payload


async def poll_usage_endpoint(token: str) -> dict | None:
    """Poll the official usage endpoint; None means "use the header fallback".

    Every failure is a None — including 401/403 — so poll_api() stays the one
    place that decides a token is dead (TokenExpired). A 429 additionally
    benches the endpoint for USAGE_ENDPOINT_COOLDOWN_S, so a rate-limited
    endpoint is never re-hammered on every cycle.
    """
    global _usage_endpoint_cooldown_until
    now = time.time()
    if now < _usage_endpoint_cooldown_until:
        return None
    headers = {
        "Authorization": f"Bearer {token}",
        "anthropic-beta": API_HEADERS_TEMPLATE["anthropic-beta"],
        "User-Agent": API_HEADERS_TEMPLATE["User-Agent"],
        "Accept": "application/json",
    }
    try:
        async with httpx.AsyncClient(timeout=20.0) as http:
            resp = await http.get(USAGE_URL, headers=headers)
    except httpx.HTTPError as e:
        log(f"Usage endpoint failed: {e}")
        return None
    if resp.status_code == 429:
        _usage_endpoint_cooldown_until = now + USAGE_ENDPOINT_COOLDOWN_S
        log(f"Usage endpoint rate-limited (429) — benched for "
            f"{USAGE_ENDPOINT_COOLDOWN_S // 60} min, using the header fallback")
        return None
    if resp.status_code != 200:
        log(f"Usage endpoint HTTP {resp.status_code}: {resp.text[:200]}")
        return None
    try:
        data = resp.json()
    except ValueError:
        log("Usage endpoint returned non-JSON")
        return None
    payload = parse_usage_response(data, now)
    if payload is None:
        return None            # not a Pro/Max shape — let the header method classify it
    add_chime_field(payload)   # adds "c":1 iff the config opts in
    add_clock_fields(payload)  # adds "t" + "tf" unless the config turns the clock off
    add_host_battery_fields(payload)   # adds "hb"/"hc" — the host's battery for the header glyph
    return payload


async def poll_api(token: str) -> dict | None:
    headers = dict(API_HEADERS_TEMPLATE)
    headers["Authorization"] = f"Bearer {token}"
    try:
        async with httpx.AsyncClient(timeout=20.0) as http:
            resp = await http.post(API_URL, headers=headers, json=API_BODY)
    except httpx.HTTPError as e:
        log(f"API call failed: {e}")
        return None
    if resp.status_code in (401, 403):
        log(f"API HTTP {resp.status_code} (token expired/invalid)")
        raise TokenExpired()
    if resp.status_code >= 400:
        log(f"API HTTP {resp.status_code}: {resp.text[:200]}")
        return None

    def hdr(name: str, default: str = "0") -> str:
        return resp.headers.get(name, default)

    now = time.time()

    def reset_minutes(reset_ts: str) -> int:
        try:
            r = float(reset_ts)
        except ValueError:
            return 0
        mins = (r - now) / 60.0
        return int(round(mins)) if mins > 0 else 0

    def pct(util: str) -> int:
        try:
            return int(round(float(util) * 100))
        except ValueError:
            return 0

    # Pro/Max accounts expose 5h/7d windows; Enterprise/overage use a single
    # spending-limit model reported via overage-utilization.
    if resp.headers.get("anthropic-ratelimit-unified-5h-utilization"):
        payload = {
            "s": pct(hdr("anthropic-ratelimit-unified-5h-utilization")),
            "sr": reset_minutes(hdr("anthropic-ratelimit-unified-5h-reset")),
            "w": pct(hdr("anthropic-ratelimit-unified-7d-utilization")),
            "wr": reset_minutes(hdr("anthropic-ratelimit-unified-7d-reset")),
            "st": hdr("anthropic-ratelimit-unified-5h-status", "unknown"),
            "acct": "pro",
            "ok": True,
        }
    else:
        reset_ts = hdr("anthropic-ratelimit-unified-overage-reset")
        payload = {
            "s": pct(hdr("anthropic-ratelimit-unified-overage-utilization")),
            "sr": reset_minutes(reset_ts),
            "w": 0,
            "wr": 0,
            "st": hdr("anthropic-ratelimit-unified-status", "unknown"),
            "acct": "ent",
            **_billing_period_info(now, reset_ts),
            "ok": True,
        }
    add_chime_field(payload)   # adds "c":1 iff the config opts in
    add_clock_fields(payload)   # adds "t" + "tf" iff the config opts in
    add_host_battery_fields(payload)
    return payload


def _billing_period_info(now: float, reset_ts: str) -> dict:
    """Fraction of billing period elapsed (tp, 0-100) and period length in days (pd).

    Billing periods are assumed calendar-monthly: period_end is the reset
    timestamp, period_start is the same day/time one calendar month earlier.

    The rate-limit headers expose only the reset timestamp, not the period
    length, so the monthly window is an assumption — but a documented one:
    Enterprise spend-limit `period` "the only value today is monthly"
    (Claude Enterprise Admin API reference). The doc notes period is an open
    string that may gain other values later; revisit this if so.
    """
    try:
        period_end = float(reset_ts)
    except ValueError:
        return {"tp": 0, "pd": 30}
    if period_end <= 0:
        # reset_ts defaults to "0" when the overage-reset header is absent.
        # fromtimestamp(0) is 1970; stepping a month back lands in 1969, and
        # datetime.timestamp() raises OSError for pre-1970 dates on Windows.
        # Benign on macOS/Linux, but guard here too to keep the daemons parallel.
        return {"tp": 0, "pd": 30}
    dt_end = datetime.datetime.fromtimestamp(period_end)
    prev_month = dt_end.month - 1 or 12
    prev_year = dt_end.year if dt_end.month > 1 else dt_end.year - 1
    prev_day = min(dt_end.day, calendar.monthrange(prev_year, prev_month)[1])
    dt_start = dt_end.replace(year=prev_year, month=prev_month, day=prev_day)
    period_start = dt_start.timestamp()
    period_len = period_end - period_start
    if period_len <= 0:
        return {"tp": 0, "pd": 30}
    pct_val = (now - period_start) / period_len * 100
    total_days = int(round(period_len / 86400))
    rd = f"{dt_end.strftime('%b')} {dt_end.day}"
    return {
        "tp": max(0, min(100, int(round(pct_val)))),
        "pd": total_days,
        "rd": rd,
    }


class PlanSelector:
    """Decide which config dir's plan is "active" across polls.

    "Active" = the plan whose session % rose most recently (recent API activity).
    A rise stamps a monotonic poll counter, so the choice is sticky and a window
    reset (a drop to 0) isn't mistaken for use. Before any rise is seen (startup)
    the highest current session % wins. Mirrors the Linux bash daemon.
    """

    def __init__(self) -> None:
        self.prev_s: dict[Path, int] = {}
        self.last_active: dict[Path, int] = {}
        self.seq = 0

    def choose(self, sessions: dict[Path, int]) -> Path:
        """Update state from this cycle's {dir: session_pct} and return the active dir."""
        self.seq += 1
        for d, s in sessions.items():
            if d in self.prev_s and s > self.prev_s[d]:
                self.last_active[d] = self.seq
            self.prev_s[d] = s
        # Most recent activity wins; ties (and the startup case) break by highest %.
        return max(sessions, key=lambda d: (self.last_active.get(d, 0), sessions[d]))


# Module-level so the active-plan state survives reconnects.
_SELECTOR = PlanSelector()


async def poll_slots(selector: PlanSelector = _SELECTOR) -> tuple[list[dict], bool]:
    """The payloads to push this cycle, one per account page, plus ``all_dead``.

    With labelled directories each account gets its own payload (tagged with an
    ``id``), so a single machine can feed several pages of the display. Without
    labels this is exactly the original path: one payload, the active plan's.
    """
    specs = read_config_dir_specs()
    payloads: dict[Path, dict] = {}
    sessions: dict[Path, int] = {}
    any_live = False
    for d, _label in specs:
        token = read_token_for(d)
        if not token:
            log(f"No token in {d}; skipping")
            continue
        try:
            payload = await poll_usage_endpoint(token)
            if payload is not None:
                _note_usage_source("endpoint")
            else:
                payload = await poll_api(token)
                if payload is not None:
                    _note_usage_source("headers")
        except TokenExpired:
            log(f"Token in {d} expired/invalid; skipping")
            continue
        except Exception as e:
            log(f"Poll error for {d}: {type(e).__name__}: {e}")
            any_live = True          # not an auth failure: retry without showing "no data"
            continue
        any_live = True
        if payload is not None:
            payloads[d] = payload
            sessions[d] = int(payload.get("s", 0) or 0)

    labelled = [(d, l) for d, l in specs if l]
    if len(labelled) > 1:
        out = []
        for d, l in labelled:
            if d in payloads:
                tagged = dict(payloads[d])
                tagged["id"] = l
                out.append(tagged)
        return out, (not any_live)

    if not payloads:
        return [], (not any_live)
    active = selector.choose(sessions)
    if len(specs) > 1:
        log(f"Active plan: {active} (s={sessions[active]})")
    one = dict(payloads[active])
    label = labelled[0][1] if labelled else read_label()
    if label != DEFAULT_SLOT:
        one["id"] = label
    return [one], False


def slot_labels() -> list[str]:
    """Every account page this machine is responsible for."""
    labelled = [l for _d, l in read_config_dir_specs() if l]
    return labelled if len(labelled) > 1 else [labelled[0] if labelled else read_label()]


async def poll_active(selector: PlanSelector = _SELECTOR) -> tuple[dict | None, bool]:
    """Poll every configured config dir; return ``(active_payload, all_dead)``.

    ``active_payload`` — the active plan's payload dict, or None when no dir
    yields a usable payload this cycle. A single configured dir (the default)
    collapses to exactly the old single-poll path.

    ``all_dead`` — True when *every* configured dir lacked a usable token this
    cycle (file/Keychain empty, or a 401/expired token), so the caller can
    signal "No data". False when at least one token authenticated — including a
    transient non-auth poll failure worth retrying silently rather than idling.

    Pure free-ride: a 401 (TokenExpired) means that dir's token has expired and
    only Claude Code (its owner) can re-seed it — we never refresh it ourselves.
    """
    dirs = read_config_dirs()
    payloads: dict[Path, dict] = {}
    sessions: dict[Path, int] = {}
    any_live = False
    for d in dirs:
        token = read_token_for(d)
        if not token:
            log(f"No token in {d}; skipping")
            continue
        try:
            # Prefer the official usage endpoint (token-free; carries the
            # per-model weekly limits). Fall back to the rate-limit headers of
            # a 1-token message call whenever it's unavailable.
            payload = await poll_usage_endpoint(token)
            if payload is not None:
                _note_usage_source("endpoint")
            else:
                payload = await poll_api(token)
                if payload is not None:
                    _note_usage_source("headers")
        except TokenExpired:
            log(f"Token in {d} expired/invalid; skipping")
            continue
        # Authenticated: a transient None here isn't an auth failure, so the
        # dir counts as live and we stay silent rather than idling the device.
        any_live = True
        if payload is not None:
            payloads[d] = payload
            sessions[d] = int(payload.get("s", 0) or 0)
    if not payloads:
        return None, not any_live
    active = selector.choose(sessions)
    if len(dirs) > 1:
        log(f"Active plan: {active} (s={sessions[active]})")
    return finalize_payload(payloads[active]), False


async def poll_active_payload(selector: PlanSelector = _SELECTOR) -> dict | None:
    """The active plan's payload, or None when no dir yields one this cycle.

    Thin wrapper over :func:`poll_active` for callers that don't need the
    all-dead flag.
    """
    payload, _dead = await poll_active(selector)
    return payload


class Session:
    def __init__(self, client: BleakClient) -> None:
        self.client = client
        self.refresh_requested = asyncio.Event()

    def _on_refresh(self, _char, _data: bytearray) -> None:
        log("Refresh requested by device")
        self.refresh_requested.set()
        if _wake is not None:
            _wake.set()

    async def setup_refresh_subscription(self) -> None:
        # start_notify awaits CoreBluetooth's CCCD-write confirmation, which
        # never arrives if the peripheral doesn't ACK the subscribe (a
        # half-open link after the OS auto-connects the HID). Unbounded, that
        # await wedges the whole daemon between "Connected" and the first poll
        # — the device then shows nothing until a manual restart. Bound it: the
        # subscription is only an optional device-initiated refresh nudge (we
        # poll every POLL_INTERVAL regardless), so on timeout we proceed.
        try:
            await asyncio.wait_for(
                self.client.start_notify(REQ_CHAR_UUID, self._on_refresh),
                timeout=10,
            )
        except (BleakError, ValueError) as e:
            log(f"Refresh subscription unavailable: {e}")
        except asyncio.TimeoutError:
            log("Refresh subscription timed out; polling without it")

    async def write_payload(self, payload: dict, note: str | None = None) -> bool:
        data = shrink_payload(payload)
        # Companion beats can come several times a minute; log them as one
        # short line instead of the full payload.
        log(f"Sending: {data.decode()}" if note is None else f"Sending ({len(data)} B): {note}")
        try:
            # Write-without-response is capped at MTU-3 bytes; anything longer
            # (trend + companion extras) goes as a long write with response.
            try:
                mtu = int(self.client.mtu_size or 23)
            except Exception:  # noqa: BLE001 - backend without MTU info
                mtu = 23
            with_response = len(data) > max(20, mtu - 3)
            await self.client.write_gatt_char(RX_CHAR_UUID, data, response=with_response)
            return True
        except BleakError as e:
            log(f"Write failed: {e}")
            return False


def _is_encryption_error(exc: BaseException) -> bool:
    """True if a connect error is a macOS bonding/encryption mismatch.

    macOS reports a stale bond as CBErrorDomain Code=15 ("Failed to encrypt
    the connection..."). Match on the message text so we don't depend on how
    bleak wraps the underlying CoreBluetooth error.
    """
    s = str(exc).lower()
    return "code=15" in s or "encrypt" in s


# blueutil talks to Bluetooth via IOBluetooth, which on recent macOS needs its
# OWN Bluetooth TCC grant (separate from the daemon's CoreBluetooth grant).
# Without it, blueutil *hangs* instead of erroring — so every call is bounded
# by a timeout and a hang is reported as a permission problem, not a crash.
BLUEUTIL_TIMEOUT = 8


def _blueutil(*args: str) -> str | None:
    """Run `blueutil <args>`, returning stdout, or None on failure/timeout.

    A timeout almost always means blueutil lacks Bluetooth permission (it
    blocks rather than failing), so we surface that cause explicitly.
    """
    try:
        return subprocess.run(
            ["blueutil", *args],
            capture_output=True, text=True,
            timeout=BLUEUTIL_TIMEOUT, check=True,
        ).stdout
    except subprocess.TimeoutExpired:
        log(f"blueutil {' '.join(args)} timed out — it likely lacks Bluetooth "
            "permission. Grant it under System Settings > Privacy & Security > "
            "Bluetooth (run `blueutil --paired` once from Terminal to prompt).")
        return None
    except (subprocess.SubprocessError, OSError) as e:
        log(f"blueutil {' '.join(args)} failed: {e}")
        return None


def unpair_macos() -> bool:
    """Forget a stale macOS bond for DEVICE_NAME so the device can re-pair.

    A Code=15 "failed to encrypt" connect error means macOS holds bonding
    keys that no longer match the ESP32's (e.g. after a firmware reflash or
    the on-device bond-clear gesture). The firmware pairs "just works" (no
    MITM), so once the stale bond is gone the next connect re-bonds silently
    with no GUI prompt.

    CoreBluetooth exposes no unpair API, so we shell out to `blueutil`. The
    daemon only knows the peripheral's CoreBluetooth UUID, not the BD_ADDR
    that blueutil needs, so we map by name via `blueutil --paired`. Returns
    True if a bond was removed. Mirrors the Linux daemon's `bluetoothctl
    remove` self-heal.
    """
    if not shutil.which("blueutil"):
        log("Stale bond detected but `blueutil` is not installed; cannot "
            "auto-recover. Run `brew install blueutil`, or forget "
            f"'{DEVICE_NAME}' in System Settings > Bluetooth and reconnect.")
        return False

    out = _blueutil("--paired")
    if out is None:
        return False

    # Each line looks like:
    #   address: 28-84-85-55-5c-3d, ... name: "Clawdmeter", ...
    addr = None
    for line in out.splitlines():
        if f'name: "{DEVICE_NAME}"' in line:
            m = re.search(r"address:\s*([0-9a-fA-F:-]+)", line)
            if m:
                addr = m.group(1)
                break
    if not addr:
        log(f"No paired '{DEVICE_NAME}' found to unpair (already forgotten?)")
        return False

    if _blueutil("--unpair", addr) is None:
        return False
    log(f"Unpaired stale bond for '{DEVICE_NAME}' [{addr}]; re-pairing on "
        "next connect")
    return True


async def connect_and_run(target, stop_event: asyncio.Event) -> bool:
    """Connect to a target and poll until disconnected or stopped.

    ``target`` is either an address string (Linux) or a BLEDevice carrying
    live CoreBluetooth details (macOS). Returns True if the connection was
    used successfully (so the caller keeps the cached address), False if the
    connection failed and the cache should be invalidated.
    """
    display = target if isinstance(target, str) else target.address
    log(f"Connecting to {display}...")
    client = BleakClient(target)
    try:
        # Bound the connect the same way #84 bounded the refresh subscribe.
        # On macOS the OS auto-connects the firmware's HID link, so
        # CoreBluetooth can hand us a half-open peripheral whose GATT connect
        # handshake never completes. BleakClient's own timeout governs
        # discovery, not connectPeripheral, so an unbounded await here wedges
        # the single-threaded daemon forever at "Connecting..." (observed ~13h,
        # device stuck on stale data). wait_for raises TimeoutError, which the
        # handler below already treats as a connection failure -> drop the
        # cached address and rescan.
        await asyncio.wait_for(client.connect(), timeout=CONNECT_TIMEOUT)
    except (BleakError, asyncio.TimeoutError) as e:
        log(f"Connection failed: {e}")
        if sys.platform == "darwin" and _is_encryption_error(e):
            log("Encryption failed — likely a stale macOS bond; self-healing")
            unpair_macos()
        return False

    if not client.is_connected:
        log("Connection failed (no error but not connected)")
        return False

    log("Connected")
    session = Session(client)
    await session.setup_refresh_subscription()

    global _cc_dirty, _wake
    _wake = asyncio.Event()                     # per connection: wakes the loop on refresh / hook events
    last_poll = 0.0
    used_successfully = False
    last_payload: dict | None = None            # last usage payload that reached the device
    last_batt: tuple[int, bool] | None = None   # host battery it carried
    last_batt_check = 0.0
    last_cc_push = 0.0
    _cc_dirty = True                            # a fresh link gets the companion state at once
    global _st_dirty
    _st_dirty = True                            # … and the Stats page numbers
    try:
        while client.is_connected and not stop_event.is_set():
            now = time.time()
            elapsed = now - last_poll
            if session.refresh_requested.is_set() or elapsed >= POLL_INTERVAL:
                session.refresh_requested.clear()
                # Pure free-ride: read whatever access token(s) Claude Code
                # currently holds across the configured config dirs and NEVER
                # refresh them ourselves. Claude Code (the token's owner) does all
                # refreshing; refreshing here would race its rotation and feed the
                # OAuth endpoint's rate limit (429). When no dir has a usable token
                # we signal "No data" so the device idles instead of holding stale
                # numbers until the CLI re-seeds it.
                # The stored token lives ~8 h and only Claude Code renews it.
                # Nudge it shortly before expiry, and again if a poll comes back
                # 401 — so a desktop-app-only user never sees "No data".
                exp = read_token_expiry()
                if tk_mod.due(exp) and TOKEN_KEEPER.can_run():
                    await keep_token_fresh("stored token expires soon" if exp and exp > now else "stored token expired")
                payload, dead = await poll_active()
                if payload is None and dead and await keep_token_fresh("token rejected (401)"):
                    payload, dead = await poll_active()          # renewed → try once more right away
                if payload is not None:
                    if await session.write_payload(payload):
                        last_poll = time.time()
                        used_successfully = True
                        last_payload = payload
                        last_batt = payload_battery_state(payload)
                        last_batt_check = last_poll
                elif dead:
                    # No live token in any config dir (missing, or a 401/expired
                    # token) -> show "No data" now instead of stale numbers. Guard
                    # last_poll on the write result (like the data path) so a
                    # failed beat retries next tick instead of throttling what may
                    # be a healthy link for a full POLL_INTERVAL.
                    log("No usable token; signalling no-data to device — run "
                        "`claude login` or use the CLI to let Claude Code renew it")
                    if await session.write_payload({"ok": False}):
                        last_poll = time.time()
                else:
                    # Transient poll failure (a live token that didn't answer this
                    # cycle) -> stay silent and retry next tick.
                    log("No usable config dir this cycle")
            elif last_payload is not None and now - last_batt_check >= HOST_BATT_CHECK_S:
                # Between polls: push the host battery the moment it changes.
                last_batt_check = now
                state = host_battery_state()
                if state != last_batt:
                    log(f"Host battery {last_batt} -> {state}; pushing now")
                    if await session.write_payload(host_battery_beat(last_payload, state)):
                        last_batt = state

            # Companion: a Claude Code hook event changed the live state (or a
            # session timed out) → re-send the last numbers with the new "cc"
            # right away, coalescing bursts into one write.
            if COMPANION.expire():
                _cc_dirty = True
                _schedule_state_save()
            if _cc_dirty and read_companion_setting() == "on" and now - last_cc_push >= COMPANION_PUSH_MIN_S:
                _cc_dirty = False
                last_cc_push = now
                beat = companion_beat(last_payload)
                if "cc" in beat and not await session.write_payload(beat, note=COMPANION.describe()):
                    _cc_dirty = True                # retry on the next tick

            # Stats page: its own small beat, only when the numbers changed.
            if _st_dirty:
                st = stats_payload()
                _st_dirty = False
                if st is not None and not await session.write_payload({"st": st}, note="stats page"):
                    _st_dirty = True

            try:
                if _wake is not None:
                    await asyncio.wait_for(_wake.wait(), timeout=TICK)
                    _wake.clear()
                else:
                    await asyncio.wait_for(session.refresh_requested.wait(), timeout=TICK)
            except asyncio.TimeoutError:
                pass
    finally:
        try:
            await client.disconnect()
        except BleakError:
            pass

    log("Device disconnected" if not stop_event.is_set() else "Stopping")
    return used_successfully


async def main() -> None:
    stop_event = asyncio.Event()
    loop = asyncio.get_running_loop()

    def _stop(*_args: object) -> None:
        log("Daemon stopping")
        stop_event.set()

    for sig in (signal.SIGINT, signal.SIGTERM):
        try:
            loop.add_signal_handler(sig, _stop)
        except NotImplementedError:
            signal.signal(sig, _stop)

    log("=== Claude Usage Tracker Daemon (BLE, macOS) ===")
    log(f"Poll interval: {POLL_INTERVAL}s")

    global HISTORY, STATS
    HISTORY = trend_mod.History(HISTORY_FILE)
    if HISTORY.samples:
        log(f"Trend history: {len(HISTORY.samples)} samples in {HISTORY_FILE}")
    stats_task = None
    if read_stats_setting() == "on" and not os.environ.get("CLAWDMETER_NO_LISTENER"):
        STATS = stats_mod.ClaudeStats(stats_project_dirs(), STATS_CACHE,
                                      exclude_substrings=(CONFIG_FILE.parent.name,), log=log)
        stats_task = asyncio.create_task(stats_loop())
    cc_server = None
    COMPANION.log = log
    _restore_companion_state()
    if os.environ.get("CLAWDMETER_NO_LISTENER"):
        log("Companion listener off (CLAWDMETER_NO_LISTENER)")
    elif read_companion_setting() == "on":
        token = companion_token()
        cc_server = await cc_mod.start_companion_server(
            COMPANION, _companion_changed, read_companion_bind(), read_companion_port(), log=log, token=token)
        if cc_server is not None:
            for line in cc_mod.join_text(read_companion_port(), token).splitlines():
                log("  " + line if line else "")
    else:
        log("Companion listener off (config: companion = off)")

    backoff = 1
    skip_addr: str | None = None  # macOS: a peripheral to skip for one cycle
    while not stop_event.is_set():
        # Apply any pending skip exactly once, then clear it so the next
        # cycle re-tries retrieveConnected (the device may have recovered).
        target = await discover_target(skip_addr=skip_addr)
        skip_addr = None
        if not target:
            log(f"Device not found, retrying in {backoff}s...")
            try:
                await asyncio.wait_for(stop_event.wait(), timeout=backoff)
            except asyncio.TimeoutError:
                pass
            backoff = min(backoff * 2, 60)
            continue

        addr = target if isinstance(target, str) else target.address
        ok = await connect_and_run(target, stop_event)
        if not ok:
            if sys.platform == "darwin":
                # No string cache to drop; instead skip this stale handle on
                # the next retrieveConnected so the scan fallback is reachable.
                skip_addr = addr
            else:
                log("Invalidating cached address")
                SAVED_ADDR_FILE.unlink(missing_ok=True)
            try:
                await asyncio.wait_for(stop_event.wait(), timeout=backoff)
            except asyncio.TimeoutError:
                pass
            backoff = min(backoff * 2, 60)
        else:
            backoff = 1

    if cc_server is not None:
        cc_server.close()
    if stats_task is not None:
        stats_task.cancel()
    if not os.environ.get("CLAWDMETER_NO_LISTENER"):
        try:
            COMPANION.save(STATE_FILE)
        except OSError as e:
            log(f"Companion: could not save {STATE_FILE}: {e}")


if __name__ == "__main__":
    if len(sys.argv) > 1 and sys.argv[1] == "link":
        # `companion/link` → the one-liners another machine runs to join this bridge.
        print(cc_mod.join_text(read_companion_port(), companion_token()))
        sys.exit(0)
    try:
        asyncio.run(main())
    except KeyboardInterrupt:
        sys.exit(0)
