/*
 * Home: one tile per screen. The thumbnail in each tile is not an icon, it is
 * drawn every frame by that screen's own code from its live data.
 *
 * Two rows of four tiles fill the screen. More screens than eight go into
 * further columns on the right, same tile size, and the whole field pans
 * sideways under the finger (it comes to rest on a column, so no tile is
 * left cut in half; a sliver of the next column shows at the edge). The first eight keep their places.
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#include <zephyr/kernel.h>

#include "screens.h"
#include "ui/raster.h"
#include "ui/theme.h"

#define COLS       4 /* tiles across the screen */
#define ROWS       2
#define MAX_TILES  24
#define TILE_W     ((GFX_W - 2 * UI_PAD - (COLS - 1) * UI_GAP) / COLS)
#define TILE_H     ((UI_CONTENT_H - 2 * UI_PAD - (ROWS - 1) * UI_GAP) / ROWS)
#define PITCH      (TILE_W + UI_GAP)
#define EDGE_GUARD 28 /* the shell's edge-swipe zone */
#define IND_W      96 /* position indicator in the bottom margin */
#define IND_H      4

static lv_obj_t *pan, *guard[2], *ind, *thumb;
static lv_obj_t *tiles[MAX_TILES], *minis[MAX_TILES];
static int tile_col[MAX_TILES];
static int tile_slot[MAX_TILES]; /* its place among the tiles shown, -1 while not listed */
static uint32_t tiles_listed;
static int n_real;      /* tiles that open a screen */
static int n_tiles;     /* those plus the test tiles */
static int n_shown;     /* the tiles listed now */
static int n_real_shown;
static int n_cols;
static int test_tiles;
static int thumb_w, shown_scroll;
static volatile int st_scroll, st_max;

static void mini_draw(lv_event_t *e)
{
	int i = (int)(intptr_t)lv_event_get_user_data(e);
	const struct screen *s = i < n_real ? shell_screen(i + 1) : NULL;
	struct raster r;

	if (!raster_begin(&r, e)) {
		return;
	}
	if (s && s->mini) {
		s->mini(&r);
		if (s->mini_text) {
			s->mini_text(lv_event_get_layer(e), &r);
		}
	} else if (s == NULL) {
		/* a test tile has nothing to show: say so with an empty ruled box */
		for (int y = r.box.y1 + 8; y < r.box.y2; y += 12) {
			raster_hline_dash(&r, r.box.x1, r.box.x2, y, C_LINE2, 255, 2, 6);
		}
	}
}

static void tile_click(lv_event_t *e)
{
	shell_goto((int)(intptr_t)lv_event_get_user_data(e));
}

static bool tile_listed(int i)
{
	return i >= n_real || shell_listed(i + 1);
}

/* Every listed tile in its place: the first eight on the screen, the rest in
 * further columns. Objects move only when the set of listed tiles changes (a
 * screen whose data comes and goes, such as Home Power). */
static void place(void)
{
	int k = 0, real = 0;

	tiles_listed = 0;
	for (int i = 0; i < n_tiles; i++) {
		if (!tile_listed(i)) {
			if (tile_slot[i] != -1) {
				lv_obj_add_flag(tiles[i], LV_OBJ_FLAG_HIDDEN);
				tile_slot[i] = -1;
			}
			continue;
		}
		tiles_listed |= BIT(i);
		real += i < n_real;
		if (tile_slot[i] != k) {
			int over = k - COLS * ROWS;
			int col = over < 0 ? k % COLS : COLS + over / ROWS;
			int row = over < 0 ? k / COLS : over % ROWS;

			lv_obj_set_pos(tiles[i], col * PITCH, row * (TILE_H + UI_GAP));
			lv_obj_remove_flag(tiles[i], LV_OBJ_FLAG_HIDDEN);
			tile_slot[i] = k;
			tile_col[i] = col;
		}
		k++;
	}
	n_shown = k;
	n_real_shown = real;
	n_cols = MAX(COLS, COLS + (n_shown - COLS * ROWS + ROWS - 1) / ROWS);

	/* With nothing to pan to, Home is as it was: no scrolling, no guards. */
	bool more = n_cols > COLS;
	int max = (n_cols - COLS) * PITCH;

	lv_obj_set_flag(pan, LV_OBJ_FLAG_SCROLLABLE, more);
	lv_obj_set_flag(guard[0], LV_OBJ_FLAG_HIDDEN, !more);
	lv_obj_set_flag(guard[1], LV_OBJ_FLAG_HIDDEN, !more);
	lv_obj_set_flag(ind, LV_OBJ_FLAG_HIDDEN, !more);
	thumb_w = IND_W * COLS / n_cols;
	lv_obj_set_width(thumb, thumb_w);
	if (lv_obj_get_scroll_x(pan) > max) {
		lv_obj_scroll_to_x(pan, max, LV_ANIM_OFF);
	}
	shown_scroll = -1;
}

static void build(void)
{
	n_real = MIN(shell_count() - 1, MAX_TILES); /* every screen but this one */
	n_tiles = MIN(n_real + test_tiles, MAX_TILES);

	lv_obj_clean(pan);
	lv_obj_scroll_to_x(pan, 0, LV_ANIM_OFF);
	for (int i = 0; i < n_tiles; i++) {
		const struct screen *s = i < n_real ? shell_screen(i + 1) : NULL;
		lv_obj_t *t = ui_card(pan, 0, 0, TILE_W, TILE_H);

		tiles[i] = t;
		tile_slot[i] = -2; /* placed by place() */
		lv_obj_add_flag(t, LV_OBJ_FLAG_CLICKABLE);
		lv_obj_set_style_bg_color(t, ui_c(C_BG2), LV_STATE_PRESSED);
		lv_obj_set_style_border_color(t, ui_c(C_AZURE), LV_STATE_PRESSED);
		if (s != NULL) {
			lv_obj_add_event_cb(t, tile_click, LV_EVENT_CLICKED, (void *)(intptr_t)(i + 1));
		}

		minis[i] = ui_box(t, 16, 16, TILE_W - 32, TILE_H - 96);
		lv_obj_add_event_cb(minis[i], mini_draw, LV_EVENT_DRAW_MAIN, (void *)(intptr_t)i);

		lv_obj_t *title = ui_label(t, &font_head_20, s ? C_TEXT : C_TEXT3, s ? s->title : "Test tile");

		lv_obj_set_pos(title, 16, TILE_H - 66);
		lv_obj_t *blurb = ui_label(t, &font_text_14, s ? C_TEXT2 : C_TEXT3,
					   s ? (s->blurb ? s->blurb : "") : "placeholder");

		lv_obj_set_pos(blurb, 16, TILE_H - 38);

		lv_obj_t *idx = ui_caption(t, "");

		lv_label_set_text_fmt(idx, "%02d", i + 1);
		lv_obj_align(idx, LV_ALIGN_BOTTOM_RIGHT, -16, -14);
	}
	place();
}

/* Come to rest on a column. LVGL's own snapping is not used: it lets the last
 * column snap to the left edge, which is three columns of nothing.
 */
static void pan_rest(lv_event_t *e)
{
	int max = (n_cols - COLS) * PITCH;
	int sx = lv_obj_get_scroll_x(pan);
	int to = CLAMP((sx + PITCH / 2) / PITCH * PITCH, 0, max);

	ARG_UNUSED(e);
	if (to != sx) {
		lv_obj_scroll_to_x(pan, to, LV_ANIM_ON);
	}
}

static void create(lv_obj_t *root)
{
	pan = ui_box(root, 0, 0, GFX_W, UI_CONTENT_H);
	lv_obj_set_style_pad_left(pan, UI_PAD, 0);
	lv_obj_set_style_pad_right(pan, UI_PAD, 0);
	lv_obj_set_style_pad_top(pan, UI_PAD, 0);
	lv_obj_set_scroll_dir(pan, LV_DIR_HOR);
	lv_obj_set_scrollbar_mode(pan, LV_SCROLLBAR_MODE_OFF);
	lv_obj_add_event_cb(pan, pan_rest, LV_EVENT_SCROLL_END, NULL);

	/* A drag that starts in the shell's edge zone changes screen: it must not
	 * pan the tiles or press one as well. These swallow it.
	 */
	guard[0] = ui_box(root, 0, 0, EDGE_GUARD, UI_CONTENT_H);
	guard[1] = ui_box(root, GFX_W - EDGE_GUARD, 0, EDGE_GUARD, UI_CONTENT_H);
	lv_obj_add_flag(guard[0], LV_OBJ_FLAG_CLICKABLE);
	lv_obj_add_flag(guard[1], LV_OBJ_FLAG_CLICKABLE);

	ind = ui_box(root, (GFX_W - IND_W) / 2, UI_CONTENT_H - UI_PAD / 2 - IND_H / 2, IND_W, IND_H);
	lv_obj_set_style_bg_color(ind, ui_c(C_LINE2), 0);
	lv_obj_set_style_bg_opa(ind, LV_OPA_COVER, 0);
	lv_obj_set_style_radius(ind, IND_H / 2, 0);
	thumb = ui_box(ind, 0, 0, IND_W, IND_H);
	lv_obj_set_style_bg_color(thumb, ui_c(C_TEXT2), 0);
	lv_obj_set_style_bg_opa(thumb, LV_OPA_COVER, 0);
	lv_obj_set_style_radius(thumb, IND_H / 2, 0);

	build();
}

static void tick(uint32_t now_ms)
{
	int sx = lv_obj_get_scroll_x(pan);
	int max = (n_cols - COLS) * PITCH;

	st_scroll = sx;
	st_max = max;
	if (max > 0 && sx != shown_scroll) {
		shown_scroll = sx;
		lv_obj_set_x(thumb, CLAMP(sx, 0, max) * (IND_W - thumb_w) / max);
	}
	for (int i = 0; i < n_tiles; i++) {
		int x = UI_PAD + tile_col[i] * PITCH - sx;

		if (tile_slot[i] >= 0 && x + TILE_W > 0 && x < GFX_W) {
			lv_obj_invalidate(minis[i]);
		}
	}
	/* No automatic pan in attract mode: while the field moves every visible tile
	 * redraws its live thumbnail each frame, and with ten screens that reached
	 * 15.7 ms and dropped a frame. The position bar shows there is more.
	 */
}

/* A tile comes or goes when its screen's data does, visible or not. */
static void service(uint32_t now_ms)
{
	uint32_t set = 0;

	ARG_UNUSED(now_ms);
	for (int i = 0; i < n_tiles; i++) {
		set |= tile_listed(i) ? BIT(i) : 0;
	}
	if (set != tiles_listed) {
		place();
	}
}

void home_test_tiles(int n)
{
	/* a test: the rebuild and the full redraw after it drop a frame or two */
	gfx_expect_drop(4);
	test_tiles = CLAMP(n, 0, MAX_TILES);
	build();
}

void home_get_status(int *n, int *real, int *scroll_x, int *scroll_max)
{
	*n = n_shown;
	*real = n_real_shown;
	*scroll_x = st_scroll;
	*scroll_max = st_max;
}

const struct screen screen_home = {
	.title = "Home",
	.create = create,
	.tick = tick,
	.service = service,
	.attract_secs = 7,
};
