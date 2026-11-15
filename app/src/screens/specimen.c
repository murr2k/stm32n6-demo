/*
 * Specimen: type scale, colour tokens and a motion test. A bring-up screen
 * used to check the design system on the real panel.
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#include "screens.h"
#include "ui/raster.h"
#include "ui/theme.h"

static void anim_x(void *obj, int32_t v)
{
	lv_obj_set_x(obj, v);
}

static void create(lv_obj_t *root)
{
	static const struct {
		const lv_font_t *font;
		uint32_t color;
		const char *text;
	} lines[] = {
		{&font_head_40, C_TEXT, "Head 40 Signal Lab"},
		{&font_head_28, C_TEXT, "Head 28 Flight Deck 59.98 Hz"},
		{&font_head_20, C_TEXT2, "Head 20 Throughput"},
		{&font_textm_20, C_TEXT, "Text Medium 20 Ethernet link"},
		{&font_textm_16, C_TEXT, "Text Medium 16 The quick brown fox"},
		{&font_text_16, C_TEXT2, "Text 16 jumps over the lazy dog 0123456789"},
		{&font_text_14, C_TEXT2, "Text 14 Cortex-M55 at 800 MHz, 3.3 V \xC2\xB1 5 %"},
		{&font_mono_14, C_TEXT, "Mono 14  0x3420_0000  16.67 ms  \xE2\x86\x91 1.2 MB/s"},
		{&font_mono_12, C_TEXT3, "MONO 12 CAPTION  FRAME TIME"},
	};
	static const uint32_t swatch[] = {C_AZURE, C_AMBER, C_VIOLET, C_MINT, C_CORAL, C_BAD,
					  C_BG1,   C_BG2,   C_LINE,   C_LINE2, C_TEXT3, C_TEXT2};
	int32_t y = UI_PAD;

	for (size_t i = 0; i < ARRAY_SIZE(lines); i++) {
		lv_obj_t *l = ui_label(root, lines[i].font, lines[i].color, lines[i].text);

		lv_obj_set_pos(l, UI_PAD, y);
		y += lv_font_get_line_height(lines[i].font) + 6;
	}

	lv_obj_t *num = ui_label(root, &font_num_96, C_TEXT, "59.98");

	lv_obj_set_pos(num, 470, 4);
	lv_obj_t *unit = ui_label(root, &font_head_28, C_TEXT2, "Hz");

	lv_obj_align_to(unit, num, LV_ALIGN_OUT_RIGHT_BOTTOM, 8, -18);
	lv_obj_t *num2 = ui_label(root, &font_num_56, C_AZURE, "16.7");

	lv_obj_set_pos(num2, 470, 112);
	lv_obj_t *num3 = ui_label(root, &font_num_32, C_AMBER, "1,000 Mb/s");

	lv_obj_set_pos(num3, 600, 130);

	for (size_t i = 0; i < ARRAY_SIZE(swatch); i++) {
		lv_obj_t *s = ui_box(root, 470 + (i % 6) * 52, 200 + (i / 6) * 52, 44, 44);

		lv_obj_set_style_bg_color(s, ui_c(swatch[i]), 0);
		lv_obj_set_style_bg_opa(s, LV_OPA_COVER, 0);
		lv_obj_set_style_radius(s, 8, 0);
		lv_obj_set_style_border_color(s, ui_c(C_LINE2), 0);
		lv_obj_set_style_border_width(s, 1, 0);
	}

	/* gradient strip: banding shows up here if anything drops to 16 bit */
	lv_obj_t *grad = ui_box(root, UI_PAD, 330, GFX_W - 2 * UI_PAD, 40);

	lv_obj_set_style_radius(grad, 8, 0);
	lv_obj_set_style_bg_opa(grad, LV_OPA_COVER, 0);
	lv_obj_set_style_bg_color(grad, ui_c(C_BG1), 0);
	lv_obj_set_style_bg_grad_color(grad, ui_c(0x16324A), 0);
	lv_obj_set_style_bg_grad_dir(grad, LV_GRAD_DIR_HOR, 0);

	/* motion: a card easing back and forth, one refresh-locked animation */
	lv_obj_t *track = ui_box(root, UI_PAD, 386, GFX_W - 2 * UI_PAD, 44);

	lv_obj_set_style_radius(track, 22, 0);
	lv_obj_set_style_bg_color(track, ui_c(C_BG2), 0);
	lv_obj_set_style_bg_opa(track, LV_OPA_COVER, 0);

	lv_obj_t *puck = ui_box(track, 4, 4, 120, 36);

	lv_obj_set_style_radius(puck, 18, 0);
	lv_obj_set_style_bg_color(puck, ui_c(C_AZURE), 0);
	lv_obj_set_style_bg_opa(puck, LV_OPA_COVER, 0);

	lv_anim_t a;

	lv_anim_init(&a);
	lv_anim_set_var(&a, puck);
	lv_anim_set_exec_cb(&a, anim_x);
	lv_anim_set_values(&a, 4, GFX_W - 2 * UI_PAD - 124);
	lv_anim_set_duration(&a, 1400);
	lv_anim_set_reverse_duration(&a, 1400);
	lv_anim_set_path_cb(&a, lv_anim_path_ease_in_out);
	lv_anim_set_repeat_count(&a, LV_ANIM_REPEAT_INFINITE);
	lv_anim_start(&a);
}

static void mini(const struct raster *r)
{
	static const uint32_t c[] = {C_AZURE, C_AMBER, C_VIOLET, C_MINT, C_CORAL};
	int w = lv_area_get_width(&r->box) / 5;

	for (int i = 0; i < 5; i++) {
		raster_disc(r, (r->box.x1 + i * w + w / 2) * RQ_ONE, ((r->box.y1 + r->box.y2) / 2) * RQ_ONE,
			    (w / 2 - 6) * RQ_ONE, c[i], 255);
	}
}

const struct screen screen_specimen = {
	.title = "Specimen",
	.blurb = "Type and colour",
	.create = create,
	.mini = mini,
};
