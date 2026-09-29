#pragma once
#include "data.h"
#include <stdint.h>

// ---------------------------------------------------------------------------
// Account slots.
//
// Upstream Clawdmeter shows exactly one account: one machine owns the board and
// its daemon writes one payload. This fork keeps a small table of accounts
// instead, addressed by the payload's "id" field (a short label the daemon
// sends, e.g. "work" / "personal"). Swiping on the usage view moves between
// them. A payload with no "id" lands in the legacy slot "main", so an
// unmodified daemon behaves exactly as before.
//
// Slots are keyed by label, not by BLE peer, on purpose: two machines each
// pushing one account and one machine pushing two config dirs both work, and
// a machine that reconnects with a new connection handle keeps its slot.
// ---------------------------------------------------------------------------

#define SLOT_MAX     3
#define SLOT_ID_LEN  12
#define SLOT_ID_DEFAULT "main"

// Length of a BLE identity address string, "aa:bb:cc:dd:ee:ff" plus NUL.
#define SLOT_ADDR_LEN 18

struct Slot {
    UsageData data;
    char      id[SLOT_ID_LEN];
    // Identity address of the machine that last wrote here. The HID buttons
    // are aimed with this rather than a connection handle: a host may keep its
    // HID link and its daemon's data link on two different connections, and
    // NimBLE reuses handles across reconnects.
    char      owner[SLOT_ADDR_LEN];
    uint32_t  last_ms;   // lv_tick of the last payload that carried real data
    bool      ok;        // last payload's `ok` flag ({"ok":false} = no data)
    bool      received;  // any valid payload since boot
    bool      used;
};

void  slots_init(void);

// Index of the slot for `id`, creating it when new. When the table is full the
// stalest slot that isn't on screen is recycled. `id` may be null/empty →
// SLOT_ID_DEFAULT. `out_is_new` (optional) reports whether the slot had to be
// created or recycled, i.e. whether the page indicator needs re-rendering.
int   slots_upsert(const char* id, bool* out_is_new);

// Index of an existing slot by label, or -1. Never creates one.
int   slots_find(const char* id);

int   slots_count(void);
Slot* slots_at(int i);

int   slots_active_index(void);
Slot* slots_active(void);
void  slots_set_active(int i);

// Move the active slot by `dir` (+1 / -1) with wraparound.
// Returns false (and changes nothing) when fewer than two slots exist.
bool  slots_cycle(int dir);
