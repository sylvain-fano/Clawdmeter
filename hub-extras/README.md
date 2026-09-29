# Running the hub headless, with several Claude accounts

The [WiFi hub](../daemon/hub.py) serves your Claude Code usage to any number of
displays. This folder is what you need to run it on a box nobody logs into (a
Proxmox container, a Pi, a NAS) and to feed one display **several accounts**, one
page each, switched with a swipe.

| File                      | Role                                                        |
| ------------------------- | ----------------------------------------------------------- |
| `systemd/clawdhub.service` | the hub (`daemon/hub.py`), as an unprivileged, sandboxed user |

## Install

```bash
# A system user whose home holds the Claude Code logins and the hub's config.
sudo useradd --system --create-home --home-dir /var/lib/clawdhub --shell /usr/sbin/nologin clawdhub

sudo git clone https://github.com/sylvain-fano/Clawdmeter /opt/clawdhub
sudo python3 -m venv /opt/clawdhub/.venv
sudo /opt/clawdhub/.venv/bin/pip install httpx zeroconf

# Claude Code itself, installed for that user: the hub asks it to renew tokens.
sudo -u clawdhub -H bash -c 'curl -fsSL https://claude.ai/install.sh | bash'
sudo -u clawdhub -H /var/lib/clawdhub/.local/bin/claude auth login

sudo cp /opt/clawdhub/hub-extras/systemd/clawdhub.service /etc/systemd/system/
sudo systemctl daemon-reload && sudo systemctl enable --now clawdhub
journalctl -u clawdhub -f
```

## Several accounts on one display

Log in once per account, each in its own config directory:

```bash
sudo -u clawdhub -H env CLAUDE_CONFIG_DIR=/var/lib/clawdhub/.claude-personal \
  /var/lib/clawdhub/.local/bin/claude auth login
```

Then label each directory in the hub's config
(`/var/lib/clawdhub/.config/claude-usage-monitor/config`) and restart the hub:

```
config_dirs = ~/.claude:work, ~/.claude-personal:personal
```

The hub polls every account each cycle and publishes one payload per account
under `acc`, each tagged with its label. The first account also stays at the
payload root, so a firmware that knows nothing about `acc` behaves as before.

Flash the WiFi build for your board (`waveshare_amoled_216_wifi` for the S3
2.16, `waveshare_amoled_216_c6_wifi` for the C6). On the display:

| Gesture                  | Effect                                |
| ------------------------ | ------------------------------------- |
| swipe right              | next account (loops)                  |
| swipe left               | Stats page                            |
| swipe up                 | Settings                              |
| serial command `account` | next account, for testing without a finger |

Labels are ASCII, at most 11 characters, and show in the title once there are
two or more accounts. `SLOT_MAX` in `firmware/src/slots.h` is 3.

## Keeping the logins alive

An access token lives about eight hours and nothing on a headless box uses
Claude Code, so nothing renews it. The hub handles this itself: when an
account's token is about to expire it runs the unmodified `claude` CLI once
against that account's directory (`claude -p … --model haiku`), and Claude Code
renews its own token. The hub never calls the OAuth endpoints and never handles
refresh tokens; that stays Claude Code's job, as
[Anthropic's terms](https://code.claude.com/docs/en/legal-and-compliance#authentication-and-credential-use)
require. Cost: one tiny Haiku call per account every eight hours or so.
Turn it off with `token_keeper = off`.

**Every machine needs its own login.** Refresh tokens rotate, so copying
`.credentials.json` from your PC creates two holders of one session, and
whichever renews first locks the other out.

If an account's token expires anyway, that account **disappears from the
display without a message** (single account left, so no `acc`, so no swipe).
`journalctl -u clawdhub | grep "Token keeper"` says why; a fresh
`claude auth login` recovers.

## Upgrading from the old token timer

Earlier versions shipped `refresh-token.py` and a `clawdhub-token.timer` that
refreshed tokens by calling the OAuth endpoint directly. They are gone; remove
them from your box:

```bash
sudo systemctl disable --now clawdhub-token.timer
sudo rm /etc/systemd/system/clawdhub-token.{service,timer}
```

If your hub ran as root with logins in `/root/.claude*`, run `claude auth login`
again as the `clawdhub` user rather than moving the files.

## The Stats page stays empty on a hub

Stats are computed by scanning `projects/*.jsonl`, the transcripts Claude Code
writes where it runs. A headless hub has none, so the page stays empty. Quotas
come from the API and follow you everywhere; stats come from local disk.
