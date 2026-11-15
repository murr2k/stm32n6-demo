/*
 * Home: one tile per screen. The thumbnail in each tile is not an icon, it is
 * drawn every frame by that screen's own code from its live data.
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#include <zephyr/kernel.h>

#include "screens.h"
#include "ui/raster.h"
#include "ui/theme.h"

#define MAX_TILES 8

static lv_obj_t *minis[MAX_TILES];
static int n_tiles;

static void mini_draw(lv_event_t *e)
{
	const struct screen *s = shell_screen((int)(intptr_t)lv_event_get_user_data(e));
	struct raster r;

	if (s && s->mini && raster_begin(&r, e)) {
		s->mini(&r);
	}
}

static void tile_click(lv_event_t *e)
{
	shell_goto((int)(intptr_t)lv_event_get_user_data(e));
}

static void create(lv_obj_t *root)
{
	int count = shell_count() - 1; /* every screen but this one */
	int cols = count <= 6 ? 3 : 4;
	int rows = (count + cols - 1) / cols;
	int tw = (GFX_W - 2 * UI_PAD - (cols - 1) * UI_GAP) / cols;
	int th = (UI_CONTENT_H - 2 * UI_PAD - (rows - 1) * UI_GAP) / rows;

	n_tiles = MIN(count, MAX_TILES);
	for (int i = 0; i < n_tiles; i++) {
		const struct screen *s = shell_screen(i + 1);
		lv_obj_t *t = ui_card(root, UI_PAD + (i % cols) * (tw + UI_GAP),
				      UI_PAD + (i / cols) * (th + UI_GAP), tw, th);

		lv_obj_add_flag(t, LV_OBJ_FLAG_CLICKABLE);
		lv_obj_set_style_bg_color(t, ui_c(C_BG2), LV_STATE_PRESSED);
		lv_obj_set_style_border_color(t, ui_c(C_AZURE), LV_STATE_PRESSED);
		lv_obj_add_event_cb(t, tile_click, LV_EVENT_CLICKED, (void *)(intptr_t)(i + 1));

		minis[i] = ui_box(t, 16, 16, tw - 32, th - 96);
		lv_obj_add_event_cb(minis[i], mini_draw, LV_EVENT_DRAW_MAIN, (void *)(intptr_t)(i + 1));

		lv_obj_t *title = ui_label(t, cols > 3 ? &font_head_20 : &font_head_28, C_TEXT, s->title);

		lv_obj_set_pos(title, 16, cols > 3 ? th - 66 : th - 74);
		lv_obj_t *blurb = ui_label(t, &font_text_14, C_TEXT2, s->blurb ? s->blurb : "");

		lv_obj_set_pos(blurb, 16, th - 38);

		lv_obj_t *idx = ui_caption(t, "");

		lv_label_set_text_fmt(idx, "%02d", i + 1);
		lv_obj_align(idx, LV_ALIGN_BOTTOM_RIGHT, -16, -14);
	}
}

static void tick(uint32_t now_ms)
{
	ARG_UNUSED(now_ms);
	for (int i = 0; i < n_tiles; i++) {
		lv_obj_invalidate(minis[i]);
	}
}

const struct screen screen_home = {
	.title = "Home",
	.create = create,
	.tick = tick,
};
