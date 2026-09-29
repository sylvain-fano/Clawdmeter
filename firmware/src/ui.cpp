#include "ui.h"
#include "slots.h"
#include "splash.h"
#ifdef CLAWD_LINK_WIFI
#include "link_wifi.h"
#endif
#include "settings.h"
#include "brightness.h"
#include "version.h"
#include <lvgl.h>
#include <time.h>
#include <stdint.h>
#include <string.h>
#include <esp_heap_caps.h>
#include "logo.h"
#include "clawd_still.h"
#include "icons.h"
#include "hal/board_caps.h"
#include "hal/sound_hal.h"
#include "idle.h"

// Custom fonts (scaled for 314 PPI, ~1.9x from original 165 PPI)
LV_FONT_DECLARE(font_tiempos_56);
LV_FONT_DECLARE(font_tiempos_34);
LV_FONT_DECLARE(font_styrene_48);
LV_FONT_DECLARE(font_styrene_28);
LV_FONT_DECLARE(font_styrene_24);
LV_FONT_DECLARE(font_styrene_20);
LV_FONT_DECLARE(font_styrene_16);
LV_FONT_DECLARE(font_styrene_14);
LV_FONT_DECLARE(font_styrene_12);
LV_FONT_DECLARE(font_mono_32);
LV_FONT_DECLARE(font_mono_18);

// Layout values computed from the active board's geometry. Populated once
// in ui_init() and treated as const for the rest of the program. Adding a
// new display size means extending compute_layout() with another
// breakpoint — never editing the screen-builder functions below.
struct Layout {
    int16_t scr_w, scr_h;
    int16_t margin;
    int16_t title_y;
    int16_t content_y;               // top of the body viewport (below the header)
    int16_t body_h;                  // scr_h - content_y: the part of the screen that slides
    int16_t content_w;

    // Usage screen
    int16_t usage_panel_h;
    int16_t usage_panel_gap;
    int16_t usage_bar_y;
    int16_t usage_reset_y;
    int16_t bar_h;
    int16_t panel_pad_x, panel_pad_y;
    int16_t pill_pad_x, pill_pad_y;
    const lv_font_t* title_font;     // screen title / clock
    const lv_font_t* pct_font;       // big percentage number
    const lv_font_t* ent_pct_font;   // enterprise spending number
    const lv_font_t* pill_font;      // "Current" / "Weekly" pill
    const lv_font_t* reset_font;     // "Resets in ..." line
    const lv_font_t* pace_font;      // enterprise "Under/On/Over pace" line
    const lv_font_t* anim_font;      // animated status line
    const lv_font_t* host_font;      // caption above the status line: a remote session's machine
    int16_t anim_y;                  // status line offset from bottom
    bool    small_icons;             // 40px logo + 24px battery (vs 80/48) on small screens
    int16_t title_nudge;             // title x-shift balancing the corner logo
    int16_t logo_y;                  // logo top edge
    int16_t batt_y;                  // battery widget top edge
    int16_t batt_w;                  // battery widget total width (body + nub), for position math
    // Battery widget: an outline drawn by LVGL with a proportional fill and the
    // percentage inside — wide enough for "100%" at batt_font.
    int16_t batt_h, batt_nub_w, batt_nub_h, batt_border, batt_radius;
    const lv_font_t* batt_font;

    // Weekly card faces: indicator dots on the reset row (right-aligned)
    int16_t face_dot_y;
    int16_t dot_size, dot_gap;

    // Settings pages. Tiles are the surfaces the controls sit on; every
    // control is sized for a fingertip on a 39 mm panel.
    int16_t tiles_bottom;            // bottom edge of the tile area
    int16_t tile_gap, tile_radius;
    int16_t tile_pad_x, tile_pad_y;
    int16_t wide_tile_h;             // clock tile (segmented picker)
    int16_t slider_tile_h;           // brightness / sleep tiles
    const lv_font_t* tile_label_font;
    const lv_font_t* ctrl_font;      // control text: segments, toggle values, slider values
    int16_t seg_h, seg_gap, seg_radius;
    int16_t toggle_w, toggle_h, toggle_knob;
    int16_t slider_h, slider_knob;
    int16_t dots_y;                  // page indicator dots (top edge)

    // About page (key/value rows + footer)
    int16_t about_row_h;
    const lv_font_t* about_key_font;
    const lv_font_t* about_val_font;
    const lv_font_t* about_hint_font;
    int16_t about_hint_y;            // footer offset from bottom

    // Pairing hint / idle screen
    int16_t pair_y1, pair_y2, pair_y3;
    int16_t idle_px;                 // sleeping-creature size on the idle screen

    int16_t glow_w;                  // alert glow ring width (all rings)
    int16_t level_dots_y;            // body-relative: page dots of the usage level
    int16_t mascot_cell;             // px per art cell for the header mascot actor (PSRAM-less boards)

    // Stats page: eight tiles in a 4×2 grid, then the activity heatmap
    int16_t st_tile_h, st_gap;
    const lv_font_t* st_label_font;
    const lv_font_t* st_value_font;
    int16_t st_cell, st_cell_gap;    // heatmap cell size and spacing
    int16_t st_weeks;                // heatmap columns
    int16_t st_hm_y;                 // body-relative top of the heatmap

    // Bluetooth screen
    int16_t bt_info_panel_h;
    int16_t bt_reset_zone_h;
    const lv_font_t* bt_title_font;
    const lv_font_t* bt_status_font;
    const lv_font_t* bt_device_font;
    const lv_font_t* bt_credit_1_font;
    const lv_font_t* bt_credit_2_font;
};
static Layout L = {};

// Pick layout values from the active board's pixel dimensions. The two
// existing boards happen to land on the two breakpoints below; new ports
// inherit the closer one — visually OK, may need a polish pass for
// pixel-perfect alignment but never blocks the port from booting.
static void compute_layout(const BoardCaps& c) {
    L.scr_w = c.width;
    L.scr_h = c.height;
    L.margin = 20;
    L.title_y = 30;

    // Values shared by the two original breakpoints; the small branch below
    // overrides them wholesale.
    L.bar_h = 24;
    L.panel_pad_x = 16;
    L.panel_pad_y = 12;
    L.pill_pad_x = 18;
    L.pill_pad_y = 6;
    L.title_font   = &font_tiempos_56;
    L.pct_font     = &font_styrene_48;
    L.ent_pct_font = &font_tiempos_56;
    L.pill_font    = &font_styrene_28;
    L.reset_font   = &font_styrene_28;
    L.pace_font    = &font_styrene_16;
    L.anim_font    = &font_mono_32;
    L.host_font    = &font_styrene_16;
    L.anim_y = -15;
    L.small_icons = false;
    L.title_nudge = 16;
    L.logo_y = L.title_y - 10;
    L.batt_h = 28;                    // large + compact; centred where the 48 px glyph sat
    L.batt_nub_w = 4;
    L.batt_nub_h = 12;
    L.batt_border = 2;
    L.batt_radius = 7;
    L.batt_font = &font_styrene_20;
    L.batt_w = 66 + 2 + L.batt_nub_w;
    L.batt_y = L.title_y + (48 - L.batt_h) / 2;
    L.pair_y1 = 40;
    L.pair_y2 = 120;
    L.pair_y3 = 160;
    L.idle_px = 160;

    if (c.height >= 460) {
        // Large layout — tuned for 480x480 (AMOLED-2.16).
        L.content_y = 100;
        L.usage_panel_h = 150;
        L.usage_panel_gap = 16;
        L.usage_bar_y = 56;
        L.usage_reset_y = 94;
        L.face_dot_y = L.usage_reset_y + 13;   // centred on the reset line
        L.dot_size = 8;
        L.dot_gap = 10;
        L.tiles_bottom = 432;
        L.tile_gap = 12;
        L.tile_radius = 10;
        L.tile_pad_x = 16;
        L.tile_pad_y = 14;
        L.wide_tile_h = 160;
        L.slider_tile_h = 100;
        L.tile_label_font = &font_styrene_20;
        L.ctrl_font = &font_styrene_24;
        L.seg_h = 60;  L.seg_gap = 6;  L.seg_radius = 12;
        L.toggle_w = 84; L.toggle_h = 40; L.toggle_knob = 32;
        L.slider_h = 12; L.slider_knob = 28;
        L.dots_y = 446;
        L.about_row_h = 38;
        L.about_key_font  = &font_styrene_20;
        L.about_val_font  = &font_styrene_20;
        L.about_hint_font = &font_styrene_16;
        L.about_hint_y = -46;   // clear of the page dots
        L.bt_info_panel_h = 160;
        L.bt_reset_zone_h = 110;
        L.bt_title_font    = &font_tiempos_56;
        L.bt_status_font   = &font_styrene_48;
        L.bt_device_font   = &font_styrene_28;
        L.bt_credit_1_font = &font_styrene_24;
        L.bt_credit_2_font = &font_styrene_20;
        L.glow_w = 15;                   // five 3 px rings
        L.level_dots_y = 2 * L.usage_panel_h + L.usage_panel_gap + 6;
        L.mascot_cell = 3;
        L.st_tile_h = 86;  L.st_gap = 8;
        L.st_label_font = &font_styrene_14;
        L.st_value_font = &font_styrene_24;
        L.st_cell = 14;  L.st_cell_gap = 4;  L.st_weeks = 24;
        L.st_hm_y = 2 * 86 + 8 + 12;
    } else if (c.height >= 300) {
        // Compact layout — tuned for 368x448 (AMOLED-1.8).
        L.content_y = 85;
        L.usage_panel_h = 130;
        L.usage_panel_gap = 12;
        L.usage_bar_y = 48;
        L.usage_reset_y = 78;
        L.face_dot_y = L.usage_reset_y + 13;
        L.dot_size = 7;
        L.dot_gap = 8;
        L.tiles_bottom = 414;
        L.tile_gap = 10;
        L.tile_radius = 10;
        L.tile_pad_x = 14;
        L.tile_pad_y = 12;
        L.wide_tile_h = 150;
        L.slider_tile_h = 90;
        L.tile_label_font = &font_styrene_16;
        L.ctrl_font = &font_styrene_20;
        L.seg_h = 52;  L.seg_gap = 6;  L.seg_radius = 10;
        L.toggle_w = 72; L.toggle_h = 34; L.toggle_knob = 26;
        L.slider_h = 10; L.slider_knob = 24;
        L.dots_y = 428;
        L.about_row_h = 32;
        L.about_key_font  = &font_styrene_16;
        L.about_val_font  = &font_styrene_16;
        L.about_hint_font = &font_styrene_14;
        L.about_hint_y = -40;
        L.bt_info_panel_h = 140;
        L.bt_reset_zone_h = 90;
        L.bt_title_font    = &font_tiempos_34;
        L.bt_status_font   = &font_styrene_28;
        L.bt_device_font   = &font_styrene_20;
        L.bt_credit_1_font = &font_styrene_16;
        L.bt_credit_2_font = &font_styrene_14;
        L.glow_w = 10;                   // five 2 px rings
        L.level_dots_y = 2 * L.usage_panel_h + L.usage_panel_gap + 5;
        L.mascot_cell = 2;
        L.st_tile_h = 74;  L.st_gap = 6;
        L.st_label_font = &font_styrene_12;
        L.st_value_font = &font_styrene_20;
        L.st_cell = 10;  L.st_cell_gap = 3;  L.st_weeks = 24;
        L.st_hm_y = 2 * 74 + 6 + 10;
    } else {
        // Small layout — tuned for 240x240 (LCD-1.54 and similar square TFTs).
        // Everything shrinks: fonts two steps down, panels ~half height, and
        // the corner logo/battery switch to the 40px/24px small assets.
        L.margin = 8;
        L.title_y = 4;
        L.content_y = 44;
        L.usage_panel_h = 74;
        L.usage_panel_gap = 6;
        L.usage_bar_y = 30;
        L.usage_reset_y = 46;
        L.bar_h = 12;
        L.panel_pad_x = 10;
        L.panel_pad_y = 6;
        L.pill_pad_x = 8;
        L.pill_pad_y = 2;
        L.title_font   = &font_tiempos_34;
        L.pct_font     = &font_styrene_24;
        L.ent_pct_font = &font_tiempos_34;
        L.pill_font    = &font_styrene_14;
        L.reset_font   = &font_styrene_14;
        L.pace_font    = &font_styrene_12;
        L.anim_font    = &font_mono_18;
        L.host_font    = &font_styrene_12;
        // Center the status line in the strip below the weekly panel; flush
        // against the bottom edge it reads as unevenly spaced.
        L.anim_y = -10;
        L.small_icons = true;
        L.title_nudge = 8;
        L.logo_y = 2;
        L.batt_h = 18;
        L.batt_nub_w = 3;
        L.batt_nub_h = 8;
        L.batt_border = 2;
        L.batt_radius = 4;
        L.batt_font = &font_styrene_12;
        L.batt_w = 44 + 2 + L.batt_nub_w;
        L.batt_y = 10 + (24 - L.batt_h) / 2;
        L.face_dot_y = L.usage_reset_y + 6;
        L.dot_size = 5;
        L.dot_gap = 6;
        L.tiles_bottom = 222;
        L.tile_gap = 6;
        L.tile_radius = 6;
        L.tile_pad_x = 8;
        L.tile_pad_y = 6;
        L.wide_tile_h = 84;
        L.slider_tile_h = 50;
        L.tile_label_font = &font_styrene_12;
        L.ctrl_font = &font_styrene_14;
        L.seg_h = 30;  L.seg_gap = 4;  L.seg_radius = 6;
        L.toggle_w = 42; L.toggle_h = 20; L.toggle_knob = 14;
        L.slider_h = 6;  L.slider_knob = 14;
        L.dots_y = 229;
        L.about_row_h = 20;
        L.about_key_font  = &font_styrene_12;
        L.about_val_font  = &font_styrene_12;
        L.about_hint_font = &font_styrene_12;
        L.about_hint_y = -16;
        L.pair_y1 = 12;
        L.pair_y2 = 56;
        L.pair_y3 = 80;
        L.idle_px = 96;
        L.bt_info_panel_h = 90;
        L.bt_reset_zone_h = 60;
        L.bt_title_font    = &font_tiempos_34;
        L.bt_status_font   = &font_styrene_20;
        L.bt_device_font   = &font_styrene_14;
        L.bt_credit_1_font = &font_styrene_12;
        L.bt_credit_2_font = &font_styrene_12;
        L.glow_w = 5;                    // five 1 px rings
        L.level_dots_y = 2 * L.usage_panel_h + L.usage_panel_gap + 3;
        L.mascot_cell = 1;
        L.st_tile_h = 40;  L.st_gap = 4;
        L.st_label_font = &font_styrene_12;
        L.st_value_font = &font_styrene_14;
        L.st_cell = 6;  L.st_cell_gap = 2;  L.st_weeks = 24;
        L.st_hm_y = 2 * 40 + 4 + 6;
    }
#ifndef BOARD_HAS_PSRAM
    // Internal SRAM only: the idle creature shrinks from 160 to 120 px
    // (≈ 10 KB instead of 21).
    if (L.idle_px > 120) L.idle_px = 120;
#endif

    L.content_w = L.scr_w - 2 * L.margin;
    L.body_h = L.scr_h - L.content_y;
}

// Anthropic brand palette — design tokens live in theme.h
#include "theme.h"
#define COL_BG        THEME_BG
#define COL_PANEL     THEME_PANEL
#define COL_TEXT      THEME_TEXT
#define COL_DIM       THEME_DIM
#define COL_ACCENT    THEME_ACCENT
#define COL_GREEN     THEME_GREEN
#define COL_AMBER     THEME_AMBER
#define COL_RED       THEME_RED
#define COL_BAR_BG    THEME_BAR_BG
#define COL_PRESSED   THEME_PANEL_PRESSED
#define COL_DOT_OFF   THEME_DOT_OFF

// ---- Header + body viewport ----
// The header (title, corner mascot, battery glyph) is static, screen-level
// chrome. Everything that navigates lives inside `body`, a clipping viewport
// from content_y down: the Usage and Settings surfaces are its children and
// slide vertically inside it, so a transition repaints only the body rows and
// nothing ever slides through the header.
static lv_obj_t* body = nullptr;
static lv_obj_t* lbl_title;                    // shared: clock/"Usage" or "Settings"
static screen_t  title_screen = SCREEN_USAGE;  // whose title the header currently shows

// ---- Usage screen widgets ----
static lv_obj_t* usage_container;
// Clock fed by the daemon: base epoch (local wall-clock seconds) + the lv_tick at
// which it landed, so the title ticks forward locally between 60s payloads.
// Whether it is SHOWN is the device's own Clock setting (settings.h).
static long     clock_base_epoch = 0;
static uint32_t clock_base_ms = 0;
static int      clock_fmt = 24;   // 12 or 24, the host's format from the daemon payload
static int      clock_last_min = -1;   // last rendered minute; avoids redrawing the title every tick
static lv_obj_t* usage_group;   // the two usage panels — shown when connected
static lv_obj_t* pair_group;    // link hint - shown when disconnected
static lv_obj_t* pair_l1 = nullptr;
static lv_obj_t* pair_l2 = nullptr;
static lv_obj_t* pair_l3 = nullptr;
#ifdef CLAWD_LINK_WIFI
// A WiFi device has nothing to pair: this screen reports the link instead, and
// the poll task overwrites all three lines as it goes (ui_set_link_hint).
#define PAIR_HINT_1 "Starting up"
#define PAIR_HINT_2 ""
#define PAIR_HINT_3 ""
#else
#define PAIR_HINT_1 "To pair"
#define PAIR_HINT_2 "hold the power button"
#define PAIR_HINT_3 "for 3 seconds, then release"
#endif

// Staged by any task, applied by the LVGL loop: LVGL is single-threaded, and
// the WiFi poll task runs beside it.
static char hint_buf[3][40];
static volatile bool hint_dirty = false;

void ui_set_link_hint(const char* l1, const char* l2, const char* l3) {
    strlcpy(hint_buf[0], l1 ? l1 : "", sizeof(hint_buf[0]));
    strlcpy(hint_buf[1], l2 ? l2 : "", sizeof(hint_buf[1]));
    strlcpy(hint_buf[2], l3 ? l3 : "", sizeof(hint_buf[2]));
    hint_dirty = true;
}

static void apply_link_hint(void) {
    if (!hint_dirty || !pair_l1) return;
    hint_dirty = false;
    lv_label_set_text(pair_l1, hint_buf[0]);
    lv_label_set_text(pair_l2, hint_buf[1]);
    lv_label_set_text(pair_l3, hint_buf[2]);
}
static lv_obj_t* bar_session;
static lv_obj_t* lbl_session_pct;
static lv_obj_t* lbl_session_label;
static lv_obj_t* lbl_session_reset;
static lv_obj_t* bar_weekly;
static lv_obj_t* lbl_weekly_pct;
static lv_obj_t* lbl_weekly_label;
static lv_obj_t* lbl_weekly_reset;
static lv_obj_t* panel_session = nullptr;
static lv_obj_t* panel_weekly = nullptr;
// Enterprise-only widgets inside panel_session
static lv_obj_t* lbl_session_pct_sym = nullptr;  // "%" in smaller font
static lv_obj_t* lbl_spending_desc = nullptr;     // "of your monthly budget"
static lv_obj_t* lbl_spending_status = nullptr;   // "Under pace" / "On pace" / "Over pace"
static lv_obj_t* lbl_anim;      // status line: connection state + whimsical idle
// "Ready" (a session is open, waiting for you): no spinner, no ellipsis — a
// calm green line with a breathing green dot in front of it.
static lv_obj_t* ready_dot = nullptr;
static lv_obj_t* lbl_host = nullptr;            // caption above the ticker: the remote machine on the line
// The ticker's three styles: whimsical (accent word, spinner glyph, ellipsis —
// no companion, or a state worth narrating), Ready (green word, breathing green
// dot: a session is waiting for you) and Idle (dim word, still dim dot: the
// companion reports and no Claude Code is running anywhere).
enum TickerMode : uint8_t { TICKER_WHIMSY, TICKER_READY, TICKER_IDLE };
static TickerMode ticker_mode = TICKER_WHIMSY;
static int32_t   ready_dot_opa = 255;

// ---- Usage level: two horizontal pages ----
// Stats sits left of Usage; the ticker and the page dots belong to the level
// itself so they stay put while pages slide.
#define LEVEL_PAGES 2
#define LEVEL_STATS 0
#define LEVEL_USAGE 1
static lv_obj_t* level = nullptr;               // the surface that slides vertically against Settings
static lv_obj_t* level_pages[LEVEL_PAGES];
static lv_obj_t* level_dots[LEVEL_PAGES];
static int       level_page = LEVEL_USAGE;
static int       title_level_page = LEVEL_USAGE; // which level page the header title describes
static uint32_t  level_dots_hide_ms = 0;         // when the dots start fading (0 = hidden)
static char      title_cur[24] = "";

// ---- Companion state (drives the ticker, the alerts and the header mascot) ----
static CompanionData cc_cur = {};
static bool      cc_seen = false;                // any "cc" ever received
static uint32_t  cc_at_ms = 0;                   // lv_tick when cc_cur landed
static uint32_t  preview_until_ms = 0;           // "Preview" alert running until then
// The header mascot on PSRAM-less boards is an actor (splash_actor_*): the
// still pose normally, these acts cycled while Claude needs you.
static lv_obj_t* header_actor = nullptr;
static const char* const HEADER_ACTS[] = { "waving", "pointing", "jumping happy" };
static const char* const ALERT_ACTS[]  = { "jumping happy", "waving", "pointing" };
static uint8_t   alert_act_idx = 0;

// ---- Stats page ----
#define ST_TILES 8
static lv_obj_t* page_stats = nullptr;
static lv_obj_t* st_value[ST_TILES];
static lv_obj_t* st_hm = nullptr;                // heatmap: ONE object, cells painted in LV_EVENT_DRAW_MAIN
static StatsData st_cur = {};
static bool      st_seen = false;
static const char* const ST_LABELS[ST_TILES] = {
    "Sessions", "Messages", "Tokens", "Active days", "Streak", "Best streak", "Peak hour", "Model",
};

// ---- Alert glow ----
// One full-screen transparent object that paints GLOW_RINGS nested rounded
// borders in LV_EVENT_DRAW_MAIN, each one band wide and dimmer than the last,
// with the panel's real corner radius (BoardCaps.corner_radius) — so the glow
// follows the glass instead of being cut off by the bezel. The breathing anim
// invalidates only the edge bands and corner squares, never the whole screen.
#define GLOW_RINGS 5
static const uint8_t GLOW_RING_OPA[GLOW_RINGS] = { 255, 170, 105, 55, 20 };
static lv_obj_t* glow_obj = nullptr;
static lv_obj_t* setup_root = nullptr;   // WiFi setup card (WiFi build)
static lv_obj_t* setup_ssid = nullptr;
static lv_obj_t* setup_pass = nullptr;
static lv_obj_t* setup_stat = nullptr;
static bool      alert_active = false;
static int32_t   glow_level = 0;                 // animated 0..255
static int       glow_radius = 0;                // corner radius in use (caps, or "radius N" over serial)
static lv_obj_t* corner_test = nullptr;          // calibration overlay (serial "corners on")

// ---- Weekly card faces ----
// Some plans meter one model separately inside the weekly window (Fable on
// Max plans — the API reports it as a weekly_scoped limit). A third full-size
// card can't fit next to the two originals on a 480 px panel, so the Weekly
// card gets FACES instead: face 0 is the classic all-models card, face i the
// identical card for scoped model i — same number, pill, bar and reset line,
// so the extra limit looks exactly like the rest of the screen. Small dots on
// the reset row show how many faces there are; the card auto-advances at the
// user's "Flip every" interval (Settings → Weekly card; Off = never) and a tap
// flips it at once — holding a while when flipping, or becoming the new
// default face when not.
// Plans without a scoped limit have one face, no dots, and the card is
// pixel-identical to the original.
static ScopedWeekly cached_scoped[MAX_SCOPED_WEEKLY];
static int       cached_scoped_count = 0;
static float     cached_weekly_pct = 0;
static int       cached_weekly_reset = -1;
static int       weekly_face = 0;           // 0 = all models, i = cached_scoped[i-1]
static uint32_t  face_next_auto_ms = 0;     // lv_tick of the next automatic flip (0 = none scheduled)
static lv_obj_t* face_dots[MAX_SCOPED_WEEKLY + 1];
// The flip interval and the default face are user settings (Settings → Weekly
// card). With flipping off, a tap picks a face and the pick is remembered.
#define FACE_HOLD_MS 20000                  // after a tap while flipping: keep the chosen face this long

// ---- Settings: three swipeable pages of real controls ----
//   Page 0  Clock (segmented picker, sliding highlight) · Battery icon · Mascot (toggles)
//   Page 1  Brightness (slider, live preview) · Sleep after (stepped slider) ·
//           Status line (toggle) · Pairing (button, two taps)
//   Page 2  Weekly card: Default face (picker built from the live limits) ·
//           Flip every (stepped slider, Off … 30 s)
//   Page 3  Alerts: Chime picker (Off / Needs you / All) + Preview · Volume slider + Glow toggle
//   Page 4  About (device info)
// Pages slide horizontally on swipe; the terra-cotta accent is the one
// "active" colour across every control so they read as a family.
static lv_obj_t* settings_container = nullptr;
#define SET_PAGES 5
#define PAGE_FACE  2
#define PAGE_ALERTS 3
#define PAGE_ABOUT 4
static lv_obj_t* settings_pages[SET_PAGES];
static lv_obj_t* page_dots[SET_PAGES];
static int       settings_page = 0;

// Clock: segmented picker
static lv_obj_t* seg_highlight = nullptr;
static lv_obj_t* seg_labels[CLOCK_MODE_COUNT];
static int16_t   seg_x[CLOCK_MODE_COUNT];
static lv_obj_t* clock_preview = nullptr;

// Toggles
struct Toggle { lv_obj_t* track; lv_obj_t* knob; lv_obj_t* value; };
static Toggle tg_battery, tg_mascot, tg_status, tg_glow;

// Companion page: chime picker (Off / Needs you / All) + Preview button
static lv_obj_t* chime_hl = nullptr;
static lv_obj_t* chime_labels[CHIME_MODE_COUNT];
static int16_t   chime_x[CHIME_MODE_COUNT];
static lv_obj_t* preview_button = nullptr;
static lv_obj_t* sl_volume = nullptr;
static lv_obj_t* lbl_volume = nullptr;

// Sliders
static lv_obj_t* sl_brightness = nullptr;
static lv_obj_t* lbl_brightness = nullptr;
static lv_obj_t* sl_sleep = nullptr;
static lv_obj_t* lbl_sleep = nullptr;

// Weekly card page: default-face picker (segments follow the live limits) + flip slider
#define FACE_SEG_MAX (1 + MAX_SCOPED_WEEKLY)
static lv_obj_t* face_seg_tile = nullptr;
static lv_obj_t* face_seg_hl = nullptr;
static lv_obj_t* face_seg_obj[FACE_SEG_MAX];
static lv_obj_t* face_seg_lbl[FACE_SEG_MAX];
static int16_t   face_seg_x[FACE_SEG_MAX];
static int       face_seg_count = 1;
static lv_obj_t* face_preview = nullptr;
static lv_obj_t* sl_flip = nullptr;
static lv_obj_t* lbl_flip = nullptr;

// Pairing button
#ifdef CLAWD_LINK_WIFI
#define PAIR_TILE_LABEL  "WiFi"
#define PAIR_BUTTON_IDLE "Set up WiFi"
#else
#define PAIR_TILE_LABEL  "Pairing"
#define PAIR_BUTTON_IDLE "Forget host"
#endif
static lv_obj_t* pair_button = nullptr;
static uint32_t  pairing_confirm_ms = 0;   // >0 while "Confirm?" is armed
static uint32_t  pairing_cleared_ms = 0;   // >0 while "Cleared" is shown
#define PAIRING_CONFIRM_MS 4000
#define PAIRING_CLEARED_MS 3000

// About rows
enum AboutRow {
    ABOUT_BOARD, ABOUT_FW, ABOUT_BLE, ABOUT_ADDR, ABOUT_BATTERY, ABOUT_RAM, ABOUT_UPTIME,
    ABOUT_COUNT,
};
static const char* const ABOUT_KEYS[ABOUT_COUNT] = {
    "Board", "Firmware", "Bluetooth", "Address", "Battery", "Free RAM", "Uptime",
};
static lv_obj_t* about_value[ABOUT_COUNT];
static uint32_t  about_last_refresh_ms = 0;
static uint32_t  preview_last_refresh_ms = 0;
static int       batt_pct_cached = -1;         // the board's own cell (-1 = none fitted)
static bool      batt_charging_cached = false;
// The host machine's battery, from the daemon payload ("hb"/"hc"). The header
// glyph shows the board's own cell when one is fitted, otherwise this — a
// USB-powered Clawdmeter has nothing of its own worth showing.
static int       host_batt_pct = -1;
static bool      host_batt_charging = false;

// ---- Motion ----
// Toggles and the segment highlight glide (ease-out); page/screen changes are
// finger-driven (below) and snap with one shared animation.
#define ANIM_SEG_MS  220
#define ANIM_CTRL_MS 180
static bool transitioning = false;         // a snap animation is in flight

// ---- Swipe engine ----
// One finger, tracked at the input-device level so it works no matter which
// tile or card the press lands on (sliders excepted — they keep their drag).
// Settings "lives below" Usage: on Usage, dragging UP pulls Settings in from
// the bottom; on any Settings page, dragging DOWN pushes it away and Usage is
// back — one gesture home from anywhere. Left/right drags page within
// Settings. The surfaces follow the finger 1:1; on release a SINGLE animation
// drives both leaving and arriving surface to the snap point in lockstep, so
// they can never drift apart. A drag past a quarter of the span, or a flick,
// commits; anything less springs back. LVGL still sends CLICKED on release,
// so a drag arms the click guard the moment it starts.
//
// Surfaces are never hidden/unhidden mid-gesture (LVGL skips redrawing a
// container that becomes visible while moving): the inactive screen is PARKED
// one span off-screen (Usage above, Settings below; non-current settings pages
// left/right of the current one) and only ever moved. HIDDEN is reserved for
// the splash, which covers everything.
enum DragMode { DRAG_NONE, DRAG_H, DRAG_V, DRAG_SPLASH, DRAG_IGNORE };
static DragMode   drag_mode = DRAG_NONE;
static lv_point_t drag_origin = {0, 0};
static bool       drag_vertical = false;
static int        drag_sign = 0;           // +1: finger moved left/up (content advances), -1: right/down
static int32_t    drag_span = 0;           // screen width (H) or height (V)
static int32_t    drag_off = 0;            // current displacement of the leaving surface
static lv_obj_t*  drag_out = nullptr;      // surface leaving
static lv_obj_t*  drag_in = nullptr;       // surface arriving (NULL = nothing there: rubber band)
static int        drag_target_page = -1;   // H: page index of drag_in
static bool       drag_commit = false;
static bool       drag_tracking = false;   // finger is down; poll its position each tick
static lv_indev_t* s_indev = nullptr;
#define DRAG_START_PX   14                 // finger travel before a press becomes a drag
#define DRAG_COMMIT_DIV 4                  // commit past span/4 …
#define DRAG_COMMIT_VEL 8                  // … or on a flick faster than this (px per indev tick)
#define SNAP_MS         200                // full-span snap time; scaled by the remaining distance
#define RUBBER_DIV      3                  // resistance when dragging where there is no page
static uint32_t  last_gesture_ms = 0;
#define GESTURE_CLICK_GUARD_MS 600

// ---- Battery widget (shared header chrome, on top) ----
// Drawn, not a bitmap: outline + nub in the text colour, a fill proportional to
// the level, and the percentage centred inside. Source and colour come from
// resolve_battery(): neutral fill, red at ≤10%, accent while charging.
static lv_obj_t* batt_root = nullptr;
static lv_obj_t* batt_body = nullptr;
static lv_obj_t* batt_fill = nullptr;
static lv_obj_t* batt_lbl = nullptr;
static lv_obj_t* logo_img;

// ---- Live-data freshness → which usage sub-view to show ----
// usage panels when data is flowing, an idle "Zzz" screen when the host is
// connected but no usage update landed within DATA_FRESH_MS, the pairing hint
// when BLE is down. Re-evaluated every loop in ui_tick_anim().
static lv_obj_t* idle_group;            // the "Zzz" idle screen
static uint32_t  last_data_ms = 0;      // lv_tick when the last valid usage update landed
static bool      data_received = false; // any valid update since boot
static bool      data_ok = true;        // last payload's ok flag; a {"ok":false} beat = "no fresh data"
static int       view_state = -1;       // -1 unknown / 0 pair / 1 idle / 2 usage
static const uint32_t DATA_FRESH_MS = 90000;  // usage counts as "live" within this window (daemon sends ~60s)
static const uint32_t CC_LIVE_MS = 15UL * 60UL * 1000UL;  // companion state drives the ticker this long after its last beat

// ---- Shared ----
static screen_t current_screen = SCREEN_USAGE;
static bool     s_ble_connected = false;   // cached BLE connection state
static uint32_t connected_at_ms = 0;       // when we last entered CONNECTED ("Connected" dwell)

// Animation state
static uint32_t anim_last_ms = 0;
static uint8_t anim_spinner_idx = 0;
static uint8_t anim_phase = 0;
static uint8_t anim_msg_idx = 0;
static uint32_t anim_msg_start = 0;
#define ANIM_MSG_MS     4000

static const char* const spinner_frames[] = {
    "\xC2\xB7", "\xE2\x9C\xBB", "\xE2\x9C\xBD",
    "\xE2\x9C\xB6", "\xE2\x9C\xB3", "\xE2\x9C\xA2",
};
#define SPINNER_COUNT 6
#define SPINNER_PHASES (2 * (SPINNER_COUNT - 1))  // 10: ping-pong 0..5..0

static const uint16_t spinner_ms[SPINNER_COUNT] = {
    260, 130, 130, 130, 130, 260,
};

static const char* const anim_messages[] = {
    "Accomplishing", "Elucidating", "Perusing",
    "Actioning", "Enchanting", "Philosophising",
    "Actualizing", "Envisioning", "Pondering",
    "Baking", "Finagling", "Pontificating",
    "Booping", "Flibbertigibbeting", "Processing",
    "Brewing", "Forging", "Puttering",
    "Calculating", "Forming", "Puzzling",
    "Cerebrating", "Frolicking", "Reticulating",
    "Channelling", "Generating", "Ruminating",
    "Churning", "Germinating", "Scheming",
    "Clauding", "Hatching", "Schlepping",
    "Coalescing", "Herding", "Shimmying",
    "Cogitating", "Honking", "Shucking",
    "Combobulating", "Hustling", "Simmering",
    "Computing", "Ideating", "Smooshing",
    "Concocting", "Imagining", "Spelunking",
    "Conjuring", "Incubating", "Spinning",
    "Considering", "Inferring", "Stewing",
    "Contemplating", "Jiving", "Sussing",
    "Cooking", "Manifesting", "Synthesizing",
    "Crafting", "Marinating", "Thinking",
    "Creating", "Meandering", "Tinkering",
    "Crunching", "Moseying", "Transmuting",
    "Deciphering", "Mulling", "Unfurling",
    "Deliberating", "Mustering", "Unravelling",
    "Determining", "Musing", "Vibing",
    "Discombobulating", "Noodling", "Wandering",
    "Divining", "Percolating", "Whirring",
    "Doing", "Wibbling",
    "Effecting", "Wizarding",
    "Working", "Wrangling",
};
#define ANIM_MSG_COUNT (sizeof(anim_messages) / sizeof(anim_messages[0]))

static lv_color_t pct_color(float pct) {
    if (pct >= 80.0f) return COL_RED;
    if (pct >= 50.0f) return COL_AMBER;
    return COL_GREEN;
}

static void format_reset_time(int mins, char* buf, size_t len) {
    if (mins < 0) {
        snprintf(buf, len, "---");
    } else if (mins < 60) {
        snprintf(buf, len, "Resets in %dm", mins);
    } else if (mins < 1440) {
        snprintf(buf, len, "Resets in %dh %dm", mins / 60, mins % 60);
    } else {
        snprintf(buf, len, "Resets in %dd %dh", mins / 1440, (mins % 1440) / 60);
    }
}

// Forward decls — callbacks defined near ui_show_screen below
static void global_click_cb(lv_event_t* e);
static void indev_cb(lv_event_t* e);
static void drag_tick(void);
static void weekly_click_cb(lv_event_t* e);
static void refresh_settings_controls(bool animate);
static void show_settings_page(int page);
static void update_page_dots(void);
static void refresh_about(void);
static void apply_header_visibility(void);
static void render_title(bool force);
static void refresh_battery_glyph(void);
static void show_level_page(int page);
static void snap_drag_ms(bool commit, uint32_t force_ms);
static void render_stats_page(void);
static lv_obj_t* make_tile(lv_obj_t* parent, int x, int y, int w, int h, bool clickable);
static void alert_start(int kind);
static void alert_stop(void);
static void level_dots_show(void);
static void update_level_dots(void);

static bool click_guarded(void) {
    return transitioning || (lv_tick_get() - last_gesture_ms) < GESTURE_CLICK_GUARD_MS;
}

// ---- Motion helpers ----
static void anim_x_exec(void* var, int32_t v) { lv_obj_set_x((lv_obj_t*)var, v); }

// Glide `obj` to x=`to` (relative to its parent, plain positioning — never use
// on objects placed with lv_obj_align, whose x is an alignment offset).
static void animate_x(lv_obj_t* obj, int32_t to, uint32_t ms, lv_anim_completed_cb_t done) {
    lv_anim_delete(obj, anim_x_exec);
    lv_anim_t a;
    lv_anim_init(&a);
    lv_anim_set_var(&a, obj);
    lv_anim_set_values(&a, lv_obj_get_x(obj), to);
    lv_anim_set_duration(&a, ms);
    lv_anim_set_exec_cb(&a, anim_x_exec);
    lv_anim_set_path_cb(&a, lv_anim_path_ease_out);
    if (done) lv_anim_set_completed_cb(&a, done);
    lv_anim_start(&a);
}

static void drag_anim_exec(void* var, int32_t v);

// Kill any in-flight motion and put every movable surface back at rest.
static void settle_slides(void) {
    lv_anim_delete(NULL, anim_x_exec);
    lv_anim_delete(&drag_off, drag_anim_exec);
    transitioning = false;
    drag_out = drag_in = nullptr;
    drag_mode = DRAG_IGNORE;      // until the next press
    if (lbl_title) lv_obj_set_style_text_opa(lbl_title, LV_OPA_COVER, 0);
}

// Park the two surfaces inside the body viewport for `active`: the active one
// at the origin, the other one body-height away (Usage above Settings). Both
// stay visible; the viewport clips the parked one.
static void park_screens(screen_t active) {
    lv_obj_clear_flag(level, LV_OBJ_FLAG_HIDDEN);
    if (settings_container) lv_obj_clear_flag(settings_container, LV_OBJ_FLAG_HIDDEN);
    if (active == SCREEN_SETTINGS) {
        lv_obj_set_pos(level, 0, -L.body_h);
        if (settings_container) lv_obj_set_pos(settings_container, 0, 0);
    } else {
        lv_obj_set_pos(level, 0, 0);
        if (settings_container) lv_obj_set_pos(settings_container, 0, L.body_h);
    }
}

static lv_obj_t* make_panel(lv_obj_t* parent, int x, int y, int w, int h) {
    lv_obj_t* panel = lv_obj_create(parent);
    lv_obj_set_pos(panel, x, y);
    lv_obj_set_size(panel, w, h);
    lv_obj_set_style_bg_color(panel, COL_PANEL, 0);
    lv_obj_set_style_bg_opa(panel, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(panel, 8, 0);
    lv_obj_set_style_border_width(panel, 0, 0);
    lv_obj_set_style_pad_left(panel, L.panel_pad_x, 0);
    lv_obj_set_style_pad_right(panel, L.panel_pad_x, 0);
    lv_obj_set_style_pad_top(panel, L.panel_pad_y, 0);
    lv_obj_set_style_pad_bottom(panel, L.panel_pad_y, 0);
    lv_obj_clear_flag(panel, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(panel, LV_OBJ_FLAG_EVENT_BUBBLE);
    return panel;
}

static lv_obj_t* make_bar(lv_obj_t* parent, int x, int y, int w, int h) {
    lv_obj_t* bar = lv_bar_create(parent);
    lv_obj_set_pos(bar, x, y);
    lv_obj_set_size(bar, w, h);
    lv_bar_set_range(bar, 0, 100);
    lv_bar_set_value(bar, 0, LV_ANIM_OFF);
    lv_obj_set_style_bg_color(bar, COL_BAR_BG, LV_PART_MAIN);
    lv_obj_set_style_bg_opa(bar, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_radius(bar, 6, LV_PART_MAIN);
    lv_obj_set_style_bg_color(bar, COL_GREEN, LV_PART_INDICATOR);
    lv_obj_set_style_bg_opa(bar, LV_OPA_COVER, LV_PART_INDICATOR);
    lv_obj_set_style_radius(bar, 6, LV_PART_INDICATOR);
    return bar;
}

// A label drawn as a rounded pill — the UI's labelling device (card names)
// and, with an accent fill, its button.
static lv_obj_t* make_pill_styled(lv_obj_t* parent, const char* text, const lv_font_t* font,
                                  int pad_x, int pad_y) {
    lv_obj_t* lbl = lv_label_create(parent);
    lv_label_set_text(lbl, text);
    lv_obj_set_style_text_font(lbl, font, 0);
    lv_obj_set_style_text_color(lbl, COL_TEXT, 0);
    lv_obj_set_style_bg_color(lbl, COL_BAR_BG, 0);
    lv_obj_set_style_bg_opa(lbl, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(lbl, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_pad_left(lbl, pad_x, 0);
    lv_obj_set_style_pad_right(lbl, pad_x, 0);
    lv_obj_set_style_pad_top(lbl, pad_y, 0);
    lv_obj_set_style_pad_bottom(lbl, pad_y, 0);
    return lbl;
}

static lv_obj_t* make_pill(lv_obj_t* parent, const char* text) {
    return make_pill_styled(parent, text, L.pill_font, L.pill_pad_x, L.pill_pad_y);
}

// A small round indicator dot (face / page indicator).
static lv_obj_t* make_dot(lv_obj_t* parent) {
    lv_obj_t* d = lv_obj_create(parent);
    lv_obj_set_size(d, L.dot_size, L.dot_size);
    lv_obj_set_style_radius(d, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_border_width(d, 0, 0);
    lv_obj_set_style_pad_all(d, 0, 0);
    lv_obj_set_style_bg_color(d, COL_DOT_OFF, 0);
    lv_obj_set_style_bg_opa(d, LV_OPA_COVER, 0);
    lv_obj_clear_flag(d, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_clear_flag(d, LV_OBJ_FLAG_SCROLLABLE);
    return d;
}

// Transparent, non-scrolling group of the given size at (x, y).
static lv_obj_t* make_group_sized(lv_obj_t* parent, int x, int y, int w, int h) {
    lv_obj_t* g = lv_obj_create(parent);
    lv_obj_set_size(g, w, h);
    lv_obj_set_pos(g, x, y);
    lv_obj_set_style_bg_opa(g, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(g, 0, 0);
    lv_obj_set_style_pad_all(g, 0, 0);
    lv_obj_clear_flag(g, LV_OBJ_FLAG_SCROLLABLE);
    return g;
}

// A surface or page inside the body viewport: body-sized, at its origin.
static lv_obj_t* make_body_group(lv_obj_t* parent) {
    return make_group_sized(parent, 0, 0, L.scr_w, L.body_h);
}

// The header battery: [ body with fill + "NN%" ][nub]
static void make_battery_widget(lv_obj_t* scr) {
    const int body_w = L.batt_w - 2 - L.batt_nub_w;
    batt_root = make_group_sized(scr, L.scr_w - L.batt_w - L.margin, L.batt_y, L.batt_w, L.batt_h);
    lv_obj_clear_flag(batt_root, LV_OBJ_FLAG_CLICKABLE);

    batt_body = lv_obj_create(batt_root);
    lv_obj_set_pos(batt_body, 0, 0);
    lv_obj_set_size(batt_body, body_w, L.batt_h);
    lv_obj_set_style_bg_opa(batt_body, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_color(batt_body, COL_TEXT, 0);
    lv_obj_set_style_border_width(batt_body, L.batt_border, 0);
    lv_obj_set_style_radius(batt_body, L.batt_radius, 0);
    lv_obj_set_style_pad_all(batt_body, 0, 0);
    lv_obj_clear_flag(batt_body, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_clear_flag(batt_body, LV_OBJ_FLAG_SCROLLABLE);

    // Fill sits inside the border with a 2 px inset so the outline stays crisp.
    batt_fill = lv_obj_create(batt_body);
    lv_obj_set_pos(batt_fill, 2, 2);
    lv_obj_set_size(batt_fill, 2, L.batt_h - 2 * L.batt_border - 4);
    lv_obj_set_style_radius(batt_fill, L.batt_radius - L.batt_border - 1, 0);
    lv_obj_set_style_border_width(batt_fill, 0, 0);
    lv_obj_set_style_pad_all(batt_fill, 0, 0);
    lv_obj_set_style_bg_color(batt_fill, COL_DOT_OFF, 0);
    lv_obj_set_style_bg_opa(batt_fill, LV_OPA_COVER, 0);
    lv_obj_clear_flag(batt_fill, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_clear_flag(batt_fill, LV_OBJ_FLAG_SCROLLABLE);

    batt_lbl = lv_label_create(batt_body);
    lv_label_set_text(batt_lbl, "");
    lv_obj_set_style_text_font(batt_lbl, L.batt_font, 0);
    lv_obj_set_style_text_color(batt_lbl, COL_TEXT, 0);
    lv_obj_center(batt_lbl);

    lv_obj_t* nub = lv_obj_create(batt_root);
    lv_obj_set_pos(nub, body_w + 2, (L.batt_h - L.batt_nub_h) / 2);
    lv_obj_set_size(nub, L.batt_nub_w, L.batt_nub_h);
    lv_obj_set_style_radius(nub, 2, 0);
    lv_obj_set_style_border_width(nub, 0, 0);
    lv_obj_set_style_pad_all(nub, 0, 0);
    lv_obj_set_style_bg_color(nub, COL_TEXT, 0);
    lv_obj_set_style_bg_opa(nub, LV_OPA_COVER, 0);
    lv_obj_clear_flag(nub, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_clear_flag(nub, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_add_flag(batt_root, LV_OBJ_FLAG_HIDDEN);
}

// ======== Usage Screen ========

static lv_obj_t* make_usage_panel(lv_obj_t* parent, int y, const char* pill_text,
                                  lv_obj_t** out_pct, lv_obj_t** out_pill,
                                  lv_obj_t** out_bar, lv_obj_t** out_reset) {
    lv_obj_t* panel = make_panel(parent, L.margin, y, L.content_w, L.usage_panel_h);

    *out_pct = lv_label_create(panel);
    lv_label_set_text(*out_pct, "---%");
    lv_obj_set_style_text_font(*out_pct, L.pct_font, 0);
    lv_obj_set_style_text_color(*out_pct, COL_TEXT, 0);
    lv_obj_set_pos(*out_pct, 0, 0);

    *out_pill = make_pill(panel, pill_text);
    lv_obj_align(*out_pill, LV_ALIGN_TOP_RIGHT, 0, 1);

    *out_bar = make_bar(panel, 0, L.usage_bar_y,
                        L.content_w - 2 * L.panel_pad_x, L.bar_h);

    *out_reset = lv_label_create(panel);
    lv_label_set_text(*out_reset, "---");
    lv_obj_set_style_text_font(*out_reset, L.reset_font, 0);
    lv_obj_set_style_text_color(*out_reset, COL_DIM, 0);
    lv_obj_set_pos(*out_reset, 0, L.usage_reset_y);

    return panel;
}

// Draw the Weekly card's current face from the cached payload. Face 0 is the
// classic all-models rendering; a scoped face swaps the number, the pill's
// text (the model's own label) and the bar — the reset line is shared because
// every weekly limit resets at the same instant. Flips snap (no bar slide);
// fresh payloads animate as they always did.
static void render_weekly_face(bool animate) {
    const int faces = 1 + cached_scoped_count;
    if (weekly_face >= faces) weekly_face = 0;
    const bool scoped = weekly_face > 0;
    const ScopedWeekly* sw = scoped ? &cached_scoped[weekly_face - 1] : nullptr;
    const float pct = scoped ? sw->pct : cached_weekly_pct;
    const int p = (int)(pct + 0.5f);

    lv_label_set_text(lbl_weekly_label, scoped ? sw->name : "Weekly");
    lv_label_set_text_fmt(lbl_weekly_pct, "%d%%", p);
    lv_bar_set_value(bar_weekly, p, animate ? LV_ANIM_ON : LV_ANIM_OFF);
    lv_obj_set_style_bg_color(bar_weekly, pct_color(pct), LV_PART_INDICATOR);
    char buf[48];
    format_reset_time(cached_weekly_reset, buf, sizeof(buf));
    lv_label_set_text(lbl_weekly_reset, buf);

    // Indicator dots, right-aligned on the reset row, face 0 leftmost. Hidden
    // entirely on single-face plans so the classic card stays untouched.
    for (int i = 0; i <= MAX_SCOPED_WEEKLY; i++) {
        if (!face_dots[i]) continue;
        if (faces > 1 && i < faces) {
            lv_obj_clear_flag(face_dots[i], LV_OBJ_FLAG_HIDDEN);
            lv_obj_align(face_dots[i], LV_ALIGN_TOP_RIGHT,
                         -(faces - 1 - i) * (L.dot_size + L.dot_gap), L.face_dot_y);
            lv_obj_set_style_bg_color(face_dots[i], i == weekly_face ? COL_TEXT : COL_DOT_OFF, 0);
        } else {
            lv_obj_add_flag(face_dots[i], LV_OBJ_FLAG_HIDDEN);
        }
    }
}

static int default_face_clamped(void) {
    const int f = settings_get().face_default;
    return f <= cached_scoped_count ? f : 0;
}

// (Re)arm the automatic flip from now, or disarm it when flipping is off.
static void schedule_face_flip(uint32_t from_now_ms) {
    const uint32_t iv = settings_face_flip_ms();
    face_next_auto_ms = (iv && cached_scoped_count > 0) ? lv_tick_get() + from_now_ms : 0;
}

static void flip_weekly_face(uint32_t hold_ms) {
    if (cached_scoped_count <= 0) return;
    weekly_face = (weekly_face + 1) % (1 + cached_scoped_count);
    if (settings_face_flip_ms() == 0) {
        // Not flipping automatically: the tap IS the choice — remember it.
        settings_set_face_default((uint8_t)weekly_face);
        face_next_auto_ms = 0;
        refresh_settings_controls(false);
    } else {
        face_next_auto_ms = lv_tick_get() + hold_ms;
    }
    render_weekly_face(false);
}

// Tap on the Weekly card: flip to the next face and hold it a while. Only wired
// (bubbling disabled) when the plan actually has faces — otherwise the tap
// bubbles up and toggles the splash like anywhere else on the screen.
static void weekly_click_cb(lv_event_t* e) {
    (void)e;
    if (click_guarded()) return;
    if (cached_scoped_count <= 0 || view_state != 2) return;
    flip_weekly_face(FACE_HOLD_MS);
}

// Pairing hint — shown when disconnected so the screen isn't empty and the
// user knows how to (re)pair. Wording matches the 3-second release gesture.
static void build_pair_group(lv_obj_t* parent) {
    pair_group = lv_obj_create(parent);
    lv_obj_set_size(pair_group, L.scr_w, L.body_h);
    lv_obj_set_pos(pair_group, 0, 0);
    lv_obj_set_style_bg_opa(pair_group, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(pair_group, 0, 0);
    lv_obj_set_style_pad_all(pair_group, 0, 0);
    lv_obj_clear_flag(pair_group, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(pair_group, LV_OBJ_FLAG_EVENT_BUBBLE);

    lv_obj_t* l1 = lv_label_create(pair_group);
    pair_l1 = l1;
    lv_label_set_text(l1, PAIR_HINT_1);
    lv_obj_set_style_text_font(l1, L.bt_status_font, 0);
    lv_obj_set_style_text_color(l1, COL_TEXT, 0);
    lv_obj_align(l1, LV_ALIGN_TOP_MID, 0, L.pair_y1);

    lv_obj_t* l2 = lv_label_create(pair_group);
    pair_l2 = l2;
    lv_label_set_text(l2, PAIR_HINT_2);
    lv_obj_set_style_text_font(l2, L.bt_device_font, 0);
    lv_obj_set_style_text_color(l2, COL_DIM, 0);
    lv_obj_align(l2, LV_ALIGN_TOP_MID, 0, L.pair_y2);

    lv_obj_t* l3 = lv_label_create(pair_group);
    pair_l3 = l3;
    lv_label_set_text(l3, PAIR_HINT_3);
    lv_obj_set_style_text_font(l3, L.bt_device_font, 0);
    lv_obj_set_style_text_color(l3, COL_DIM, 0);
    lv_obj_align(l3, LV_ALIGN_TOP_MID, 0, L.pair_y3);

    lv_obj_add_flag(pair_group, LV_OBJ_FLAG_HIDDEN);  // ui_update_ble_status decides
}

// Idle "Zzz" screen — shown when the host is connected but no usage update has
// landed recently (token expired, daemon down, host asleep…). Full-screen, like
// the pairing hint, so we never render hours-old numbers as if they were live.
static void build_idle_group(lv_obj_t* parent) {
    idle_group = lv_obj_create(parent);
    lv_obj_set_size(idle_group, L.scr_w, L.body_h);
    lv_obj_set_pos(idle_group, 0, 0);
    lv_obj_set_style_bg_opa(idle_group, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(idle_group, 0, 0);
    lv_obj_set_style_pad_all(idle_group, 0, 0);
    lv_obj_clear_flag(idle_group, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(idle_group, LV_OBJ_FLAG_EVENT_BUBBLE);

    // A shrunk-down resting creature (the official cloud-ride animation)
    // sits between the header and the status line; the animated "Listening…"
    // status line carries the words, so no extra text is needed here.
    lv_obj_t* creature = splash_mini_create(idle_group, "cloud", L.idle_px);
    if (creature) lv_obj_align(creature, LV_ALIGN_CENTER, 0, -20);

    lv_obj_add_flag(idle_group, LV_OBJ_FLAG_HIDDEN);  // update_view_state decides
}

// ---- Companion helpers ----
static bool cc_is_attention(uint8_t st) { return st == CC_ATTENTION || st == CC_TURN_DONE; }

// ---- Stats page ----
// The Claude app's stats card, in the device's own language: eight tiles in a
// 4×2 grid (label above value) and a Sunday→Saturday activity heatmap of the
// last weeks below, today's column last. Numbers come from the daemon's
// transcript scan ("st"); until they arrive every tile reads "---".
static void fmt_thousands(uint32_t n, char* buf, size_t len) {
    char raw[16];
    snprintf(raw, sizeof(raw), "%lu", (unsigned long)n);
    const int digits = (int)strlen(raw);
    size_t o = 0;
    for (int i = 0; i < digits && o + 2 < len; i++) {
        if (i && (digits - i) % 3 == 0) buf[o++] = ',';
        buf[o++] = raw[i];
    }
    buf[o] = 0;
}

static void fmt_tokens(uint64_t n, char* buf, size_t len) {
    if (n >= 1000000000ULL)      snprintf(buf, len, n >= 100000000000ULL ? "%.0fB" : "%.1fB", (double)n / 1e9);
    else if (n >= 1000000ULL)    snprintf(buf, len, n >= 100000000ULL ? "%.0fM" : "%.1fM", (double)n / 1e6);
    else if (n >= 1000ULL)       snprintf(buf, len, n >= 100000ULL ? "%.0fK" : "%.1fK", (double)n / 1e3);
    else                         snprintf(buf, len, "%lu", (unsigned long)n);
}

static lv_color_t heat_color(char c) {
    switch (c) {
    case '1': return lv_color_mix(COL_ACCENT, COL_BAR_BG, 72);
    case '2': return lv_color_mix(COL_ACCENT, COL_BAR_BG, 130);
    case '3': return lv_color_mix(COL_ACCENT, COL_BAR_BG, 195);
    case '4': return COL_ACCENT;
    default:  return COL_BAR_BG;
    }
}

static void render_stats_page(void) {
    if (!page_stats) return;
    char buf[24];
    const bool have = st_seen;
    for (int i = 0; i < ST_TILES; i++) {
        if (!have) { lv_label_set_text(st_value[i], "---"); continue; }
        switch (i) {
        case 0: fmt_thousands(st_cur.sessions, buf, sizeof(buf)); break;
        case 1: fmt_thousands(st_cur.messages, buf, sizeof(buf)); break;
        case 2: fmt_tokens(st_cur.tokens, buf, sizeof(buf)); break;
        case 3: snprintf(buf, sizeof(buf), "%u", st_cur.active_days); break;
        case 4: snprintf(buf, sizeof(buf), "%ud", st_cur.streak); break;
        case 5: snprintf(buf, sizeof(buf), "%ud", st_cur.best_streak); break;
        case 6:
            if (st_cur.peak_hour < 0) strlcpy(buf, "---", sizeof(buf));
            else if (clock_fmt == 12) {
                int h12 = st_cur.peak_hour % 12; if (h12 == 0) h12 = 12;
                snprintf(buf, sizeof(buf), "%d %s", h12, st_cur.peak_hour < 12 ? "AM" : "PM");
            } else snprintf(buf, sizeof(buf), "%02d:00", st_cur.peak_hour);
            break;
        default: strlcpy(buf, st_cur.model[0] ? st_cur.model : "---", sizeof(buf)); break;
        }
        lv_label_set_text(st_value[i], buf);
    }
    if (st_hm) lv_obj_invalidate(st_hm);            // the draw callback reads st_cur
}

// The heat string is whole weeks, oldest first; show the last st_weeks columns.
static char heat_cell(int idx) {
    const int total = L.st_weeks * 7;
    const int have_days = st_seen ? (int)st_cur.days : 0;
    if (!have_days) return '0';
    const int skip = have_days > total ? have_days - total : 0;          // more history than columns: drop the oldest
    const int pad = have_days < total ? total - have_days : 0;           // less: leading empty cells
    if (idx < pad) return '0';
    const int day = idx - pad + skip;
    return day < have_days ? st_cur.heat[day] : 'x';
}

// 24 × 7 rounded squares from one draw call each — 168 LVGL objects would eat
// a third of the C6's pool for a static picture.
static void heat_draw_cb(lv_event_t* e) {
    lv_layer_t* layer = lv_event_get_layer(e);
    lv_obj_t* obj = (lv_obj_t*)lv_event_get_target(e);
    if (!layer || !obj) return;
    lv_area_t o;
    lv_obj_get_coords(obj, &o);
    lv_draw_fill_dsc_t d;
    lv_draw_fill_dsc_init(&d);
    d.radius = L.st_cell >= 10 ? 3 : 1;
    d.opa = LV_OPA_COVER;
    const int step = L.st_cell + L.st_cell_gap;
    int idx = 0;
    for (int c = 0; c < L.st_weeks; c++) {
        for (int r = 0; r < 7; r++, idx++) {
            const char ch = heat_cell(idx);
            if (ch == 'x') continue;
            d.color = heat_color(ch);
            lv_area_t a = { o.x1 + c * step, o.y1 + r * step, o.x1 + c * step + L.st_cell - 1, o.y1 + r * step + L.st_cell - 1 };
            lv_draw_fill(layer, &d, &a);
        }
    }
}

static void build_stats_page(lv_obj_t* page) {
    const int tile_w = (L.content_w - 3 * L.st_gap) / 4;
    for (int i = 0; i < ST_TILES; i++) {
        const int col = i % 4, row = i / 4;
        lv_obj_t* t = make_tile(page, L.margin + col * (tile_w + L.st_gap), row * (L.st_tile_h + L.st_gap),
                                tile_w, L.st_tile_h, false);
        lv_obj_set_style_pad_left(t, 8, 0);
        lv_obj_set_style_pad_right(t, 8, 0);
        lv_obj_set_style_pad_top(t, 10, 0);
        lv_obj_set_style_pad_bottom(t, 10, 0);
        lv_obj_t* l = lv_label_create(t);
        lv_label_set_text(l, ST_LABELS[i]);
        lv_obj_set_style_text_font(l, L.st_label_font, 0);
        lv_obj_set_style_text_color(l, COL_DIM, 0);
        lv_obj_set_width(l, tile_w - 16);
        lv_label_set_long_mode(l, LV_LABEL_LONG_CLIP);
        lv_obj_align(l, LV_ALIGN_TOP_LEFT, 0, 0);
        st_value[i] = lv_label_create(t);
        lv_label_set_text(st_value[i], "---");
        lv_obj_set_style_text_font(st_value[i], L.st_value_font, 0);
        lv_obj_set_style_text_color(st_value[i], COL_TEXT, 0);
        lv_obj_set_width(st_value[i], tile_w - 16);
        lv_label_set_long_mode(st_value[i], LV_LABEL_LONG_CLIP);
        lv_obj_align(st_value[i], LV_ALIGN_BOTTOM_LEFT, 0, 0);
    }
    // Heatmap: st_weeks columns × 7 rows, centred, painted by heat_draw_cb.
    const int cols = L.st_weeks;
    const int hm_w = cols * L.st_cell + (cols - 1) * L.st_cell_gap;
    const int hm_h = 7 * L.st_cell + 6 * L.st_cell_gap;
    st_hm = make_group_sized(page, (L.scr_w - hm_w) / 2, L.st_hm_y, hm_w, hm_h);
    lv_obj_clear_flag(st_hm, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(st_hm, heat_draw_cb, LV_EVENT_DRAW_MAIN, NULL);
    render_stats_page();
}

// ---- Level page dots: visible only around a horizontal gesture ----
static void level_dots_opa_exec(void* var, int32_t v) {
    (void)var;
    for (int i = 0; i < LEVEL_PAGES; i++)
        if (level_dots[i]) lv_obj_set_style_opa(level_dots[i], (lv_opa_t)v, 0);
}
static int32_t level_dots_opa = 0;

static void update_level_dots(void) {
    for (int i = 0; i < LEVEL_PAGES; i++)
        if (level_dots[i]) lv_obj_set_style_bg_color(level_dots[i], i == level_page ? COL_TEXT : COL_DOT_OFF, 0);
}

static void level_dots_show(void) {
    lv_anim_delete(&level_dots_opa, level_dots_opa_exec);
    level_dots_opa = 255;
    level_dots_opa_exec(nullptr, 255);
    level_dots_hide_ms = lv_tick_get() + 900;
}

static void level_dots_fade(void) {
    lv_anim_t a;
    lv_anim_init(&a);
    lv_anim_set_var(&a, &level_dots_opa);
    lv_anim_set_values(&a, level_dots_opa, 0);
    lv_anim_set_duration(&a, 400);
    lv_anim_set_exec_cb(&a, level_dots_opa_exec);
    lv_anim_set_path_cb(&a, lv_anim_path_ease_in);
    lv_anim_start(&a);
}

static void init_usage_screen(void) {
    // The level: two pages side by side, plus the ticker and dots.
    level = make_body_group(body);
    page_stats = make_body_group(level);
    usage_container = make_body_group(level);
    level_pages[LEVEL_STATS] = page_stats;
    level_pages[LEVEL_USAGE] = usage_container;
    for (int i = 0; i < LEVEL_PAGES; i++) {
        lv_obj_add_event_cb(level_pages[i], global_click_cb, LV_EVENT_CLICKED, NULL);
    }

    // Usage panels (shown when connected) live in a transparent body-size group
    // so they can be toggled against the pairing hint as one unit.
    usage_group = make_body_group(usage_container);
    lv_obj_add_flag(usage_group, LV_OBJ_FLAG_EVENT_BUBBLE);

    panel_session = make_usage_panel(usage_group, 0, "Current",
                     &lbl_session_pct, &lbl_session_label,
                     &bar_session, &lbl_session_reset);

    // Enterprise-only overlays inside panel_session — hidden until enterprise data arrives
    lbl_session_pct_sym = lv_label_create(panel_session);
    lv_label_set_text(lbl_session_pct_sym, "%");
    lv_obj_set_style_text_font(lbl_session_pct_sym, L.reset_font, 0);
    lv_obj_set_style_text_color(lbl_session_pct_sym, COL_TEXT, 0);
    lv_obj_add_flag(lbl_session_pct_sym, LV_OBJ_FLAG_HIDDEN);

    lbl_spending_desc = lv_label_create(panel_session);
    lv_label_set_text(lbl_spending_desc, "of your monthly budget");
    lv_obj_set_style_text_font(lbl_spending_desc, L.reset_font, 0);
    lv_obj_set_style_text_color(lbl_spending_desc, COL_DIM, 0);
    lv_obj_set_pos(lbl_spending_desc, 0, L.usage_reset_y);
    lv_obj_add_flag(lbl_spending_desc, LV_OBJ_FLAG_HIDDEN);

    lbl_spending_status = lv_label_create(panel_session);
    lv_label_set_text(lbl_spending_status, "");
    lv_obj_set_style_text_font(lbl_spending_status, L.pace_font, 0);
    lv_obj_set_pos(lbl_spending_status, 0, L.usage_reset_y + 20);
    lv_obj_add_flag(lbl_spending_status, LV_OBJ_FLAG_HIDDEN);

    panel_weekly = make_usage_panel(usage_group,
                     L.usage_panel_h + L.usage_panel_gap, "Weekly",
                     &lbl_weekly_pct, &lbl_weekly_label,
                     &bar_weekly, &lbl_weekly_reset);
    // Recolor enabled so enterprise period box can color pace and reset separately
    lv_label_set_recolor(lbl_weekly_reset, true);

    // Face indicator dots on the reset row (hidden on single-face plans) and
    // the tap-to-flip handler. Bubbling to the splash toggle is switched off
    // in ui_update() only while faces exist.
    for (int i = 0; i <= MAX_SCOPED_WEEKLY; i++) {
        face_dots[i] = make_dot(panel_weekly);
        lv_obj_add_flag(face_dots[i], LV_OBJ_FLAG_HIDDEN);
    }
    lv_obj_add_event_cb(panel_weekly, weekly_click_cb, LV_EVENT_CLICKED, NULL);

    build_pair_group(usage_container);
    build_idle_group(usage_container);

    build_stats_page(page_stats);

    // Status line — on every level page. Driven by ui_tick_anim(). Fixed width
    // so a long companion label is cut with an ellipsis instead of overflowing.
    lbl_anim = lv_label_create(level);
    lv_label_set_text(lbl_anim, "");
    lv_obj_set_style_text_font(lbl_anim, L.anim_font, 0);
    lv_obj_set_style_text_color(lbl_anim, COL_ACCENT, 0);
    lv_obj_set_style_text_align(lbl_anim, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_size(lbl_anim, L.content_w, lv_font_get_line_height(L.anim_font) + 2);
    lv_label_set_long_mode(lbl_anim, LV_LABEL_LONG_DOT);
    lv_obj_align(lbl_anim, LV_ALIGN_BOTTOM_MID, 0, L.anim_y);

    // The machine a remote session runs on — a small dim caption right above
    // the status line, only while such a session holds it (see ui_tick_anim).
    // The line itself stays for what Claude is doing: at 32 px mono it holds
    // about 22 characters, a host prefix would leave nothing of the label.
    lbl_host = lv_label_create(level);
    lv_label_set_text(lbl_host, "");
    lv_obj_set_style_text_font(lbl_host, L.host_font, 0);
    lv_obj_set_style_text_color(lbl_host, COL_DIM, 0);
    lv_obj_set_style_text_align(lbl_host, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_size(lbl_host, L.content_w, lv_font_get_line_height(L.host_font) + 2);
    lv_label_set_long_mode(lbl_host, LV_LABEL_LONG_DOT);
    lv_obj_align(lbl_host, LV_ALIGN_BOTTOM_MID, 0, L.anim_y - lv_font_get_line_height(L.anim_font) - 1);
    lv_obj_add_flag(lbl_host, LV_OBJ_FLAG_HIDDEN);

    ready_dot = make_dot(level);
    lv_obj_set_size(ready_dot, L.dot_size + 4, L.dot_size + 4);
    lv_obj_set_style_bg_color(ready_dot, COL_GREEN, 0);
    lv_obj_add_flag(ready_dot, LV_OBJ_FLAG_HIDDEN);

    // Page dots of the level, hidden until a horizontal gesture reveals them.
    const int total_w = LEVEL_PAGES * L.dot_size + (LEVEL_PAGES - 1) * L.dot_gap;
    for (int i = 0; i < LEVEL_PAGES; i++) {
        level_dots[i] = make_dot(level);
        lv_obj_set_pos(level_dots[i], (L.scr_w - total_w) / 2 + i * (L.dot_size + L.dot_gap), L.level_dots_y);
        lv_obj_set_style_opa(level_dots[i], LV_OPA_TRANSP, 0);
    }
    update_level_dots();
    show_level_page(LEVEL_USAGE);
}

// ======== Settings Screen ========

// A tile: the card surface a control sits on. Clickable tiles darken while
// pressed (toggles, the pairing button); control tiles (sliders, the clock
// picker) leave the taps to their controls.
static lv_obj_t* make_tile(lv_obj_t* parent, int x, int y, int w, int h, bool clickable) {
    lv_obj_t* t = lv_obj_create(parent);
    lv_obj_set_pos(t, x, y);
    lv_obj_set_size(t, w, h);
    lv_obj_set_style_bg_color(t, COL_PANEL, 0);
    lv_obj_set_style_bg_opa(t, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(t, L.tile_radius, 0);
    lv_obj_set_style_border_width(t, 0, 0);
    lv_obj_set_style_pad_left(t, L.tile_pad_x, 0);
    lv_obj_set_style_pad_right(t, L.tile_pad_x, 0);
    lv_obj_set_style_pad_top(t, L.tile_pad_y, 0);
    lv_obj_set_style_pad_bottom(t, L.tile_pad_y, 0);
    lv_obj_clear_flag(t, LV_OBJ_FLAG_SCROLLABLE);
    if (clickable) lv_obj_set_style_bg_color(t, COL_PRESSED, LV_STATE_PRESSED);
    else           lv_obj_clear_flag(t, LV_OBJ_FLAG_CLICKABLE);
    return t;
}

static lv_obj_t* tile_label(lv_obj_t* tile, const char* text) {
    lv_obj_t* l = lv_label_create(tile);
    lv_label_set_text(l, text);
    lv_obj_set_style_text_font(l, L.tile_label_font, 0);
    lv_obj_set_style_text_color(l, COL_DIM, 0);
    lv_obj_align(l, LV_ALIGN_TOP_LEFT, 0, 0);
    return l;
}

// Right-aligned control value on the tile's top row.
static lv_obj_t* tile_value_label(lv_obj_t* tile) {
    lv_obj_t* l = lv_label_create(tile);
    lv_label_set_text(l, "");
    lv_obj_set_style_text_font(l, L.ctrl_font, 0);
    lv_obj_set_style_text_color(l, COL_TEXT, 0);
    lv_obj_align(l, LV_ALIGN_TOP_RIGHT, 0, -2);
    return l;
}

// --- Toggle: a track with a gliding knob; the whole tile is the button ---
static Toggle make_toggle(lv_obj_t* tile) {
    Toggle t;
    t.track = lv_obj_create(tile);
    lv_obj_set_size(t.track, L.toggle_w, L.toggle_h);
    lv_obj_align(t.track, LV_ALIGN_BOTTOM_LEFT, 0, 0);
    lv_obj_set_style_radius(t.track, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_border_width(t.track, 0, 0);
    lv_obj_set_style_pad_all(t.track, 0, 0);
    lv_obj_set_style_bg_color(t.track, COL_BAR_BG, 0);
    lv_obj_set_style_bg_opa(t.track, LV_OPA_COVER, 0);
    lv_obj_clear_flag(t.track, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_clear_flag(t.track, LV_OBJ_FLAG_SCROLLABLE);

    t.knob = lv_obj_create(t.track);
    lv_obj_set_size(t.knob, L.toggle_knob, L.toggle_knob);
    lv_obj_set_pos(t.knob, (L.toggle_h - L.toggle_knob) / 2, (L.toggle_h - L.toggle_knob) / 2);
    lv_obj_set_style_radius(t.knob, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_border_width(t.knob, 0, 0);
    lv_obj_set_style_pad_all(t.knob, 0, 0);
    lv_obj_set_style_bg_color(t.knob, COL_DIM, 0);
    lv_obj_set_style_bg_opa(t.knob, LV_OPA_COVER, 0);
    lv_obj_clear_flag(t.knob, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_clear_flag(t.knob, LV_OBJ_FLAG_SCROLLABLE);

    t.value = lv_label_create(tile);
    lv_label_set_text(t.value, "");
    lv_obj_set_style_text_font(t.value, L.ctrl_font, 0);
    lv_obj_set_style_text_color(t.value, COL_TEXT, 0);
    lv_obj_align_to(t.value, t.track, LV_ALIGN_OUT_RIGHT_MID, 12, 0);
    return t;
}

static void set_toggle(Toggle& t, bool on, const char* on_text, const char* off_text, bool animate) {
    const int inset = (L.toggle_h - L.toggle_knob) / 2;
    const int x = on ? L.toggle_w - L.toggle_knob - inset : inset;
    if (animate) animate_x(t.knob, x, ANIM_CTRL_MS, NULL);
    else         lv_obj_set_x(t.knob, x);
    lv_obj_set_style_bg_color(t.track, on ? COL_ACCENT : COL_BAR_BG, 0);
    lv_obj_set_style_bg_color(t.knob,  on ? COL_TEXT   : COL_DIM, 0);
    lv_label_set_text(t.value, on ? on_text : off_text);
    lv_obj_set_style_text_color(t.value, on ? COL_TEXT : COL_DIM, 0);
    lv_obj_align_to(t.value, t.track, LV_ALIGN_OUT_RIGHT_MID, 12, 0);
}

// --- Slider: LVGL slider restyled — thin track, accent fill, big round knob,
//     fat-finger hit area, and drags never bubble up as page swipes ---
static lv_obj_t* make_slider(lv_obj_t* tile, int y, int w, int32_t min, int32_t max) {
    lv_obj_t* s = lv_slider_create(tile);
    lv_obj_set_pos(s, 0, y);
    lv_obj_set_size(s, w, L.slider_h);
    lv_slider_set_range(s, min, max);
    lv_obj_set_style_bg_color(s, COL_BAR_BG, LV_PART_MAIN);
    lv_obj_set_style_bg_opa(s, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_radius(s, LV_RADIUS_CIRCLE, LV_PART_MAIN);
    lv_obj_set_style_bg_color(s, COL_ACCENT, LV_PART_INDICATOR);
    lv_obj_set_style_bg_opa(s, LV_OPA_COVER, LV_PART_INDICATOR);
    lv_obj_set_style_radius(s, LV_RADIUS_CIRCLE, LV_PART_INDICATOR);
    lv_obj_set_style_bg_color(s, COL_TEXT, LV_PART_KNOB);
    lv_obj_set_style_bg_opa(s, LV_OPA_COVER, LV_PART_KNOB);
    lv_obj_set_style_radius(s, LV_RADIUS_CIRCLE, LV_PART_KNOB);
    lv_obj_set_style_pad_all(s, (L.slider_knob - L.slider_h) / 2, LV_PART_KNOB);
    lv_obj_set_ext_click_area(s, L.slider_knob);
    lv_obj_clear_flag(s, LV_OBJ_FLAG_GESTURE_BUBBLE);
    return s;
}

// --- Clock picker: four segments with one sliding accent highlight ---
static void seg_click_cb(lv_event_t* e) {
    if (click_guarded()) return;
    const int mode = (int)(intptr_t)lv_event_get_user_data(e);
    settings_set_clock((uint8_t)mode);
    clock_last_min = -1;
    refresh_settings_controls(true);
}

static void render_clock_preview(void) {
    if (!clock_preview) return;
    const uint8_t mode = settings_get().clock;
    char buf[16];
    if (mode == CLOCK_OFF) {
        strlcpy(buf, "Usage", sizeof(buf));
    } else if (clock_base_epoch == 0) {
        strlcpy(buf, "--:--", sizeof(buf));
    } else {
        const int fmt = (mode == CLOCK_12H) ? 12 : (mode == CLOCK_24H) ? 24 : clock_fmt;
        time_t cur = (time_t)(clock_base_epoch + (lv_tick_get() - clock_base_ms) / 1000);
        struct tm tmv;
        gmtime_r(&cur, &tmv);
        if (fmt == 12) {
            int h12 = tmv.tm_hour % 12;
            if (h12 == 0) h12 = 12;
            snprintf(buf, sizeof(buf), "%d:%02d %s", h12, tmv.tm_min, tmv.tm_hour < 12 ? "AM" : "PM");
        } else {
            snprintf(buf, sizeof(buf), "%02d:%02d", tmv.tm_hour, tmv.tm_min);
        }
    }
    lv_label_set_text(clock_preview, buf);
    lv_obj_set_style_text_color(clock_preview, mode == CLOCK_OFF ? COL_DIM : COL_TEXT, 0);
}

static void set_clock_segment(int mode, bool animate) {
    if (mode < 0 || mode >= CLOCK_MODE_COUNT) mode = 0;
    if (animate) animate_x(seg_highlight, seg_x[mode], ANIM_SEG_MS, NULL);
    else         lv_obj_set_x(seg_highlight, seg_x[mode]);
    for (int i = 0; i < CLOCK_MODE_COUNT; i++)
        lv_obj_set_style_text_color(seg_labels[i], i == mode ? COL_TEXT : COL_DIM, 0);
}

// --- Slider + toggle + button handlers ---
static void brightness_slider_cb(lv_event_t* e) {
    const lv_event_code_t code = lv_event_get_code(e);
    const int pct = (int)lv_slider_get_value(sl_brightness);
    if (code == LV_EVENT_VALUE_CHANGED) {
        brightness_preview_level(brightness_pct_to_level(pct));   // live while dragging
        lv_label_set_text_fmt(lbl_brightness, "%d%%", pct);
    } else if (code == LV_EVENT_RELEASED) {
        brightness_set_level(brightness_pct_to_level(pct));       // persist once
    }
}

static void sleep_slider_cb(lv_event_t* e) {
    const lv_event_code_t code = lv_event_get_code(e);
    const int idx = (int)lv_slider_get_value(sl_sleep);
    if (code == LV_EVENT_VALUE_CHANGED) {
        lv_label_set_text(lbl_sleep, settings_sleep_label((uint8_t)idx));
        lv_obj_set_style_text_color(lbl_sleep, idx == SLEEP_NEVER ? COL_DIM : COL_TEXT, 0);
    } else if (code == LV_EVENT_RELEASED) {
        settings_set_sleep((uint8_t)idx);
    }
}

enum ToggleId { TG_BATTERY, TG_MASCOT, TG_STATUS, TG_GLOW };
static void toggle_tile_cb(lv_event_t* e) {
    if (click_guarded()) return;
    const Settings& s = settings_get();
    switch ((int)(intptr_t)lv_event_get_user_data(e)) {
    case TG_BATTERY: settings_set_show_battery(!s.show_battery); break;
    case TG_MASCOT:  settings_set_show_mascot(!s.show_mascot); break;
    case TG_STATUS:  settings_set_show_status(!s.show_status); break;
    case TG_GLOW:    settings_set_alert_glow(!s.alert_glow); break;
    default: break;
    }
    refresh_settings_controls(true);
    apply_header_visibility();
}

// Two taps within PAIRING_CONFIRM_MS: forget the bonded host and re-advertise
// (same effect as the hold-power gesture).
static void pairing_tile_cb(lv_event_t* e) {
    (void)e;
    if (click_guarded()) return;
    if (pairing_cleared_ms) return;
    if (pairing_confirm_ms) {
#ifdef CLAWD_LINK_WIFI
        pairing_confirm_ms = 0;
        link_wifi_forget_and_restart();        // reboots into the setup hotspot
        return;
#else
        ble_clear_bonds();
        pairing_confirm_ms = 0;
        pairing_cleared_ms = lv_tick_get();
#endif
    } else {
        pairing_confirm_ms = lv_tick_get();
    }
    refresh_settings_controls(false);
}

static void render_pairing_button(void) {
    if (!pair_button) return;
    if (pairing_cleared_ms) {
        lv_label_set_text(pair_button, "Cleared");
        lv_obj_set_style_bg_color(pair_button, COL_BAR_BG, 0);
        lv_obj_set_style_text_color(pair_button, COL_DIM, 0);
    } else if (pairing_confirm_ms) {
        lv_label_set_text(pair_button, "Confirm?");
        lv_obj_set_style_bg_color(pair_button, COL_ACCENT, 0);
        lv_obj_set_style_text_color(pair_button, COL_TEXT, 0);
    } else {
        lv_label_set_text(pair_button, PAIR_BUTTON_IDLE);
        lv_obj_set_style_bg_color(pair_button, COL_BAR_BG, 0);
        lv_obj_set_style_text_color(pair_button, COL_TEXT, 0);
    }
}

// --- Weekly card page ---
static void face_seg_click_cb(lv_event_t* e) {
    if (click_guarded()) return;
    const int face = (int)(intptr_t)lv_event_get_user_data(e);
    if (face >= face_seg_count) return;
    settings_set_face_default((uint8_t)face);
    weekly_face = face;
    if (cached_scoped_count > 0) render_weekly_face(false);
    schedule_face_flip(settings_face_flip_ms());
    refresh_settings_controls(true);
}

static void flip_slider_cb(lv_event_t* e) {
    const lv_event_code_t code = lv_event_get_code(e);
    const int idx = (int)lv_slider_get_value(sl_flip);
    if (code == LV_EVENT_VALUE_CHANGED) {
        lv_label_set_text(lbl_flip, settings_face_flip_label((uint8_t)idx));
        lv_obj_set_style_text_color(lbl_flip, idx == FLIP_OFF ? COL_DIM : COL_TEXT, 0);
    } else if (code == LV_EVENT_RELEASED) {
        settings_set_face_flip((uint8_t)idx);
        weekly_face = default_face_clamped();
        if (cached_scoped_count > 0) render_weekly_face(false);
        schedule_face_flip(settings_face_flip_ms());
    }
}

// Lay the picker's segments out for the faces the plan actually has (Weekly +
// each scoped limit, labelled with the API's own names) and move the highlight.
static void layout_face_segments(bool animate) {
    if (!face_seg_tile) return;
    face_seg_count = 1 + cached_scoped_count;
    const int inner_w = L.content_w - 2 * L.tile_pad_x;
    const int seg_w = (inner_w - (face_seg_count - 1) * L.seg_gap) / face_seg_count;
    const int seg_y = L.wide_tile_h - 2 * L.tile_pad_y - L.seg_h;
    for (int i = 0; i < FACE_SEG_MAX; i++) {
        if (i < face_seg_count) {
            face_seg_x[i] = i * (seg_w + L.seg_gap);
            lv_obj_set_pos(face_seg_obj[i], face_seg_x[i], seg_y);
            lv_obj_set_size(face_seg_obj[i], seg_w, L.seg_h);
            lv_label_set_text(face_seg_lbl[i], i == 0 ? "Weekly" : cached_scoped[i - 1].name);
            lv_obj_clear_flag(face_seg_obj[i], LV_OBJ_FLAG_HIDDEN);
        } else {
            lv_obj_add_flag(face_seg_obj[i], LV_OBJ_FLAG_HIDDEN);
        }
    }
    const int sel = default_face_clamped();
    lv_obj_set_size(face_seg_hl, seg_w, L.seg_h);
    lv_obj_set_y(face_seg_hl, seg_y);
    if (animate) animate_x(face_seg_hl, face_seg_x[sel], ANIM_SEG_MS, NULL);
    else         lv_obj_set_x(face_seg_hl, face_seg_x[sel]);
    for (int i = 0; i < face_seg_count; i++)
        lv_obj_set_style_text_color(face_seg_lbl[i], i == sel ? COL_TEXT : COL_DIM, 0);

    // Preview: the default face's current reading.
    if (face_preview) {
        if (!data_received) {
            lv_label_set_text(face_preview, "---");
            lv_obj_set_style_text_color(face_preview, COL_DIM, 0);
        } else {
            const float pct = sel == 0 ? cached_weekly_pct : cached_scoped[sel - 1].pct;
            lv_label_set_text_fmt(face_preview, "%d%%", (int)(pct + 0.5f));
            lv_obj_set_style_text_color(face_preview, COL_TEXT, 0);
        }
    }
}

static void build_face_page(lv_obj_t* page) {
    const int inner_w = L.content_w - 2 * L.tile_pad_x;
    face_seg_tile = make_tile(page, L.margin, 0, L.content_w, L.wide_tile_h, false);
    tile_label(face_seg_tile, "Default face");
    face_preview = tile_value_label(face_seg_tile);

    face_seg_hl = lv_obj_create(face_seg_tile);
    lv_obj_set_style_radius(face_seg_hl, L.seg_radius, 0);
    lv_obj_set_style_border_width(face_seg_hl, 0, 0);
    lv_obj_set_style_bg_color(face_seg_hl, COL_ACCENT, 0);
    lv_obj_set_style_bg_opa(face_seg_hl, LV_OPA_COVER, 0);
    lv_obj_clear_flag(face_seg_hl, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_clear_flag(face_seg_hl, LV_OBJ_FLAG_SCROLLABLE);
    for (int i = 0; i < FACE_SEG_MAX; i++) {
        lv_obj_t* seg = lv_obj_create(face_seg_tile);
        lv_obj_set_style_bg_opa(seg, LV_OPA_TRANSP, 0);
        lv_obj_set_style_border_width(seg, 0, 0);
        lv_obj_set_style_pad_all(seg, 0, 0);
        lv_obj_clear_flag(seg, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_add_event_cb(seg, face_seg_click_cb, LV_EVENT_CLICKED, (void*)(intptr_t)i);
        face_seg_obj[i] = seg;
        face_seg_lbl[i] = lv_label_create(seg);
        lv_label_set_text(face_seg_lbl[i], "");
        lv_obj_set_style_text_font(face_seg_lbl[i], L.ctrl_font, 0);
        lv_obj_center(face_seg_lbl[i]);
    }

    const int slider_y = L.slider_tile_h - 2 * L.tile_pad_y - L.slider_knob / 2 - L.slider_h / 2;
    lv_obj_t* tf = make_tile(page, L.margin, L.wide_tile_h + L.tile_gap, L.content_w, L.slider_tile_h, false);
    tile_label(tf, "Flip every");
    lbl_flip = tile_value_label(tf);
    sl_flip = make_slider(tf, slider_y, inner_w, 0, FLIP_MODE_COUNT - 1);
    lv_obj_add_event_cb(sl_flip, flip_slider_cb, LV_EVENT_VALUE_CHANGED, NULL);
    lv_obj_add_event_cb(sl_flip, flip_slider_cb, LV_EVENT_RELEASED, NULL);

    layout_face_segments(false);
}

// --- Companion page: chime picker + Preview, Auto-switch / Glow toggles ---
static void chime_seg_click_cb(lv_event_t* e) {
    if (click_guarded()) return;
    settings_set_alert_chime((uint8_t)(intptr_t)lv_event_get_user_data(e));
    refresh_settings_controls(true);
}

static void preview_click_cb(lv_event_t* e) {
    (void)e;
    if (click_guarded()) return;
    ui_preview_alert();
}

static void set_chime_segment(int mode, bool animate) {
    if (!chime_hl) return;
    if (mode < 0 || mode >= CHIME_MODE_COUNT) mode = 0;
    if (animate) animate_x(chime_hl, chime_x[mode], ANIM_SEG_MS, NULL);
    else         lv_obj_set_x(chime_hl, chime_x[mode]);
    for (int i = 0; i < CHIME_MODE_COUNT; i++)
        lv_obj_set_style_text_color(chime_labels[i], i == mode ? COL_TEXT : COL_DIM, 0);
}

// Volume: the codec follows the finger (so you hear it as you slide), the
// value persists on release and the needs-you chime auditions the new level.
static void volume_slider_cb(lv_event_t* e) {
    const lv_event_code_t code = lv_event_get_code(e);
    const int v = (int)lv_slider_get_value(sl_volume);
    if (code == LV_EVENT_VALUE_CHANGED) {
        lv_label_set_text_fmt(lbl_volume, "%d%%", v);
        sound_hal_set_volume(v);
    } else if (code == LV_EVENT_RELEASED) {
        settings_set_volume((uint8_t)v);
        sound_hal_set_volume(v);
        sound_hal_play_alert(0);
    }
}

static void build_alerts_page(lv_obj_t* page) {
    const int inner_w = L.content_w - 2 * L.tile_pad_x;

    lv_obj_t* tile = make_tile(page, L.margin, 0, L.content_w, L.wide_tile_h, false);
    tile_label(tile, "Chime");
    preview_button = make_pill_styled(tile, "Preview", L.ctrl_font, 18, 6);
    lv_obj_align(preview_button, LV_ALIGN_TOP_RIGHT, 0, -6);
    lv_obj_add_flag(preview_button, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_style_bg_color(preview_button, COL_PRESSED, LV_STATE_PRESSED);
    lv_obj_add_event_cb(preview_button, preview_click_cb, LV_EVENT_CLICKED, NULL);

    const int seg_w = (inner_w - (CHIME_MODE_COUNT - 1) * L.seg_gap) / CHIME_MODE_COUNT;
    const int seg_y = L.wide_tile_h - 2 * L.tile_pad_y - L.seg_h;
    chime_hl = lv_obj_create(tile);
    lv_obj_set_size(chime_hl, seg_w, L.seg_h);
    lv_obj_set_style_radius(chime_hl, L.seg_radius, 0);
    lv_obj_set_style_border_width(chime_hl, 0, 0);
    lv_obj_set_style_bg_color(chime_hl, COL_ACCENT, 0);
    lv_obj_set_style_bg_opa(chime_hl, LV_OPA_COVER, 0);
    lv_obj_clear_flag(chime_hl, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_clear_flag(chime_hl, LV_OBJ_FLAG_SCROLLABLE);
    for (int i = 0; i < CHIME_MODE_COUNT; i++) {
        chime_x[i] = i * (seg_w + L.seg_gap);
        lv_obj_t* seg = lv_obj_create(tile);
        lv_obj_set_pos(seg, chime_x[i], seg_y);
        lv_obj_set_size(seg, seg_w, L.seg_h);
        lv_obj_set_style_bg_opa(seg, LV_OPA_TRANSP, 0);
        lv_obj_set_style_border_width(seg, 0, 0);
        lv_obj_set_style_pad_all(seg, 0, 0);
        lv_obj_clear_flag(seg, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_add_event_cb(seg, chime_seg_click_cb, LV_EVENT_CLICKED, (void*)(intptr_t)i);
        chime_labels[i] = lv_label_create(seg);
        lv_label_set_text(chime_labels[i], settings_alert_chime_label((uint8_t)i));
        lv_obj_set_style_text_font(chime_labels[i], L.ctrl_font, 0);
        lv_obj_center(chime_labels[i]);
    }
    lv_obj_set_pos(chime_hl, chime_x[0], seg_y);

    // Second row: the Volume slider with the Glow toggle beside it.
    const int slider_y = L.slider_tile_h - 2 * L.tile_pad_y - L.slider_knob / 2 - L.slider_h / 2;
    const int glow_w = L.content_w * 3 / 8;
    const int vol_w = L.content_w - L.tile_gap - glow_w;
    const int row_y = L.wide_tile_h + L.tile_gap;
    lv_obj_t* tv = make_tile(page, L.margin, row_y, vol_w, L.slider_tile_h, false);
    tile_label(tv, "Volume");
    lbl_volume = tile_value_label(tv);
    sl_volume = make_slider(tv, slider_y, vol_w - 2 * L.tile_pad_x, 0, 100);
    lv_obj_add_event_cb(sl_volume, volume_slider_cb, LV_EVENT_VALUE_CHANGED, NULL);
    lv_obj_add_event_cb(sl_volume, volume_slider_cb, LV_EVENT_RELEASED, NULL);

    lv_obj_t* tg = make_tile(page, L.margin + vol_w + L.tile_gap, row_y, glow_w, L.slider_tile_h, true);
    tile_label(tg, "Glow");
    tg_glow = make_toggle(tg);
    lv_obj_add_event_cb(tg, toggle_tile_cb, LV_EVENT_CLICKED, (void*)(intptr_t)TG_GLOW);
}

static void build_about_page(lv_obj_t* page) {
    const int rows_h = ABOUT_COUNT * L.about_row_h;
    lv_obj_t* panel = make_panel(page, L.margin, 0, L.content_w,
                                 rows_h + 2 * L.panel_pad_y);
    lv_obj_clear_flag(panel, LV_OBJ_FLAG_EVENT_BUBBLE);
    lv_obj_clear_flag(panel, LV_OBJ_FLAG_CLICKABLE);
    for (int i = 0; i < ABOUT_COUNT; i++) {
        const int y = i * L.about_row_h;
        lv_obj_t* key = lv_label_create(panel);
        lv_label_set_text(key, ABOUT_KEYS[i]);
        lv_obj_set_style_text_font(key, L.about_key_font, 0);
        lv_obj_set_style_text_color(key, COL_DIM, 0);
        lv_obj_set_pos(key, 0, y);

        about_value[i] = lv_label_create(panel);
        lv_label_set_text(about_value[i], "");
        lv_obj_set_style_text_font(about_value[i], L.about_val_font, 0);
        lv_obj_set_style_text_color(about_value[i], COL_TEXT, 0);
        lv_obj_align(about_value[i], LV_ALIGN_TOP_RIGHT, 0, y);
    }
    lv_obj_t* credit = lv_label_create(page);
    lv_label_set_text(credit, "github.com/TheOriUHD/Clawdmeter");
    lv_obj_set_style_text_font(credit, L.about_hint_font, 0);
    lv_obj_set_style_text_color(credit, COL_DIM, 0);
    lv_obj_align(credit, LV_ALIGN_BOTTOM_MID, 0, L.about_hint_y);
}

static void build_settings_screen(void) {
    settings_container = make_body_group(body);
    for (int pg = 0; pg < SET_PAGES; pg++) settings_pages[pg] = make_body_group(settings_container);

    const int half_w = (L.content_w - L.tile_gap) / 2;
    const int inner_w = L.content_w - 2 * L.tile_pad_x;
    const int top = 0;                                 // body-relative
    const int bottom = L.tiles_bottom - L.content_y;

    // ---- Page 0: Clock picker (wide) + Battery icon / Mascot toggles ----
    {
        lv_obj_t* pg = settings_pages[0];
        lv_obj_t* tile = make_tile(pg, L.margin, top, L.content_w, L.wide_tile_h, false);
        tile_label(tile, "Clock");
        clock_preview = tile_value_label(tile);

        const int seg_w = (inner_w - (CLOCK_MODE_COUNT - 1) * L.seg_gap) / CLOCK_MODE_COUNT;
        const int seg_y = L.wide_tile_h - 2 * L.tile_pad_y - L.seg_h;
        seg_highlight = lv_obj_create(tile);
        lv_obj_set_size(seg_highlight, seg_w, L.seg_h);
        lv_obj_set_style_radius(seg_highlight, L.seg_radius, 0);
        lv_obj_set_style_border_width(seg_highlight, 0, 0);
        lv_obj_set_style_bg_color(seg_highlight, COL_ACCENT, 0);
        lv_obj_set_style_bg_opa(seg_highlight, LV_OPA_COVER, 0);
        lv_obj_clear_flag(seg_highlight, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_clear_flag(seg_highlight, LV_OBJ_FLAG_SCROLLABLE);
        static const char* const SEG_TEXT[CLOCK_MODE_COUNT] = { "Off", "Auto", "12h", "24h" };
        for (int i = 0; i < CLOCK_MODE_COUNT; i++) {
            seg_x[i] = i * (seg_w + L.seg_gap);
            lv_obj_t* seg = lv_obj_create(tile);
            lv_obj_set_pos(seg, seg_x[i], seg_y);
            lv_obj_set_size(seg, seg_w, L.seg_h);
            lv_obj_set_style_bg_opa(seg, LV_OPA_TRANSP, 0);
            lv_obj_set_style_border_width(seg, 0, 0);
            lv_obj_set_style_pad_all(seg, 0, 0);
            lv_obj_clear_flag(seg, LV_OBJ_FLAG_SCROLLABLE);
            lv_obj_add_event_cb(seg, seg_click_cb, LV_EVENT_CLICKED, (void*)(intptr_t)i);
            seg_labels[i] = lv_label_create(seg);
            lv_label_set_text(seg_labels[i], SEG_TEXT[i]);
            lv_obj_set_style_text_font(seg_labels[i], L.ctrl_font, 0);
            lv_obj_center(seg_labels[i]);
        }
        lv_obj_set_pos(seg_highlight, seg_x[0], seg_y);

        const int row_y = top + L.wide_tile_h + L.tile_gap;
        const int row_h = bottom - row_y;
        lv_obj_t* t1 = make_tile(pg, L.margin, row_y, half_w, row_h, true);
        tile_label(t1, "Battery icon");
        tg_battery = make_toggle(t1);
        lv_obj_add_event_cb(t1, toggle_tile_cb, LV_EVENT_CLICKED, (void*)(intptr_t)TG_BATTERY);

        lv_obj_t* t2 = make_tile(pg, L.margin + half_w + L.tile_gap, row_y, half_w, row_h, true);
        tile_label(t2, "Mascot");
        tg_mascot = make_toggle(t2);
        lv_obj_add_event_cb(t2, toggle_tile_cb, LV_EVENT_CLICKED, (void*)(intptr_t)TG_MASCOT);
    }

    // ---- Page 1: Brightness + Sleep sliders (wide), Status line toggle + Pairing ----
    {
        lv_obj_t* pg = settings_pages[1];
        const int slider_y = L.slider_tile_h - 2 * L.tile_pad_y - L.slider_knob / 2 - L.slider_h / 2;

        lv_obj_t* tb = make_tile(pg, L.margin, top, L.content_w, L.slider_tile_h, false);
        tile_label(tb, "Brightness");
        lbl_brightness = tile_value_label(tb);
        sl_brightness = make_slider(tb, slider_y, inner_w, 5, 100);
        lv_obj_add_event_cb(sl_brightness, brightness_slider_cb, LV_EVENT_VALUE_CHANGED, NULL);
        lv_obj_add_event_cb(sl_brightness, brightness_slider_cb, LV_EVENT_RELEASED, NULL);

        const int ts_y = top + L.slider_tile_h + L.tile_gap;
        lv_obj_t* ts = make_tile(pg, L.margin, ts_y, L.content_w, L.slider_tile_h, false);
        tile_label(ts, "Sleep after");
        lbl_sleep = tile_value_label(ts);
        sl_sleep = make_slider(ts, slider_y, inner_w, 0, SLEEP_MODE_COUNT - 1);
        lv_obj_add_event_cb(sl_sleep, sleep_slider_cb, LV_EVENT_VALUE_CHANGED, NULL);
        lv_obj_add_event_cb(sl_sleep, sleep_slider_cb, LV_EVENT_RELEASED, NULL);

        const int row_y = ts_y + L.slider_tile_h + L.tile_gap;
        const int row_h = bottom - row_y;
        lv_obj_t* t3 = make_tile(pg, L.margin, row_y, half_w, row_h, true);
        tile_label(t3, "Status line");
        tg_status = make_toggle(t3);
        lv_obj_add_event_cb(t3, toggle_tile_cb, LV_EVENT_CLICKED, (void*)(intptr_t)TG_STATUS);

        lv_obj_t* t4 = make_tile(pg, L.margin + half_w + L.tile_gap, row_y, half_w, row_h, true);
        tile_label(t4, PAIR_TILE_LABEL);
        pair_button = make_pill_styled(t4, PAIR_BUTTON_IDLE, L.ctrl_font, 18, 8);
        lv_obj_align(pair_button, LV_ALIGN_BOTTOM_LEFT, 0, 0);
        lv_obj_add_event_cb(t4, pairing_tile_cb, LV_EVENT_CLICKED, NULL);
    }

    // ---- Page 2: Weekly card (default face + flip interval) ----
    build_face_page(settings_pages[PAGE_FACE]);

    // ---- Page 3: Alerts (chime picker + Preview, volume, glow) ----
    build_alerts_page(settings_pages[PAGE_ALERTS]);

    // ---- Page 4: About ----
    build_about_page(settings_pages[PAGE_ABOUT]);

    // Page indicator dots, centred under the tile area.
    const int total_w = SET_PAGES * L.dot_size + (SET_PAGES - 1) * L.dot_gap;
    for (int pg = 0; pg < SET_PAGES; pg++) {
        page_dots[pg] = make_dot(settings_container);
        lv_obj_set_pos(page_dots[pg], (L.scr_w - total_w) / 2 + pg * (L.dot_size + L.dot_gap),
                       L.dots_y - L.content_y);
    }

    show_settings_page(0);
    refresh_settings_controls(false);
    lv_obj_add_flag(settings_container, LV_OBJ_FLAG_HIDDEN);   // boot screen is the splash
}

static void update_page_dots(void) {
    for (int pg = 0; pg < SET_PAGES; pg++)
        lv_obj_set_style_bg_color(page_dots[pg], pg == settings_page ? COL_TEXT : COL_DOT_OFF, 0);
}

// Snap straight to page `page` (finger-driven moves go through the swipe
// engine). Other pages park one width to the left/right, still visible.
static void show_settings_page(int page) {
    if (page < 0) page = 0;
    if (page >= SET_PAGES) page = SET_PAGES - 1;
    settings_page = page;
    update_page_dots();
    for (int pg = 0; pg < SET_PAGES; pg++)
        lv_obj_set_x(settings_pages[pg], pg < page ? -L.scr_w : pg > page ? L.scr_w : 0);
    if (page == PAGE_ABOUT) {
        refresh_about();
        about_last_refresh_ms = lv_tick_get();
    }
}

static void refresh_settings_controls(bool animate) {
    if (!settings_container) return;
    const Settings& s = settings_get();
    set_clock_segment(s.clock, animate);
    render_clock_preview();
    set_toggle(tg_battery, s.show_battery, "Shown", "Hidden", animate);
    set_toggle(tg_mascot, s.show_mascot, "Shown", "Hidden", animate);
    set_toggle(tg_status, s.show_status, "Shown", "Hidden", animate);
    set_toggle(tg_glow, s.alert_glow, "On", "Off", animate);
    set_chime_segment(s.alert_chime, animate);
    if (sl_volume) {
        lv_slider_set_value(sl_volume, s.volume, animate ? LV_ANIM_ON : LV_ANIM_OFF);
        lv_label_set_text_fmt(lbl_volume, "%d%%", s.volume);
    }
    lv_slider_set_value(sl_brightness, brightness_get_pct(), animate ? LV_ANIM_ON : LV_ANIM_OFF);
    lv_label_set_text_fmt(lbl_brightness, "%d%%", brightness_get_pct());
    lv_slider_set_value(sl_sleep, s.sleep, animate ? LV_ANIM_ON : LV_ANIM_OFF);
    lv_label_set_text(lbl_sleep, settings_sleep_label(s.sleep));
    lv_obj_set_style_text_color(lbl_sleep, s.sleep == SLEEP_NEVER ? COL_DIM : COL_TEXT, 0);
    if (sl_flip) {
        lv_slider_set_value(sl_flip, s.face_flip, animate ? LV_ANIM_ON : LV_ANIM_OFF);
        lv_label_set_text(lbl_flip, settings_face_flip_label(s.face_flip));
        lv_obj_set_style_text_color(lbl_flip, s.face_flip == FLIP_OFF ? COL_DIM : COL_TEXT, 0);
    }
    layout_face_segments(animate);
    render_pairing_button();
}

static void refresh_about(void) {
    if (!settings_container) return;
    char buf[48];
    lv_label_set_text(about_value[ABOUT_BOARD], board_caps().name);
    lv_label_set_text(about_value[ABOUT_FW], FW_VERSION);
    lv_label_set_text(about_value[ABOUT_BLE], s_ble_connected ? "Connected" : "Advertising");
    lv_label_set_text(about_value[ABOUT_ADDR], ble_get_mac_address());

    if (batt_pct_cached >= 0)
        snprintf(buf, sizeof(buf), "%d%%%s", batt_pct_cached,
                 batt_charging_cached ? ", charging" : "");
    else if (host_batt_pct >= 0)
        snprintf(buf, sizeof(buf), "Host %d%%%s", host_batt_pct,
                 host_batt_charging ? ", plugged in" : "");
    else
        snprintf(buf, sizeof(buf), "None");
    lv_label_set_text(about_value[ABOUT_BATTERY], buf);

    const size_t free_b = heap_caps_get_free_size(MALLOC_CAP_INTERNAL);
    if (free_b) snprintf(buf, sizeof(buf), "%u KB", (unsigned)(free_b / 1024));
    else        snprintf(buf, sizeof(buf), "n/a");
    lv_label_set_text(about_value[ABOUT_RAM], buf);

    const uint32_t up_s = lv_tick_get() / 1000;
    if (up_s >= 3600) snprintf(buf, sizeof(buf), "%luh %02lum",
                               (unsigned long)(up_s / 3600), (unsigned long)((up_s % 3600) / 60));
    else              snprintf(buf, sizeof(buf), "%lum %02lus",
                               (unsigned long)(up_s / 60), (unsigned long)(up_s % 60));
    lv_label_set_text(about_value[ABOUT_UPTIME], buf);
}

// ======== Alert glow ========
// Four gradient strips hugging the screen edge, faded in from the outside;
// one animation breathes their opacity while Claude needs you. Strips (not a
// full-screen border) so a frame of the breath repaints only the edges.
static int glow_band(void) {
    int b = L.glow_w / GLOW_RINGS;
    return b < 1 ? 1 : b;
}

// Paint the rings. Runs only for the areas LVGL is redrawing (the invalidated
// bands + corners), so a breath costs about the edge area, not the screen.
static void glow_draw_cb(lv_event_t* e) {
    lv_layer_t* layer = lv_event_get_layer(e);
    if (!layer) return;
    const int band = glow_band();
    for (int i = 0; i < GLOW_RINGS; i++) {
        lv_draw_border_dsc_t d;
        lv_draw_border_dsc_init(&d);
        d.color = COL_ACCENT;
        d.width = band;
        d.side = LV_BORDER_SIDE_FULL;
        d.opa = (lv_opa_t)((glow_level * GLOW_RING_OPA[i]) / 255);
        const int r = glow_radius - i * band;
        d.radius = r < 0 ? 0 : r;
        lv_area_t a = { i * band, i * band, L.scr_w - 1 - i * band, L.scr_h - 1 - i * band };
        lv_draw_border(layer, &d, &a);
    }
}

// Invalidate just what the rings touch: four edge bands plus the corner
// squares the rounded corners sweep through.
static void glow_invalidate(void) {
    if (!glow_obj) return;
    const int w = L.glow_w;
    const int c = glow_radius + w;                // corner square side
    lv_area_t a;
    a.x1 = 0; a.y1 = 0; a.x2 = L.scr_w - 1; a.y2 = w - 1;                  lv_obj_invalidate_area(glow_obj, &a);
    a.y1 = L.scr_h - w; a.y2 = L.scr_h - 1;                                   lv_obj_invalidate_area(glow_obj, &a);
    a.x1 = 0; a.y1 = 0; a.x2 = w - 1; a.y2 = L.scr_h - 1;                  lv_obj_invalidate_area(glow_obj, &a);
    a.x1 = L.scr_w - w; a.x2 = L.scr_w - 1;                                   lv_obj_invalidate_area(glow_obj, &a);
    a.x1 = 0; a.y1 = 0; a.x2 = c; a.y2 = c;                                   lv_obj_invalidate_area(glow_obj, &a);
    a.x1 = L.scr_w - 1 - c; a.x2 = L.scr_w - 1;                               lv_obj_invalidate_area(glow_obj, &a);
    a.y1 = L.scr_h - 1 - c; a.y2 = L.scr_h - 1;                               lv_obj_invalidate_area(glow_obj, &a);
    a.x1 = 0; a.x2 = c;                                                       lv_obj_invalidate_area(glow_obj, &a);
}

static void glow_anim_exec(void* var, int32_t v) {
    (void)var;
    glow_level = v;
    glow_invalidate();
}

static void build_glow(lv_obj_t* scr) {
    glow_radius = board_caps().corner_radius;
    glow_obj = make_group_sized(scr, 0, 0, L.scr_w, L.scr_h);
    lv_obj_clear_flag(glow_obj, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(glow_obj, glow_draw_cb, LV_EVENT_DRAW_MAIN, NULL);
    lv_obj_add_flag(glow_obj, LV_OBJ_FLAG_HIDDEN);
}

// ---- Corner calibration overlay ----
// Six outlines hugging the screen edge at candidate radii, each in its own
// colour, with a legend in the middle: the one whose corners run along the
// bezel (neither cut off nor leaving a black gap) is the glass's radius.
static const int      CORNER_RADII[6]  = { 40, 55, 70, 85, 100, 115 };
static const uint32_t CORNER_COLORS[6] = { 0xfaf9f5, 0xd97757, 0x788c5d, 0xc0392b, 0x4a90d9, 0xe0c060 };
static const char* const CORNER_NAMES[6] = { "white", "orange", "green", "red", "blue", "yellow" };

static void corner_draw_cb(lv_event_t* e) {
    lv_layer_t* layer = lv_event_get_layer(e);
    if (!layer) return;
    for (int i = 0; i < 6; i++) {
        lv_draw_border_dsc_t d;
        lv_draw_border_dsc_init(&d);
        d.color = lv_color_hex(CORNER_COLORS[i]);
        d.width = 2;
        d.side = LV_BORDER_SIDE_FULL;
        d.opa = LV_OPA_COVER;
        d.radius = CORNER_RADII[i];
        lv_area_t a = { 0, 0, L.scr_w - 1, L.scr_h - 1 };
        lv_draw_border(layer, &d, &a);
    }
}

void ui_debug_corners(bool on) {
    if (on && !corner_test) {
        corner_test = make_group_sized(lv_screen_active(), 0, 0, L.scr_w, L.scr_h);
        lv_obj_clear_flag(corner_test, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_set_style_bg_color(corner_test, COL_BG, 0);
        lv_obj_set_style_bg_opa(corner_test, LV_OPA_COVER, 0);
        lv_obj_add_event_cb(corner_test, corner_draw_cb, LV_EVENT_DRAW_MAIN, NULL);
        lv_obj_t* legend = lv_label_create(corner_test);
        char buf[160] = "Corner radius\n";
        for (int i = 0; i < 6; i++) {
            char line[32];
            snprintf(line, sizeof(line), "%s %d\n", CORNER_NAMES[i], CORNER_RADII[i]);
            strlcat(buf, line, sizeof(buf));
        }
        lv_label_set_text(legend, buf);
        lv_obj_set_style_text_font(legend, L.about_key_font, 0);
        lv_obj_set_style_text_color(legend, COL_DIM, 0);
        lv_obj_set_style_text_align(legend, LV_TEXT_ALIGN_CENTER, 0);
        lv_obj_center(legend);
        for (int i = 0; i < 6; i++) {
            // Each outline's corner arc ends on the top edge at x = radius:
            // a tick in the outline's colour marks where to look.
            lv_obj_t* tick = lv_obj_create(corner_test);
            lv_obj_set_size(tick, 2, 12);
            lv_obj_set_pos(tick, CORNER_RADII[i] - 1, 2);
            lv_obj_set_style_bg_color(tick, lv_color_hex(CORNER_COLORS[i]), 0);
            lv_obj_set_style_bg_opa(tick, LV_OPA_COVER, 0);
            lv_obj_set_style_border_width(tick, 0, 0);
            lv_obj_set_style_radius(tick, 0, 0);
            lv_obj_clear_flag(tick, LV_OBJ_FLAG_CLICKABLE);
        }
    } else if (!on && corner_test) {
        lv_obj_delete(corner_test);
        corner_test = nullptr;
    }
}

void ui_debug_glow_radius(int radius) {
    glow_radius = radius < 0 ? 0 : radius;
    if (glow_obj && !lv_obj_has_flag(glow_obj, LV_OBJ_FLAG_HIDDEN)) lv_obj_invalidate(glow_obj);
}

// kind 0 = Claude needs you (permission / question), 1 = a long turn finished.
static void alert_start(int kind) {
    const Settings& s = settings_get();
    idle_note_activity();                          // wake a dimmed panel
    if (s.alert_glow && glow_obj) {
        glow_level = 0;
        lv_obj_clear_flag(glow_obj, LV_OBJ_FLAG_HIDDEN);
        lv_anim_delete(&glow_level, glow_anim_exec);
        lv_anim_t a;
        lv_anim_init(&a);
        lv_anim_set_var(&a, &glow_level);
        lv_anim_set_values(&a, 40, 255);
        lv_anim_set_duration(&a, 1100);
        lv_anim_set_playback_duration(&a, 1100);
        lv_anim_set_repeat_count(&a, LV_ANIM_REPEAT_INFINITE);
        lv_anim_set_path_cb(&a, lv_anim_path_ease_in_out);
        lv_anim_set_exec_cb(&a, glow_anim_exec);
        lv_anim_start(&a);
    }
    const bool chime = (kind == 0) ? (s.alert_chime != CHIME_OFF) : (s.alert_chime == CHIME_ALL);
    if (chime) sound_hal_play_alert(kind);
    alert_active = true;
    alert_act_idx = 0;
    if (header_actor) splash_actor_play(ALERT_ACTS[0], false, true);
}

static void alert_stop(void) {
    lv_anim_delete(&glow_level, glow_anim_exec);
    if (glow_obj) lv_obj_add_flag(glow_obj, LV_OBJ_FLAG_HIDDEN);
    if (!alert_active) return;
    alert_active = false;
    if (header_actor) splash_actor_stop();          // back to the still pose
}

// ======== Public API ========

void ui_init(void) {
    compute_layout(board_caps());

    lv_obj_t* scr = lv_screen_active();
    lv_obj_set_style_bg_color(scr, COL_BG, 0);
    lv_obj_set_style_bg_opa(scr, LV_OPA_COVER, 0);
    // A screen that can scroll swallows drags as scroll attempts (the walking
    // mascot briefly pokes past the edge on PSRAM boards, and dragged pages live
    // off-screen mid-gesture). Pages never need to scroll, so disable it.
    lv_obj_clear_flag(scr, LV_OBJ_FLAG_SCROLLABLE);
    // The swipe engine listens to the pointer device itself, so a drag that
    // starts on any card, tile or empty space is handled the same way.
    s_indev = lv_indev_get_next(NULL);
    if (s_indev) lv_indev_add_event_cb(s_indev, indev_cb, LV_EVENT_ALL, NULL);

    // Body viewport (clips the sliding surfaces), then the surfaces, then the
    // shared header title above them.
    body = make_group_sized(scr, 0, L.content_y, L.scr_w, L.body_h);
    init_usage_screen();
    build_settings_screen();

    lbl_title = lv_label_create(scr);
    lv_label_set_text(lbl_title, "Usage");
    lv_obj_set_style_text_font(lbl_title, L.title_font, 0);
    lv_obj_set_style_text_color(lbl_title, COL_TEXT, 0);
    // The nudge balances the corner logo on the left; smaller on small
    // screens where the logo is 40px and the battery icon sits closer.
    lv_obj_align(lbl_title, LV_ALIGN_TOP_MID, L.title_nudge, L.title_y);

    splash_init(scr);

    if (splash_get_root()) {
        lv_obj_add_event_cb(splash_get_root(), global_click_cb, LV_EVENT_CLICKED, NULL);
    }

    // Corner mascot in the old logo slot. The still Clawd is shorter than the
    // 80/40 px slot the spark logo used; center it vertically in that slot.
    {
        const int slot  = L.small_icons ? LOGO_SMALL_HEIGHT : LOGO_HEIGHT;
        const int art_h = L.small_icons ? CLAWD_STILL_SMALL_H : CLAWD_STILL_H;
        const int top   = L.logo_y + (slot - art_h) / 2;
#ifdef BOARD_HAS_PSRAM
        // Animated: idles, does acts, and takes walk-off/lurk trips.
        splash_mascot_create(scr, L.margin, top + art_h, L.small_icons ? 2 : 3);
#else
        // No PSRAM: a small actor canvas — the still pose, and the alert acts
        // (jump, wave, point) while Claude needs you. Feet on the slot's bottom.
        header_actor = splash_actor_create(scr, HEADER_ACTS, (int)(sizeof(HEADER_ACTS) / sizeof(HEADER_ACTS[0])), L.mascot_cell);
        if (header_actor) {
            const int feet = top + art_h + 4;
            lv_obj_set_pos(header_actor, L.margin, feet - splash_actor_height());
            lv_obj_clear_flag(header_actor, LV_OBJ_FLAG_CLICKABLE);
        }
        logo_img = header_actor;
#endif
    }

    // Every board gets the widget: without a cell (or a PMU at all) it shows the
    // host's battery when the daemon sends one, and nothing otherwise.
    make_battery_widget(scr);
    build_glow(scr);
}

void ui_update(const UsageData* data) {
    if (!data->valid) return;
    data_ok = data->ok;
    if (!data->ok) return;          // a {"ok":false} "no data" beat → fall through to idle, keep last numbers
    last_data_ms = lv_tick_get();   // a real usage update just landed
    data_received = true;

    host_batt_pct = data->host_batt_pct;
    host_batt_charging = data->host_batt_charging;
    refresh_battery_glyph();

    if (data->clock_epoch > 0) {    // daemon supplied wall-clock time → the title clock may run
        clock_base_epoch = data->clock_epoch;
        clock_base_ms = last_data_ms;
        clock_fmt = data->clock_fmt;
    } else {                        // no time from the daemon → the title reverts to "Usage"
        clock_base_epoch = 0;
    }
    if (current_screen == SCREEN_SETTINGS) render_clock_preview();   // keep the picker's preview live

    int s_pct = (int)(data->session_pct + 0.5f);

    if (data->enterprise) {
        // Spending box: big number-only label + small "%" symbol + desc + pace
        lv_obj_set_style_text_font(lbl_session_pct, L.ent_pct_font, 0);
        lv_label_set_text(lbl_session_label, "Spending");
        lv_obj_add_flag(lbl_session_reset, LV_OBJ_FLAG_HIDDEN);
        lv_obj_clear_flag(lbl_session_pct_sym, LV_OBJ_FLAG_HIDDEN);
        lv_obj_clear_flag(lbl_spending_desc,   LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(lbl_spending_status,   LV_OBJ_FLAG_HIDDEN);
        if (panel_weekly) lv_obj_clear_flag(panel_weekly, LV_OBJ_FLAG_HIDDEN);
    } else {
        lv_obj_set_style_text_font(lbl_session_pct, L.pct_font, 0);
        lv_label_set_text(lbl_session_label, "Current");
        lv_obj_clear_flag(lbl_session_reset, LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(lbl_session_pct_sym, LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(lbl_spending_desc,   LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(lbl_spending_status, LV_OBJ_FLAG_HIDDEN);
        if (panel_weekly) lv_obj_clear_flag(panel_weekly, LV_OBJ_FLAG_HIDDEN);
    }

    // Weekly faces. Scoped limits (e.g. Fable) come from the "ws" key: absence →
    // one face, classic card; 0% is a real reading. Enterprise has no weekly
    // window at all. While faces exist the Weekly card handles its own taps
    // (flip) instead of bubbling them up to the splash toggle.
    if (!data->enterprise) {
        cached_weekly_pct = data->weekly_pct;
        cached_weekly_reset = data->weekly_reset_mins;
    }
    const int prev_scoped = cached_scoped_count;
    cached_scoped_count = data->enterprise ? 0 : data->scoped_weekly_count;
    for (int i = 0; i < cached_scoped_count; i++) cached_scoped[i] = data->scoped_weekly[i];
    if (weekly_face > cached_scoped_count) weekly_face = 0;
    if (cached_scoped_count > 0) {
        lv_obj_clear_flag(panel_weekly, LV_OBJ_FLAG_EVENT_BUBBLE);
        lv_obj_set_style_bg_color(panel_weekly, COL_PRESSED, LV_STATE_PRESSED);
        if (prev_scoped == 0) {                       // faces just appeared: start on the default
            weekly_face = default_face_clamped();
            schedule_face_flip(settings_face_flip_ms());
        } else if (settings_face_flip_ms() == 0) {
            face_next_auto_ms = 0;
        } else if (face_next_auto_ms == 0) {
            schedule_face_flip(settings_face_flip_ms());
        }
    } else {
        lv_obj_add_flag(panel_weekly, LV_OBJ_FLAG_EVENT_BUBBLE);
        lv_obj_set_style_bg_color(panel_weekly, COL_PANEL, LV_STATE_PRESSED);
        face_next_auto_ms = 0;
    }
    const bool faces_changed = (prev_scoped != cached_scoped_count);

    char buf[48];

    // Pace vars used in both enterprise blocks below
    const char* pace_text = "Under pace";
    lv_color_t  pace_color = COL_GREEN;
    const char* pace_hex   = "788c5d";   // matches THEME_GREEN
    if (data->session_pct > (float)data->time_pct + 15.0f) {
        pace_text = "Over pace";  pace_color = COL_RED;   pace_hex = "c0392b";
    } else if (data->session_pct > (float)data->time_pct - 15.0f) {
        pace_text = "On pace";    pace_color = COL_AMBER; pace_hex = "d97757";
    }

    if (data->enterprise) {
        lv_label_set_text_fmt(lbl_session_pct, "%d", s_pct);
        lv_obj_align_to(lbl_session_pct_sym, lbl_session_pct,
                        LV_ALIGN_OUT_RIGHT_TOP, 4, 12);
    } else {
        lv_label_set_text_fmt(lbl_session_pct, "%d%%", s_pct);
        format_reset_time(data->session_reset_mins, buf, sizeof(buf));
        lv_label_set_text(lbl_session_reset, buf);
    }

    lv_bar_set_value(bar_session, s_pct, LV_ANIM_ON);
    lv_obj_set_style_bg_color(bar_session, pct_color(data->session_pct), LV_PART_INDICATOR);

    if (data->enterprise) {
        // Period box: time % + dynamic pace color + "Resets <date>" label
        lv_label_set_text(lbl_weekly_label, "Period");
        lv_label_set_text_fmt(lbl_weekly_pct, "%d%%", data->time_pct);
        lv_bar_set_value(bar_weekly, data->time_pct, LV_ANIM_ON);
        lv_color_t bar_pace = (data->session_pct <= (float)data->time_pct) ? COL_GREEN :
                              (data->session_pct <= (float)data->time_pct + 15.0f) ? COL_AMBER :
                              COL_RED;
        lv_obj_set_style_bg_color(bar_weekly, bar_pace, LV_PART_INDICATOR);
        snprintf(buf, sizeof(buf), "#%s %s# - #faf9f5 Resets %s#",
                 pace_hex, pace_text, data->reset_date);
        lv_label_set_text(lbl_weekly_reset, buf);
    } else {
        render_weekly_face(true);
    }

    // Keep the Weekly-card settings page truthful: rebuild its picker when the
    // set of faces changed, and refresh the preview reading on every payload
    // while that page is on show.
    if (settings_container) {
        if (faces_changed) refresh_settings_controls(false);
        else if (current_screen == SCREEN_SETTINGS && settings_page == PAGE_FACE) layout_face_segments(false);
    }
}

static bool data_fresh(void) {
    return data_received && data_ok && (lv_tick_get() - last_data_ms) < DATA_FRESH_MS;
}

// Which battery the header shows: the board's own cell if fitted, else the
// host's (only while its data is fresh), else nothing.
struct BattView { bool shown; bool host; int pct; bool charging; };
static BattView resolve_battery(void) {
    BattView v = { false, false, -1, false };
    if (!batt_root || !settings_get().show_battery) return v;
    if (batt_pct_cached >= 0) {
        v.shown = true; v.pct = batt_pct_cached; v.charging = batt_charging_cached;
    } else if (host_batt_pct >= 0 && data_fresh()) {
        v.shown = true; v.host = true; v.pct = host_batt_pct; v.charging = host_batt_charging;
    }
    return v;
}

static void refresh_battery_glyph(void) {
    if (!batt_root) return;
    const BattView v = resolve_battery();
    if (v.shown) {
        const int inner_w = (L.batt_w - 2 - L.batt_nub_w) - 2 * L.batt_border - 4;
        int fill_w = inner_w * v.pct / 100;
        if (v.pct > 0 && fill_w < 3) fill_w = 3;
        if (fill_w > inner_w) fill_w = inner_w;
        lv_obj_set_width(batt_fill, fill_w > 0 ? fill_w : 1);
        lv_obj_set_style_bg_opa(batt_fill, fill_w > 0 ? LV_OPA_COVER : LV_OPA_TRANSP, 0);
        lv_obj_set_style_bg_color(batt_fill,
            v.charging ? COL_ACCENT : (v.pct <= 10 ? COL_RED : COL_DOT_OFF), 0);
        lv_label_set_text_fmt(batt_lbl, "%d%%", v.pct);
    }
    apply_header_visibility();
}

// Pick the usage-view sub-screen: pairing hint (BLE down), the idle "Zzz" screen
// (connected but data has gone stale), or the live usage panels. Only re-lays-out
// on an actual change. The animated status line stays visible everywhere — it
// reads "Listening…" on the idle screen, keeping it alive rather than frozen.
static void update_view_state(void) {
    if (!usage_group || !pair_group || !idle_group) return;
    int v;
    if (!s_ble_connected) {
        v = 0;  // pairing hint
    } else if (data_received && data_ok && (lv_tick_get() - last_data_ms) < DATA_FRESH_MS) {
        v = 2;  // live usage
    } else {
        v = 1;  // idle / Zzz
    }
    if (v == view_state) return;
    view_state = v;
    lv_obj_add_flag(pair_group, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(idle_group, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(usage_group, LV_OBJ_FLAG_HIDDEN);
    lv_obj_clear_flag(v == 0 ? pair_group : v == 1 ? idle_group : usage_group,
                      LV_OBJ_FLAG_HIDDEN);
    refresh_battery_glyph();   // a stale host battery disappears with the live data
}

// The shared header title: "Settings" on Settings; on the usage level the
// page decides — the Working page's state word, "Trend", or the clock / "Usage"
// (clock only when the daemon supplied the time AND the device's Clock setting
// is on). Rewrites the label only when the text actually changes, so calling
// it every tick is free.
static void render_title(bool force) {
    (void)force;
    if (!lbl_title) return;
    char want[32];
    if (title_screen == SCREEN_SETTINGS) {
        strlcpy(want, "Settings", sizeof(want));
    } else if (title_level_page == LEVEL_STATS) {
        strlcpy(want, "Stats", sizeof(want));
    } else {
        // Several accounts: the title says which one is shown, otherwise you
        // cannot tell what you are looking at after a switch. One account: as before.
        char acct[SLOT_ID_LEN + 3] = "";
        if (slots_count() >= 2) {
            Slot* s = slots_active();
            if (s && s->id[0]) snprintf(acct, sizeof(acct), "%s  ", s->id);
        }
        const uint8_t mode = settings_get().clock;
        if (mode == CLOCK_OFF || clock_base_epoch == 0) {
            snprintf(want, sizeof(want), "%s%s", acct, acct[0] ? "" : "Usage");
            if (!want[0]) strlcpy(want, "Usage", sizeof(want));
            if (acct[0]) {   // clock off: the label alone, no padding
                size_t n = strlen(want);
                while (n && want[n-1] == ' ') want[--n] = '\0';
            }
        } else {
            const int fmt = (mode == CLOCK_12H) ? 12 : (mode == CLOCK_24H) ? 24 : clock_fmt;
            time_t cur = (time_t)(clock_base_epoch + (lv_tick_get() - clock_base_ms) / 1000);
            struct tm tmv;
            gmtime_r(&cur, &tmv);   // epoch is already local wall-clock → gmtime keeps it as-is
            if (fmt == 12) {
                int h12 = tmv.tm_hour % 12;
                if (h12 == 0) h12 = 12;
                snprintf(want, sizeof(want), "%s%d:%02d %s", acct, h12, tmv.tm_min, tmv.tm_hour < 12 ? "AM" : "PM");
            } else {
                snprintf(want, sizeof(want), "%s%02d:%02d", acct, tmv.tm_hour, tmv.tm_min);
            }
        }
    }
    if (strcmp(want, title_cur) != 0) {
        strlcpy(title_cur, want, sizeof(title_cur));
        lv_label_set_text(lbl_title, want);
    }
}

static void ready_dot_anim_exec(void* var, int32_t v) {
    (void)var;
    ready_dot_opa = v;
    if (ready_dot) lv_obj_set_style_opa(ready_dot, (lv_opa_t)v, 0);
}

// Switch the ticker's style (see TickerMode). Ready and Idle share the
// "dot + one word" layout; only Ready breathes.
static void set_ticker_mode(TickerMode mode) {
    if (mode == ticker_mode) return;
    ticker_mode = mode;
    if (!lbl_anim || !ready_dot) return;
    lv_anim_delete(&ready_dot_opa, ready_dot_anim_exec);
    if (mode == TICKER_WHIMSY) {
        lv_obj_add_flag(ready_dot, LV_OBJ_FLAG_HIDDEN);
        lv_obj_set_style_text_color(lbl_anim, COL_ACCENT, 0);
        return;
    }
    const bool ready = (mode == TICKER_READY);
    const char* word = ready ? "Ready" : "Idle";
    const lv_color_t col = ready ? COL_GREEN : COL_DIM;
    lv_obj_set_style_text_color(lbl_anim, col, 0);
    lv_obj_set_style_bg_color(ready_dot, col, 0);
    lv_label_set_text(lbl_anim, word);
    // Place the dot just left of the centred word.
    lv_point_t sz;
    lv_text_get_size(&sz, word, L.anim_font, 0, 0, LV_COORD_MAX, LV_TEXT_FLAG_NONE);
    const int dot_w = lv_obj_get_width(ready_dot);
    const int gap = dot_w;
    lv_obj_update_layout(lbl_anim);
    const int cx = lv_obj_get_x(lbl_anim) + lv_obj_get_width(lbl_anim) / 2;
    const int cy = lv_obj_get_y(lbl_anim) + lv_obj_get_height(lbl_anim) / 2;
    lv_obj_set_pos(ready_dot, cx - sz.x / 2 - gap - dot_w, cy - dot_w / 2);
    lv_obj_clear_flag(ready_dot, LV_OBJ_FLAG_HIDDEN);
    if (!ready) {                                   // Idle: a still, dim dot
        ready_dot_opa = 255;
        lv_obj_set_style_opa(ready_dot, LV_OPA_COVER, 0);
        return;
    }
    lv_anim_t a;
    lv_anim_init(&a);
    lv_anim_set_var(&a, &ready_dot_opa);
    lv_anim_set_values(&a, 255, 70);
    lv_anim_set_duration(&a, 1400);
    lv_anim_set_playback_duration(&a, 1400);
    lv_anim_set_repeat_count(&a, LV_ANIM_REPEAT_INFINITE);
    lv_anim_set_path_cb(&a, lv_anim_path_ease_in_out);
    lv_anim_set_exec_cb(&a, ready_dot_anim_exec);
    lv_anim_start(&a);
}

void ui_tick_anim(void) {
    const uint32_t now = lv_tick_get();
    drag_tick();

    // The Usage surface is parked, not destroyed, while Settings is up — keep
    // its pair/idle/live sub-view current so a drag never reveals a stale one.
    if (setup_root) return;                    // the setup card owns the screen
    apply_link_hint();
    update_view_state();
    if (view_state == 1) splash_mini_tick();   // the idle creature keeps breathing

    // Level page dots fade out shortly after a horizontal gesture settles.
    if (level_dots_hide_ms && !drag_tracking && !transitioning && (int32_t)(now - level_dots_hide_ms) >= 0) {
        level_dots_hide_ms = 0;
        level_dots_fade();
    }
    // The Preview alert ends by itself.
    if (preview_until_ms && (int32_t)(now - preview_until_ms) >= 0) {
        preview_until_ms = 0;
        if (!(cc_seen && cc_is_attention(cc_cur.state))) alert_stop();
    }
    // An alert cycles the header mascot's exuberant acts back to back.
    if (alert_active && header_actor && splash_actor_is_idle()) {
        alert_act_idx = (uint8_t)((alert_act_idx + 1) % (sizeof(ALERT_ACTS) / sizeof(ALERT_ACTS[0])));
        splash_actor_play(ALERT_ACTS[alert_act_idx], false, true);
    }

    if (current_screen == SCREEN_SETTINGS) {
        if (settings_page == PAGE_ABOUT && now - about_last_refresh_ms >= 1000) {
            about_last_refresh_ms = now;
            refresh_about();
        }
        if (settings_page == 0 && now - preview_last_refresh_ms >= 10000) {
            preview_last_refresh_ms = now;
            render_clock_preview();
        }
        // Expire the two-tap pairing confirmation / the "Cleared" notice.
        if (pairing_confirm_ms && now - pairing_confirm_ms >= PAIRING_CONFIRM_MS) {
            pairing_confirm_ms = 0;
            render_pairing_button();
        }
        if (pairing_cleared_ms && now - pairing_cleared_ms >= PAIRING_CLEARED_MS) {
            pairing_cleared_ms = 0;
            render_pairing_button();
        }
        // fall through: the Usage surface keeps its clock and ticker alive
        // while parked, so it is never stale when a drag reveals it.
    }
    if (current_screen == SCREEN_SPLASH) return;

    render_title(false);

    // Weekly card: advance the face on its own clock (a tap resets the clock
    // with a longer hold, see weekly_click_cb). Only while actually on show,
    // and only when the user has automatic flipping on.
    if (current_screen == SCREEN_USAGE && cached_scoped_count > 0 && view_state == 2 &&
        face_next_auto_ms && (int32_t)(now - face_next_auto_ms) >= 0) {
        const uint32_t iv = settings_face_flip_ms();
        if (iv) flip_weekly_face(iv); else face_next_auto_ms = 0;
    }

    if (now - anim_msg_start >= ANIM_MSG_MS) {
        anim_msg_idx = (anim_msg_idx + 1) % ANIM_MSG_COUNT;
        anim_msg_start = now;
    }

    // Companion-driven lines. "Known": the bridge reported within CC_LIVE_MS
    // (it does with every usage payload). "Live": it knows of a session. A
    // session waiting for you → the calm green Ready; a bridge that knows of
    // no session at all → a dim Idle, not the whimsical spinner words, which
    // would suggest work that isn't happening.
    const bool cc_known = cc_seen && now - cc_at_ms < CC_LIVE_MS;
    const bool cc_live = cc_known && cc_cur.sessions > 0 && cc_cur.state != CC_NONE;
    const bool settled = s_ble_connected && now - connected_at_ms >= 5000;
    // A session on another machine holds the line: name the machine above it.
    if (lbl_host) {
        const char* host = (settled && cc_live && cc_cur.host[0] && settings_get().show_status) ? cc_cur.host : "";
        if (host[0]) {
            if (strcmp(lv_label_get_text(lbl_host), host) != 0) lv_label_set_text(lbl_host, host);
            lv_obj_clear_flag(lbl_host, LV_OBJ_FLAG_HIDDEN);
        } else {
            lv_obj_add_flag(lbl_host, LV_OBJ_FLAG_HIDDEN);
        }
    }
    if (settled && cc_live && cc_cur.state == CC_IDLE) {
        set_ticker_mode(TICKER_READY);
        return;
    }
    if (settled && cc_known && !cc_live && view_state != 1) {   // "No data" keeps its hint
        set_ticker_mode(TICKER_IDLE);
        return;
    }
    set_ticker_mode(TICKER_WHIMSY);

    if (now - anim_last_ms < spinner_ms[anim_spinner_idx]) return;
    anim_last_ms = now;
    anim_phase = (anim_phase + 1) % SPINNER_PHASES;
    anim_spinner_idx = (anim_phase < SPINNER_COUNT) ? anim_phase
                                                    : (SPINNER_PHASES - anim_phase);

    // Status text by priority. Whimsical messages only when connected & settled.
    const char* text;
    if (!s_ble_connected) {
        text = "Waiting";              // advertising / waiting for a host connection
    } else if (view_state == 1) {      // idle — alternate so it reads as alive AND data-less
        text = (anim_msg_idx & 1) ? "No data" : "Listening";
    } else if (now - connected_at_ms < 5000) {
        text = "Connected";
    } else if (cc_live) {
        // The companion knows what Claude is doing right now — say that
        // instead of a random word. "Thinking" keeps the whimsical vocabulary:
        // those are Claude Code's own spinner words.
        switch (cc_cur.state) {
        case CC_TOOL:       text = cc_cur.label[0] ? cc_cur.label : "Working"; break;
        case CC_DONE:
        case CC_TURN_DONE:  text = "Your turn"; break;
        case CC_ATTENTION:  text = cc_cur.label[0] ? cc_cur.label : "Needs you"; break;   // "Permission: Bash", "Plan to approve"
        case CC_ERROR:      text = "Error"; break;
        case CC_COMPACTING: text = "Compacting"; break;
        default:            text = anim_messages[anim_msg_idx]; break;
        }
    } else {
        text = anim_messages[anim_msg_idx];
    }

    // All states share the whimsical style: "<glyph> <Title-case word>…" (the
    // machine a remote session runs on is the small caption above, see lbl_host).
    static char buf[80];
    snprintf(buf, sizeof(buf), "%s %s\xE2\x80\xA6",
             spinner_frames[anim_spinner_idx], text);
    lv_label_set_text(lbl_anim, buf);
}

static screen_t prev_non_splash_screen = SCREEN_USAGE;

// Header chrome (battery glyph, corner mascot) and the status ticker follow the
// user's settings on every non-splash page; the splash hides all of them.
static void apply_header_visibility(void) {
    const Settings& s = settings_get();
    const bool on_splash = (current_screen == SCREEN_SPLASH);
    if (lbl_title) {
        if (on_splash) lv_obj_add_flag(lbl_title, LV_OBJ_FLAG_HIDDEN);
        else           lv_obj_clear_flag(lbl_title, LV_OBJ_FLAG_HIDDEN);
    }
    if (batt_root) {
        if (on_splash || !resolve_battery().shown) lv_obj_add_flag(batt_root, LV_OBJ_FLAG_HIDDEN);
        else                                       lv_obj_clear_flag(batt_root, LV_OBJ_FLAG_HIDDEN);
    }
    const bool mascot = !on_splash && s.show_mascot;
    splash_mascot_set_visible(mascot);
    if (logo_img) {
        if (mascot) lv_obj_clear_flag(logo_img, LV_OBJ_FLAG_HIDDEN);
        else        lv_obj_add_flag(logo_img, LV_OBJ_FLAG_HIDDEN);
    }
    if (lbl_anim) {
        if (s.show_status) lv_obj_clear_flag(lbl_anim, LV_OBJ_FLAG_HIDDEN);
        else               lv_obj_add_flag(lbl_anim, LV_OBJ_FLAG_HIDDEN);
    }
    if (ready_dot) {
        if (s.show_status && ticker_mode != TICKER_WHIMSY) lv_obj_clear_flag(ready_dot, LV_OBJ_FLAG_HIDDEN);
        else                               lv_obj_add_flag(ready_dot, LV_OBJ_FLAG_HIDDEN);
    }
    if (lbl_host && (on_splash || !s.show_status)) lv_obj_add_flag(lbl_host, LV_OBJ_FLAG_HIDDEN);   // ui_tick_anim re-shows it
}

// One touch settles everything that is currently asking for it, rather than one
// thing per tap: an alert waiting to be acknowledged AND the splash standing in
// front of the numbers both go on the same press. Entering the splash is the
// odd one out — it is a plain toggle, not a dismissal, so it only happens when
// the tap found nothing to clear (dismissing an alert must never throw the
// screensaver up in your face).
static void global_click_cb(lv_event_t* e) {
    (void)e;
    if (click_guarded()) return;

    bool settled = false;

    if (alert_active || preview_until_ms) {          // acknowledge the alert
        preview_until_ms = 0;
        alert_stop();
        settled = true;
    }
    if (current_screen == SCREEN_SPLASH) {           // and step out of the splash
        ui_show_screen(prev_non_splash_screen);
        settled = true;
    }

    if (!settled) ui_show_screen(SCREEN_SPLASH);
}

// ======== Swipe engine ========

static bool obj_is_slider(lv_obj_t* obj) {
    while (obj) {
        if (lv_obj_check_type(obj, &lv_slider_class)) return true;
        obj = lv_obj_get_parent(obj);
    }
    return false;
}

// Place both surfaces for a displacement `off` of the leaving one; the arriving
// one sits exactly one span behind it in the drag direction.
static void set_drag_offset(int32_t off) {
    drag_off = off;
    if (!drag_out) return;
    const int32_t in_off = off + drag_sign * drag_span;
    if (drag_vertical) {
        lv_obj_set_y(drag_out, off);
        if (drag_in) lv_obj_set_y(drag_in, in_off);
    } else {
        lv_obj_set_x(drag_out, off);
        if (drag_in) lv_obj_set_x(drag_in, in_off);
    }
    // The header stays; its title cross-fades: out to 0 at half way, then the
    // arriving page's title fades back in. Settings pages share one title, so
    // only vertical moves and usage-level paging fade.
    if (drag_in && drag_span > 0 && (drag_vertical || current_screen == SCREEN_USAGE)) {
        const int32_t p255 = (LV_ABS(off) * 255) / drag_span;          // 0..255 progress
        const int32_t opa = LV_ABS(255 - 2 * p255);                       // V shape
        screen_t want_scr = title_screen;
        int      want_page = title_level_page;
        if (drag_vertical) {
            want_scr = (p255 >= 128)
                ? (drag_in == settings_container ? SCREEN_SETTINGS : SCREEN_USAGE)
                : (drag_out == settings_container ? SCREEN_SETTINGS : SCREEN_USAGE);
            want_page = level_page;
        } else {
            want_scr = SCREEN_USAGE;
            want_page = (p255 >= 128) ? drag_target_page : level_page;
        }
        if (want_scr != title_screen || want_page != title_level_page) {
            title_screen = want_scr;
            title_level_page = want_page;
            render_title(true);
        }
        lv_obj_set_style_text_opa(lbl_title, (lv_opa_t)(opa > 255 ? 255 : opa), 0);
    }
}

static void drag_anim_exec(void* var, int32_t v) { (void)var; set_drag_offset(v); }

// The snap leaves the surfaces exactly where they park (leaving one a span
// away, arriving one at the origin — or the reverse when it sprang back), so
// only the bookkeeping remains.
static void drag_anim_done(lv_anim_t* a) {
    (void)a;
    if (drag_out && drag_commit && drag_in) {
        if (drag_vertical) {
            const bool to_settings = (drag_in == settings_container);
            current_screen = to_settings ? SCREEN_SETTINGS : SCREEN_USAGE;
            prev_non_splash_screen = current_screen;
            if (to_settings) preview_last_refresh_ms = lv_tick_get();
            apply_header_visibility();
        } else if (current_screen == SCREEN_USAGE) {
            show_level_page(drag_target_page);      // level paging
        } else {
            settings_page = drag_target_page;
            update_page_dots();
            if (settings_page == PAGE_ABOUT) {
                refresh_about();
                about_last_refresh_ms = lv_tick_get();
            }
        }
    }
    drag_out = drag_in = nullptr;
    transitioning = false;
    // Title settles on the page that is now on show, fully opaque.
    title_screen = current_screen;
    title_level_page = level_page;
    lv_obj_set_style_text_opa(lbl_title, LV_OPA_COVER, 0);
    render_title(true);
}

// Snap the usage level to `page` (finger-driven moves go through the swipe
// engine). Other pages park one width to the left/right, still visible.
static void show_level_page(int page) {
    if (page < 0) page = 0;
    if (page >= LEVEL_PAGES) page = LEVEL_PAGES - 1;
    level_page = page;
    update_level_dots();
    for (int i = 0; i < LEVEL_PAGES; i++)
        lv_obj_set_x(level_pages[i], i < page ? -L.scr_w : i > page ? L.scr_w : 0);
    if (page == LEVEL_STATS) render_stats_page();
    if (current_screen == SCREEN_USAGE) {
        title_level_page = level_page;
        render_title(true);
    }
}

// Animate from the current displacement to the snap point — one animation for
// both surfaces. Duration scales with the distance left to travel (or is
// forced, for the serial QA command).
static void snap_drag_ms(bool commit, uint32_t force_ms) {
    drag_commit = commit && drag_in != nullptr;
    const int32_t target = drag_commit ? -drag_sign * drag_span : 0;
    const int32_t remaining = LV_ABS(target - drag_off);
    uint32_t ms = drag_span > 0 ? (uint32_t)((int64_t)SNAP_MS * remaining / drag_span) : 0;
    if (ms < 80) ms = 80;
    if (force_ms) ms = force_ms;
    transitioning = true;
    lv_anim_t a;
    lv_anim_init(&a);
    lv_anim_set_var(&a, &drag_off);
    lv_anim_set_values(&a, drag_off, target);
    lv_anim_set_duration(&a, ms);
    lv_anim_set_exec_cb(&a, drag_anim_exec);
    lv_anim_set_path_cb(&a, lv_anim_path_ease_out);
    lv_anim_set_completed_cb(&a, drag_anim_done);
    lv_anim_start(&a);
}
static void snap_drag(bool commit) { snap_drag_ms(commit, 0); }

// First movement past DRAG_START_PX: decide the axis and which surfaces move.
static void begin_drag(int32_t dx, int32_t dy) {
    last_gesture_ms = lv_tick_get();          // the release will also arrive as CLICKED — swallow it
    drag_in = nullptr;
    drag_out = nullptr;
    drag_target_page = -1;
    if (current_screen == SCREEN_SPLASH) { drag_mode = DRAG_SPLASH; return; }

    const bool horizontal = LV_ABS(dx) >= LV_ABS(dy);
    if (horizontal) {
        drag_mode = DRAG_H;
        drag_vertical = false;
        drag_span = L.scr_w;
        drag_sign = dx < 0 ? +1 : -1;
        if (current_screen == SCREEN_SETTINGS) {
            drag_out = settings_pages[settings_page];
            const int target = settings_page + drag_sign;
            if (target >= 0 && target < SET_PAGES) {
                drag_target_page = target;
                drag_in = settings_pages[target];      // already parked one width away
            }
        } else {                                        // usage level: Stats ◂ Usage
            drag_out = level_pages[level_page];
            const int target = level_page + drag_sign;
            if (target >= 0 && target < LEVEL_PAGES) {
                drag_target_page = target;
                drag_in = level_pages[target];
            } else if (level_page == LEVEL_USAGE) {
                // No page on this side: the gesture used to do nothing but
                // bounce the screen. It now moves to the next account, and the
                // numbers change during the slide, which gives the finger the
                // visual feedback a long press lacked.
                ui_cycle_account(drag_sign);
            }
            level_dots_show();
        }
    } else {
        drag_mode = DRAG_V;
        drag_vertical = true;
        drag_span = L.body_h;
        drag_sign = dy < 0 ? +1 : -1;          // finger up → Settings rises from below
        if (current_screen == SCREEN_USAGE) {
            drag_out = level;
            if (drag_sign > 0) {                    // Settings is parked below, ready
                pairing_confirm_ms = 0;
                pairing_cleared_ms = 0;
                refresh_settings_controls(false);
                show_settings_page(0);
                drag_in = settings_container;
            }
        } else {                                    // Settings: down goes home
            drag_out = settings_container;
            if (drag_sign < 0) drag_in = level;   // parked above
        }
    }
    set_drag_offset(0);
}

// Follow the finger. With no surface to reveal (past the last page, or the
// wrong way), the drag meets rubber resistance instead of moving fully.
static void update_drag(int32_t dx, int32_t dy) {
    const int32_t d = drag_vertical ? dy : dx;
    int32_t off;
    if (!drag_in) {
        off = d / RUBBER_DIV;
    } else if (drag_sign > 0) {
        off = d > 0 ? d / RUBBER_DIV : (d < -drag_span ? -drag_span : d);
    } else {
        off = d < 0 ? d / RUBBER_DIV : (d > drag_span ? drag_span : d);
    }
    set_drag_offset(off);
}

static void end_drag(lv_indev_t* indev) {
    if (drag_mode == DRAG_SPLASH) {
        drag_mode = DRAG_NONE;
        ui_show_screen(prev_non_splash_screen);
        return;
    }
    if (drag_mode == DRAG_H || drag_mode == DRAG_V) {
        bool commit = false;
        if (drag_in) {
            lv_point_t v;
            lv_indev_get_vect(indev, &v);
            const int32_t vel = drag_vertical ? v.y : v.x;
            const int32_t moved = drag_sign > 0 ? -drag_off : drag_off;   // progress towards the target
            const bool flick = drag_sign > 0 ? (vel < -DRAG_COMMIT_VEL) : (vel > DRAG_COMMIT_VEL);
            commit = moved > drag_span / DRAG_COMMIT_DIV || (flick && moved > DRAG_START_PX);
        }
        snap_drag(commit);
    }
    drag_mode = DRAG_NONE;
}

// Input-device hook. LVGL forwards PRESSED and RELEASED to the device (not
// PRESSING), so the press opens tracking, drag_tick() polls the finger every
// UI tick, and the release ends it.
static void indev_cb(lv_event_t* e) {
    lv_indev_t* indev = lv_indev_active();
    if (!indev) return;
    switch (lv_event_get_code(e)) {
    case LV_EVENT_PRESSED:
        lv_indev_get_point(indev, &drag_origin);
        drag_mode = DRAG_NONE;
        if (transitioning || obj_is_slider(lv_indev_get_active_obj())) drag_mode = DRAG_IGNORE;
        drag_tracking = true;
        break;
    case LV_EVENT_RELEASED:
        drag_tracking = false;
        end_drag(indev);
        break;
    default:
        break;
    }
}

// Called every UI tick while a finger is down: turn its travel into a drag.
static void drag_tick(void) {
    if (!drag_tracking || !s_indev) return;
    if (drag_mode == DRAG_IGNORE || drag_mode == DRAG_SPLASH) return;
    if (lv_indev_get_state(s_indev) != LV_INDEV_STATE_PRESSED) return;
    lv_point_t p;
    lv_indev_get_point(s_indev, &p);
    const int32_t dx = p.x - drag_origin.x;
    const int32_t dy = p.y - drag_origin.y;
    if (drag_mode == DRAG_NONE) {
        if (LV_ABS(dx) < DRAG_START_PX && LV_ABS(dy) < DRAG_START_PX) return;
        begin_drag(dx, dy);
        if (drag_mode != DRAG_H && drag_mode != DRAG_V) return;
    }
    update_drag(dx, dy);
}

void ui_show_screen(screen_t screen) {
    settle_slides();
    splash_hide();

    switch (screen) {
    case SCREEN_SPLASH:
        // The splash covers the screen; keep the parked surfaces out of the
        // render pass entirely (the C6 draws the splash straight to the panel).
        lv_obj_add_flag(level, LV_OBJ_FLAG_HIDDEN);
        if (settings_container) lv_obj_add_flag(settings_container, LV_OBJ_FLAG_HIDDEN);
        alert_stop();
        splash_show();
        break;
    case SCREEN_USAGE:
        park_screens(SCREEN_USAGE);
        break;
    case SCREEN_ABOUT:                       // About lives on the last settings page
        settings_page = PAGE_ABOUT;
        screen = SCREEN_SETTINGS;
        /* fall through */
    case SCREEN_SETTINGS:
        pairing_confirm_ms = 0;
        pairing_cleared_ms = 0;
        refresh_settings_controls(false);
        show_settings_page(settings_page);
        preview_last_refresh_ms = lv_tick_get();
        park_screens(SCREEN_SETTINGS);
        break;
    default: break;
    }

    if (screen != SCREEN_SPLASH) prev_non_splash_screen = screen;
    current_screen = screen;
    if (screen != SCREEN_SPLASH) {
        title_screen = screen;
        title_level_page = level_page;
        render_title(true);
    }
    apply_header_visibility();
}

void ui_toggle_splash(void) {
    if (current_screen == SCREEN_SPLASH) ui_show_screen(prev_non_splash_screen);
    else                                  ui_show_screen(SCREEN_SPLASH);
}

screen_t ui_get_current_screen(void) {
    return current_screen;
}

void ui_refresh_settings(void) {
    refresh_settings_controls(true);
    apply_header_visibility();
    clock_last_min = -1;   // title clock re-evaluates its mode on the next tick
}

void ui_flip_weekly_face(void) {
    flip_weekly_face(FACE_HOLD_MS);
}

void ui_show_settings_page(int page) {
    if (page < 0) page = 0;
    if (page >= SET_PAGES) page = SET_PAGES - 1;
    settings_page = page;
    ui_show_screen(SCREEN_SETTINGS);
}

void ui_debug_swipe(int dir, uint32_t ms) {
    if (transitioning || current_screen == SCREEN_SPLASH || dir == 0) return;
    drag_tracking = false;
    drag_mode = DRAG_NONE;
    begin_drag(0, dir > 0 ? -DRAG_START_PX : DRAG_START_PX);   // finger up → Settings
    const bool ok = (drag_mode == DRAG_V && drag_in != nullptr);
    drag_mode = DRAG_NONE;
    if (!ok) { drag_out = drag_in = nullptr; return; }
    snap_drag_ms(true, ms);
}

void ui_update_ble_status(ble_state_t state, const char* name, const char* mac) {
    (void)name; (void)mac;
    bool was_connected = s_ble_connected;
    s_ble_connected = (state == BLE_STATE_CONNECTED);

    if (s_ble_connected && !was_connected) connected_at_ms = lv_tick_get();
    // pair / idle / usage — picked from connection + data freshness.
    update_view_state();
}

void ui_update_battery(int percent, bool charging) {
    batt_pct_cached = percent;          // -1 = the PMU sees no cell
    batt_charging_cached = charging;
    refresh_battery_glyph();
}

// ======== Companion / Stats ========

void ui_companion_update(const CompanionData* cc) {
    if (!cc || !cc->present) return;
    const uint32_t now = lv_tick_get();
    const uint8_t prev = cc_seen ? cc_cur.state : (uint8_t)CC_NONE;
    const bool was_attention = cc_seen && cc_is_attention(prev) && cc_cur.sessions > 0;
    cc_cur = *cc;
    cc_at_ms = now;
    cc_seen = true;
    const uint8_t st = cc_cur.state;
    const bool attention = cc_is_attention(st) && cc_cur.sessions > 0;
    // Claude needs you (or finished a long turn): glow, chime, the mascot jumps
    // and waves — until you tap, or the state moves on. Everything else shows
    // in the ticker only (see ui_tick_anim).
    if (attention && !was_attention) {
        preview_until_ms = 0;
        alert_start(st == CC_ATTENTION ? 0 : 1);
    } else if (!attention && alert_active && !preview_until_ms) {
        alert_stop();
    }
}

void ui_stats_update(const StatsData* st) {
    if (!st || !st->present) return;
    st_cur = *st;
    st_seen = true;
    render_stats_page();
}

// ---- Glissement entre comptes ----------------------------------------------
//
// Each account behaves like a page of its own, without duplicating the
// widgets: they take tens of KB and exist only once. So the content is
// translated off screen, the numbers are swapped, and it comes back in from the
// other edge. It looks like a real slide; in memory nothing moved.
//
// The translation is a style, not a position: it does not interfere with the
// placement of the elements, which stay aligned as the layout put them.

static int acct_anim_dir = 0;

static void acct_slide_exec(void* var, int32_t v) {
    lv_obj_set_style_translate_x((lv_obj_t*)var, v, 0);
}

static void acct_slide_in(lv_anim_t* a) {
    (void)a;
    if (!usage_group) return;
    // The content is out: swap the numbers while it is invisible, then bring it
    // back in from the opposite edge.
    Slot* s = slots_active();
    if (s) {
        ui_update(&s->data);
        render_title(true);
    }
    const int32_t from = acct_anim_dir > 0 ? L.scr_w : -L.scr_w;
    lv_obj_set_style_translate_x(usage_group, from, 0);
    lv_anim_t in;
    lv_anim_init(&in);
    lv_anim_set_var(&in, usage_group);
    lv_anim_set_values(&in, from, 0);
    lv_anim_set_duration(&in, 160);
    lv_anim_set_exec_cb(&in, acct_slide_exec);
    lv_anim_set_path_cb(&in, lv_anim_path_ease_out);
    lv_anim_start(&in);
}

// Next/previous account page. No effect below two accounts.
// Called by the horizontal swipe and by the "account" serial command.
bool ui_cycle_account(int dir) {
    if (slots_count() < 2) return false;
    if (!slots_cycle(dir)) return false;
    Slot* s = slots_active();
    if (!s) return false;

    Serial.printf("slots: page active -> %s (%d/%d)\n",
                  s->id, slots_active_index() + 1, slots_count());

    if (!usage_group) {                 // not built yet: plain switch
        ui_update(&s->data);
        render_title(true);
        return true;
    }
    // Slide out in the direction of the finger, back in from the opposite edge.
    acct_anim_dir = dir;
    lv_anim_delete(usage_group, acct_slide_exec);
    lv_anim_t out;
    lv_anim_init(&out);
    lv_anim_set_var(&out, usage_group);
    lv_anim_set_values(&out, 0, dir > 0 ? -L.scr_w : L.scr_w);
    lv_anim_set_duration(&out, 140);
    lv_anim_set_exec_cb(&out, acct_slide_exec);
    lv_anim_set_path_cb(&out, lv_anim_path_ease_in);
    lv_anim_set_completed_cb(&out, acct_slide_in);
    lv_anim_start(&out);
    return true;
}

void ui_show_level_page(int page) {
    ui_show_screen(SCREEN_USAGE);
    show_level_page(page);
}

void ui_debug_tap(void) {
    global_click_cb(nullptr);       // the very path a finger takes
}

bool ui_alert_active(void) { return alert_active || preview_until_ms != 0; }

// ---- WiFi setup card ---------------------------------------------------------
// Shown while the device is running its own hotspot: everything you need to get
// it onto the house network, on the device itself. The key is deliberately on
// screen — the access point is WPA2, so the house password never crosses the
// air in the clear, and the only way to learn the key is to be looking at it.

bool ui_setup_showing(void) { return setup_root != nullptr; }

void ui_show_setup(const char* ssid, const char* status) {
    if (!ssid) {                                   // done: back to the normal screens
        if (setup_root) { lv_obj_delete(setup_root); setup_root = nullptr; }
        return;
    }
    if (!setup_root) {
        // The boot splash and the header mascot are canvases blitted straight to
        // the panel, underneath LVGL's compositor — z-order cannot hold them
        // back, so they tore through this card. Stop them, and park the card on
        // the top layer so nothing LVGL draws can cover it either.
        splash_actor_stop();
        splash_mascot_set_visible(false);
        splash_hide();
        setup_root = make_group_sized(lv_layer_top(), 0, 0, L.scr_w, L.scr_h);
        lv_obj_set_style_bg_color(setup_root, COL_BG, 0);
        lv_obj_set_style_bg_opa(setup_root, LV_OPA_COVER, 0);

        lv_obj_t* head = lv_label_create(setup_root);
        lv_label_set_text(head, "Set me up");
        lv_obj_set_style_text_font(head, L.title_font, 0);
        lv_obj_set_style_text_color(head, COL_TEXT, 0);
        lv_obj_align(head, LV_ALIGN_TOP_MID, 0, L.scr_h / 6);

        lv_obj_t* hint = lv_label_create(setup_root);
        lv_label_set_text(hint, "Join this network from your phone");
        lv_obj_set_style_text_font(hint, L.reset_font, 0);
        lv_obj_set_style_text_color(hint, COL_DIM, 0);
        lv_obj_align_to(hint, head, LV_ALIGN_OUT_BOTTOM_MID, 0, 10);

        setup_ssid = lv_label_create(setup_root);
        lv_obj_set_style_text_font(setup_ssid, L.pill_font, 0);
        lv_obj_set_style_text_color(setup_ssid, COL_ACCENT, 0);
        lv_obj_align(setup_ssid, LV_ALIGN_CENTER, 0, -6);

        setup_pass = lv_label_create(setup_root);
        lv_label_set_text(setup_pass, "no password needed");
        lv_obj_set_style_text_font(setup_pass, L.reset_font, 0);
        lv_obj_set_style_text_color(setup_pass, COL_DIM, 0);
        lv_obj_align(setup_pass, LV_ALIGN_CENTER, 0, 44);

        setup_stat = lv_label_create(setup_root);
        lv_obj_set_style_text_font(setup_stat, L.reset_font, 0);
        lv_obj_set_style_text_color(setup_stat, COL_DIM, 0);
        lv_obj_set_width(setup_stat, L.content_w);
        lv_obj_set_style_text_align(setup_stat, LV_TEXT_ALIGN_CENTER, 0);
        lv_obj_align(setup_stat, LV_ALIGN_BOTTOM_MID, 0, -L.scr_h / 8);
    }
    if (strcmp(lv_label_get_text(setup_ssid), ssid) != 0) lv_label_set_text(setup_ssid, ssid);
    if (strcmp(lv_label_get_text(setup_stat), status ? status : "") != 0)
        lv_label_set_text(setup_stat, status ? status : "");   // rewriting every tick flickers
}

void ui_preview_alert(void) {
    preview_until_ms = lv_tick_get() + 6000;
    alert_start(0);
}

