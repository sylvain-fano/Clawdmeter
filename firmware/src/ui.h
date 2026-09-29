#pragma once
#include "data.h"
#include "ble.h"

// Screens. Splash ⇄ Usage toggle on tap (as always); Usage → Settings (tile
// pages) → About are reached by swiping left (swipe right goes back). See ui.cpp.
enum screen_t {
    SCREEN_SPLASH,
    SCREEN_USAGE,
    SCREEN_SETTINGS,
    SCREEN_ABOUT,
    SCREEN_COUNT,
};

void ui_init(void);
void ui_update(const UsageData* data);
void ui_tick_anim(void);
void ui_show_screen(screen_t screen);
void ui_toggle_splash(void);
screen_t ui_get_current_screen(void);
void ui_update_ble_status(ble_state_t state, const char* name, const char* mac);
void ui_update_battery(int percent, bool charging);

// Re-read settings + brightness into the Settings tiles and re-apply the
// header/status visibility. Called after any setting changes outside the page
// (e.g. the PWR button cycling brightness).
void ui_refresh_settings(void);

// Weekly card: advance to the next face (all models → scoped model …). Same as
// tapping the card; no-op when the plan has no scoped weekly limit.
void ui_flip_weekly_face(void);

// Show the Settings screen on a given tile page (0-based, clamped).
void ui_show_settings_page(int page);

// QA: run the committed vertical transition (dir +1 = up to Settings, -1 =
// down to Usage) as if a finger had flicked it; ms overrides the snap time
// (0 = default). Lets a serial session measure transition frame rates.
void ui_debug_swipe(int dir, uint32_t ms);
// Inject a screen tap (serial `tap`) — the same handler a finger reaches.
void ui_debug_tap(void);
// True while an alert (glow / chime / mascot) is waiting to be acknowledged.
bool ui_alert_active(void);

// WiFi setup portal (WiFi build): a full-screen card naming the hotspot to
// join. Call again to update `status`; pass nullptr for ssid to take it down.
void ui_show_setup(const char* ssid, const char* status);
// True while that card owns the screen — the creature animations blit straight
// to the panel, so they must be held off while it is up.
bool ui_setup_showing(void);

// Replace the three lines of the disconnected-screen hint (WiFi build: what
// the link is doing). Safe to call from another task - it only stages text,
// which the LVGL loop applies on its next tick.
void ui_set_link_hint(const char* l1, const char* l2, const char* l3);

// Companion ("cc") and Stats ("st") payload keys — applied independently of
// the usage numbers (either may arrive on its own beat).
void ui_companion_update(const CompanionData* cc);
void ui_stats_update(const StatsData* st);

// Usage-level pages: 0 = Stats, 1 = Usage. Snaps.
void ui_show_level_page(int page);
bool ui_cycle_account(int dir);

// Run the full "needs you" alert once — glow, mascot, chime per the settings —
// without a companion event (Settings → Alerts → Preview; serial "preview").
void ui_preview_alert(void);

// Calibration: outline the screen at several corner radii in distinct colours
// (serial "corners on|off") so the glass's real radius can be read off the
// device; "radius N" then moves the alert glow's corner radius live.
void ui_debug_corners(bool on);
void ui_debug_glow_radius(int radius);
