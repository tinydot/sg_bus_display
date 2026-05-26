/*
 * Singapore Bus Arrival Display
 * Hardware: Waveshare ESP32-S3-RLCD-4.2 (ST7305, 400x300 mono)
 * Data:     LTA DataMall BusArrival v3
 * Graphics: LVGL v8 on Waveshare's DisplayPort driver
 *
 * Layout (75 / 25 split):
 *   Top 75 %   : Stop 59621 - buses 801, 803, 861 (stacked rows)
 *   Bottom 25 %: Stop 59181 - bus 811 (single compact inline row)
 *
 * Build notes:
 *  - Use the same Tools settings as Waveshare's 08_LVGL_V8_Test example
 *    that you confirmed works.
 *  - This sketch lives in your normal Arduino sketchbook
 *    (~/Documents/Arduino/sg_bus_display/), NOT inside the Waveshare
 *    repo. The display_bsp.h and lvgl_bsp.h headers come from the
 *    Waveshare example's local src/ folder, so we copy them in.
 *    See the README for the copy steps.
 *  - Refresh interval is 30s. The ST7305 isn't designed for fast
 *    repaints and LTA arrival data doesn't change faster than that
 *    anyway.
 */

#include <Arduino.h>
#include <WiFi.h>
#include <HTTPClient.h>
#include <WiFiClientSecure.h>
#include <ArduinoJson.h>
#include <time.h>
#include "esp_pm.h"

// Waveshare's RLCD + LVGL helpers (copied from their example into
// this sketch folder under src/app_bsp/ — see README).
#include "display_bsp.h"
#include "src/app_bsp/lvgl_bsp.h"

#include "secrets.h"

// ---------------- Types (declared early so Arduino's auto-generated
// function prototypes can see them) ----------------

struct StopConfig {
  const char* code;
  const char* label;          // shown as the stop header
  const char* services[6];    // null-terminated list, max 6
};

struct ServiceArrival {
  char service[6];   // e.g. "801"
  char iso1[32];     // last-known NextBus  ISO arrival, "" if never seen
  char iso2[32];     // last-known NextBus2 ISO arrival, "" if never seen
};

// Persisted across loop iterations. rows[] is populated once from
// StopConfig.services and never resized; fetchStop() only overwrites
// iso1/iso2 when the LTA response carries a non-empty value, so a
// missing service in one response keeps counting down from the last
// good fetch instead of flashing "--".
struct StopArrivals {
  ServiceArrival rows[6];
  int  count;
  bool ok;
  char error[48];
};

struct StopUi {
  lv_obj_t* header;
  lv_obj_t* badge[4];        // black-filled rectangle behind bus number
  lv_obj_t* svc_label[4];    // bus number (white-on-black)
  lv_obj_t* next_label[4];   // "next" minutes column
  lv_obj_t* after_label[4];  // "after" minutes column
};

// ---------------- User config ----------------

static const StopConfig STOP_A = {
  "59621",
  "Stop 59621",
  { "801", "803", "861", nullptr }
};

static const StopConfig STOP_B = {
  "59181",
  "Stop 59181",
  { "811", nullptr }
};

// How often to refresh arrivals, in milliseconds.
static const uint32_t REFRESH_MS = 30UL * 1000UL;

// ---------------- Display port (matches Waveshare example) ----------------
// Pins from Waveshare's 08_LVGL_V8_Test: SCK=12, MOSI=11, CS=5, DC=40, RST=41
DisplayPort RlcdPort(12, 11, 5, 40, 41, 400, 300);

// LVGL pushes a 16-bit colour buffer; the RLCD is 1-bit, so threshold
// to black/white the same way Waveshare's example does.
static void Lvgl_FlushCallback(lv_display_t* drv, const lv_area_t* area, uint8_t* color_map) {
  uint16_t* buffer = (uint16_t*)color_map;
  for (int y = area->y1; y <= area->y2; y++) {
    for (int x = area->x1; x <= area->x2; x++) {
      uint8_t color = (*buffer < 0x7fff) ? ColorBlack : ColorWhite;
      RlcdPort.RLCD_SetPixel(x, y, color);
      buffer++;
    }
  }
  RlcdPort.RLCD_Display();
  lv_disp_flush_ready(drv);
}

// ---------------- LVGL UI handles ----------------

static StopUi ui_a;
static StopUi ui_b;
static lv_obj_t* ui_footer;
static lv_obj_t* ui_status; // shown only when something is wrong

static lv_style_t style_header;
static lv_style_t style_row;
static lv_style_t style_small;
static lv_style_t style_badge;
static lv_style_t style_badge_text;

// ---------------- LTA fetch ----------------

// Parse ISO 8601 with timezone (e.g. 2026-05-22T14:46:27+08:00) and
// return whole minutes from now. -1 on failure, 0 if already due.
static int minutesUntil(const char* iso) {
  if (!iso || !*iso) return -1;

  int year, mon, day, hh, mm, ss;
  int tz_h = 0, tz_m = 0;
  char tz_sign = '+';

  int n = sscanf(iso, "%d-%d-%dT%d:%d:%d%c%d:%d",
                 &year, &mon, &day, &hh, &mm, &ss,
                 &tz_sign, &tz_h, &tz_m);
  if (n < 6) return -1;

  struct tm t = {};
  t.tm_year = year - 1900;
  t.tm_mon  = mon - 1;
  t.tm_mday = day;
  t.tm_hour = hh;
  t.tm_min  = mm;
  t.tm_sec  = ss;

  // We want to interpret the tm struct as UTC and convert to a UTC
  // time_t. mktime() always uses the current TZ env var, so we
  // temporarily force TZ=UTC, call mktime(), then restore SGT.
  // (timegm() exists in glibc but isn't reliably available in
  // ESP-IDF's newlib.)
  char* old_tz = getenv("TZ");
  String saved_tz = old_tz ? String(old_tz) : String("");
  setenv("TZ", "UTC0", 1);
  tzset();
  time_t arr = mktime(&t);
  if (saved_tz.length()) setenv("TZ", saved_tz.c_str(), 1);
  else                   unsetenv("TZ");
  tzset();

  // Now arr is the UTC time_t for the tm fields treated as UTC.
  // Subtract the timestamp's own offset to get the true UTC instant.
  if (n >= 9) {
    long tz_off = (long)tz_h * 3600 + (long)tz_m * 60;
    if (tz_sign == '-') tz_off = -tz_off;
    arr -= tz_off;
  }

  time_t now_utc = time(nullptr);
  if (now_utc < 1700000000) return -1; // clock not yet synced

  long diff = (long)(arr - now_utc);
  // A cached timestamp more than a minute in the past is stale — the
  // bus has long since left and LTA just hasn't given us a fresh value.
  // Treat it as no-data so the UI shows "--" rather than a stuck "Arr".
  if (diff < -60) return -1;
  if (diff < 0) diff = 0;
  return (int)((diff + 30) / 60);
}

static void initStopArrivals(StopArrivals& out, const StopConfig& cfg) {
  out.count = 0;
  out.ok = false;
  out.error[0] = '\0';
  for (int i = 0; cfg.services[i] != nullptr
                  && out.count < (int)(sizeof(out.rows) / sizeof(out.rows[0])); i++) {
    ServiceArrival& r = out.rows[out.count++];
    strncpy(r.service, cfg.services[i], sizeof(r.service) - 1);
    r.service[sizeof(r.service) - 1] = '\0';
    r.iso1[0] = '\0';
    r.iso2[0] = '\0';
  }
}

static bool fetchStop(const StopConfig& cfg, StopArrivals& out) {
  // Note: out.rows / out.count are preserved across calls so cached
  // ISO timestamps survive a failed or partial fetch. We only clear
  // the ok/error flags here.
  out.ok = false;
  out.error[0] = '\0';

  if (WiFi.status() != WL_CONNECTED) {
    strncpy(out.error, "No WiFi", sizeof(out.error));
    return false;
  }

  WiFiClientSecure client;
  client.setInsecure(); // not worth pinning the LTA cert for this
  HTTPClient http;

  String url = String("https://datamall2.mytransport.sg/ltaodataservice/v3/BusArrival?BusStopCode=") + cfg.code;
  http.setTimeout(8000);
  if (!http.begin(client, url)) {
    strncpy(out.error, "begin failed", sizeof(out.error));
    return false;
  }
  http.addHeader("AccountKey", LTA_ACCOUNT_KEY);
  http.addHeader("accept", "application/json");

  int code = http.GET();
  if (code != 200) {
    snprintf(out.error, sizeof(out.error), "HTTP %d", code);
    Serial.printf("[%s] HTTP %d (fail), rssi=%d, heap=%u\n",
                  cfg.code, code,
                  (int)WiFi.RSSI(), (unsigned)ESP.getFreeHeap());
    http.end();
    return false;
  }

  // Read response body as a string first. http.getStream() can deliver
  // chunked-transfer data that ArduinoJson chokes on with "InvalidInput".
  String payload = http.getString();
  http.end();

  // Trim any leading whitespace just in case.
  int firstBrace = payload.indexOf('{');
  if (firstBrace > 0) payload.remove(0, firstBrace);

  Serial.printf("[%s] HTTP %d, payload len=%d, rssi=%d, heap=%u\n",
                cfg.code, code, payload.length(),
                (int)WiFi.RSSI(), (unsigned)ESP.getFreeHeap());
  if (payload.length() < 200) {
    Serial.printf("[%s] payload: %s\n", cfg.code, payload.c_str());
  }

  // Parse the full response. It's ~2-3 KB which is comfortable on
  // the S3.
  JsonDocument doc;
  DeserializationError err = deserializeJson(doc, payload);

  if (err) {
    snprintf(out.error, sizeof(out.error), "JSON: %s", err.c_str());
    return false;
  }

  JsonArray services = doc["Services"].as<JsonArray>();
  Serial.printf("[%s] Services in response: %d\n", cfg.code, (int)services.size());
  for (JsonObject s : services) {
    const char* sn = s["ServiceNo"] | "";
    if (!*sn) continue;

    // Find the matching cached row for this configured service.
    ServiceArrival* row = nullptr;
    for (int i = 0; i < out.count; i++) {
      if (strcmp(out.rows[i].service, sn) == 0) { row = &out.rows[i]; break; }
    }
    if (!row) continue;

    const char* iso1 = s["NextBus"]["EstimatedArrival"]  | "";
    const char* iso2 = s["NextBus2"]["EstimatedArrival"] | "";

    // Only overwrite the cache when LTA gave us a real timestamp.
    // An empty string means "no info this poll" — keep the previous
    // value so the displayed minutes keep decrementing.
    if (*iso1) {
      strncpy(row->iso1, iso1, sizeof(row->iso1) - 1);
      row->iso1[sizeof(row->iso1) - 1] = '\0';
    }
    if (*iso2) {
      strncpy(row->iso2, iso2, sizeof(row->iso2) - 1);
      row->iso2[sizeof(row->iso2) - 1] = '\0';
    }
    Serial.printf("  %s: iso1='%s'%s -> %d min, iso2='%s'%s -> %d min\n",
                  sn,
                  row->iso1, *iso1 ? "" : " (cached)", minutesUntil(row->iso1),
                  row->iso2, *iso2 ? "" : " (cached)", minutesUntil(row->iso2));
  }

  out.ok = true;
  return true;
}

// ---------------- UI build ----------------

static void formatMin(int m, char* buf, size_t n) {
  if (m < 0)       snprintf(buf, n, "--");
  else if (m == 0) snprintf(buf, n, "Arr");
  else             snprintf(buf, n, "%d", m);
}

// Build the static UI for Stop A (the large top block).
// No stop-info header and no column headers — just stacked rows of
// bus badges with their Next/After minute columns. Up to 4 rows fit
// within ~225 px of height at 56 px row pitch.
static void buildStopBlock(StopUi& ui, const StopConfig& cfg,
                           int y_offset, const char* /*labelText*/) {
  lv_obj_t* parent = lv_scr_act();
  (void)cfg;

  // The header field is kept on the struct for ABI stability but
  // intentionally unused at this layout size.
  ui.header = nullptr;

  // Up to 4 rows per stop. Each row: black badge with bus number,
  // then "next" and "after" minutes right-aligned in their columns.
  for (int i = 0; i < 4; i++) {
    int ry = y_offset + 4 + i * 56;

    // Badge: black-filled rectangle behind the bus number
    ui.badge[i] = lv_obj_create(parent);
    lv_obj_remove_style_all(ui.badge[i]);
    lv_obj_add_style(ui.badge[i], &style_badge, 0);
    lv_obj_set_size(ui.badge[i], 160, 48);
    lv_obj_set_pos(ui.badge[i], 6, ry);

    ui.svc_label[i] = lv_label_create(ui.badge[i]);
    lv_obj_add_style(ui.svc_label[i], &style_badge_text, 0);
    lv_obj_center(ui.svc_label[i]);
    lv_label_set_text(ui.svc_label[i], "");

    // Minutes columns — right-aligned so a 2-digit number lines up
    // with a 1-digit number above it.
    ui.next_label[i]  = lv_label_create(parent);
    ui.after_label[i] = lv_label_create(parent);

    lv_obj_add_style(ui.next_label[i],  &style_row, 0);
    lv_obj_add_style(ui.after_label[i], &style_row, 0);

    lv_obj_set_width(ui.next_label[i],  110);
    lv_obj_set_width(ui.after_label[i], 110);
    lv_obj_set_style_text_align(ui.next_label[i],  LV_TEXT_ALIGN_RIGHT, 0);
    lv_obj_set_style_text_align(ui.after_label[i], LV_TEXT_ALIGN_RIGHT, 0);

    lv_obj_set_pos(ui.next_label[i],  170, ry + 6);
    lv_obj_set_pos(ui.after_label[i], 285, ry + 6);

    lv_label_set_text(ui.next_label[i],  "");
    lv_label_set_text(ui.after_label[i], "");

    // Hidden until renderStop fills in real data
    lv_obj_add_flag(ui.badge[i], LV_OBJ_FLAG_HIDDEN);
  }
}

static void buildStopBlockCompact(StopUi& ui, const StopConfig& cfg,
                                  int y_offset) {
  buildStopBlock(ui, cfg, y_offset, cfg.label);
}

static void buildUi() {
  lv_obj_t* scr = lv_scr_act();

  // White background; the panel is reflective so "white" is the
  // unmarked state.
  lv_obj_set_style_bg_color(scr, lv_color_white(), 0);
  lv_obj_set_style_bg_opa(scr, LV_OPA_COVER, 0);

  // Reusable styles. Fonts are doubled from the original 16/12 to
  // 32/24, so enable LV_FONT_MONTSERRAT_32 and LV_FONT_MONTSERRAT_24
  // in your lv_conf.h (e.g. ~/Documents/Arduino/libraries/lvgl/lv_conf.h).
  lv_style_init(&style_header);
  lv_style_set_text_font(&style_header, &lv_font_montserrat_32);
  lv_style_set_text_color(&style_header, lv_color_black());

  lv_style_init(&style_row);
  lv_style_set_text_font(&style_row, &lv_font_montserrat_32);
  lv_style_set_text_color(&style_row, lv_color_black());

  // style_small is only used by the footer (updated time) and the
  // status bar (boot/error messages). Halved from the previous _24.
  lv_style_init(&style_small);
  lv_style_set_text_font(&style_small, &lv_font_montserrat_12);
  lv_style_set_text_color(&style_small, lv_color_black());

  // Bus-number badge: black-filled rounded rectangle.
  lv_style_init(&style_badge);
  lv_style_set_bg_color(&style_badge, lv_color_black());
  lv_style_set_bg_opa(&style_badge, LV_OPA_COVER);
  lv_style_set_radius(&style_badge, 3);
  lv_style_set_border_width(&style_badge, 0);
  lv_style_set_pad_all(&style_badge, 0);

  // White text on the black badge.
  lv_style_init(&style_badge_text);
  lv_style_set_text_font(&style_badge_text, &lv_font_montserrat_32);
  lv_style_set_text_color(&style_badge_text, lv_color_white());

  // 75/25 split: Stop A occupies y = 0..223, Stop B occupies y = 225..298.
  buildStopBlock(ui_a, STOP_A, 0, STOP_A.label);
  buildStopBlockCompact(ui_b, STOP_B, 200);

  // Horizontal divider between stops
  static lv_point_precise_t divider_pts[] = { {0, 200}, {400, 200} };
  lv_obj_t* divider = lv_line_create(scr);
  lv_line_set_points(divider, divider_pts, 2);
  static lv_style_t style_line;
  lv_style_init(&style_line);
  lv_style_set_line_width(&style_line, 1);
  lv_style_set_line_color(&style_line, lv_color_black());
  lv_obj_add_style(divider, &style_line, 0);

  // Footer (bottom-right, updated time) sits below Stop B's row.
  ui_footer = lv_label_create(scr);
  lv_obj_add_style(ui_footer, &style_small, 0);
  lv_obj_set_pos(ui_footer, 320, 286);
  lv_label_set_text(ui_footer, "");

  // Status bar (only used for boot/error messages)
  ui_status = lv_label_create(scr);
  lv_obj_add_style(ui_status, &style_small, 0);
  lv_obj_set_pos(ui_status, 6, 286);
  lv_label_set_text(ui_status, "Booting...");
}

// ---------------- UI update ----------------

static void renderStop(StopUi& ui, const StopConfig& /*cfg*/, const StopArrivals& arr) {
  // Walk cached rows (populated once from StopConfig.services).
  // Minutes are recomputed from the cached ISO string on every render
  // so a row keeps counting down between successful fetches.
  int row = 0;
  for (int i = 0; i < arr.count && row < 3; i++) {
    int m1 = minutesUntil(arr.rows[i].iso1);
    int m2 = minutesUntil(arr.rows[i].iso2);

    char b1[8], b2[8];
    formatMin(m1, b1, sizeof(b1));
    formatMin(m2, b2, sizeof(b2));

    lv_label_set_text(ui.svc_label[row],   arr.rows[i].service);
    lv_label_set_text(ui.next_label[row],  b1);
    lv_label_set_text(ui.after_label[row], b2);
    lv_obj_clear_flag(ui.badge[row], LV_OBJ_FLAG_HIDDEN);
    row++;
  }
  // Hide any leftover rows from a previous render
  for (; row < 3; row++) {
    lv_label_set_text(ui.svc_label[row],   "");
    lv_label_set_text(ui.next_label[row],  "");
    lv_label_set_text(ui.after_label[row], "");
    lv_obj_add_flag(ui.badge[row], LV_OBJ_FLAG_HIDDEN);
  }
}

static void renderAll(const StopArrivals& a, const StopArrivals& b) {
  if (Lvgl_lock(-1)) {
    renderStop(ui_a, STOP_A, a);
    renderStop(ui_b, STOP_B, b);

    struct tm tm_now;
    if (getLocalTime(&tm_now, 5)) {
      char buf[24];
      strftime(buf, sizeof(buf), "Updated %H:%M", &tm_now);
      lv_label_set_text(ui_footer, buf);
    }

    // Status line: blank if both stops fetched cleanly, else show
    // the first error we have.
    if (!a.ok)      lv_label_set_text(ui_status, a.error);
    else if (!b.ok) lv_label_set_text(ui_status, b.error);
    else            lv_label_set_text(ui_status, "");

    Lvgl_unlock();
  }
}

// ---------------- WiFi & setup ----------------

static void setStatus(const char* msg) {
  if (Lvgl_lock(-1)) {
    lv_label_set_text(ui_status, msg);
    Lvgl_unlock();
  }
}

static void connectWiFi() {
  WiFi.mode(WIFI_STA);
  WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
  Serial.print("WiFi");
  for (int i = 0; i < 60 && WiFi.status() != WL_CONNECTED; i++) {
    delay(500);
    Serial.print(".");
  }
  Serial.println(WiFi.status() == WL_CONNECTED ? " connected" : " FAILED");

  // Let the radio doze between beacons. We only talk to the AP once
  // every ~30 s, so max modem-sleep is safe and saves tens of mA.
  WiFi.setSleep(WIFI_PS_MAX_MODEM);
}

// Full WiFi stack reset. Used after repeated fetch failures when
// WiFi.status() lies about being CONNECTED (zombie association: the
// AP dropped us, DHCP lease expired, or lwIP got stuck). A plain
// WiFi.reconnect() is not enough in that state — we need to tear the
// stack down and re-associate from scratch.
static void hardResetWiFi() {
  Serial.println("WiFi: hard reset");
  WiFi.disconnect(true, true);   // disconnect + erase stored config
  delay(500);
  connectWiFi();
}

void setup() {
  Serial.begin(115200);
  delay(200);
  Serial.println("\nSG Bus Display");

  // JSON parse + LVGL repaint of a handful of labels every 30 s is
  // trivial work — 80 MHz is plenty and roughly halves active current
  // versus the 240 MHz default.
  setCpuFrequencyMhz(80);

  // Automatic light sleep during FreeRTOS idle. The SoC drops to 10 MHz
  // (or sleeps outright) between LVGL's 5 ms ticks and during the 30 s
  // poll gap. Combined with WIFI_PS_MAX_MODEM this lets both CPU and
  // radio doze whenever there's nothing to do.
  esp_pm_config_t pm_config = {
    .max_freq_mhz = 80,
    .min_freq_mhz = 10,
    .light_sleep_enable = true,
  };
  esp_pm_configure(&pm_config);

  // 1. Bring up the panel and LVGL exactly like the Waveshare example.
  RlcdPort.RLCD_Init();
  Lvgl_PortInit(400, 300, Lvgl_FlushCallback);

  // 2. Build the static UI under the LVGL lock.
  if (Lvgl_lock(-1)) {
    buildUi();
    Lvgl_unlock();
  }

  // 3. Network + clock.
  setStatus("Connecting WiFi...");
  connectWiFi();

  setStatus("Syncing time...");
  // Sync to UTC. mktime() in minutesUntil() relies on the system
  // timezone being UTC. We set the SGT timezone via TZ env var only
  // for the footer's localtime display.
  configTime(0, 0, "pool.ntp.org", "sg.pool.ntp.org");
  setenv("TZ", "SGT-8", 1);
  tzset();
  for (int i = 0; i < 20 && time(nullptr) < 1700000000; i++) delay(250);

  setStatus("");
}

void loop() {
  // Persist arrival caches across iterations so a missing entry in one
  // poll falls back to the previous ISO timestamp instead of "--".
  static StopArrivals a, b;
  static bool arrivals_initialized = false;
  if (!arrivals_initialized) {
    initStopArrivals(a, STOP_A);
    initStopArrivals(b, STOP_B);
    arrivals_initialized = true;
  }

  // Stagger the two LTA calls by REFRESH_MS instead of firing them
  // back-to-back. Each stop is polled once per (2 * REFRESH_MS) but
  // the UI still repaints every REFRESH_MS — between fetches the
  // cached ISO timestamps keep counting down.
  //
  // After 3 consecutive failed fetches we assume WiFi.status() is
  // lying (zombie association) and force a full stack reset. A plain
  // WiFi.reconnect() doesn't recover from that state.
  static int consecutive_fails = 0;
  auto pollOne = [&](const StopConfig& cfg, StopArrivals& out) {
    if (WiFi.status() != WL_CONNECTED) WiFi.reconnect();
    bool ok = fetchStop(cfg, out);
    if (ok) {
      consecutive_fails = 0;
    } else if (++consecutive_fails >= 3) {
      hardResetWiFi();
      consecutive_fails = 0;
    }
  };

  pollOne(STOP_A, a);
  renderAll(a, b);
  vTaskDelay(pdMS_TO_TICKS(REFRESH_MS));

  pollOne(STOP_B, b);
  renderAll(a, b);
  vTaskDelay(pdMS_TO_TICKS(REFRESH_MS));
}
