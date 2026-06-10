# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## Project Overview

Arduino sketch for a Waveshare ESP32-S3-RLCD-4.2 device that displays Singapore bus arrival times on a 400×300 monochrome reflective LCD. It fetches real-time data from the LTA DataMall API v3 for two configurable bus stops.

**Display note:** despite being a reflective LCD, this panel is *not* bistable — it is volatile and needs continuous power to hold the image. A reboot (`ESP.restart()`) blanks the screen until `buildUi()` repaints it (~5–10 s window).

## Build & Flash

This is a pure Arduino IDE project — there is no Makefile, package.json, or CLI build tool.

**Build steps (Arduino IDE):**
1. Copy `secrets_example.h` to `secrets.h` and fill in credentials (this file is gitignored)
2. Copy `src/` from Waveshare's `08_LVGL_V8_Test` example into this sketch directory
3. Install **ArduinoJson v7.x** via Arduino Library Manager
4. Board settings: ESP32S3 Dev Module, USB CDC On Boot: Enabled, Flash Size: 16MB, PSRAM: OPI PSRAM
5. Upload via Arduino IDE

There are no automated tests or linters for this project.

## Architecture

### Display Pipeline
LVGL (16-bit color buffer) → `Lvgl_FlushCallback()` (threshold at `0x7fff` → 1-bit mono) → `DisplayPort` → ST7305 SPI panel

### Threading Model & Rendering
- LVGL rendering is **on-demand**: there is no free-running LVGL task and no periodic `lv_tick_inc` timer (both were removed so the CPU can light-sleep across the poll gap instead of waking every 5 ms). LVGL gets time lazily via `lv_tick_set_cb`.
- After mutating widgets under the mutex (`Lvgl_lock()`/`Lvgl_unlock()`), call `Lvgl_refresh()` (→ `lv_refr_now()`) *before* unlocking to repaint synchronously. Forgetting this leaves the change invisible — nothing else will ever flush it.
- This only works because the UI has no animations or input devices; if either is ever added, a periodic `lv_timer_handler()` caller must be reintroduced.
- Main loop: fetches API data, calls `renderAll()`, then sleeps in ≤30 s `vTaskDelay` chunks for an adaptive interval (re-rendering between chunks so countdowns keep ticking during long gaps).

### User Configuration (`sg_bus_display.ino`, "User config" section)
- `STOP_A` / `STOP_B`: bus stop codes and the service numbers to display (up to 6 each)
- Adaptive polling (`nextRefreshMs()`): `REFRESH_MS` (30 s) when a bus is within `SLOW_THRESHOLD_MIN` (10 min) or no data is cached; `REFRESH_SLOW_MS` (90 s) when nothing is due soon; `REFRESH_QUIET_MS` (5 min) during SGT quiet hours (`QUIET_START_MIN`–`QUIET_END_MIN`, default 00:30–05:30)

### Power Management
- `enterIdlePm()` / `enterActivePm()` switch between a low-power profile (DFS 10–80 MHz, auto light sleep, `WIFI_PS_MAX_MODEM`) and a full-power profile for the TLS fetch burst (handshakes fail under the idle profile).
- `applyPmConfig()` checks the `esp_pm_configure()` return value and logs at boot whether automatic light sleep is actually active — `ESP_ERR_NOT_SUPPORTED`/`ESP_ERR_INVALID_ARG` means the Arduino core was built without `CONFIG_PM_ENABLE`/tickless idle and the idle profile is a no-op.

### API Integration
- HTTPS GET to `https://datamall2.mytransport.sg/ltaodataservice/v3/BusArrival`
- Parsed with ArduinoJson v7 (`DeserializationOption::Filter` for memory efficiency)
- Time parsing: ISO 8601 strings, UTC internally; TZ env var is temporarily swapped to "SGT-8" for `mktime` conversion

### Heap management (important)
The arduino-esp32 HTTPS stack has long-standing leaks (issues [#3679](https://github.com/espressif/arduino-esp32/issues/3679), [#5781](https://github.com/espressif/arduino-esp32/issues/5781), [#6257](https://github.com/espressif/arduino-esp32/issues/6257), [#10261](https://github.com/espressif/arduino-esp32/issues/10261)) that drip ~100–400 B per request through mbedTLS + lwIP. Without mitigation the device fails TLS handshakes (`HTTPClient -1`) after a few hours, recovers briefly, then re-fails as the heap fragments.

`fetchStop()` mitigates this with:
- **Static `WiFiClientSecure` + `HTTPClient`** — avoids per-call ~23 KB malloc churn and sidesteps the handshake-fail leak that compounds across reconstructions.
- **`setReuse(false)`** — releases the ~50 KB mbedTLS session context after each GET, raising steady-state free heap from ~64 KB to ~110 KB.

A residual ~500 B/cycle leak remains (lwIP TIME_WAIT TCBs + mbedTLS handshake churn). It can't be fixed from sketch level without forking the core or reaching past Arduino API boundaries. `loop()` handles it with:
- **Proactive `ESP.restart()`** when `getFreeHeap() < 45 KB` (typically ~1 hour into operation).
- **Reactive `ESP.restart()`** after 3 consecutive fetch failures.

The reboot blanks the screen for ~5–10 s. Do not attempt `WiFi.disconnect()`/`reconnect()` cycles as a substitute — they don't reclaim mbedTLS state.

### UI Layout (`buildUi()`)
Static LVGL layout built once at startup — top half for Stop A, bottom half for Stop B, separated by a divider line. `renderStop()` updates only text labels; no layout changes occur at runtime.

### Key Files
| File | Purpose |
|------|---------|
| `sg_bus_display.ino` | All application logic: WiFi, API fetch, time parsing, UI build & render |
| `display.h` | Thin wrapper forwarding calls to `DisplayPort` |
| `display_bsp.h/cpp` | Waveshare ST7305 SPI panel driver |
| `src/app_bsp/lvgl_bsp.cpp` | LVGL FreeRTOS port (task, mutex, dual SPIRAM buffers) |
| `src/ui_src/generated/` | NXP SquareLine-generated UI code — do not edit directly |
| `src/ui_src/custom/custom.c` | Custom event handler hooks (currently empty) |
| `secrets_example.h` | Template showing required credential defines |
