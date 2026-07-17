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
static lv_obj_t* ui_status;  // shown only when something is wrong
static lv_obj_t* ui_battery; // "NN%" battery indicator
static lv_obj_t* ui_alert;   // MRT disruption banner (top-left); blank when clear
static lv_obj_t* ui_weather; // weather strip (top-right)

// Cached values rendered into ui_alert / ui_weather. Updated on their
// own (slower) cadence — we don't want to hammer LTA TrainServiceAlerts
// or Open-Meteo at the bus-arrival 30 s rate.
static char g_alert_text[64]   = "";
static char g_weather_text[24] = "";

// Waveshare ESP32-S3-RLCD-4.2 routes VBAT through a 3x divider into
// GPIO4 (ADC1 ch3). 18650 cell: 2.5 V empty -> 4.2 V full.
static const int BAT_ADC_PIN = 4;
static const uint32_t BAT_EMPTY_MV = 2500;
static const uint32_t BAT_FULL_MV  = 4200;

static lv_style_t style_header;
static lv_style_t style_row;
static lv_style_t style_small;
static lv_style_t style_badge;
static lv_style_t style_badge_text;

// ---------------- Power management ----------------

// Idle: CPU may drop to 10 MHz, light sleep allowed, radio in max
// modem sleep. Active: CPU pinned at 80 MHz, no light sleep, radio
// stays awake. TLS handshakes fail (HTTPClient -1) under the idle
// profile, so we switch to active for the fetch burst.
static void enterIdlePm() {
  esp_pm_config_t cfg = {
    .max_freq_mhz = 80,
    .min_freq_mhz = 10,
    .light_sleep_enable = true,
  };
  esp_pm_configure(&cfg);
  WiFi.setSleep(WIFI_PS_MAX_MODEM);
}

static void enterActivePm() {
  esp_pm_config_t cfg = {
    .max_freq_mhz = 80,
    .min_freq_mhz = 80,
    .light_sleep_enable = false,
  };
  esp_pm_configure(&cfg);
  WiFi.setSleep(false);
}

// ---------------- HTTPS plumbing ----------------

// Single TLS client + HTTPClient shared by every endpoint (BusArrival,
// TrainServiceAlerts, Open-Meteo). Constructing a fresh WiFiClientSecure
// per call burns ~23 KB malloc/free; reusing one and toggling
// setReuse(false) on every GET keeps steady-state heap stable. See the
// long heap-leak note above fetchStop() for why.
static WiFiClientSecure g_tls;
static HTTPClient       g_http;
static bool             g_tls_inited = false;

static int httpGetBody(const char* url, bool send_lta_key,
                       String& body_out, char* err, size_t err_n) {
  body_out = "";
  err[0] = '\0';
  if (WiFi.status() != WL_CONNECTED) {
    snprintf(err, err_n, "No WiFi");
    return -1;
  }
  if (!g_tls_inited) {
    g_tls.setInsecure();   // not worth pinning certs for public data
    g_tls_inited = true;
  }
  g_http.setReuse(false);
  g_http.setTimeout(8000);
  if (!g_http.begin(g_tls, url)) {
    snprintf(err, err_n, "begin failed");
    return -1;
  }
  if (send_lta_key) g_http.addHeader("AccountKey", LTA_ACCOUNT_KEY);
  g_http.addHeader("accept", "application/json");
  int code = g_http.GET();
  if (code == 200) {
    body_out = g_http.getString();
  } else {
    snprintf(err, err_n, "HTTP %d", code);
  }
  g_http.end();
  return code;
}

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

  String url = String("https://datamall2.mytransport.sg/ltaodataservice/v3/BusArrival?BusStopCode=") + cfg.code;
  String payload;
  int code = httpGetBody(url.c_str(), /*send_lta_key=*/true,
                         payload, out.error, sizeof(out.error));
  if (code != 200) {
    Serial.printf("[%s] HTTP %d (fail), rssi=%d, heap=%u\n",
                  cfg.code, code,
                  (int)WiFi.RSSI(), (unsigned)ESP.getFreeHeap());
    return false;
  }

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

// ---------------- MRT disruption banner ----------------

// Hits LTA's TrainServiceAlerts endpoint. Writes a short banner string
// into out_buf — empty when service is normal, otherwise something like
// "MRT: NSL, EWL disrupted". Returns true on a successful poll (even if
// "normal"), false on network/parse failure so the caller can keep the
// previously-cached banner instead of clearing it.
static bool fetchMrtAlerts(char* out_buf, size_t out_n) {
  String payload;
  char err[32];
  int code = httpGetBody(
    "https://datamall2.mytransport.sg/ltaodataservice/TrainServiceAlerts",
    /*send_lta_key=*/true, payload, err, sizeof(err));
  if (code != 200) {
    Serial.printf("[mrt] HTTP fail: %s, heap=%u\n",
                  err, (unsigned)ESP.getFreeHeap());
    return false;
  }

  JsonDocument doc;
  if (deserializeJson(doc, payload)) return false;

  // Response shape: { "value": [ { "Status": 1|2, "AffectedSegments": [...] } ] }
  JsonObject v = doc["value"][0];
  int status = v["Status"] | 1;
  if (status == 1) {
    out_buf[0] = '\0';
    return true;
  }

  // Disrupted — collect affected line codes (e.g. "NSL", "EWL"). LTA
  // gives at most a handful; we'll list up to 4 to keep the banner short.
  char lines[40] = "";
  int n = 0;
  for (JsonObject seg : v["AffectedSegments"].as<JsonArray>()) {
    const char* line = seg["Line"] | "";
    if (!*line) continue;
    if (n > 0) strncat(lines, ",", sizeof(lines) - strlen(lines) - 1);
    strncat(lines, line, sizeof(lines) - strlen(lines) - 1);
    if (++n >= 4) break;
  }
  if (n == 0) snprintf(out_buf, out_n, "MRT disruption");
  else        snprintf(out_buf, out_n, "MRT: %s disrupted", lines);
  return true;
}

// ---------------- Weather strip ----------------

// Open-Meteo: free, no key, HTTPS. Singapore coords. We ask for current
// temperature and weather_code, and translate the WMO code to a short
// 3-4 char label that fits the 18 px header. Writes into out_buf like
// "29C CLR" or "27C RAIN".
static const char* wmoLabel(int code) {
  if (code <= 1)               return "CLR";
  if (code <= 3)               return "CLD";
  if (code >= 45 && code <= 48) return "FOG";
  if (code >= 51 && code <= 67) return "RAIN";
  if (code >= 71 && code <= 77) return "SNOW";
  if (code >= 80 && code <= 82) return "SHWR";
  if (code >= 95)              return "STRM";
  return "";
}

static bool fetchWeather(char* out_buf, size_t out_n) {
  String payload;
  char err[32];
  // Singapore: 1.35 N, 103.82 E. tz=auto keeps "current" aligned with SGT.
  int code = httpGetBody(
    "https://api.open-meteo.com/v1/forecast"
    "?latitude=1.35&longitude=103.82"
    "&current=temperature_2m,weather_code"
    "&timezone=Asia%2FSingapore",
    /*send_lta_key=*/false, payload, err, sizeof(err));
  if (code != 200) {
    Serial.printf("[wx] HTTP fail: %s, heap=%u\n",
                  err, (unsigned)ESP.getFreeHeap());
    return false;
  }

  JsonDocument doc;
  if (deserializeJson(doc, payload)) return false;

  float t  = doc["current"]["temperature_2m"]   | 0.0f;
  int   wc = doc["current"]["weather_code"]     | -1;
  const char* lbl = wmoLabel(wc);
  if (*lbl) snprintf(out_buf, out_n, "%dC %s", (int)(t + 0.5f), lbl);
  else      snprintf(out_buf, out_n, "%dC", (int)(t + 0.5f));
  return true;
}

// ---------------- Battery ----------------

static int readBatteryPercent() {
  // Average 16 reads; analogReadMilliVolts applies the ESP32-S3's
  // factory ADC calibration so we don't have to hand-tune.
  const int samples = 16;
  uint32_t sum_mv = 0;
  for (int i = 0; i < samples; i++) sum_mv += analogReadMilliVolts(BAT_ADC_PIN);
  uint32_t mv = (sum_mv / samples) * 3;  // undo the 3x divider
  if (mv <= BAT_EMPTY_MV) return 0;
  if (mv >= BAT_FULL_MV)  return 100;
  return (int)((mv - BAT_EMPTY_MV) * 100 / (BAT_FULL_MV - BAT_EMPTY_MV));
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
  // Stop A's y_offset is 20 (was 0) to leave room for the top
  // alert+weather strip; Stop B's y_offset is unchanged at 200.
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

  // Top strip (y = 0..18) holds the MRT disruption banner on the left
  // and the weather summary on the right. Both use the small font.
  // Stop A starts at y = 20 to clear it; Stop B is unchanged at y = 200.
  ui_alert = lv_label_create(scr);
  lv_obj_add_style(ui_alert, &style_small, 0);
  lv_obj_set_pos(ui_alert, 6, 4);
  lv_obj_set_width(ui_alert, 280);
  lv_label_set_long_mode(ui_alert, LV_LABEL_LONG_DOT);
  lv_label_set_text(ui_alert, "");

  ui_weather = lv_label_create(scr);
  lv_obj_add_style(ui_weather, &style_small, 0);
  lv_obj_set_width(ui_weather, 100);
  lv_obj_set_style_text_align(ui_weather, LV_TEXT_ALIGN_RIGHT, 0);
  lv_obj_set_pos(ui_weather, 294, 4);
  lv_label_set_text(ui_weather, "");

  buildStopBlock(ui_a, STOP_A, 20, STOP_A.label);
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

  // Battery indicator, centered between status and footer.
  ui_battery = lv_label_create(scr);
  lv_obj_add_style(ui_battery, &style_small, 0);
  lv_obj_set_pos(ui_battery, 200, 286);
  lv_label_set_text(ui_battery, "");
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
  // Sample the battery outside the LVGL lock — analogRead is slow-ish.
  int batt = readBatteryPercent();

  if (Lvgl_lock(-1)) {
    renderStop(ui_a, STOP_A, a);
    renderStop(ui_b, STOP_B, b);

    struct tm tm_now;
    if (getLocalTime(&tm_now, 5)) {
      char buf[24];
      strftime(buf, sizeof(buf), "Updated %H:%M", &tm_now);
      lv_label_set_text(ui_footer, buf);
    }

    char bbuf[8];
    snprintf(bbuf, sizeof(bbuf), "%d%%", batt);
    lv_label_set_text(ui_battery, bbuf);

    // Top strip — both populated from background fetches that run on
    // their own slower cadence. Strings are empty until the first
    // successful poll, which keeps the strip cleanly blank at boot.
    lv_label_set_text(ui_alert,   g_alert_text);
    lv_label_set_text(ui_weather, g_weather_text);

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
  // radio doze whenever there's nothing to do. We turn this off around
  // the HTTPS fetch — TLS handshakes don't survive light sleep + max
  // modem sleep + 10 MHz min and come back as HTTPClient error -1.
  enterIdlePm();

  // 1. Bring up the panel and LVGL exactly like the Waveshare example.
  RlcdPort.RLCD_Init();
  Lvgl_PortInit(400, 300, Lvgl_FlushCallback);

  // 2. Build the static UI under the LVGL lock.
  if (Lvgl_lock(-1)) {
    buildUi();
    Lvgl_unlock();
  }

  // 3. Network + clock. Run the initial join + NTP under active PM —
  // light sleep + max modem sleep during association is just as bad
  // for TLS-style timing as it is during the GET burst.
  enterActivePm();
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

  enterIdlePm();
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

  // Poll both stops back-to-back in one WiFi wake window, then idle for
  // REFRESH_MS. Keeping the radio active for one longer burst (instead
  // of two separate bursts staggered by REFRESH_MS) lets it stay in
  // deeper modem-sleep across the gap.
  //
  // After 3 consecutive failed fetches we reboot. The usual cause is
  // free heap dropping below what an mbedTLS handshake needs (~50 KB),
  // and only a full restart reclaims memory held by LVGL, the WiFi
  // stack, and accumulated TLS state.
  static int consecutive_fails = 0;
  auto pollOne = [&](const StopConfig& cfg, StopArrivals& out) {
    if (WiFi.status() != WL_CONNECTED) WiFi.reconnect();
    bool ok = fetchStop(cfg, out);
    if (ok) {
      consecutive_fails = 0;
    } else if (++consecutive_fails >= 3) {
      Serial.printf("Restarting: %d consecutive fails, heap=%u\n",
                    consecutive_fails, (unsigned)ESP.getFreeHeap());
      Serial.flush();
      ESP.restart();
    }
  };

  enterActivePm();
  // Proactive restart if heap is already too low to handshake.
  // Steady-state free heap is ~110 KB with setReuse(false); the
  // residual leak (lwIP TIME_WAIT + mbedTLS handshake churn) drifts
  // it down ~500 B/cycle, so this typically fires after ~1 hour of
  // continuous polling — well before mbedTLS_ssl_setup would start
  // returning -1.
  if (ESP.getFreeHeap() < 45000) {
    Serial.printf("Restarting: low heap=%u before fetch\n",
                  (unsigned)ESP.getFreeHeap());
    Serial.flush();
    ESP.restart();
  }
  pollOne(STOP_A, a);
  pollOne(STOP_B, b);

  // MRT alerts every 4th cycle (~2 min) and weather every 20th cycle
  // (~10 min). Both share the same HTTPS client as fetchStop and run
  // inside the active-PM window so handshakes are reliable. Failures
  // are non-fatal — they don't bump consecutive_fails, since the bus
  // ETAs are the primary function and we'd rather not reboot just
  // because Open-Meteo blinked.
  static uint32_t cycle = 0;
  if (cycle % 4 == 0)  fetchMrtAlerts(g_alert_text,   sizeof(g_alert_text));
  if (cycle % 20 == 0) fetchWeather(g_weather_text,   sizeof(g_weather_text));
  cycle++;

  enterIdlePm();

  renderAll(a, b);
  vTaskDelay(pdMS_TO_TICKS(REFRESH_MS));
}
