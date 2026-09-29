#!/usr/bin/env python3
"""Tests for labelled accounts: one display page per config dir.

Covers the `dir:label` syntax, poll_slots, and the hub's per-account token
keepers, which must ask Claude Code to renew each account in its own dir.

Run: python -m pytest daemon/tests/test_multi_account.py -x -q
"""
import asyncio
import time
from pathlib import Path
from unittest.mock import AsyncMock, patch

import daemon.claude_usage_daemon as mod


def _run(coro):
    return asyncio.run(coro)


def _config(tmp_path, monkeypatch, text):
    cfg = tmp_path / "config"
    cfg.write_text(text)
    monkeypatch.setattr(mod, "CONFIG_FILE", cfg)


# ---------------------------------------------------------------------------
# read_config_dir_specs
# ---------------------------------------------------------------------------

def test_specs_default_has_no_label(tmp_path, monkeypatch):
    monkeypatch.setattr(mod, "CONFIG_FILE", tmp_path / "absent")
    assert mod.read_config_dir_specs() == [(mod.DEFAULT_CONFIG_DIR, None)]


def test_specs_parse_labels(tmp_path, monkeypatch):
    _config(tmp_path, monkeypatch, "config_dirs = ~/.claude:work, ~/.claude-personal:personal\n")
    assert mod.read_config_dir_specs() == [
        (Path.home() / ".claude", "work"),
        (Path.home() / ".claude-personal", "personal"),
    ]


def test_specs_mix_labelled_and_plain(tmp_path, monkeypatch):
    _config(tmp_path, monkeypatch, "config_dirs = ~/.claude:work, ~/.claude-x\n")
    assert [lab for _d, lab in mod.read_config_dir_specs()] == ["work", None]


def test_specs_label_is_clipped_and_made_ascii(tmp_path, monkeypatch):
    _config(tmp_path, monkeypatch, "config_dirs = ~/.claude:un très long libellé\n")
    (_d, label), = mod.read_config_dir_specs()
    assert label.isascii() and len(label) <= mod.SLOT_ID_MAX


def test_specs_windows_drive_letter_is_not_a_label(tmp_path, monkeypatch):
    _config(tmp_path, monkeypatch, r"config_dirs = C:\Users\me\.claude" + "\n")
    (_d, label), = mod.read_config_dir_specs()
    assert label is None


def test_config_dirs_ignores_labels(tmp_path, monkeypatch):
    _config(tmp_path, monkeypatch, "config_dirs = ~/.claude:work, ~/.claude-personal:personal\n")
    assert mod.read_config_dirs() == [Path.home() / ".claude", Path.home() / ".claude-personal"]


# ---------------------------------------------------------------------------
# poll_slots
# ---------------------------------------------------------------------------

def _poll(monkeypatch, usage_by_token):
    """Run poll_slots with `read_token_for` returning the dir's name as token."""
    monkeypatch.setattr(mod, "read_token_for", lambda d: d.name if d.name in usage_by_token else None)
    fake = AsyncMock(side_effect=lambda tok: dict(usage_by_token[tok]))
    with patch.object(mod, "poll_usage_endpoint", fake):
        return _run(mod.poll_slots(mod.PlanSelector()))


def test_poll_slots_one_payload_per_labelled_account(tmp_path, monkeypatch):
    _config(tmp_path, monkeypatch, "config_dirs = /x/a:work, /x/b:personal\n")
    slots, dead = _poll(monkeypatch, {"a": {"s": 10}, "b": {"s": 70}})
    assert not dead
    assert [(p["id"], p["s"]) for p in slots] == [("work", 10), ("personal", 70)]


def test_poll_slots_skips_an_account_without_token(tmp_path, monkeypatch):
    _config(tmp_path, monkeypatch, "config_dirs = /x/a:work, /x/b:personal\n")
    slots, dead = _poll(monkeypatch, {"b": {"s": 70}})
    assert not dead
    assert [p["id"] for p in slots] == ["personal"]


def test_poll_slots_all_dead_when_no_account_has_a_token(tmp_path, monkeypatch):
    _config(tmp_path, monkeypatch, "config_dirs = /x/a:work, /x/b:personal\n")
    slots, dead = _poll(monkeypatch, {})
    assert slots == [] and dead


def test_poll_slots_single_unlabelled_account_stays_untagged(tmp_path, monkeypatch):
    _config(tmp_path, monkeypatch, "config_dirs = /x/a\n")
    slots, _dead = _poll(monkeypatch, {"a": {"s": 33}})
    assert slots == [{"s": 33}]


def test_slot_labels(tmp_path, monkeypatch):
    _config(tmp_path, monkeypatch, "config_dirs = /x/a:work, /x/b:personal\n")
    assert mod.slot_labels() == ["work", "personal"]


# ---------------------------------------------------------------------------
# hub.AccountKeepers
# ---------------------------------------------------------------------------

def _keeper_runs(tmp_path, monkeypatch, expiry_by_dir):
    """Tick AccountKeepers once; return the config_dir each spawned run got."""
    import daemon.hub as hub_mod
    d = hub_mod.daemon
    monkeypatch.setattr(d, "read_token_keeper_setting", lambda: "on")
    monkeypatch.setattr(d, "read_config_dirs", lambda: list(expiry_by_dir))
    monkeypatch.setattr(d, "read_token_expiry", lambda cd: expiry_by_dir[cd])
    monkeypatch.setattr(d, "CONFIG_FILE", tmp_path / "config")
    seen = []

    async def fake_run(self, reason, now=None, config_dir=None):
        seen.append(config_dir)
        return True

    monkeypatch.setattr(d.tk_mod.TokenKeeper, "run", fake_run)

    async def go():
        k = hub_mod.AccountKeepers()
        k.tick(time.time())
        await asyncio.gather(*k.tasks.values())

    _run(go())
    return seen


def test_keepers_renew_only_the_accounts_about_to_expire(tmp_path, monkeypatch):
    now = time.time()
    soon, later = Path("/x/soon"), Path("/x/later")
    seen = _keeper_runs(tmp_path, monkeypatch, {soon: now + 60, later: now + 6 * 3600})
    assert seen == [soon]


def test_keepers_leave_the_default_dir_to_claude_code_defaults(tmp_path, monkeypatch):
    import daemon.hub as hub_mod
    default = hub_mod.daemon.DEFAULT_CONFIG_DIR
    seen = _keeper_runs(tmp_path, monkeypatch, {default: time.time() - 1})
    assert seen == [None]


def test_keepers_skip_unknown_expiry(tmp_path, monkeypatch):
    assert _keeper_runs(tmp_path, monkeypatch, {Path("/x/a"): None}) == []
