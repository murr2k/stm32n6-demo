/*
 * Cinema: clips from the flash and from the microSD card, played full screen.
 *
 * The hardware codec decodes each frame to YCbCr on the player thread. This
 * screen's draw handler then has DMA2D convert that frame straight into the
 * LVGL frame buffer, and LVGL draws the controls over it. So the picture is
 * never copied by the CPU, and the overlay is ordinary UI.
 *
 * The gallery is rebuilt when the card's contents change (an upload from the
 * host, tools/n6v.py). That reads the card and decodes a thumbnail per clip
 * on the UI thread: tens of milliseconds, once, and never while a clip plays.
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#include <stdio.h>
#include <string.h>

#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>

#include <lvgl_private.h> /* lv_cover_check_info_t */

#include "media/player.h"
#include "media/sdcard.h"
#include "platform/psram.h"
#include "screens.h"
#include "ui/raster.h"
#include "ui/theme.h"

LOG_MODULE_REGISTER(cinema, LOG_LEVEL_INF);

#define MAX_CARDS PLAYER_MAX_CLIPS
#define CARD_W    244
#define CARD_H    250
#define ROW_Y     40
#define TH_W      212
#define TH_H      127
#define OVERLAY_MS 3200
#define EDGE_GUARD 28                     /* the shell's edge-swipe zone */

static int n_clips;
static uint32_t *scratch;                 /* ARGB8888 800x480 in PSRAM: partial redraws of the player view */
static uint32_t scratch_frame = UINT32_MAX;
static uint32_t *thumb[MAX_CARDS];        /* the picture on card i, NULL if it has none */
/* What each picture buffer shows, so that a change of the list re-decodes only
 * what is new. Buffers come from the PSRAM bump allocator and are never freed:
 * `px` without `ok` is one waiting to be used again.
 */
static struct thumb_slot {
	uint32_t *px;
	bool ok, card;
	uint32_t size;
	char key[SD_NAME_MAX];            /* file name on the card, title in flash */
} th[MAX_CARDS];
static uint8_t *still_mcu;                /* one decoded frame, for make_thumb() */
static lv_obj_t *row, *l_card;
static uint32_t lib_gen;                  /* the card generation the gallery shows */
static uint32_t scan_gen;                 /* the one being read */
enum { LIB_IDLE, LIB_SCAN, LIB_SCANNED, LIB_THUMBS };
static volatile int lib_state;
static volatile bool thumbs_dirty;
static K_SEM_DEFINE(bg_wake, 0, 1);
static int pending_play = -1;

/* player view */
static lv_obj_t *pv, *top_bar, *bottom_bar, *l_title, *l_time, *l_stats, *btn_play, *scrub;
static volatile int open_clip = -1;
static uint32_t overlay_until;
static bool overlay_on;
static uint32_t shown_frame = UINT32_MAX;
static bool scrubbing;

/* ---- thumbnails --------------------------------------------------------- */

/* Decode one frame of a clip and box-filter it down to a card's picture:
 * every source pixel contributes, so fine detail does not alias.
 *
 * Straight from the codec's output, without DMA2D or a full-size RGB copy,
 * so that it can run on the background thread while the UI thread draws. The
 * codec delivers 4:2:0 as MCUs of 384 bytes in raster order: four 8x8 luma
 * blocks (top left, top right, bottom left, bottom right), then Cb, then Cr.
 */
static bool make_thumb(int clip, uint32_t *px)
{
	const struct n6v_header *h = player_clip(clip);
	struct jpeg_info info;

	if (still_mcu == NULL || px == NULL ||
	    player_decode_still(clip, h->n_frames / 3, still_mcu, JPEG_MCU_BYTES(800, 480), &info) != 0) {
		return false;
	}
	int mcus_per_row = info.width / 16;

	for (int y = 0; y < TH_H; y++) {
		int sy0 = y * info.height / TH_H, sy1 = MAX((y + 1) * info.height / TH_H, sy0 + 1);

		for (int x = 0; x < TH_W; x++) {
			int sx0 = x * info.width / TH_W, sx1 = MAX((x + 1) * info.width / TH_W, sx0 + 1);
			int32_t luma = 0, cb = 0, cr = 0, n = 0;

			for (int sy = sy0; sy < sy1; sy++) {
				for (int sx = sx0; sx < sx1; sx++, n++) {
					const uint8_t *m = still_mcu + ((sy >> 4) * mcus_per_row + (sx >> 4)) * 384;
					int c = ((sy & 15) >> 1) * 8 + ((sx & 15) >> 1);

					luma += m[(((sy >> 3) & 1) * 2 + ((sx >> 3) & 1)) * 64 + (sy & 7) * 8 + (sx & 7)];
					cb += m[256 + c];
					cr += m[320 + c];
				}
			}
			luma /= n;
			cb = cb / n - 128;
			cr = cr / n - 128;
			/* JFIF: full-range BT.601, the same matrix DMA2D applies to the video */
			int32_t r = luma + ((91881 * cr) >> 16);
			int32_t g = luma - ((22554 * cb + 46802 * cr) >> 16);
			int32_t b = luma + ((116130 * cb) >> 16);

			px[y * TH_W + x] = 0xff000000u | (CLAMP(r, 0, 255) << 16) | (CLAMP(g, 0, 255) << 8) | CLAMP(b, 0, 255);
		}
	}
	return true;
}

/* Match pictures to the clip list. A clip keeps its picture when it is the
 * same clip as before (same place, name and size) and its file is not one of
 * the `n_stale` names just uploaded (n_stale < 0: every clip on the card is
 * suspect). The rest get a buffer and wait for make_thumb().
 */
static void assign_thumbs(char (*stale)[SD_NAME_MAX], int n_stale)
{
	static struct thumb_slot old[MAX_CARDS];
	uint32_t *spare[MAX_CARDS];
	int n_spare = 0;

	memcpy(old, th, sizeof(old));
	memset(th, 0, sizeof(th));
	for (int i = 0; i < n_clips; i++) {
		const struct n6v_header *h = player_clip(i);
		bool card = player_clip_on_card(i);
		const char *key = card ? player_clip_file(i) : h->title;
		bool keep = !(card && n_stale < 0);

		for (int k = 0; card && k < n_stale; k++) {
			keep = keep && strcmp(stale[k], key) != 0;
		}
		th[i].card = card;
		th[i].size = h->file_size;
		strncpy(th[i].key, key, sizeof(th[i].key) - 1);
		for (int j = 0; keep && j < MAX_CARDS; j++) {
			if (old[j].px && old[j].ok && old[j].card == card && old[j].size == th[i].size &&
			    strcmp(old[j].key, th[i].key) == 0) {
				th[i].px = old[j].px;
				th[i].ok = true;
				old[j].px = NULL;
				break;
			}
		}
	}
	for (int j = 0; j < MAX_CARDS; j++) {
		if (old[j].px) {
			spare[n_spare++] = old[j].px;
		}
	}
	for (int i = 0; i < MAX_CARDS; i++) {
		if (i < n_clips && !th[i].ok) {
			th[i].px = n_spare ? spare[--n_spare] : psram_alloc(TH_W * TH_H * 4, 32);
		} else if (i >= n_clips && n_spare) {
			th[i].px = spare[--n_spare]; /* parked until the list grows again */
		}
		thumb[i] = th[i].ok ? th[i].px : NULL;
	}
}

/* Make the pictures that are missing. */
static void make_thumbs(void)
{
	for (int i = 0; i < n_clips; i++) {
		if (th[i].px != NULL && !th[i].ok) {
			while (open_clip >= 0) {
				k_msleep(100); /* the codec belongs to the clip that is playing */
			}
			th[i].ok = make_thumb(i, th[i].px);
			if (th[i].ok) {
				thumb[i] = th[i].px;
				thumbs_dirty = true;
			}
		}
	}
}

/* Background thread: what a change of the card's contents needs and the UI
 * thread has no time for. Reading the clip list off the card (LIB_SCAN), and
 * after the UI thread has put that list in place, the new pictures
 * (LIB_THUMBS). The UI thread moves the state to LIB_SCAN and from
 * LIB_SCANNED to LIB_THUMBS; this thread makes the other two steps. While a
 * step is this thread's, the UI thread leaves the list and th[] alone.
 */
static void bg_thread(void *p1, void *p2, void *p3)
{
	for (;;) {
		k_sem_take(&bg_wake, K_FOREVER);
		if (lib_state == LIB_SCAN) {
			player_scan();
			lib_state = LIB_SCANNED;
		} else if (lib_state == LIB_THUMBS) {
			make_thumbs();
			lib_state = LIB_IDLE;
		}
	}
}
/* Below the player's threads, above the network services. FatFs wants about 1 KB of stack for a long name. */
K_THREAD_DEFINE(cinema_bg, 6144, bg_thread, NULL, NULL, NULL, 6, 0, 0);

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

/* The picture covers everything under it. LVGL cannot know that of a widget
 * that draws itself, and would fill the screen background first: 1.5 MB of
 * writes before every video frame, for pixels the picture then replaces.
 */
static void pv_cover(lv_event_t *e)
{
	lv_cover_check_info_t *info = lv_event_get_param(e);

	if (info->res != LV_COVER_RES_MASKED) {
		info->res = LV_COVER_RES_COVER;
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
	shell_set_cover(false);
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
	shell_set_cover(true);
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
	lv_obj_add_event_cb(pv, pv_cover, LV_EVENT_COVER_CHECK, NULL);
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
	lv_obj_set_ext_click_area(scrub, 14); /* a finger, not a stylus */

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

/* One card per clip in a row that scrolls sideways when there are more than three. */
static void build_gallery(void)
{
	lv_obj_clean(row);
	lv_obj_scroll_to_x(row, 0, LV_ANIM_OFF);

	if (n_clips == 0) {
		lv_obj_t *l = ui_label(row, &font_textm_20, C_TEXT2, "No clips");

		lv_obj_align(l, LV_ALIGN_CENTER, 0, -14);
		l = ui_label(row, &font_mono_14, C_TEXT3, "tools/n6v.py encode, then upload (card) or pack and flash");
		lv_obj_align(l, LV_ALIGN_CENTER, 0, 18);
		return;
	}
	for (int i = 0; i < n_clips; i++) {
		const struct n6v_header *h = player_clip(i);
		uint32_t secs = (uint32_t)((uint64_t)h->n_frames * h->fps_den / h->fps_num);
		lv_obj_t *c = ui_card(row, UI_PAD + i * (CARD_W + UI_GAP), 0, CARD_W, CARD_H);

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
		/* where the clip is read from: the two paths are not the same machinery */
		lv_obj_t *tag = ui_caption(c, player_clip_on_card(i) ? "SD CARD" : "FLASH");

		lv_obj_set_style_text_color(tag, ui_c(player_clip_on_card(i) ? C_AZURE : C_TEXT3), 0);
		lv_obj_align(tag, LV_ALIGN_TOP_RIGHT, -16, 216);
	}
}

static void create(lv_obj_t *root)
{
	struct sd_status sd;

	scratch = psram_alloc(GFX_W * GFX_H * 4, 32);
	still_mcu = psram_alloc(JPEG_MCU_BYTES(800, 480), 32);
	if (scratch != NULL) {
		player_init();
	}
	sdcard_get_status(&sd);
	lib_gen = sd.generation;
	/* nothing is on screen yet: the first pictures are made here and now */
	n_clips = MIN(player_clip_count(), MAX_CARDS);
	assign_thumbs(NULL, 0);
	make_thumbs();
	thumbs_dirty = false;

	lv_obj_set_pos(ui_caption(root, "CLIPS"), UI_PAD, 12);
	lv_obj_t *fmt = ui_caption(root, "MOTION JPEG  HARDWARE DECODE  DMA2D COLOUR CONVERSION");

	lv_obj_align(fmt, LV_ALIGN_TOP_RIGHT, -UI_PAD, 12);

	row = ui_box(root, 0, ROW_Y, GFX_W, CARD_H);
	lv_obj_add_flag(row, LV_OBJ_FLAG_SCROLLABLE);
	lv_obj_set_scroll_dir(row, LV_DIR_HOR);
	lv_obj_set_scrollbar_mode(row, LV_SCROLLBAR_MODE_OFF);
	lv_obj_set_style_pad_right(row, UI_PAD, 0);
	/* A drag that starts in the shell's edge zone changes screen: it must not
	 * scroll the row or press a card as well. These swallow it.
	 */
	lv_obj_add_flag(ui_box(root, 0, ROW_Y, EDGE_GUARD, CARD_H), LV_OBJ_FLAG_CLICKABLE);
	lv_obj_add_flag(ui_box(root, GFX_W - EDGE_GUARD, ROW_Y, EDGE_GUARD, CARD_H), LV_OBJ_FLAG_CLICKABLE);

	l_card = ui_caption(root, "");
	lv_obj_set_pos(l_card, UI_PAD, ROW_Y + CARD_H + 16);

	build_gallery();
	build_player();
}

/* The card's state under the gallery: free space, or an upload as it arrives. */
static void update_card_line(const struct sd_status *sd)
{
	static char shown[96];
	char text[96];

	if (sd->receiving) {
		snprintf(text, sizeof(text), "RECEIVING %s   %u %%   %u.%u MB/s", sd->rx_name,
			 sd->rx_size ? (unsigned int)((uint64_t)sd->rx_done * 100 / sd->rx_size) : 0,
			 sd->rx_kbps / 8000, (sd->rx_kbps / 800) % 10);
	} else if (!sd->mounted) {
		snprintf(text, sizeof(text), "NO SD CARD");
	} else if (!sd->space_known) {
		snprintf(text, sizeof(text), "SD CARD");
	} else {
		snprintf(text, sizeof(text), "SD CARD   %u.%u GB FREE OF %u.%u GB   UPLOAD OVER ETHERNET: tools/n6v.py upload",
			 sd->free_mb / 1024, (sd->free_mb % 1024) * 10 / 1024, sd->total_mb / 1024,
			 (sd->total_mb % 1024) * 10 / 1024);
	}
	if (strcmp(text, shown) != 0) {
		strcpy(shown, text);
		lv_label_set_text(l_card, text);
	}
}

/* Whether visible or not: pick up a change of the card's contents. All this
 * thread does is start the background work and, when the new list has been
 * read and no clip is open, swap it in and rebuild the cards. The reading and
 * the decoding are the background thread's (bg_thread).
 */
static void service(uint32_t now_ms)
{
	static uint32_t t0;
	struct sd_status sd;

	if (thumbs_dirty) {
		thumbs_dirty = false;
		lv_obj_invalidate(row);
	}
	if (now_ms - t0 < 250 || scratch == NULL) {
		return;
	}
	t0 = now_ms;
	sdcard_get_status(&sd);
	update_card_line(&sd);
	if (lib_state == LIB_IDLE && sd.generation != lib_gen && !sd.receiving) {
		scan_gen = sd.generation;
		lib_state = LIB_SCAN;
		k_sem_give(&bg_wake);
	} else if (lib_state == LIB_SCANNED && open_clip < 0 && player_commit() >= 0) {
		/* the files behind generations lib_gen + 1 .. scan_gen, if the card still remembers them all */
		static char stale[SD_CHANGES][SD_NAME_MAX];
		int n_stale = 0;

		if (sd.generation - lib_gen > SD_CHANGES) {
			n_stale = -1;
		} else {
			for (uint32_t g = lib_gen + 1; g != scan_gen + 1; g++) {
				memcpy(stale[n_stale++], sd.changed[g % SD_CHANGES], SD_NAME_MAX);
			}
		}
		lib_gen = scan_gen;
		n_clips = MIN(player_clip_count(), MAX_CARDS);
		assign_thumbs(stale, n_stale);
		build_gallery();
		lib_state = LIB_THUMBS;
		k_sem_give(&bg_wake);
	}
}

/* Remote control: open a clip by number as soon as this screen is up. */
void cinema_play(int clip)
{
	pending_play = clip;
}

static void tick(uint32_t now_ms)
{
	static uint32_t text_t0;
	struct player_status st;

	if (pending_play >= 0) {
		int clip = pending_play;

		pending_play = -1;
		close_player();
		open_player(clip);
	}
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
		char source[72];

		if (!st.on_card) {
			snprintf(source, sizeof(source), "flash, read in place");
		} else if (st.buffering) {
			snprintf(source, sizeof(source), "SD card\nBUFFER   waiting for the card\nSTALLS   %u", st.stalls);
		} else {
			snprintf(source, sizeof(source), "SD card\nBUFFER   %u.%u MB ahead\nSTALLS   %u", st.ahead_kb / 1024,
				 (st.ahead_kb % 1024) * 10 / 1024, st.stalls);
		}
		lv_label_set_text_fmt(l_stats,
				      "FRAME    %u / %u\nDECODE   %u.%u ms\nCONVERT  %u.%u ms\n"
				      "BITRATE  %u.%u Mbit/s\nFRAME    %u KB\nDROPPED  %u\nCLOCK    %s\nSEEK     %u ms\n"
				      "SOURCE   %s",
				      st.frame + 1, st.n_frames, st.decode_us / 1000, (st.decode_us / 100) % 10,
				      st.convert_us / 1000, (st.convert_us / 100) % 10, st.kbps / 1000,
				      (st.kbps / 100) % 10, st.frame_bytes / 1024, st.dropped,
				      st.audio_clock ? "audio DAC" : "system", (st.seek_us + 500) / 1000, source);
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

/* Attract mode: play the first clip once, then let the shell move on. */
static void attract_start(void)
{
	if (n_clips > 0 && open_clip < 0) {
		open_player(0);
		overlay_until = k_uptime_get_32() + 1500;
	}
}

static bool attract_busy(void)
{
	struct player_status st;

	if (open_clip < 0) {
		return false;
	}
	player_get_status(&st);
	return st.playing;
}

const struct screen screen_cinema = {
	.title = "Cinema",
	.blurb = "Video with sound",
	.create = create,
	.tick = tick,
	.service = service,
	.hide = hide,
	.mini = mini,
	.attract_secs = 3,
	.attract_start = attract_start,
	.attract_busy = attract_busy,
};
