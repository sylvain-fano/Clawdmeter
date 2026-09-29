#include <Arduino.h>
#include <Wire.h>
#include <lvgl.h>
#include <ArduinoJson.h>
#include <esp_heap_caps.h>

#include "data.h"
#include "ui.h"
#include "ble.h"
#include "splash.h"
#include "slots.h"
#include "usage_rate.h"
#include "idle.h"
#include "idle_cfg.h"
#include "brightness.h"
#include "settings.h"

#include "hal/board_caps.h"
#include "hal/display_hal.h"
#include "hal/touch_hal.h"
#include "hal/input_hal.h"
#include "hal/power_hal.h"
#ifdef CLAWD_LINK_WIFI
#include "link_wifi.h"
#endif
#include <Preferences.h>
#include <esp_system.h>

static void print_boot_record(void);

#include "hal/imu_hal.h"
#include "hal/sound_hal.h"

static UsageData     usage = {};
static CompanionData companion = {};
static StatsData     stats = {};

// ---- LVGL draw buffers (partial render mode) ----
// PSRAM-equipped boards (S3) use two 40-line strips. PSRAM-free boards (the
// ESP32-C6) render from internal SRAM with two 60-line strips (57.6 KB each).
// Two matters: the board's display HAL may hand a strip to DMA and return at
// once, so LVGL renders the next strip while the previous one is still being
// pushed to the panel (it waits through display_hal_wait() only right before
// the next flush). Tall strips matter too — every strip costs another walk of
// the whole object tree, and the software renderer's style lookups dominate.
// Measured on the C6 for a full transition frame: 24 short strips → 95 ms;
// 4 tall synchronous strips → ~40 ms; overlapped strips → see `stats`.
#ifdef BOARD_HAS_PSRAM
#define BUF_LINES 40
#define LV_BUF_CAPS (MALLOC_CAP_SPIRAM)
#else
#define BUF_LINES 60
#define LV_BUF_CAPS (MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT)
#endif
static uint16_t* buf1 = nullptr;
static uint16_t* buf2 = nullptr;

static uint32_t my_tick(void) { return millis(); }

// ---- Render statistics (serial "stats") ----
// Frames = LVGL refresh cycles that actually drew something; ms = render +
// flush wall time per frame. The C6 renders in software into small strips and
// flushes each over a blocking QSPI write, so this is THE number to watch
// when a full-screen transition feels choppy.
static uint32_t stat_frames = 0, stat_ms_acc = 0, stat_ms_max = 0, stat_frame_start = 0;
static uint32_t stat_flush_us = 0;           // time spent inside the panel flush
static void refr_start_cb(lv_event_t* e) { (void)e; stat_frame_start = millis(); }
static void refr_ready_cb(lv_event_t* e) {
    (void)e;
    const uint32_t d = millis() - stat_frame_start;
    if (d < 2) return;                       // idle refresh, nothing drawn
    stat_frames++;
    stat_ms_acc += d;
    if (d > stat_ms_max) stat_ms_max = d;
}
static void print_stats(void) {
    Serial.printf("stats: frames=%lu avg=%lums (flush %lums) max=%lums free=%u\n",
        (unsigned long)stat_frames,
        (unsigned long)(stat_frames ? stat_ms_acc / stat_frames : 0),
        (unsigned long)(stat_frames ? stat_flush_us / 1000 / stat_frames : 0),
        (unsigned long)stat_ms_max, (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL));
    stat_frames = stat_ms_acc = stat_ms_max = stat_flush_us = 0;
}

// Hand the strip to the board. The HAL may return before the pixels are on
// the panel (DMA); LVGL then keeps rendering into the other buffer and calls
// my_flush_wait_cb() before it needs this one again. No lv_display_flush_ready()
// here — LVGL clears the flushing flag itself after the wait callback returns.
static void my_flush_cb(lv_display_t* disp, const lv_area_t* area, uint8_t* px_map) {
    (void)disp;
    int32_t w = area->x2 - area->x1 + 1;
    int32_t h = area->y2 - area->y1 + 1;
    const uint32_t t0 = micros();
    display_hal_draw_bitmap(area->x1, area->y1, w, h, (uint16_t*)px_map);
    stat_flush_us += micros() - t0;
}

static void my_flush_wait_cb(lv_display_t* disp) {
    (void)disp;
    const uint32_t t0 = micros();
    display_hal_wait();
    stat_flush_us += micros() - t0;
}

static void rounder_cb(lv_event_t* e) {
    lv_area_t* area = (lv_area_t*)lv_event_get_param(e);
    display_hal_round_area(&area->x1, &area->y1, &area->x2, &area->y2);
}

// Touch policy is driven by IDLE_WAKE_ON_TOUCH:
//   true  → a press edge while asleep wakes the device and the first touch is
//           swallowed (mirrors the button wake-consumption); a press while
//           awake counts as activity.
//   false → touch never counts as activity and is fully swallowed while the
//           panel is dark, so pets/sleeves can't wake it overnight and LVGL
//           can't quietly toggle splash<->usage on a black panel.
static void my_touch_cb(lv_indev_t* indev, lv_indev_data_t* data) {
    uint16_t x, y;
    bool pressed;
    touch_hal_read(&x, &y, &pressed);
    const bool raw_pressed = pressed;

    if (IDLE_WAKE_ON_TOUCH) {
        static bool touch_was = false;
        static bool touch_wake_swallowed = false;
        if (raw_pressed && !touch_was) {
            // Press edge — consume as wake if asleep.
            if (idle_consume_wake_press()) {
                touch_wake_swallowed = true;
                pressed = false;
            }
        } else if (!raw_pressed && touch_was) {
            // Release edge.
            if (touch_wake_swallowed) {
                touch_wake_swallowed = false;
                pressed = false;
            }
        } else if (raw_pressed && touch_wake_swallowed) {
            // Held finger through wake — keep hiding until release.
            pressed = false;
        }
        touch_was = raw_pressed;
    } else if (idle_is_asleep()) {
        pressed = false;
    }

    if (pressed) {
        data->point.x = x;
        data->point.y = y;
        data->state = LV_INDEV_STATE_PRESSED;
    } else {
        data->state = LV_INDEV_STATE_RELEASED;
    }
}

// Companion ("cc") and Stats ("st") ride along on any payload — either may
// arrive alone, without usage numbers. Both are applied independently of the
// usage keys (see loop()).
static void parse_companion(JsonDocument& doc, CompanionData* cc, StatsData* st) {
    cc->present = doc["cc"].is<JsonObject>();
    if (cc->present) {
        JsonObject o = doc["cc"];
        int st = o["s"] | 0;
        cc->state = (st >= 0 && st < CC_STATE_COUNT) ? (uint8_t)st : (uint8_t)CC_NONE;
        cc->sessions  = (uint8_t)(o["n"] | 0);
        cc->attention = (uint8_t)(o["a"] | 0);
        cc->agents    = (uint8_t)(o["g"] | 0);
        cc->elapsed_s = o["e"] | 0;
        strlcpy(cc->label,   o["l"] | "", sizeof(cc->label));
        strlcpy(cc->project, o["p"] | "", sizeof(cc->project));
        strlcpy(cc->model,   o["m"] | "", sizeof(cc->model));
        strlcpy(cc->host,    o["h"] | "", sizeof(cc->host));
    }
    st->present = doc["st"].is<JsonObject>();
    if (st->present) {
        JsonObject o = doc["st"];
        st->sessions    = o["se"] | 0UL;
        st->messages    = o["me"] | 0UL;
        st->tokens      = (uint64_t)(o["tk"] | 0.0);      // may exceed 32 bits
        st->active_days = (uint16_t)(o["ad"] | 0);
        st->streak      = (uint16_t)(o["cs"] | 0);
        st->best_streak = (uint16_t)(o["ls"] | 0);
        st->peak_hour   = (int8_t)(o["ph"] | -1);
        strlcpy(st->model, o["fm"] | "", sizeof(st->model));
        const char* hm = o["hm"] | "";
        strlcpy(st->heat, hm, sizeof(st->heat));
        st->days = (uint16_t)strlen(st->heat);
    }
}

// Fills a UsageData from a JSON object. The source is either the payload root
// or an entry of the "acc" array when the hub publishes several accounts: both
// carry exactly the same keys.
template <typename SrcT>
static void parse_usage_fields(SrcT doc, UsageData* out) {
    out->has_usage = !doc["ok"].isNull();   // a companion-only beat has no usage keys at all

    out->session_pct = doc["s"] | 0.0f;
    out->session_reset_mins = doc["sr"] | -1;
    out->weekly_pct = doc["w"] | 0.0f;
    out->weekly_reset_mins = doc["wr"] | -1;
    // Weekly scoped-model limits ("ws": [{"n":"Fable","p":4}, ...]). Absent
    // key (no scoped limits / old daemon) → count 0 and the Weekly card keeps
    // its single bar; 0% is a real reading, never a "hidden" sentinel.
    out->scoped_weekly_count = 0;
    for (JsonObjectConst lim : doc["ws"].template as<JsonArrayConst>()) {
        if (out->scoped_weekly_count >= MAX_SCOPED_WEEKLY) break;
        const char* n = lim["n"] | "";
        if (!n[0]) continue;
        ScopedWeekly& sw = out->scoped_weekly[out->scoped_weekly_count++];
        strlcpy(sw.name, n, sizeof(sw.name));
        sw.pct = lim["p"] | 0.0f;
    }
    strlcpy(out->status, doc["st"] | "unknown", sizeof(out->status));
    out->chime = doc["c"] | false;   // absent (old daemon / chime off) → stay silent
    const char* acct = doc["acct"] | "pro";
    out->enterprise = (strcmp(acct, "ent") == 0);
    out->time_pct = doc["tp"] | 0;
    out->period_days = doc["pd"] | 30;
    strlcpy(out->reset_date, doc["rd"] | "", sizeof(out->reset_date));
    out->host_batt_pct = doc["hb"] | -1;   // the host's battery for the header glyph (absent = none)
    out->host_batt_charging = (int)(doc["hc"] | 0) != 0;
    out->clock_epoch = doc["t"] | 0L;
    out->clock_fmt = doc["tf"] | 24;
    out->ok = doc["ok"] | false;
    out->valid = true;
}

// Parse a JSON line into UsageData (+ the companion/trend extras).
static bool parse_json(const char* json, UsageData* out, CompanionData* cc, StatsData* st) {
    JsonDocument doc;
    DeserializationError err = deserializeJson(doc, json);
    if (err) {
        Serial.printf("JSON parse error: %s\n", err.c_str());
        return false;
    }
    parse_companion(doc, cc, st);
    parse_usage_fields(doc, out);

    // Multi-account: the hub publishes one entry per account under "acc", each
    // tagged with an "id". Every account is filed in its own page; the root
    // stays the active account, which keeps the single-account path unchanged.
    JsonArrayConst accounts = doc["acc"].as<JsonArrayConst>();
    if (!accounts.isNull() && accounts.size() > 0) {
        for (JsonObjectConst a : accounts) {
            const char* id = a["id"] | "";
            bool is_new = false;
            int idx = slots_upsert(id, &is_new);
            Slot* sl = slots_at(idx);
            if (!sl) continue;
            parse_usage_fields(a, &sl->data);
            sl->ok       = sl->data.ok;
            sl->received = true;
            sl->last_ms  = lv_tick_get();
            if (is_new) Serial.printf("slots: new page '%s' (%d total)\n",
                                      sl->id, slots_count());
        }
    }
    return true;
}

// ---- Serial command buffer ----
#define CMD_BUF_SIZE 64
static char cmd_buf[CMD_BUF_SIZE];
static int cmd_pos = 0;

static void send_screenshot() {
#ifndef BOARD_HAS_PSRAM
    // A full RGB565 framebuffer doesn't fit in internal SRAM on PSRAM-free
    // boards (e.g. 480×480×2 = 460 KB). Capture is unsupported there.
    Serial.println("SCREENSHOT_UNSUPPORTED");
    return;
#else
    const uint32_t w = board_caps().width;
    const uint32_t h = board_caps().height;
    const uint32_t row_bytes = w * 2;
    const uint32_t buf_size = row_bytes * h;
    uint8_t* sbuf = (uint8_t*)heap_caps_malloc(buf_size, MALLOC_CAP_SPIRAM);
    if (!sbuf) {
        Serial.println("SCREENSHOT_ERR");
        return;
    }

    lv_draw_buf_t draw_buf;
    lv_draw_buf_init(&draw_buf, w, h, LV_COLOR_FORMAT_RGB565, row_bytes, sbuf, buf_size);

    lv_result_t res = lv_snapshot_take_to_draw_buf(lv_screen_active(), LV_COLOR_FORMAT_RGB565, &draw_buf);
    if (res != LV_RESULT_OK) {
        heap_caps_free(sbuf);
        Serial.println("SCREENSHOT_ERR");
        return;
    }

    Serial.printf("SCREENSHOT_START %lu %lu %lu\n",
        (unsigned long)w, (unsigned long)h, (unsigned long)buf_size);
    Serial.flush();
    Serial.write(sbuf, buf_size);
    Serial.flush();
    Serial.println();
    Serial.println("SCREENSHOT_END");
    heap_caps_free(sbuf);
#endif
}

static void check_serial_cmd() {
    while (Serial.available()) {
        char c = Serial.read();
        if (c == '\n' || c == '\r') {
            cmd_buf[cmd_pos] = '\0';
            if (strcmp(cmd_buf, "screenshot") == 0) send_screenshot();
            else if (strcmp(cmd_buf, "buzz") == 0)  sound_hal_play_reset();
            // "alert [0|1]" plays the companion chime; "preview" runs the whole
            // needs-you alert (glow + mascot + chime); "cc <state> [label]" fakes a
            // companion payload (states: 0 none 1 idle 2 thinking 3 tool 4 done
            // 5 needs-you 6 error 7 compacting 8 turn-done); "trend demo" fakes a
            // week of history.
            else if (strncmp(cmd_buf, "alert", 5) == 0) sound_hal_play_alert(atoi(cmd_buf + 5));
            // "volume N" sets + saves the loudness; "corners on|off" shows the
            // corner-radius calibration outlines; "radius N" moves the glow's
            // corner radius live (0 = square) until the next boot.
            else if (strncmp(cmd_buf, "volume ", 7) == 0) {
                const int v = atoi(cmd_buf + 7);
                settings_set_volume((uint8_t)(v < 0 ? 0 : v > 100 ? 100 : v));
                sound_hal_set_volume(settings_get().volume);
                ui_refresh_settings();
                Serial.printf("volume -> %u%%\n", settings_get().volume);
            }
            else if (strncmp(cmd_buf, "corners", 7) == 0) ui_debug_corners(strstr(cmd_buf, "off") == nullptr);
            // "render plain|swapped": LVGL renders RGB565 and the HAL swaps on flush, or
            // LVGL renders bus order directly. "dma on|off": async DMA vs the library
            // path. A/B tools for pixel artifacts on a board with no screenshot.
            else if (strncmp(cmd_buf, "render ", 7) == 0) {
                const bool plain = strcmp(cmd_buf + 7, "plain") == 0;
                lv_display_t* d = lv_display_get_default();
                display_hal_wait();
                display_hal_set_swap_on_flush(plain);
                lv_display_set_color_format(d, (plain || !board_caps().be_pixels) ? LV_COLOR_FORMAT_RGB565
                                                                                   : LV_COLOR_FORMAT_RGB565_SWAPPED);
                lv_obj_invalidate(lv_screen_active());
                Serial.printf("render -> %s\n", plain ? "plain (HAL swaps, the default)" : "swapped (LVGL renders bus order)");
            }
            else if (strncmp(cmd_buf, "dma ", 4) == 0) {
                const bool on = strcmp(cmd_buf + 4, "on") == 0;
                display_hal_set_async(on);
                lv_obj_invalidate(lv_screen_active());
                Serial.printf("dma -> %s\n", on ? "async" : "library path");
            }
            else if (strncmp(cmd_buf, "radius ", 7) == 0) { ui_debug_glow_radius(atoi(cmd_buf + 7)); Serial.printf("glow radius -> %d\n", atoi(cmd_buf + 7)); }
            else if (strcmp(cmd_buf, "preview") == 0)   ui_preview_alert();
            else if (strncmp(cmd_buf, "cc ", 3) == 0) {
                CompanionData fake = {};
                fake.present = true;
                int st = atoi(cmd_buf + 3);
                fake.state = (st >= 0 && st < CC_STATE_COUNT) ? (uint8_t)st : 0;
                fake.sessions = fake.state ? 1 : 0;
                fake.attention = (fake.state == CC_ATTENTION || fake.state == CC_TURN_DONE) ? 1 : 0;
                const char* sp = strchr(cmd_buf + 3, ' ');
                strlcpy(fake.label, sp ? sp + 1 : "Serial test", sizeof(fake.label));
                strlcpy(fake.project, "Clawdmeter", sizeof(fake.project));
                strlcpy(fake.model, "Fable 5.1", sizeof(fake.model));
                ui_companion_update(&fake);
                Serial.printf("cc -> state %d '%s'\n", fake.state, fake.label);
            }
            else if (strcmp(cmd_buf, "demo stats") == 0) {
                StatsData fake = {};
                fake.present = true;
                fake.sessions = 21; fake.messages = 32384; fake.tokens = 35200000ULL;
                fake.active_days = 19; fake.streak = 5; fake.best_streak = 8; fake.peak_hour = 23;
                strlcpy(fake.model, "Fable 5", sizeof(fake.model));
                static const char* HM = "000000000000000000000000000000000000000000000000000000000000000000000000000000000000"
                                        "0000000000000000000000000000100000000000000000000000000002000000000000000000000000"
                                        "3241004312xxxx";
                strlcpy(fake.heat, HM, sizeof(fake.heat));
                fake.days = (uint16_t)strlen(fake.heat);
                ui_stats_update(&fake);
                Serial.println("stats -> demo data");
            }
            // "page splash|usage|settings|settings2|about" and "flip" — drive
            // the screens over serial (QA on boards without the framebuffer
            // screenshot).
            else if (strncmp(cmd_buf, "page ", 5) == 0) {
                const char* p = cmd_buf + 5;
                if      (strcmp(p, "splash") == 0)    ui_show_screen(SCREEN_SPLASH);
                else if (strcmp(p, "usage") == 0)     ui_show_screen(SCREEN_USAGE);
                else if (strcmp(p, "settings") == 0)  ui_show_settings_page(0);
                else if (strcmp(p, "settings2") == 0) ui_show_settings_page(1);
                else if (strcmp(p, "settings3") == 0) ui_show_settings_page(2);
                else if (strcmp(p, "settings4") == 0) ui_show_settings_page(3);
                else if (strcmp(p, "settings5") == 0) ui_show_settings_page(4);
                else if (strcmp(p, "about") == 0)     ui_show_screen(SCREEN_ABOUT);
                else if (strcmp(p, "stats") == 0)     ui_show_level_page(0);
                Serial.printf("page -> %s\n", p);
            }
            else if (strcmp(cmd_buf, "flip") == 0) ui_flip_weekly_face();
            // "swipe up|down [ms]" runs the vertical transition; "stats" prints
            // and resets the frame counters (measure a transition's frame rate).
            else if (strncmp(cmd_buf, "swipe ", 6) == 0) {
                const char* a = cmd_buf + 6;
                int dir = (strncmp(a, "up", 2) == 0) ? +1 : (strncmp(a, "down", 4) == 0) ? -1 : 0;
                const char* sp = strchr(a, ' ');
                uint32_t ms = sp ? (uint32_t)atoi(sp + 1) : 0;
                if (dir) { stat_frames = stat_ms_acc = stat_ms_max = stat_flush_us = 0; ui_debug_swipe(dir, ms); }
                Serial.printf("swipe %s %lu\n", dir > 0 ? "up" : dir < 0 ? "down" : "?", (unsigned long)ms);
            }
            else if (strcmp(cmd_buf, "stats") == 0) print_stats();
#ifdef CLAWD_LINK_WIFI
            else if (strcmp(cmd_buf, "wifi") == 0) link_wifi_status();
            // "account" cycles to the next account page, to check the switch
            // without a finger (boards with no screenshot path only have serial).
            else if (strcmp(cmd_buf, "account") == 0) {
                if (!ui_cycle_account(+1))
                    Serial.printf("account: %d page(s), rien a faire\n", slots_count());
            }
            else if (strncmp(cmd_buf, "wifi ", 5) == 0) {
                char* ssid = cmd_buf + 5;
                char* sp = strchr(ssid, ' ');          // SSID, space, then the rest is the key
                if (!sp) Serial.println("usage: wifi <ssid> <password>");
                else { *sp = '\0'; link_wifi_set_credentials(ssid, sp + 1); }
            }
#endif
            else if (strcmp(cmd_buf, "tap") == 0) {
                const bool a0 = ui_alert_active();
                const bool s0 = ui_get_current_screen() == SCREEN_SPLASH;
                ui_debug_tap();
                Serial.printf("tap: alert %d->%d, splash %d->%d\n", (int)a0, (int)ui_alert_active(),
                              (int)s0, (int)(ui_get_current_screen() == SCREEN_SPLASH));
            }
            else if (strcmp(cmd_buf, "power") == 0) { power_hal_debug_dump(); print_boot_record(); }
            cmd_pos = 0;
        } else if (cmd_pos < CMD_BUF_SIZE - 1) {
            cmd_buf[cmd_pos++] = c;
        }
    }
}

// Each board provides this. Must bring up the shared I2C bus (Wire.begin
// with the board's SDA/SCL pins) and any board-private hardware that has
// to settle before display/touch (e.g. an IO expander gating the LCD
// reset line). Called exactly once at the start of setup().
extern "C" void board_init(void);

void setup() {
    Serial.begin(115200);
    delay(300);
    Serial.println("{\"ready\":true}");

    board_init();

    display_hal_init();
    display_hal_begin();
    idle_init();        // takes over panel brightness and starts the idle timer
    brightness_init();  // load the user's saved brightness level and apply via idle
    settings_init();    // user prefs (clock, header icons, sleep timeout) — pushes the idle timeout

    power_hal_init();
    imu_hal_init();
    sound_hal_init();
    sound_hal_set_volume(settings_get().volume);   // the codec is up; push the saved loudness
    touch_hal_init();

    // ---- LVGL ----
    const int W = board_caps().width;
    const int H = board_caps().height;

    lv_init();
    lv_tick_set_cb(my_tick);

    buf1 = (uint16_t*)heap_caps_malloc(W * BUF_LINES * 2, LV_BUF_CAPS);
    buf2 = (uint16_t*)heap_caps_malloc(W * BUF_LINES * 2, LV_BUF_CAPS);
    if (!buf1 || !buf2) Serial.println("LVGL: draw buffer alloc FAILED");

    lv_display_t* disp = lv_display_create(W, H);
    // Render straight into the byte order the panel bus wants (see BoardCaps).
    lv_display_set_color_format(disp, board_caps().be_pixels ? LV_COLOR_FORMAT_RGB565_SWAPPED
                                                             : LV_COLOR_FORMAT_RGB565);
    lv_display_set_flush_cb(disp, my_flush_cb);
    lv_display_set_flush_wait_cb(disp, my_flush_wait_cb);
    lv_display_set_buffers(disp, buf1, buf2, W * BUF_LINES * 2,
                           LV_DISPLAY_RENDER_MODE_PARTIAL);
    lv_display_add_event_cb(disp, rounder_cb, LV_EVENT_INVALIDATE_AREA, NULL);
    lv_display_add_event_cb(disp, refr_start_cb, LV_EVENT_REFR_START, NULL);
    lv_display_add_event_cb(disp, refr_ready_cb, LV_EVENT_REFR_READY, NULL);

    lv_indev_t* indev = lv_indev_create();
    lv_indev_set_type(indev, LV_INDEV_TYPE_POINTER);
    lv_indev_set_read_cb(indev, my_touch_cb);

    ble_init();
    input_hal_init();

    ui_init();
    ui_update_ble_status(ble_get_state(), ble_get_device_name(), ble_get_mac_address());
    ui_update_battery(power_hal_battery_pct(), power_hal_is_charging());
    ui_show_screen(SCREEN_SPLASH);

    // Memory headroom after every widget exists — the PSRAM-less C6 is the
    // board that would run out first, so make it visible on every boot.
    lv_mem_monitor_t mon;
    lv_mem_monitor(&mon);
    Serial.printf("Dashboard ready (%s, %dx%d), waiting for data on BLE... "
                  "free heap %u B, LVGL pool %u%% used\n",
        board_caps().name, W, H,
        (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL), (unsigned)mon.used_pct);
    print_boot_record();
}

// ---- Boot record -------------------------------------------------------------
// The board has no battery: every power hiccup is a full restart, and the
// interesting ones happen while it is plugged into a charger with no serial
// console attached. So each boot is counted in NVS together with why it
// started, and the tally is printed on the next boot (and by `power`). A run
// of power-on resets with no unplugging is the signature of a supply that
// cannot hold the rail up.
static uint32_t boot_count = 0;
static const char* boot_reason_name = "?";

static const char* reset_reason_str(esp_reset_reason_t r) {
    switch (r) {
    case ESP_RST_POWERON:   return "power-on";     // rail came up (or collapsed and returned)
    case ESP_RST_EXT:       return "reset pin";
    case ESP_RST_SW:        return "software";
    case ESP_RST_PANIC:     return "panic";
    case ESP_RST_INT_WDT:   return "int watchdog";
    case ESP_RST_TASK_WDT:  return "task watchdog";
    case ESP_RST_WDT:       return "watchdog";
    case ESP_RST_DEEPSLEEP: return "deep sleep";
    case ESP_RST_BROWNOUT:  return "BROWNOUT";     // the MCU's own detector saw the rail fall
    case ESP_RST_SDIO:      return "sdio";
    default:                return "unknown";
    }
}

static void print_boot_record(void) {
    static bool loaded = false;
    if (!loaded) {
        loaded = true;
        boot_reason_name = reset_reason_str(esp_reset_reason());
        Preferences p;
        if (p.begin("clawdboot", false)) {
            boot_count = p.getULong("n", 0) + 1;
            p.putULong("n", boot_count);
            p.end();
        }
    }
    Serial.printf("boot: #%lu, reason=%s, uptime %lus\n",
                  (unsigned long)boot_count, boot_reason_name, (unsigned long)(millis() / 1000));
}

static ble_state_t last_ble_state = BLE_STATE_INIT;

// Hold-to-pair gesture: hold the PWR button ~3s, then RELEASE → clear all BLE
// bonds and re-advertise. Clearing on *release* (not while held) is deliberate:
// holding to power the device OFF (AXP hardware shutdown at 8s) must not wipe
// the bond — a power-off hold never releases before shutdown. To stop a
// "chicken-out" release just before 8s from pairing, the gesture disarms at 6s.
//
//   ~1.5s long-press edge → PENDING
//   3.0s (+1500)          → ARMED   (release from here clears bonds)
//   6.0s (+4500)          → DISARMED (no clear; AXP powers off at 8s)
#define PAIR_ARM_AFTER_LONG_MS    1500   // 3.0s total
#define PAIR_DISARM_AFTER_LONG_MS 4500   // 6.0s total
enum pair_state_t { PAIR_IDLE, PAIR_PENDING, PAIR_ARMED };
static pair_state_t pair_state        = PAIR_IDLE;
static uint32_t     pair_long_seen_ms = 0;

static void pair_tick(void) {
    if (pair_state == PAIR_IDLE && power_hal_pwr_long_pressed()) {
        pair_state = PAIR_PENDING;
        pair_long_seen_ms = millis();
        (void)power_hal_pwr_released();  // drain any stale release edge
        Serial.println("PWR long-press: hold to ~3s then release to pair");
        return;
    }
    if (pair_state == PAIR_IDLE) return;

    if (power_hal_pwr_released()) {
        if (pair_state == PAIR_ARMED) {
            Serial.println("Pair: released in window — clearing bonds, advertising");
            ble_clear_bonds();
        } else {
            Serial.println("Pair: released too early — cancelled");
        }
        pair_state = PAIR_IDLE;
        return;
    }

    uint32_t held = millis() - pair_long_seen_ms;
    if (pair_state == PAIR_PENDING && held >= PAIR_ARM_AFTER_LONG_MS) {
        pair_state = PAIR_ARMED;
        Serial.println("Pair: armed — release to pair");
    } else if (pair_state == PAIR_ARMED && held >= PAIR_DISARM_AFTER_LONG_MS) {
        pair_state = PAIR_IDLE;  // power-off territory; don't pair
        Serial.println("Pair: disarmed (holding toward power-off)");
    }
}

void loop() {
    idle_tick();
    lv_timer_handler();
    ui_tick_anim();
    ble_tick();
    power_hal_tick();
    imu_hal_tick();
    sound_hal_tick();
    splash_tick();
    splash_mascot_tick();
    if (!ui_setup_showing()) splash_actor_tick();   // the card owns the panel while it is up
    // Rotation transition (blank + ramp) would fight the idle fade — skip
    // ticks while the panel is dark. A rotation that happens during sleep
    // is detected by the next tick after wake and ramped in then.
    if (!idle_is_asleep()) display_hal_tick();

    // ---- Physical buttons ----
    //   PRIMARY   → HID Space  (Claude Code voice-mode PTT)
    //   SECONDARY → HID Shift+Tab  (mode toggle; only if the board has one)
    //   PWR       → on splash: cycle animations; on usage: cycle brightness;
    //               hold ~3s + release: pairing mode
    // First press from sleep is consumed as a wake-only event by
    // idle_consume_wake_press(); the normal action fires from the second
    // press. Activity bookkeeping happens inside idle_consume_wake_press
    // so no separate idle_note_activity() call is needed here.
#ifdef CLAWD_LINK_WIFI
    // Both buttons held together for ten seconds forces the WiFi setup hotspot,
    // whatever state the device is in. Ten seconds and two buttons is long
    // enough that it cannot happen by accident, and it is the way back when the
    // screen cannot be reached (wrong network, no hub, moved house).
    {
        static uint32_t both_since = 0;
        const bool both = input_hal_is_held(INPUT_BTN_PRIMARY) && input_hal_is_held(INPUT_BTN_SECONDARY);
        const uint32_t now_ms = millis();
        if (!both) {
            both_since = 0;
        } else if (both_since == 0) {
            both_since = now_ms;
        } else if (now_ms - both_since >= 10000) {
            both_since = 0;
            Serial.println("buttons: both held 10s - forcing WiFi setup");
            link_wifi_forget_and_restart();
        }
    }
#endif

    {
        static bool primary_was = false;
        static bool primary_wake_swallowed = false;
        bool primary_now = input_hal_is_held(INPUT_BTN_PRIMARY);
        if (primary_now != primary_was) {
            if (primary_now) {
                if (idle_consume_wake_press()) primary_wake_swallowed = true;
                else                            ble_keyboard_press(0x2C, 0);  // HID Space, no mods
            } else {
                if (primary_wake_swallowed) primary_wake_swallowed = false;
                else                        ble_keyboard_release();
            }
            primary_was = primary_now;
        }

        if (board_caps().button_count >= 2) {
            static bool secondary_was = false;
            static bool secondary_wake_swallowed = false;
            bool secondary_now = input_hal_is_held(INPUT_BTN_SECONDARY);
            if (secondary_now != secondary_was) {
                if (secondary_now) {
                    if (idle_consume_wake_press()) secondary_wake_swallowed = true;
                    else                            ble_keyboard_press(0x2B, 0x02);  // HID Tab + LEFT_SHIFT
                } else {
                    if (secondary_wake_swallowed) secondary_wake_swallowed = false;
                    else                          ble_keyboard_release();
                }
                secondary_was = secondary_now;
            }
        }

        if (power_hal_pwr_pressed()) {
            if (!idle_consume_wake_press()) {
                // On splash: cycle animations. On the usage view: cycle
                // screen brightness (single non-splash view, no more screens).
                if (ui_get_current_screen() == SCREEN_SPLASH) splash_next();
                else { brightness_cycle(); ui_refresh_settings(); }
            }
        }

        pair_tick();
    }

    ble_state_t bs = ble_get_state();
    if (bs != last_ble_state) {
        last_ble_state = bs;
        ui_update_ble_status(bs, ble_get_device_name(), ble_get_mac_address());
    }

    static int  last_pct      = -2;
    static bool last_charging = false;
    int  pct      = power_hal_battery_pct();
    bool charging = power_hal_is_charging();
    if (pct != last_pct || charging != last_charging) {
        if (pct != last_pct) ble_set_battery_level(pct);
        last_pct = pct;
        last_charging = charging;
        ui_update_battery(pct, charging);
    }

    check_serial_cmd();

    if (ble_has_data()) {
        if (parse_json(ble_get_data(), &usage, &companion, &stats)) {
            if (!usage.has_usage) {
                // An extras-only beat (companion state or stats): no usage numbers to apply.
                if (companion.present) ui_companion_update(&companion);
                if (stats.present)     ui_stats_update(&stats);
                ble_send_ack();
                if (companion.present)
                    Serial.printf("companion: state=%d n=%d '%s' free=%u\n", companion.state,
                                  companion.sessions, companion.label,
                                  (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL));
                if (stats.present)
                    Serial.printf("stats: sessions=%lu messages=%lu days=%u heat=%u free=%u\n",
                                  (unsigned long)stats.sessions, (unsigned long)stats.messages,
                                  stats.active_days, stats.days,
                                  (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL));
                delay(5);
                return;
            }
            int g_before = usage_rate_group();
            bool session_reset = usage_rate_sample(usage.session_pct);
            int g_after = usage_rate_group();
            // 5-hour session limit refilled → chime so the user knows they can
            // use Claude again (no-op on boards without a buzzer). Gated on the
            // daemon's opt-in `chime` config; the `buzz` serial cmd ignores it.
            if (session_reset && usage.chime) {
                Serial.println("session reset detected — chime");
                sound_hal_play_reset();
            }
            if (g_after != g_before) {
                Serial.printf("usage rate: group %d -> %d (s=%.2f%%)\n",
                    g_before, g_after, usage.session_pct);
                if (splash_is_active()) splash_pick_for_current_rate();
            }
            // With several accounts the screen shows the page the user picked,
            // not the account the hub put at the root. With one account (or no
            // "acc") slots_active() is null and the root payload is used, as
            // it always was.
            {
                Slot* act = slots_count() > 0 ? slots_active() : nullptr;
                ui_update(act ? &act->data : &usage);
            }
            if (companion.present) ui_companion_update(&companion);
            if (stats.present)     ui_stats_update(&stats);
            ble_send_ack();
            // One line per payload so a serial log proves what the device saw
            // (boards without the framebuffer screenshot rely on this).
            Serial.printf("usage: s=%d%% w=%d%% scoped=%d%s%s%s clock=%s hostbatt=%d%s cc=%d/%d st=%d free=%u\n",
                (int)(usage.session_pct + 0.5f), (int)(usage.weekly_pct + 0.5f),
                usage.scoped_weekly_count,
                usage.scoped_weekly_count ? " (" : "",
                usage.scoped_weekly_count ? usage.scoped_weekly[0].name : "",
                usage.scoped_weekly_count ? ")" : "",
                usage.clock_epoch ? "yes" : "no",
                usage.host_batt_pct, usage.host_batt_charging ? "+" : "",
                companion.present ? companion.state : -1, companion.sessions, stats.present,
                (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL));
        } else {
            ble_send_nack();
        }
    }

    delay(5);
}
