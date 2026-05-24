/*
 * Singapore Bus Arrival Display
 * Hardware: Waveshare ESP32-S3-RLCD-4.2 (ST7305, 400x300 mono)
 * Data:     LTA DataMall BusArrival v3
 * Graphics: LVGL v8 on Waveshare's DisplayPort driver
 *
 * Layout (split screen):
 *   Top half  : Stop 59621 - buses 801, 803, 861
 *   Bottom half: Stop 59181 - bus 811
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
  int  mins1;        // -1 = no data, 0 = "Arr"
  int  mins2;        // -1 = no data
};

struct StopArrivals {
  ServiceArrival rows[6];
  int  count;
  bool ok;
  char error[48];
};

struct StopUi {
  lv_obj_t* header;
  lv_obj_t* svc_label[4];    // bus number column
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
  if (diff < 0) diff = 0;
  return (int)((diff + 30) / 60);
}

static bool fetchStop(const StopConfig& cfg, StopArrivals& out) {
  out.count = 0;
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

  Serial.printf("[%s] HTTP %d, payload len=%d\n", cfg.code, code, payload.length());
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
  Serial.printf("[%s] Services in filtered doc: %d\n", cfg.code, (int)services.size());
  for (JsonObject s : services) {
    if (out.count >= (int)(sizeof(out.rows) / sizeof(out.rows[0]))) break;
    const char* sn = s["ServiceNo"] | "";
    if (!*sn) continue;

    // Only keep services we care about for this stop.
    bool wanted = false;
    for (int i = 0; cfg.services[i] != nullptr; i++) {
      if (strcmp(cfg.services[i], sn) == 0) { wanted = true; break; }
    }
    if (!wanted) continue;

    ServiceArrival& row = out.rows[out.count++];
    strncpy(row.service, sn, sizeof(row.service) - 1);
    row.service[sizeof(row.service) - 1] = '\0';
    const char* iso1 = s["NextBus"]["EstimatedArrival"]  | "";
    const char* iso2 = s["NextBus2"]["EstimatedArrival"] | "";
    row.mins1 = minutesUntil(iso1);
    row.mins2 = minutesUntil(iso2);
    Serial.printf("  %s: iso1='%s' -> %d min, iso2='%s' -> %d min\n",
                  sn, iso1, row.mins1, iso2, row.mins2);
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

// Build the static UI once. We hand out labels we'll update later.
static void buildStopBlock(StopUi& ui, const StopConfig& cfg,
                           int y_offset, const char* /*labelText*/) {
  lv_obj_t* parent = lv_scr_act();

  // Header label, top-left of this half
  ui.header = lv_label_create(parent);
  lv_obj_add_style(ui.header, &style_header, 0);
  lv_obj_set_pos(ui.header, 8, y_offset + 2);
  lv_label_set_text(ui.header, cfg.label);

  // Column headers
  lv_obj_t* h_bus  = lv_label_create(parent);
  lv_obj_t* h_next = lv_label_create(parent);
  lv_obj_t* h_aft  = lv_label_create(parent);
  lv_obj_add_style(h_bus,  &style_small, 0);
  lv_obj_add_style(h_next, &style_small, 0);
  lv_obj_add_style(h_aft,  &style_small, 0);
  lv_obj_set_pos(h_bus,   8,  y_offset + 32);
  lv_obj_set_pos(h_next, 180, y_offset + 32);
  lv_obj_set_pos(h_aft,  300, y_offset + 32);
  lv_label_set_text(h_bus,  "Bus");
  lv_label_set_text(h_next, "Next");
  lv_label_set_text(h_aft,  "After");

  // Up to 4 rows per stop. Pre-create all 4 and hide unused ones by
  // leaving them empty.
  for (int i = 0; i < 4; i++) {
    int ry = y_offset + 52 + i * 20;

    ui.svc_label[i]   = lv_label_create(parent);
    ui.next_label[i]  = lv_label_create(parent);
    ui.after_label[i] = lv_label_create(parent);

    lv_obj_add_style(ui.svc_label[i],   &style_row, 0);
    lv_obj_add_style(ui.next_label[i],  &style_row, 0);
    lv_obj_add_style(ui.after_label[i], &style_row, 0);

    lv_obj_set_pos(ui.svc_label[i],     8, ry);
    lv_obj_set_pos(ui.next_label[i],  180, ry);
    lv_obj_set_pos(ui.after_label[i], 300, ry);

    lv_label_set_text(ui.svc_label[i],   "");
    lv_label_set_text(ui.next_label[i],  "");
    lv_label_set_text(ui.after_label[i], "");
  }
}

static void buildUi() {
  lv_obj_t* scr = lv_scr_act();

  // White background; the panel is reflective so "white" is the
  // unmarked state.
  lv_obj_set_style_bg_color(scr, lv_color_white(), 0);
  lv_obj_set_style_bg_opa(scr, LV_OPA_COVER, 0);

  // Reusable styles. Stick to the Montserrat sizes the example's
  // lv_conf.h enables (12, 14, 16). If you want larger fonts, edit
  // lv_conf.h to set LV_FONT_MONTSERRAT_20 / _22 to 1.
  lv_style_init(&style_header);
  lv_style_set_text_font(&style_header, &lv_font_montserrat_16);
  lv_style_set_text_color(&style_header, lv_color_black());

  lv_style_init(&style_row);
  lv_style_set_text_font(&style_row, &lv_font_montserrat_16);
  lv_style_set_text_color(&style_row, lv_color_black());

  lv_style_init(&style_small);
  lv_style_set_text_font(&style_small, &lv_font_montserrat_12);
  lv_style_set_text_color(&style_small, lv_color_black());

  // Stop A occupies y = 0..148, Stop B occupies y = 150..298.
  buildStopBlock(ui_a, STOP_A,   0, STOP_A.label);
  buildStopBlock(ui_b, STOP_B, 150, STOP_B.label);

  // Horizontal divider between stops
  static lv_point_precise_t divider_pts[] = { {0, 149}, {400, 149} };
  lv_obj_t* divider = lv_line_create(scr);
  lv_line_set_points(divider, divider_pts, 2);
  static lv_style_t style_line;
  lv_style_init(&style_line);
  lv_style_set_line_width(&style_line, 1);
  lv_style_set_line_color(&style_line, lv_color_black());
  lv_obj_add_style(divider, &style_line, 0);

  // Footer (bottom-right, updated time)
  ui_footer = lv_label_create(scr);
  lv_obj_add_style(ui_footer, &style_small, 0);
  lv_obj_set_pos(ui_footer, 280, 284);
  lv_label_set_text(ui_footer, "");

  // Status bar (only used for boot/error messages)
  ui_status = lv_label_create(scr);
  lv_obj_add_style(ui_status, &style_small, 0);
  lv_obj_set_pos(ui_status, 8, 284);
  lv_label_set_text(ui_status, "Booting...");
}

// ---------------- UI update ----------------

static void renderStop(StopUi& ui, const StopConfig& cfg, const StopArrivals& arr) {
  // Walk configured services in declared order; show "--" when LTA
  // returned no data for one.
  int row = 0;
  for (int i = 0; cfg.services[i] != nullptr && row < 4; i++) {
    const char* svc = cfg.services[i];
    int m1 = -1, m2 = -1;
    if (arr.ok) {
      for (int j = 0; j < arr.count; j++) {
        if (strcmp(arr.rows[j].service, svc) == 0) {
          m1 = arr.rows[j].mins1;
          m2 = arr.rows[j].mins2;
          break;
        }
      }
    }

    char b1[8], b2[8];
    formatMin(m1, b1, sizeof(b1));
    formatMin(m2, b2, sizeof(b2));

    lv_label_set_text(ui.svc_label[row],   svc);
    lv_label_set_text(ui.next_label[row],  b1);
    lv_label_set_text(ui.after_label[row], b2);
    row++;
  }
  // Clear any leftover rows from a previous render
  for (; row < 4; row++) {
    lv_label_set_text(ui.svc_label[row],   "");
    lv_label_set_text(ui.next_label[row],  "");
    lv_label_set_text(ui.after_label[row], "");
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
}

void setup() {
  Serial.begin(115200);
  delay(200);
  Serial.println("\nSG Bus Display");

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
  StopArrivals a{}, b{};
  fetchStop(STOP_A, a);
  fetchStop(STOP_B, b);
  renderAll(a, b);

  // Sleep until next refresh. LVGL runs in its own task on Waveshare's
  // BSP so we don't need to call lv_timer_handler() ourselves.
  if (WiFi.status() != WL_CONNECTED) WiFi.reconnect();
  vTaskDelay(pdMS_TO_TICKS(REFRESH_MS));
}
