#pragma once

#include "lvgl.h"

typedef void (*DispFlushCb)(lv_display_t * disp, const lv_area_t * area, uint8_t * color_p);

void Lvgl_PortInit(int width, int height,DispFlushCb flush_cb);
bool Lvgl_lock(int timeout_ms);
void Lvgl_unlock(void);
// Repaint any pending widget changes synchronously. Must be called with
// the LVGL lock held, after mutating widgets.
void Lvgl_refresh(void);
