# SG Bus Arrival Display — Waveshare ESP32-S3-RLCD-4.2

Pulls arrival times for two bus stops from LTA DataMall and shows
them on the 400×300 reflective LCD, split top/bottom.

## Prerequisite

You must have **first compiled and flashed Waveshare's
`08_LVGL_V8_Test` example** successfully. This sketch reuses the
panel driver and LVGL helpers from that example. If `08_LVGL_V8_Test`
doesn't work on your board, this sketch won't either.

## Setup steps

### 1. Copy Waveshare's BSP files into this sketch folder

The Waveshare example has a local `src/` folder with the panel driver
and LVGL port. Copy that whole folder into the same directory as
`sg_bus_display.ino`:

```bash
cp -R ~/Downloads/ESP32-S3-RLCD-4.2/02_Example/Arduino/08_LVGL_V8_Test/src \
      ~/Documents/Arduino/sg_bus_display/
```

You should also copy `display_bsp.h` and `display_bsp.cpp` (or
whatever the example's top-level non-LVGL files are):

```bash
cp ~/Downloads/ESP32-S3-RLCD-4.2/02_Example/Arduino/08_LVGL_V8_Test/display_bsp.* \
   ~/Documents/Arduino/sg_bus_display/
```

After this, your sketch folder should look like:

```
sg_bus_display/
  sg_bus_display.ino
  secrets.h                    <- you create this in step 3
  secrets_example.h
  README.md
  display_bsp.h
  display_bsp.cpp
  src/
    app_bsp/
      lvgl_bsp.h
      lvgl_bsp.cpp
      ...
```

If the Waveshare example's actual layout is different, just mirror
whatever it has — the goal is for `#include "display_bsp.h"` and
`#include "src/app_bsp/lvgl_bsp.h"` to resolve.

### 2. Install ArduinoJson

In Arduino IDE: Sketch → Include Library → Manage Libraries →
search "ArduinoJson" by Benoit Blanchon → install v7.x.

### 3. Create `secrets.h`

```bash
cp secrets_example.h secrets.h
```

Open `secrets.h` and fill in your WiFi SSID, password, and LTA
AccountKey. Don't commit this file.

### 4. Build and upload

Tools menu — use the **same settings that worked for
`08_LVGL_V8_Test`**:

- Board: ESP32S3 Dev Module
- USB CDC On Boot: Enabled
- Flash Size: 16MB
- PSRAM: OPI PSRAM

Compile, then upload.

## What you should see

After ~10-15 seconds (WiFi connect + NTP + 2 API calls):

```
Stop 59621
Bus       Next      After
801        3          14
803        7          19
861        Arr        12

Stop 59181
Bus       Next      After
811        5          --

                        Updated 18:42
```

- `--` means no estimate (often outside operating hours).
- `Arr` means the bus is at the stop.
- Refreshes every 30 seconds.

## Changing stops or buses

Top of `sg_bus_display.ino`:

```cpp
static const StopConfig STOP_A = {
  "59621",
  "Stop 59621",                       // free-text label
  { "801", "803", "861", nullptr }    // up to 6, end with nullptr
};
```

## Common errors

- **`fatal error: lvgl.h: No such file`** → LVGL library not at
  `~/Documents/Arduino/libraries/lvgl/`. Make sure the v8 folder
  is renamed to plain `lvgl`.
- **`fatal error: display_bsp.h: No such file`** → step 1 above
  wasn't done, or the file is in a subfolder. The `#include` is
  relative to the .ino file.
- **`HTTP 401`** → bad LTA AccountKey.
- **All times show `--`** → either NTP not synced (wait 30s), or
  buses aren't running at this time of day.
- **Screen never updates after first render** → check Serial Monitor
  for fetch errors. If WiFi dropped, the next loop will reconnect.

## Why LVGL? Why not write to the panel directly?

Could be done that way, but Waveshare's library only exposes the
panel through LVGL on this board, and LVGL gives us nice fonts and
antialiasing essentially for free. The cost is roughly 100KB of
flash, which is fine on the ESP32-S3.
