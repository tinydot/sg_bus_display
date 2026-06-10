#include <stdio.h>
#include <string.h>
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>
#include <esp_log.h>
#include <esp_timer.h>
#include "lvgl_bsp.h"

static SemaphoreHandle_t lvgl_mux = NULL;
#define BYTES_PER_PIXEL (LV_COLOR_FORMAT_GET_SIZE(LV_COLOR_FORMAT_RGB565))

// On-demand rendering port. The UI is static between data refreshes, so
// there is no free-running LVGL task and no periodic lv_tick_inc timer
// (the original Waveshare port woke the CPU every 5 ms around the clock,
// defeating automatic light sleep). Instead:
//   - LVGL reads time lazily through lv_tick_set_cb (esp_timer-backed),
//   - the sketch mutates widgets under Lvgl_lock() and then calls
//     Lvgl_refresh() to repaint synchronously via lv_refr_now().
// This only works because the UI has no animations and no input devices;
// nothing needs lv_timer_handler() to run periodically.

static uint32_t Lvgl_TickGet(void)
{
    return (uint32_t)(esp_timer_get_time() / 1000ULL);
}

bool Lvgl_lock(int timeout_ms)
{
  	const TickType_t timeout_ticks = (timeout_ms == -1) ? portMAX_DELAY : pdMS_TO_TICKS(timeout_ms);
  	return xSemaphoreTake(lvgl_mux, timeout_ticks) == pdTRUE;
}

void Lvgl_unlock(void)
{
  	assert(lvgl_mux && "bsp_display_start must be called first");
  	xSemaphoreGive(lvgl_mux);
}

void Lvgl_refresh(void)
{
    lv_refr_now(NULL);
}

void Lvgl_PortInit(int width, int height, DispFlushCb flush_cb) {
    lvgl_mux = xSemaphoreCreateMutex();
    lv_init();
    lv_tick_set_cb(Lvgl_TickGet);

    lv_display_t * disp = lv_display_create(width, height);
    lv_display_set_flush_cb(disp, flush_cb);

	size_t buffer_size = width * height * BYTES_PER_PIXEL;
	uint8_t *buffer_1 = NULL;
    uint8_t *buffer_2 = NULL;
    buffer_1 = (uint8_t *)heap_caps_malloc(buffer_size, MALLOC_CAP_SPIRAM);
	buffer_2 = (uint8_t *)heap_caps_malloc(buffer_size, MALLOC_CAP_SPIRAM);
    assert(buffer_1);
    assert(buffer_2);

    lv_display_set_buffers(disp, buffer_1, buffer_2, buffer_size, LV_DISPLAY_RENDER_MODE_FULL);
}
