#!/usr/bin/env python3
"""Tests for labelled accounts: one display page per config dir.

Covers the `dir:label` syntax, poll_slots, and the hub's token refresher, which
must strip the label before it looks for a credentials file.

Run: python -m pytest daemon/tests/test_multi_account.py -x -q
"""
import asyncio
import importlib.util
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
# hub-extras/refresh-token.py
# ---------------------------------------------------------------------------

def _load_refresher():
    path = Path(__file__).resolve().parents[2] / "hub-extras" / "refresh-token.py"
    spec = importlib.util.spec_from_file_location("refresh_token", path)
    m = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(m)
    return m


def test_refresher_strips_the_label_before_looking_for_credentials(tmp_path, monkeypatch):
    rt = _load_refresher()
    cfg = tmp_path / "config"
    cfg.write_text("config_dirs = ~/.claude:work, ~/.claude-personal:personal\n")
    monkeypatch.setattr(rt, "DAEMON_CONFIG", cfg)
    monkeypatch.delenv("CLAUDE_CREDENTIALS_PATH", raising=False)
    assert rt.config_dirs() == [Path.home() / ".claude", Path.home() / ".claude-personal"]


def test_refresher_leaves_a_still_valid_token_alone(tmp_path):
    import json
    import time

    rt = _load_refresher()
    cred = tmp_path / ".credentials.json"
    blob = {"claudeAiOauth": {"accessToken": "a", "refreshToken": "r",
                              "expiresAt": int((time.time() + 6 * 3600) * 1000)}}
    cred.write_text(json.dumps(blob))
    with patch.object(rt.httpx, "post") as post:
        assert rt.refresh_one(cred) == 0
    post.assert_not_called()
    assert json.loads(cred.read_text()) == blob
