"""Keep Claude Code's stored OAuth token fresh without owning the refresh.

The daemon rides on the token Claude Code keeps in the Keychain (macOS) or its
credentials file (Windows/Linux) and, by design, never refreshes it itself —
Claude Code owns rotation. That token lives about eight hours. Whoever uses
the Claude desktop app instead of the CLI never triggers a CLI refresh, so the
stored token expires, the daemon gets 401s and the device blanks to "No data".

The keeper's answer is to let Claude Code do its own refresh: run one tiny
print-mode call (`claude -p … --model haiku`) when the stored token is about
to expire or already rejected. Claude Code refreshes and writes the new token
back where the daemon reads it. Costs one Haiku call every eight hours; the
spawned session is kept off the device (its hooks point at a dead address) and
never runs a tool.
"""

from __future__ import annotations

import asyncio
import os
import shutil
import time
from pathlib import Path

REFRESH_MARGIN_S = 10 * 60      # act this long before the stored expiry
MIN_GAP_S = 15 * 60             # never spawn the CLI more often than this
CLI_TIMEOUT_S = 120

CLI_CANDIDATES = [
    "~/.local/bin/claude", "/opt/homebrew/bin/claude", "/usr/local/bin/claude",
    "~/.claude/local/claude", "~/.npm-global/bin/claude", "/usr/bin/claude",
    "~/AppData/Roaming/npm/claude.cmd", "~/AppData/Local/Programs/claude/claude.exe",
]


def find_claude_cli() -> str | None:
    """Claude Code's executable — launchd/Task Scheduler PATHs are minimal, so look in the usual homes too."""
    found = shutil.which("claude")
    if found:
        return found
    for cand in CLI_CANDIDATES:
        p = Path(os.path.expanduser(cand))
        if p.is_file() and os.access(p, os.X_OK):
            return str(p)
    return None


def due(expires_at_s: float | None, now: float | None = None, margin_s: float = REFRESH_MARGIN_S) -> bool:
    """True when the stored token is gone or about to go."""
    if expires_at_s is None:
        return False
    now = time.time() if now is None else now
    return expires_at_s - now <= margin_s


class TokenKeeper:
    def __init__(self, cwd: Path | str | None = None, min_gap_s: float = MIN_GAP_S, log=print) -> None:
        self.cwd = str(cwd) if cwd else None
        self.min_gap_s = min_gap_s
        self.log = log
        self.last_run = 0.0
        self.cli: str | None = None

    def can_run(self, now: float | None = None) -> bool:
        now = time.time() if now is None else now
        return now - self.last_run >= self.min_gap_s

    async def run(self, reason: str, now: float | None = None, config_dir: Path | None = None) -> bool:
        """Spawn one print-mode Claude Code call so it refreshes its own token.

        `config_dir` points the call at another account's directory
        (CLAUDE_CONFIG_DIR). Returns True when the call exited cleanly (the
        token was very likely renewed); False when the CLI is missing, on
        cooldown, or failed.
        """
        now = time.time() if now is None else now
        if os.environ.get("CLAWDMETER_NO_TOKEN_KEEPER"):     # tests / operators who never want a spawn
            return False
        if not self.can_run(now):
            return False
        self.last_run = now
        self.cli = self.cli or find_claude_cli()
        if not self.cli:
            self.log("Token keeper: `claude` CLI not found — sign in with the CLI once to renew the stored token")
            return False
        env = dict(os.environ)
        env["CLAWDMETER_URL"] = "http://127.0.0.1:9"     # the hooks of this throwaway session go nowhere
        env.pop("CLAUDECODE", None)
        env.pop("CLAUDE_CODE_ENTRYPOINT", None)
        if config_dir is not None:
            env["CLAUDE_CONFIG_DIR"] = str(config_dir)
        where = f" in {config_dir}" if config_dir is not None else ""
        self.log(f"Token keeper: {reason}; asking Claude Code to renew its token{where} ({self.cli})")
        try:
            if self.cwd:
                Path(self.cwd).mkdir(parents=True, exist_ok=True)
            proc = await asyncio.create_subprocess_exec(
                self.cli, "-p", "Reply with exactly the word ok.", "--model", "haiku",
                stdin=asyncio.subprocess.DEVNULL, stdout=asyncio.subprocess.DEVNULL,
                stderr=asyncio.subprocess.PIPE, cwd=self.cwd, env=env)
            try:
                _, err = await asyncio.wait_for(proc.communicate(), timeout=CLI_TIMEOUT_S)
            except asyncio.TimeoutError:
                proc.kill()
                self.log("Token keeper: Claude Code did not finish in time")
                return False
        except OSError as e:
            self.log(f"Token keeper: could not run Claude Code: {e}")
            return False
        if proc.returncode != 0:
            msg = (err or b"").decode("utf-8", "replace").strip().splitlines()
            self.log(f"Token keeper: Claude Code exited {proc.returncode}: {msg[-1] if msg else ''}")
            return False
        self.log("Token keeper: Claude Code renewed its token")
        return True
