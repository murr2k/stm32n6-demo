/*
 * Design tokens and small building blocks shared by every screen.
 *
 * One dark, instrument-grade theme. Colour carries meaning: azure is the
 * primary data hue, amber the secondary, and green / amber / red are reserved
 * for state. Type is IBM Plex: Sans for text, Sans Condensed for headings and
 * large numerals, Mono for readouts that must not jitter.
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#ifndef UI_THEME_H
#define UI_THEME_H

#include <lvgl.h>

#include "fonts/fonts.h"

/* ---- colour ------------------------------------------------------------- */
#define C_BG0     0x080C12 /* screen */
#define C_BG1     0x0F151E /* card */
#define C_BG2     0x18212D /* track, raised, pressed */
#define C_LINE    0x222D3C /* hairline */
#define C_LINE2   0x34435A /* stronger divider, axis */
#define C_TEXT    0xE9EEF5
#define C_TEXT2   0x98A6B8
#define C_TEXT3   0x5F6D80

#define C_AZURE   0x4CB5FF /* primary data */
#define C_AMBER   0xFFB13D /* secondary data */
#define C_VIOLET  0xB58CFF
#define C_MINT    0x3DDCA0
#define C_CORAL   0xFF6B7A

#define C_OK      C_MINT
#define C_WARN    C_AMBER
#define C_BAD     0xFF5252

/* ---- metrics ------------------------------------------------------------ */
#define UI_BAR_H      36           /* status bar */
#define UI_CONTENT_Y  UI_BAR_H
#define UI_CONTENT_H  (480 - UI_BAR_H)
#define UI_PAD        16
#define UI_GAP        12
#define UI_RADIUS     10

/* ---- motion ------------------------------------------------------------- */
#define UI_T_FAST     140
#define UI_T_BASE     260
#define UI_T_SLOW     420

static inline lv_color_t ui_c(uint32_t hex)
{
	return lv_color_hex(hex);
}

void ui_theme_init(void);

/* A bare object: no theme styles, no scrolling, not clickable. */
lv_obj_t *ui_box(lv_obj_t *parent, int32_t x, int32_t y, int32_t w, int32_t h);

/* A card: BG1 fill, hairline border, rounded. */
lv_obj_t *ui_card(lv_obj_t *parent, int32_t x, int32_t y, int32_t w, int32_t h);

lv_obj_t *ui_label(lv_obj_t *parent, const lv_font_t *font, uint32_t color, const char *text);

/* Small uppercase caption used above values: Mono 12, letter-spaced, TEXT3. */
lv_obj_t *ui_caption(lv_obj_t *parent, const char *text);

/* Blend two colours, t in 0..255. */
lv_color_t ui_mix(uint32_t a, uint32_t b, uint8_t t);

#endif
