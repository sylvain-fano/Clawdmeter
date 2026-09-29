#include "slots.h"
#include <string.h>

static Slot s_slots[SLOT_MAX];
static int  s_count  = 0;
static int  s_active = 0;

void slots_init(void) {
    memset(s_slots, 0, sizeof(s_slots));
    s_count  = 0;
    s_active = 0;
}

int slots_count(void) { return s_count; }

Slot* slots_at(int i) {
    if (i < 0 || i >= s_count) return nullptr;
    return &s_slots[i];
}

int slots_active_index(void) { return s_active; }

Slot* slots_active(void) {
    if (s_count == 0) return nullptr;
    return &s_slots[s_active];
}

void slots_set_active(int i) {
    if (i >= 0 && i < s_count) s_active = i;
}

bool slots_cycle(int dir) {
    if (s_count < 2) return false;
    int n = s_active + (dir >= 0 ? 1 : -1);
    if (n < 0) n = s_count - 1;
    if (n >= s_count) n = 0;
    s_active = n;
    return true;
}

static void normalize(const char* id, char* key) {
    if (!id || !*id) strncpy(key, SLOT_ID_DEFAULT, SLOT_ID_LEN - 1);
    else             strncpy(key, id,              SLOT_ID_LEN - 1);
    key[SLOT_ID_LEN - 1] = '\0';
}

int slots_find(const char* id) {
    char key[SLOT_ID_LEN];
    normalize(id, key);
    for (int i = 0; i < s_count; i++) {
        if (strcmp(s_slots[i].id, key) == 0) return i;
    }
    return -1;
}

int slots_upsert(const char* id, bool* out_is_new) {
    char key[SLOT_ID_LEN];
    normalize(id, key);

    int found = slots_find(key);
    if (found >= 0) {
        if (out_is_new) *out_is_new = false;
        return found;
    }

    int idx;
    if (s_count < SLOT_MAX) {
        idx = s_count++;
    } else {
        // Table full — recycle the stalest page, never the one being looked at:
        // wiping that would swap the numbers under the user's eyes.
        idx = -1;
        for (int i = 0; i < s_count; i++) {
            if (i == s_active) continue;
            if (idx < 0 || s_slots[i].last_ms < s_slots[idx].last_ms) idx = i;
        }
        if (idx < 0) idx = s_active;   // SLOT_MAX == 1; nothing else to take
    }
    memset(&s_slots[idx], 0, sizeof(s_slots[idx]));
    strncpy(s_slots[idx].id, key, SLOT_ID_LEN - 1);
    s_slots[idx].used = true;
    if (out_is_new) *out_is_new = true;
    return idx;
}
