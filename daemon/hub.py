#!/usr/bin/env python3
"""Clawdmeter WiFi hub — the always-on half of the device's second mode.

The BLE mode ties a Clawdmeter to one nearby Mac or PC: the numbers stop when
that machine sleeps, walks out of range or runs on battery. In WiFi mode the
device joins the home network instead and talks to this hub, which runs on
anything that is always on (a NAS, a Pi, one of the VMs). From there:

  * every machine with Claude Code posts its hook events here, exactly as it
    already does to the BLE bridge — `companion link` prints the same one-liner
    and the same served installer works unchanged;
  * this process polls the usage numbers and scans the transcripts for the
    Stats page, reusing the daemon's own code;
  * any number of Clawdmeters long-poll `/device/poll` and get the identical
    JSON payload the BLE transport delivers, so the firmware's parser, UI and
    alerts need no idea which radio brought it.

Discovery is mDNS: the hub advertises `_clawdmeter._tcp` and devices pick it up
with nothing configured. Devices are not asked for the companion token — they
are display-only consumers on the home LAN and hold no secrets; the hook port
keeps its token because posting events changes what everyone sees.

Run:  python3 hub.py            (add --port / --hook-port to move the ports)
"""

from __future__ import annotations

import argparse
import asyncio
import json
import os
import socket
import sys
import time
from pathlib import Path

try:
    import claude_usage_daemon as daemon
    import companion as cc_mod
    import stats as stats_mod
    import trend as trend_mod
except ImportError:  # pragma: no cover - package import path
    from . import claude_usage_daemon as daemon
    from . import companion as cc_mod
    from . import stats as stats_mod
    from . import trend as trend_mod

DEVICE_PORT = 47394           # Clawdmeters poll here; hooks keep 47393
MDNS_TYPE = "_clawdmeter._tcp.local."
LONG_POLL_S = 25.0            # hold a device's request this long before saying "nothing new"
RESYNC_S = 600.0              # push at least this often so device clocks cannot drift
# Fields that tick on their own and must not count as news: the device runs its
# own clock from `t` and its own elapsed counter from `cc.e`. Without this the
# payload differs on every rebuild, every long-poll returns instantly, and the
# whole point of holding the connection is lost.
VOLATILE = ("t", "tf")
DEVICE_STALE_S = 300.0        # a device unheard from this long has gone
PAYLOAD_MAX = 1400            # WiFi has no 500 B BLE cap; keep it inside one MTU


def log(msg: str) -> None:
    print(f"[{time.strftime('%H:%M:%S')}] {msg}", flush=True)


class Hub:
    """The current payload plus the devices watching it.

    One payload serves every device: they are all showing the same account. A
    monotonic sequence number is what makes long-polling cheap — a device sends
    the last sequence it drew and only gets an answer when the hub has moved on.
    """

    def __init__(self) -> None:
        self.payload: dict = {}
        self.seq: int = 0
        self._signature_last: str | None = None
        self._last_bump: float = 0.0
        self._changed = asyncio.Event()
        self.devices: dict[str, dict] = {}

    @staticmethod
    def _signature(payload: dict) -> str:
        """What counts as a change worth waking every device for."""
        body = {k: v for k, v in payload.items() if k not in VOLATILE}
        cc = body.get("cc")
        if isinstance(cc, dict):
            body["cc"] = {k: v for k, v in cc.items() if k != "e"}
        return json.dumps(body, sort_keys=True, separators=(",", ":"))

    def publish(self, payload: dict) -> None:
        """Store the payload; wake devices only when something real moved.

        The freshest clock always rides along with whatever does wake them, and
        RESYNC_S guarantees a push even on a completely idle account.
        """
        sig = self._signature(payload)
        now = time.time()
        self.payload = payload
        if sig == self._signature_last and now - self._last_bump < RESYNC_S:
            return
        self._signature_last = sig
        self._last_bump = now
        self.seq += 1
        self._changed.set()
        self._changed.clear()

    async def wait_for_change(self, timeout: float) -> None:
        try:
            await asyncio.wait_for(self._changed.wait(), timeout=timeout)
        except asyncio.TimeoutError:
            pass

    def note_device(self, did: str, addr: str) -> bool:
        """Record a device poll. True when this is one we had not seen."""
        now = time.time()
        for k, v in list(self.devices.items()):
            if now - v["last_seen"] >= DEVICE_STALE_S:
                del self.devices[k]
                log(f"Device {k} ({v['addr']}) went quiet")
        fresh = did not in self.devices
        self.devices[did] = {"last_seen": now, "addr": addr}
        return fresh

    def encode(self) -> bytes:
        """The payload as the device receives it, trimmed to one MTU.

        The Stats block is the big optional part, so it is what goes first —
        the usage numbers and the live companion state matter more per byte.
        """
        body = dict(self.payload)
        data = json.dumps(body, separators=(",", ":"), ensure_ascii=False).encode()
        for drop in ("st", "tr"):
            if len(data) <= PAYLOAD_MAX:
                break
            body.pop(drop, None)
            data = json.dumps(body, separators=(",", ":"), ensure_ascii=False).encode()
        return data


async def serve_devices(hub: Hub, host: str, port: int):
    """A tiny HTTP/1.1 server devices long-poll.

    GET /device/poll?seq=<n>&id=<name>  -> 200 with the payload when the hub has
                                           moved past <n>, else 204 after a hold.
    GET /device/state                   -> the payload, no waiting (for curl).
    """

    async def handle(reader: asyncio.StreamReader, writer: asyncio.StreamWriter) -> None:
        try:
            method, path, headers, _ = await cc_mod._read_request(reader)
            peer = writer.get_extra_info("peername")
            addr = str(peer[0]) if peer else "?"
            route, _, query = path.partition("?")
            args = dict(kv.split("=", 1) for kv in query.split("&") if "=" in kv)

            if method != "GET" or not route.startswith("/device"):
                writer.write(cc_mod._response(404, "not found"))
                return

            if route == "/device/state":
                writer.write(cc_mod._response(200, hub.encode().decode(), "application/json"))
                return

            if route != "/device/poll":
                writer.write(cc_mod._response(404, "not found"))
                return

            did = (args.get("id") or addr)[:32]
            if hub.note_device(did, addr):
                log(f"Device {did} ({addr}) joined — {len(hub.devices)} on the hub")
            try:
                since = int(args.get("seq", "0"))
            except ValueError:
                since = 0

            if since == hub.seq:                      # up to date: hold the line
                await hub.wait_for_change(LONG_POLL_S)
            if since == hub.seq:
                writer.write(cc_mod._response(204, ""))
                return
            body = hub.encode().decode()
            writer.write((f"HTTP/1.1 200 OK\r\nContent-Type: application/json\r\n"
                          f"X-Clawdmeter-Seq: {hub.seq}\r\n"
                          f"Content-Length: {len(body.encode())}\r\nConnection: close\r\n\r\n"
                          ).encode() + body.encode())
        except (asyncio.TimeoutError, asyncio.IncompleteReadError, ValueError, UnicodeDecodeError):
            try:
                writer.write(cc_mod._response(400, "bad request"))
            except Exception:
                pass
        finally:
            try:
                await writer.drain()
                writer.close()
            except Exception:
                pass

    server = await asyncio.start_server(handle, host, port, reuse_address=True)
    log(f"Devices: http://{host}:{port}/device/poll")
    return server


async def advertise_mdns(port: int):
    """Publish _clawdmeter._tcp so devices need no address configured.

    Must use the *async* zeroconf interface: the synchronous one hands a
    coroutine to the running loop and blocks waiting for it, which on an
    asyncio hub deadlocks until it times out and takes the process with it.

    Optional — without the zeroconf package the hub still works, devices just
    have to be told the address once.
    """
    try:
        from zeroconf import ServiceInfo
        from zeroconf.asyncio import AsyncZeroconf
    except ImportError:
        log("mDNS off (pip install zeroconf) — devices need the hub address set by hand")
        return None
    # Advertise LAN addresses only. A Tailscale address (100.64/10) resolves
    # from the same host name, and a device that picks it cannot route there —
    # which looks exactly like "the hub was found and then never answered".
    usable = [a for a in cc_mod.local_addresses() if a[0].isdigit() and not cc_mod.is_cgnat(a)]
    addrs = [socket.inet_aton(a) for a in usable]
    if not addrs:
        log("mDNS off: no usable IPv4 address")
        return None
    log(f"mDNS: advertising {', '.join(usable)}")
    host = cc_mod.hostname_short() or "clawdmeter-hub"
    info = ServiceInfo(MDNS_TYPE, f"{host}.{MDNS_TYPE}", addresses=addrs, port=port,
                       properties={"path": "/device/poll"}, server=f"{host}.local.")
    try:
        azc = AsyncZeroconf()
        # allow_name_change: a second hub on the same LAN (or a test beside the
        # real one) gets "... 2" rather than failing outright. Devices browse by
        # service type, so a renamed instance is still found.
        await azc.async_register_service(info, allow_name_change=True)
    except Exception as e:  # noqa: BLE001 - discovery is a convenience, never fatal
        log(f"mDNS off: {e}")
        return None
    log(f"mDNS: advertising {MDNS_TYPE} on port {port} as {host}.local")
    return (azc, info)


class AccountKeepers:
    """One token keeper per account directory.

    A headless hub runs no Claude Code session, so nothing renews the stored
    OAuth tokens. Instead of refreshing them itself (Anthropic's terms reserve
    that to its own apps), the hub asks the unmodified `claude` CLI to run one
    tiny call against each account shortly before its token expires. Calls run
    as background tasks so the devices keep their heartbeat meanwhile.
    """

    def __init__(self) -> None:
        self.keepers: dict[Path, daemon.tk_mod.TokenKeeper] = {}
        self.tasks: dict[Path, asyncio.Task] = {}

    def tick(self, now: float) -> None:
        if daemon.read_token_keeper_setting() != "on":
            return
        for d in daemon.read_config_dirs():
            if d in self.tasks and not self.tasks[d].done():
                continue
            exp = daemon.read_token_expiry(d)
            if not daemon.tk_mod.due(exp, now):
                continue
            keeper = self.keepers.setdefault(
                d, daemon.tk_mod.TokenKeeper(cwd=daemon.CONFIG_FILE.parent, log=log))
            if not keeper.can_run(now):
                continue
            reason = "token expires soon" if exp > now else "token expired"
            target = None if d == daemon.DEFAULT_CONFIG_DIR else d
            self.tasks[d] = asyncio.create_task(keeper.run(reason, now, config_dir=target))


async def main() -> None:
    ap = argparse.ArgumentParser(description="Clawdmeter WiFi hub")
    ap.add_argument("--port", type=int, default=DEVICE_PORT, help="device port")
    ap.add_argument("--hook-port", type=int, default=None, help="Claude Code hook port")
    ap.add_argument("--bind", default="0.0.0.0")
    args = ap.parse_args()

    hub = Hub()
    log("=== Clawdmeter WiFi hub ===")

    # A hook event is the latency-sensitive path: "Claude needs you" should
    # reach every desk as fast as the network allows, not on the next tick.
    hook_woke = asyncio.Event()

    # Hooks, stats and history are the daemon's own machinery, unchanged.
    daemon.HISTORY = trend_mod.History(daemon.HISTORY_FILE)
    hook_port = args.hook_port or daemon.read_companion_port()
    token = daemon.companion_token()
    daemon.COMPANION.log = log
    cc_server = await cc_mod.start_companion_server(
        daemon.COMPANION, hook_woke.set, daemon.read_companion_bind(), hook_port, log=log, token=token)
    if daemon.read_stats_setting() == "on":
        daemon.STATS = stats_mod.ClaudeStats(daemon.stats_project_dirs(), daemon.STATS_CACHE,
                                             exclude_substrings=(daemon.CONFIG_FILE.parent.name,), log=log)
    dev_server = await serve_devices(hub, args.bind, args.port)
    mdns = await advertise_mdns(args.port)
    for line in cc_mod.join_text(hook_port, token).splitlines():
        log("  " + line if line else "")

    keepers = AccountKeepers()
    last_poll = 0.0
    last_stats = 0.0
    try:
        while True:
            now = time.time()
            if now - last_poll >= daemon.POLL_INTERVAL:
                last_poll = now
                keepers.tick(now)
                # poll_slots returns one payload per labelled account (tagged
                # with an "id"), or a single one when only one is configured.
                slots, dead = await daemon.poll_slots()
                payload = None
                if not slots and dead:
                    payload = {"ok": False}
                elif slots:
                    # The first account stays at the root, so a firmware that
                    # ignores "acc" behaves exactly as before.
                    payload = dict(slots[0])
                    if len(slots) > 1:
                        payload["acc"] = slots
                if payload is not None:
                    daemon.record_history(payload)
                    daemon.LAST_USAGE = payload
            if daemon.STATS is not None and now - last_stats >= daemon.STATS_REFRESH_S:
                last_stats = now
                await asyncio.to_thread(daemon.STATS.refresh)

            # Rebuild every tick: the companion state moves between polls, and
            # a device must see "needs you" within a heartbeat, not a minute.
            daemon.COMPANION.expire()
            body = dict(getattr(daemon, "LAST_USAGE", {}) or {})
            daemon.add_companion_fields(body)
            if body:
                daemon.add_clock_fields(body)
            st = daemon.stats_payload()
            if st is not None:
                body["st"] = st
            hub.publish(body)
            try:                                    # wake at once on a hook event
                await asyncio.wait_for(hook_woke.wait(), timeout=0.5)
                hook_woke.clear()
            except asyncio.TimeoutError:
                pass
    finally:
        if dev_server is not None:
            dev_server.close()
        if cc_server is not None:
            cc_server.close()
        if mdns is not None:
            azc, info = mdns
            await azc.async_unregister_service(info)
            await azc.async_close()


if __name__ == "__main__":
    try:
        asyncio.run(main())
    except KeyboardInterrupt:
        pass
