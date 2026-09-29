#!/usr/bin/env python3
"""Keep the Claude Code OAuth tokens used by a headless Clawdmeter hub alive.

The hub reads ~/.claude/.credentials.json but never renews the token. On a
workstation Claude Code does that itself; on a headless box nobody does, and
the hub starts serving {"ok": false} as soon as the access token expires
(about 8 hours). This script fills the gap without touching the hub: run it
from a systemd timer and the hub keeps reading the same file, none the wiser.

The write is atomic (temporary file + rename), so a power cut mid-write cannot
leave a truncated credentials.json, which would blind the hub until someone
intervenes.

WARNING: the client_id and the endpoint below are Claude Code's own and are not
publicly documented. They can change without notice; when they do, the refresh
fails cleanly (see the journal) and a fresh `claude auth login` is needed.

Refresh tokens ROTATE: every renewal revokes the previous one. Never copy the
same credentials file to two machines, each machine needs its own login.
"""

import json
import os
import sys
import time
from pathlib import Path

import httpx

# The directories to maintain are the ones the hub polls. Reading the hub's own
# config avoids keeping two lists in sync: a forgotten account would otherwise
# expire in silence.
DAEMON_CONFIG = Path.home() / ".config" / "claude-usage-monitor" / "config"
DEFAULT_DIR = Path.home() / ".claude"
TOKEN_URL = "https://console.anthropic.com/v1/oauth/token"
CLIENT_ID = "9d1c250a-e61b-44d9-88ed-5944d1962f5e"
# Renew ahead of expiry: the timer fires every 30 min, so the margin must exceed
# that interval or a dead token could be served between two runs.
MARGIN_S = 45 * 60


def log(msg: str) -> None:
    print(f"[{time.strftime('%H:%M:%S')}] {msg}", flush=True)


def config_dirs() -> list[Path]:
    """Account directories to maintain, read from the hub's config."""
    if override := os.environ.get("CLAUDE_CREDENTIALS_PATH"):
        return [Path(override).parent]
    try:
        for line in DAEMON_CONFIG.read_text().splitlines():
            line = line.split("#", 1)[0].strip()
            if "=" in line:
                key, val = line.split("=", 1)
                if key.strip().lower() == "config_dirs":
                    dirs = []
                    for part in (s.strip() for s in val.split(",")):
                        if not part:
                            continue
                        # The hub accepts a ":label" suffix (the page name on the
                        # display). Left in, we would look for "~/.claude:pro",
                        # which does not exist, and no token would be maintained.
                        if ":" in part[2:]:
                            head, _, _tail = part.rpartition(":")
                            if head.strip():
                                part = head.strip()
                        dirs.append(Path(part).expanduser())
                    if dirs:
                        return dirs
    except OSError:
        pass
    return [DEFAULT_DIR]


def refresh_one(cred: Path) -> int:
    """Renew one directory's token. 0 = renewed or nothing to do, 1 = failure."""
    if not cred.exists():
        log(f"ERROR: {cred} is missing")
        return 1

    blob = json.loads(cred.read_text())
    oauth = blob.get("claudeAiOauth")
    if not oauth:
        log("ERROR: no claudeAiOauth block")
        return 1

    expires_at = oauth.get("expiresAt", 0) / 1000
    remaining = expires_at - time.time()
    if remaining > MARGIN_S:
        log(f"token still valid for {remaining/60:.0f} min, nothing to do")
        return 0

    refresh_token = oauth.get("refreshToken")
    if not refresh_token:
        log("ERROR: no refreshToken, cannot renew (run `claude auth login`)")
        return 1

    log(f"token expires in {remaining/60:.0f} min, renewing")
    try:
        r = httpx.post(
            TOKEN_URL,
            json={"grant_type": "refresh_token", "refresh_token": refresh_token, "client_id": CLIENT_ID},
            headers={"Content-Type": "application/json"},
            timeout=30.0,
        )
    except Exception as e:
        log(f"FAILED (network): {type(e).__name__}: {e}")
        return 1

    if r.status_code != 200:
        log(f"FAILED (HTTP {r.status_code}): {r.text[:200]}")
        return 1

    data = r.json()
    access = data.get("access_token")
    if not access:
        log("FAILED: no access_token in the response")
        return 1

    oauth["accessToken"] = access
    # The refresh token rotates: when a new one comes back it replaces the old
    # one, otherwise the old one stays valid.
    if data.get("refresh_token"):
        oauth["refreshToken"] = data["refresh_token"]
    expires_in = int(data.get("expires_in", 0))
    if expires_in:
        oauth["expiresAt"] = int((time.time() + expires_in) * 1000)
    blob["claudeAiOauth"] = oauth

    tmp = cred.with_suffix(".tmp")
    tmp.write_text(json.dumps(blob))
    os.chmod(tmp, 0o600)
    os.replace(tmp, cred)  # atomic
    new_exp = time.strftime("%Y-%m-%d %H:%M", time.localtime(oauth["expiresAt"] / 1000))
    log(f"OK, new token valid until {new_exp}")
    return 0


def main() -> int:
    rc = 0
    for d in config_dirs():
        log(f"--- {d} ---")
        if refresh_one(d / ".credentials.json") != 0:
            rc = 1
    return rc


if __name__ == "__main__":
    sys.exit(main())
