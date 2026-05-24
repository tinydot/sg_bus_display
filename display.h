#pragma once
/*
 * Thin wrapper around Waveshare's ST7305 driver.
 *
 * The Waveshare Arduino demo for the ESP32-S3-RLCD-4.2 includes an
 * "LCD_Driver" / "ST7305" class with GFX-style primitives. APIs vary
 * slightly between firmware revisions of their demo repo. This file
 * isolates those calls so you only edit ONE place if names differ.
 *
 * Expected primitives in the Waveshare driver (typical names):
 *   LCD_Init();                           // init SPI + panel
 *   LCD_Clear(WHITE);                     // wipe framebuffer
 *   LCD_Update();                         // push framebuffer to panel
 *   Paint_DrawString_EN(x, y, str, font, bg, fg);
 *   Paint_DrawLine(x0, y0, x1, y1, color, width, style);
 *   extern sFONT Font12, Font16, Font20, Font24;
 *
 * If your copy of the library names things differently, adapt the
 * function bodies below. The rest of the sketch only uses the
 * display_* functions declared here.
 */

#include <Arduino.h>

// Pull in the Waveshare ST7305 / Paint headers. These header names
// match the Waveshare ESP32-S3-RLCD-4.2 Arduino demo as of 2026.
// If your library puts them elsewhere, fix these two includes.
#include "LCD_Driver.h"
#include "GUI_Paint.h"
#include "fonts.h"

// Colours in the Waveshare lib are uint16 even on mono panels; BLACK
// and WHITE are the only meaningful values for the ST7305.
#ifndef BLACK
#define BLACK 0x0000
#endif
#ifndef WHITE
#define WHITE 0xFFFF
#endif

enum DisplayFont { FONT_SMALL, FONT_ROW, FONT_HEADER };

// Tracks the current font so the draw helpers can pick the right
// sFONT pointer.
static sFONT* g_currentFont = &Font16;

inline void display_begin() {
  Config_Init();       // GPIO + SPI init in the Waveshare demo
  LCD_Init();          // ST7305 panel init
  // Allocate the Paint framebuffer for 400x300 mono. The Waveshare
  // demo defines image buffer macros; if yours uses a different
  // allocation pattern, mirror it from their example.
  static uint8_t imageBuf[(400 * 300) / 8];
  Paint_NewImage(imageBuf, 400, 300, 0, WHITE);
  Paint_SelectImage(imageBuf);
  Paint_Clear(WHITE);
}

inline void display_clear() {
  Paint_Clear(WHITE);
}

inline void display_flush() {
  // Push the framebuffer to the panel. The Waveshare demo names
  // vary: LCD_Display(), LCD_Update(), or LCD_Refresh(). Pick the
  // one your library provides.
  LCD_Display(Paint_GetImage());
}

inline void display_setFont(DisplayFont f) {
  switch (f) {
    case FONT_SMALL:  g_currentFont = &Font12; break;
    case FONT_ROW:    g_currentFont = &Font20; break;
    case FONT_HEADER: g_currentFont = &Font24; break;
  }
}

inline void display_drawText(int x, int y, const char* s) {
  // Paint_DrawString_EN takes the TOP-LEFT of the glyph box. The
  // sketch passes y as a baseline-ish reference (~the line's
  // bottom). Subtract the font height to convert. This keeps the
  // sketch coordinate math readable.
  int top = y - g_currentFont->Height;
  if (top < 0) top = 0;
  Paint_DrawString_EN(x, top, s, g_currentFont, WHITE, BLACK);
}

inline void display_drawHLine(int x, int y, int w) {
  Paint_DrawLine(x, y, x + w, y, BLACK, DOT_PIXEL_1X1, LINE_STYLE_SOLID);
}
