/*
 * Cinema: clips stored in flash, played full screen.
 *
 * The hardware codec decodes each frame to YCbCr on the player thread. This
 * screen's draw handler then has DMA2D convert that frame straight into the
 * LVGL frame buffer, and LVGL draws the controls over it. So the picture is
 * never copied by the CPU, and the overlay is ordinary UI.
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#include <stdio.h>
#include <string.h>

#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>

#include "media/player.h"
#include "platform/psram.h"
#include "screens.h"
#include "ui/raster.h"
#include "ui/theme.h"

LOG_MODULE_REGISTER(cinema, LOG_LEVEL_INF);

#define MAX_CARDS 3
#define TH_W      212
#define TH_H      127
#define OVERLAY_MS 3200

static int n_clips;
static uint32_t *scratch;                 /* ARGB8888 800x480 in PSRAM: thumbnails, partial redraws */
static uint32_t scratch_frame = UINT32_MAX;
static uint32_t *thumb[MAX_CARDS];

/* player view */
static lv_obj_t *pv, *top_bar, *bottom_bar, *l_title, *l_time, *l_stats, *btn_play, *scrub;
static int open_clip = -1;
static uint32_t overlay_until;
static bool overlay_on;
static uint32_t shown_frame = UINT32_MAX;
static bool scrubbing;

/* ---- thumbnails --------------------------------------------------------- */

static void make_thumb(int clip)
{
	static uint8_t *mcu;
	const struct n6v_header *h = player_clip(clip);
	struct jpeg_info info;

	if (mcu == NULL) {
		mcu = psram_alloc(JPEG_MCU_BYTES(800, 480), 32);
	}
	thumb[clip] = psram_alloc(TH_W * TH_H * 4, 32);
	if (mcu == NULL || thumb[clip] == NULL ||
	    player_decode_still(clip, h->n_frames / 3, mcu, JPEG_MCU_BYTES(800, 480), &info) != 0 ||
	    jpegdec_convert(mcu, &info, scratch, GFX_W, NULL) != 0) {
		thumb[clip] = NULL;
		return;
	}
	/* box filter: every source pixel contributes, so fine detail does not alias */
	for (int y = 0; y < TH_H; y++) {
		int sy0 = y * info.height / TH_H, sy1 = MAX((y + 1) * info.height / TH_H, sy0 + 1);

		for (int x = 0; x < TH_W; x++) {
			int sx0 = x * info.width / TH_W, sx1 = MAX((x + 1) * info.width / TH_W, sx0 + 1);
			uint32_t r = 0, g = 0, b = 0, n = 0;

			for (int sy = sy0; sy < sy1; sy++) {
				const uint32_t *row = scratch + sy * GFX_W;

				for (int sx = sx0; sx < sx1; sx++, n++) {
					r += (row[sx] >> 16) & 0xff;
					g += (row[sx] >> 8) & 0xff;
					b += row[sx] & 0xff;
				}
			}
			thumb[clip][y * TH_W + x] = 0xff000000u | ((r / n) << 16) | ((g / n) << 8) | (b / n);
		}
	}
}

static void blit(const struct raster *r, const uint32_t *src, int w, int h)
{
	for (int y = MAX(r->box.y1, r->clip.y1); y <= MIN(r->box.y1 + h - 1, r->clip.y2); y++) {
		int x1 = MAX(r->box.x1, r->clip.x1), x2 = MIN(r->box.x1 + w - 1, r->clip.x2);

		if (x2 >= x1) {
			memcpy(r->base + y * r->stride + x1, src + (y - r->box.y1) * w + (x1 - r->box.x1),
			       (x2 - x1 + 1) * 4);
		}
	}
}

static void thumb_draw(lv_event_t *e)
{
	int clip = (int)(intptr_t)lv_event_get_user_data(e);
	struct raster r;

	if (raster_begin(&r, e) && thumb[clip]) {
		blit(&r, thumb[clip], TH_W, TH_H);
	}
}

/* ---- player view -------------------------------------------------------- */

static void pv_draw(lv_event_t *e)
{
	struct jpeg_info info;
	struct raster r;
	uint32_t frame, us = 0;
	bool fresh;

	if (!raster_begin(&r, e)) {
		return;
	}
	const uint8_t *mcu = player_acquire(&info, &frame, &fresh);

	if (mcu == NULL) {
		raster_fill(&r, r.box.x1, r.box.y1, r.box.x2, r.box.y2, 0x000000);
		return;
	}
	shown_frame = frame;
	if (r.clip.x1 == r.box.x1 && r.clip.y1 == r.box.y1 && r.clip.x2 == r.box.x2 && r.clip.y2 == r.box.y2) {
		/* whole view: convert into the frame buffer itself */
		jpegdec_convert(mcu, &info, r.base + r.box.y1 * r.stride + r.box.x1, r.stride, &us);
		player_note_convert(us);
	} else {
		/* a partial redraw (an overlay label changed): go through the copy */
		if (scratch_frame != frame) {
			jpegdec_convert(mcu, &info, scratch, GFX_W, NULL);
			scratch_frame = frame;
		}
		blit(&r, scratch, GFX_W, GFX_H);
	}
}

static void show_overlay(bool on)
{
	overlay_on = on;
	lv_obj_t *const parts[] = {top_bar, bottom_bar, l_stats};

	for (size_t i = 0; i < ARRAY_SIZE(parts); i++) {
		if (on) {
			lv_obj_remove_flag(parts[i], LV_OBJ_FLAG_HIDDEN);
		} else {
			lv_obj_add_flag(parts[i], LV_OBJ_FLAG_HIDDEN);
		}
	}
	if (on) {
		overlay_until = k_uptime_get_32() + OVERLAY_MS;
	}
	lv_obj_invalidate(pv);
}

static void close_player(void)
{
	if (open_clip < 0) {
		return;
	}
	player_close();
	open_clip = -1;
	lv_obj_add_flag(pv, LV_OBJ_FLAG_HIDDEN);
}

static void open_player(int clip)
{
	const struct n6v_header *h = player_clip(clip);

	if (h == NULL || player_open(clip) != 0) {
		return;
	}
	open_clip = clip;
	shown_frame = scratch_frame = UINT32_MAX;
	lv_label_set_text(l_title, h->title);
	lv_obj_remove_flag(pv, LV_OBJ_FLAG_HIDDEN);
	lv_obj_move_foreground(pv);
	player_set_loop(false);
	player_play();
	show_overlay(true);
}

static void pv_click(lv_event_t *e)
{
	ARG_UNUSED(e);
	show_overlay(!overlay_on);
}

static void back_click(lv_event_t *e)
{
	ARG_UNUSED(e);
	close_player();
}

static void play_click(lv_event_t *e)
{
	struct player_status st;

	ARG_UNUSED(e);
	player_get_status(&st);
	if (st.playing) {
		player_pause();
	} else {
		player_play();
	}
	show_overlay(true);
}

static void play_draw(lv_event_t *e)
{
	struct player_status st;
	struct raster r;

	if (!raster_begin(&r, e)) {
		return;
	}
	player_get_status(&st);
	int cx = (r.box.x1 + r.box.x2) / 2, cy = (r.box.y1 + r.box.y2) / 2;

	raster_ring(&r, cx * RQ_ONE + 8, cy * RQ_ONE + 8, 22 * RQ_ONE, 2 * RQ_ONE, C_TEXT, 230);
	if (st.playing) {
		raster_fill(&r, cx - 7, cy - 9, cx - 3, cy + 9, C_TEXT);
		raster_fill(&r, cx + 3, cy - 9, cx + 7, cy + 9, C_TEXT);
	} else {
		for (int dy = -10; dy <= 10; dy++) { /* triangle pointing right */
			int half = (10 - LV_ABS(dy)) * 16 / 10;

			raster_fill(&r, cx - 5, cy + dy, cx - 5 + half, cy + dy, C_TEXT);
		}
	}
}

static void scrub_draw(lv_event_t *e)
{
	struct player_status st;
	struct raster r;

	if (!raster_begin(&r, e)) {
		return;
	}
	player_get_status(&st);
	int cy = (r.box.y1 + r.box.y2) / 2;
	int w = lv_area_get_width(&r.box) - 16;
	int x0 = r.box.x1 + 8;
	int px = st.n_frames > 1 ? (int)((uint64_t)st.frame * w / (st.n_frames - 1)) : 0;

	raster_fill_a(&r, x0, cy - 2, x0 + w, cy + 1, C_TEXT, 70);
	raster_fill(&r, x0, cy - 2, x0 + px, cy + 1, C_AZURE);
	raster_disc(&r, (x0 + px) * RQ_ONE, cy * RQ_ONE, (scrubbing ? 10 : 7) * RQ_ONE, C_TEXT, 255);
}

static void scrub_touch(lv_event_t *e)
{
	lv_event_code_t code = lv_event_get_code(e);
	struct player_status st;
	lv_point_t p;
	lv_area_t a;

	if (code == LV_EVENT_RELEASED || code == LV_EVENT_PRESS_LOST) {
		scrubbing = false;
		show_overlay(true);
		return;
	}
	scrubbing = true;
	player_get_status(&st);
	lv_indev_get_point(lv_indev_active(), &p);
	lv_obj_get_coords(scrub, &a);
	int w = lv_area_get_width(&a) - 16;
	int x = CLAMP(p.x - a.x1 - 8, 0, w);

	/* intra-only video: any frame is a direct lookup, so scrubbing is immediate */
	player_seek((uint32_t)((uint64_t)x * (st.n_frames - 1) / w));
	show_overlay(true);
}

static lv_obj_t *shade(lv_obj_t *parent, int y, int h)
{
	lv_obj_t *o = ui_box(parent, 0, y, GFX_W, h);

	lv_obj_set_style_bg_color(o, ui_c(0x000000), 0);
	lv_obj_set_style_bg_opa(o, LV_OPA_60, 0);
	return o;
}

static void build_player(void)
{
	pv = ui_box(lv_screen_active(), 0, 0, GFX_W, GFX_H);
	lv_obj_add_flag(pv, LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_HIDDEN);
	lv_obj_add_event_cb(pv, pv_draw, LV_EVENT_DRAW_MAIN, NULL);
	lv_obj_add_event_cb(pv, pv_click, LV_EVENT_SHORT_CLICKED, NULL);

	top_bar = shade(pv, 0, 52);
	lv_obj_t *back = ui_label(top_bar, &font_textm_16, C_TEXT, "\xE2\x86\x90  Clips");

	lv_obj_set_style_pad_all(back, 14, 0);
	lv_obj_add_flag(back, LV_OBJ_FLAG_CLICKABLE);
	lv_obj_set_ext_click_area(back, 12);
	lv_obj_add_event_cb(back, back_click, LV_EVENT_CLICKED, NULL);
	lv_obj_align(back, LV_ALIGN_LEFT_MID, 4, 0);
	l_title = ui_label(top_bar, &font_head_20, C_TEXT, "");
	lv_obj_align(l_title, LV_ALIGN_CENTER, 0, 0);

	bottom_bar = shade(pv, GFX_H - 72, 72);
	btn_play = ui_box(bottom_bar, 14, 10, 52, 52);
	lv_obj_add_flag(btn_play, LV_OBJ_FLAG_CLICKABLE);
	lv_obj_add_event_cb(btn_play, play_draw, LV_EVENT_DRAW_MAIN, NULL);
	lv_obj_add_event_cb(btn_play, play_click, LV_EVENT_CLICKED, NULL);

	scrub = ui_box(bottom_bar, 78, 14, 560, 44);
	lv_obj_add_flag(scrub, LV_OBJ_FLAG_CLICKABLE);
	lv_obj_add_event_cb(scrub, scrub_draw, LV_EVENT_DRAW_MAIN, NULL);
	lv_obj_add_event_cb(scrub, scrub_touch, LV_EVENT_PRESSING, NULL);
	lv_obj_add_event_cb(scrub, scrub_touch, LV_EVENT_RELEASED, NULL);
	lv_obj_add_event_cb(scrub, scrub_touch, LV_EVENT_PRESS_LOST, NULL);

	l_time = ui_label(bottom_bar, &font_mono_14, C_TEXT, "0:00 / 0:00");
	lv_obj_align(l_time, LV_ALIGN_RIGHT_MID, -UI_PAD, 0);

	/* stats for nerds */
	l_stats = ui_label(pv, &font_mono_12, C_TEXT, "");
	lv_obj_set_style_bg_color(l_stats, ui_c(0x000000), 0);
	lv_obj_set_style_bg_opa(l_stats, LV_OPA_60, 0);
	lv_obj_set_style_pad_all(l_stats, 8, 0);
	lv_obj_set_style_radius(l_stats, 6, 0);
	lv_obj_set_style_text_line_space(l_stats, 3, 0);
	lv_obj_align(l_stats, LV_ALIGN_TOP_RIGHT, -UI_PAD, 64);
}

/* ---- gallery ------------------------------------------------------------ */

static void card_click(lv_event_t *e)
{
	open_player((int)(intptr_t)lv_event_get_user_data(e));
}

static void create(lv_obj_t *root)
{
	scratch = psram_alloc(GFX_W * GFX_H * 4, 32);
	/* not MIN(player_init(), ...): the macro would evaluate the call twice */
	int found = scratch ? player_init() : 0;

	n_clips = MIN(found, MAX_CARDS);

	lv_obj_set_pos(ui_caption(root, "CLIPS IN FLASH"), UI_PAD, 12);
	lv_obj_t *fmt = ui_caption(root, "MOTION JPEG  HARDWARE DECODE  DMA2D COLOUR CONVERSION");

	lv_obj_align(fmt, LV_ALIGN_TOP_RIGHT, -UI_PAD, 12);

	if (n_clips == 0) {
		lv_obj_t *l = ui_label(root, &font_textm_20, C_TEXT2, "No clips in flash");

		lv_obj_align(l, LV_ALIGN_CENTER, 0, -14);
		l = ui_label(root, &font_mono_14, C_TEXT3, "tools/n6v.py encode, pack, flash");
		lv_obj_align(l, LV_ALIGN_CENTER, 0, 18);
		return;
	}
	for (int i = 0; i < n_clips; i++) {
		const struct n6v_header *h = player_clip(i);
		uint32_t secs = (uint32_t)((uint64_t)h->n_frames * h->fps_den / h->fps_num);
		lv_obj_t *c = ui_card(root, UI_PAD + i * (244 + UI_GAP), 40, 244, 250);

		make_thumb(i);
		lv_obj_add_flag(c, LV_OBJ_FLAG_CLICKABLE);
		lv_obj_set_style_bg_color(c, ui_c(C_BG2), LV_STATE_PRESSED);
		lv_obj_set_style_border_color(c, ui_c(C_AZURE), LV_STATE_PRESSED);
		lv_obj_add_event_cb(c, card_click, LV_EVENT_CLICKED, (void *)(intptr_t)i);

		lv_obj_t *t = ui_box(c, 16, 16, TH_W, TH_H);

		lv_obj_add_event_cb(t, thumb_draw, LV_EVENT_DRAW_MAIN, (void *)(intptr_t)i);
		lv_obj_set_pos(ui_label(c, &font_head_20, C_TEXT, h->title), 16, 156);

		lv_obj_t *meta = ui_label(c, &font_mono_12, C_TEXT2, "");

		lv_label_set_text_fmt(meta, "%u:%02u   %u fps   %u.%u MB", secs / 60, secs % 60, h->fps_num / h->fps_den,
				      h->file_size / 1000000, (h->file_size / 100000) % 10);
		lv_obj_set_pos(meta, 16, 190);
		lv_obj_set_pos(ui_caption(c, "TAP TO PLAY"), 16, 216);
	}
	build_player();
}

static void tick(uint32_t now_ms)
{
	static uint32_t text_t0;
	struct player_status st;

	if (open_clip < 0) {
		return;
	}
	player_get_status(&st);

	if (overlay_on && st.playing && !scrubbing && (int32_t)(now_ms - overlay_until) > 0) {
		show_overlay(false);
	}
	if (!st.playing && !overlay_on) {
		show_overlay(true); /* paused or finished: keep the controls up */
	}
	if (st.frame != shown_frame) {
		lv_obj_invalidate(pv);
	}
	if (overlay_on && now_ms - text_t0 >= 200) {
		const struct n6v_header *h = player_clip(open_clip);
		uint32_t fps = h->fps_num / h->fps_den;
		uint32_t pos = st.frame / fps, len = st.n_frames / fps;

		text_t0 = now_ms;
		lv_label_set_text_fmt(l_time, "%u:%02u / %u:%02u", pos / 60, pos % 60, len / 60, len % 60);
		lv_label_set_text_fmt(l_stats,
				      "FRAME    %u / %u\nDECODE   %u.%u ms\nCONVERT  %u.%u ms\n"
				      "BITRATE  %u.%u Mbit/s\nFRAME    %u KB\nDROPPED  %u",
				      st.frame + 1, st.n_frames, st.decode_us / 1000, (st.decode_us / 100) % 10,
				      st.convert_us / 1000, (st.convert_us / 100) % 10, st.kbps / 1000,
				      (st.kbps / 100) % 10, st.frame_bytes / 1024, st.dropped);
		/* label changes are small dirty areas; redraw the whole view with them
		 * so the picture takes the fast path
		 */
		lv_obj_invalidate(pv);
	}
}

static void hide(void)
{
	close_player();
}

static void mini(const struct raster *r)
{
	if (n_clips > 0 && thumb[0]) {
		int w = MIN(lv_area_get_width(&r->box), TH_W), h = MIN(lv_area_get_height(&r->box), TH_H);

		blit(r, thumb[0], TH_W, h);
		ARG_UNUSED(w);
	}
}

const struct screen screen_cinema = {
	.title = "Cinema",
	.blurb = "Video, decoded in hardware",
	.create = create,
	.tick = tick,
	.hide = hide,
	.mini = mini,
};
