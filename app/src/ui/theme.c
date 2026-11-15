/*
 * Theme building blocks.
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#include "theme.h"

void ui_theme_init(void)
{
	lv_obj_t *scr = lv_screen_active();

	lv_obj_remove_style_all(scr);
	lv_obj_set_style_bg_color(scr, ui_c(C_BG0), 0);
	lv_obj_set_style_bg_opa(scr, LV_OPA_COVER, 0);
	lv_obj_set_style_text_color(scr, ui_c(C_TEXT), 0);
	lv_obj_set_style_text_font(scr, &font_text_16, 0);
	lv_obj_remove_flag(scr, LV_OBJ_FLAG_SCROLLABLE);
}

lv_obj_t *ui_box(lv_obj_t *parent, int32_t x, int32_t y, int32_t w, int32_t h)
{
	lv_obj_t *o = lv_obj_create(parent);

	lv_obj_remove_style_all(o);
	lv_obj_set_pos(o, x, y);
	lv_obj_set_size(o, w, h);
	lv_obj_remove_flag(o, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE);
	return o;
}

lv_obj_t *ui_card(lv_obj_t *parent, int32_t x, int32_t y, int32_t w, int32_t h)
{
	lv_obj_t *o = ui_box(parent, x, y, w, h);

	lv_obj_set_style_bg_color(o, ui_c(C_BG1), 0);
	lv_obj_set_style_bg_opa(o, LV_OPA_COVER, 0);
	lv_obj_set_style_border_color(o, ui_c(C_LINE), 0);
	lv_obj_set_style_border_width(o, 1, 0);
	lv_obj_set_style_radius(o, UI_RADIUS, 0);
	lv_obj_set_style_clip_corner(o, false, 0);
	return o;
}

lv_obj_t *ui_label(lv_obj_t *parent, const lv_font_t *font, uint32_t color, const char *text)
{
	lv_obj_t *l = lv_label_create(parent);

	lv_obj_remove_style_all(l);
	lv_obj_set_style_text_font(l, font, 0);
	lv_obj_set_style_text_color(l, ui_c(color), 0);
	lv_label_set_text(l, text);
	return l;
}

lv_obj_t *ui_caption(lv_obj_t *parent, const char *text)
{
	lv_obj_t *l = ui_label(parent, &font_mono_12, C_TEXT3, text);

	lv_obj_set_style_text_letter_space(l, 1, 0);
	return l;
}

int32_t ui_text_width(const lv_font_t *font, const char *text)
{
	lv_point_t size;

	lv_text_get_size(&size, text, font, 0, 0, LV_COORD_MAX, LV_TEXT_FLAG_NONE);
	return size.x;
}

int32_t ui_draw_text(lv_layer_t *layer, const lv_font_t *font, uint32_t color, int32_t x, int32_t y,
		     const char *text)
{
	lv_draw_label_dsc_t dsc;
	int32_t w = ui_text_width(font, text);
	lv_area_t a = {x, y, x + w + 2, y + lv_font_get_line_height(font)};

	lv_draw_label_dsc_init(&dsc);
	dsc.font = font;
	dsc.color = lv_color_hex(color);
	dsc.text = text;
	dsc.text_local = 1;
	lv_draw_label(layer, &dsc, &a);
	return w;
}

lv_color_t ui_mix(uint32_t a, uint32_t b, uint8_t t)
{
	return lv_color_mix(lv_color_hex(b), lv_color_hex(a), t);
}
