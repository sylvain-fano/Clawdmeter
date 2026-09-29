# Running the hub headless, with several Claude accounts

The [WiFi hub](../README.md#two-modes-bluetooth-or-wifi-with-a-hub) serves your
Claude Code usage to any number of displays. This folder is what you need to run
it on a box nobody logs into (a Proxmox container, a Pi, a NAS) and to feed one
display **several accounts**, one page each, switched with a swipe.

| File | Role |
| --- | --- |
| `refresh-token.py` | keeps every account's OAuth token alive, since no Claude Code runs on the box to do it |
| `systemd/clawdhub.service` | the hub itself (`daemon/hub.py`) |
| `systemd/clawdhub-token.{service,timer}` | runs `refresh-token.py` every 30 minutes |

The examples assume the repo is in `/opt/clawdhub` and a virtualenv with
`httpx` and `zeroconf` in `/opt/clawdhub/.venv`. Adapt the paths.

## Several accounts on one display

Label each Claude config directory in the hub's config
(`~/.config/claude-usage-monitor/config`):

```
config_dirs = ~/.claude:work, ~/.claude-personal:personal
```

The hub polls every account each cycle and publishes one payload per account
under `acc`, each tagged with its label. The first account also stays at the
payload root, so a firmware that knows nothing about `acc` behaves exactly as it
did before.

Flash the WiFi build for your board (`waveshare_amoled_216_wifi` for the S3
2.16, `waveshare_amoled_216_c6_wifi` for the C6). On the display:

| Gesture | Effect |
| --- | --- |
| swipe right | next account (loops) |
| swipe left | Stats page |
| swipe up | Settings |
| serial command `account` | next account, for testing without a finger |

The account label shows in the title, next to the clock, once there are two or
more accounts. Labels are ASCII, at most 11 characters.

`SLOT_MAX` in `firmware/src/slots.h` is 3. Past that, the page that was updated
least recently is recycled, never the one you are looking at. Raise the constant
and reflash for more.

### Adding an account

No code change:

```bash
CLAUDE_CONFIG_DIR=/root/.claude-other claude auth login --claudeai
# then in the hub's config:
#   config_dirs = ~/.claude:work, ~/.claude-personal:personal, ~/.claude-other:other
systemctl restart clawdhub
```

## The OAuth token trap

**Refresh tokens rotate: each renewal revokes the previous token.** Copying
`.credentials.json` from your PC to the hub therefore creates two holders of the
same token, and whichever renews first locks the other out.

**Every machine needs its own login.** Run `claude auth login` on the hub, once
per account, instead of copying a credentials file. `refresh-token.py` then keeps
those tokens fresh (it renews when less than 45 minutes remain, and reads
`config_dirs` from the hub's config, so a new account needs no change to it).

If the refresher stops working the display goes to "No data" a few hours later
and the journal (`journalctl -u clawdhub-token`) says why. A fresh
`claude auth login` recovers.

The OAuth `client_id` and endpoint in `refresh-token.py` are Claude Code's own and
are not publicly documented. They may change without notice.

## The Stats page stays empty on a hub

Stats are computed by scanning `projects/*.jsonl`, the transcripts Claude Code
writes where it runs. A headless hub only holds a login, so it has none, and the
page stays empty even with `stats = on`. Quotas come from the API and follow you
everywhere; stats come from local disk and stay local. Getting them on a hub
would mean sharing your machines' `~/.claude/projects` with it.
