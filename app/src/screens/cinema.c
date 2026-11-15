/*
 * Cinema. For now this is the proof of the decode path: one JPEG stored in
 * flash, decoded by the hardware codec and colour-converted by DMA2D, shown
 * with its measured timings. The player grows from here.
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>

#include "media/jpegdec.h"
#include "platform/psram.h"
#include "screens.h"
#include "ui/raster.h"
#include "ui/theme.h"

LOG_MODULE_REGISTER(cinema, LOG_LEVEL_INF);

static const uint8_t poster_jpg[] = {
#include "poster.jpg.inc"
};

static uint32_t *frame;              /* ARGB8888, GFX_W x GFX_H, PSRAM */
static struct jpeg_frame info;
static bool have_frame;
static lv_obj_t *view, *l_stats;

static void decode_poster(void)
{
	int ret = jpegdec_decode(poster_jpg, sizeof(poster_jpg), frame, GFX_W, &info);

	have_frame = ret == 0;
	if (have_frame) {
		LOG_INF("poster %ux%u: decode %u us, convert %u us", info.width, info.height, info.decode_us,
			info.convert_us);
	} else {
		LOG_ERR("poster decode failed (%d)", ret);
	}
}

static void view_draw(lv_event_t *e)
{
	struct raster r;

	if (!raster_begin(&r, e) || !have_frame) {
		return;
	}
	/* straight copy of the visible rows: the picture is larger than the pane */
	for (int y = r.clip.y1; y <= r.clip.y2; y++) {
		const uint32_t *src = frame + (y - r.box.y1) * GFX_W + (r.clip.x1 - r.box.x1);
		uint32_t *dst = r.base + y * r.stride + r.clip.x1;

		memcpy(dst, src, (r.clip.x2 - r.clip.x1 + 1) * 4);
	}
}

static void create(lv_obj_t *root)
{
	frame = psram_alloc(GFX_W * GFX_H * 4, 32);
	if (frame == NULL || jpegdec_init() != 0) {
		lv_obj_align(ui_label(root, &font_textm_20, C_BAD, "JPEG decoder unavailable"), LV_ALIGN_CENTER, 0, 0);
		return;
	}
	decode_poster();

	view = ui_box(root, 0, 0, GFX_W, UI_CONTENT_H);
	lv_obj_add_event_cb(view, view_draw, LV_EVENT_DRAW_MAIN, NULL);

	l_stats = ui_label(root, &font_mono_12, C_TEXT, "");
	lv_obj_set_style_bg_color(l_stats, ui_c(0x000000), 0);
	lv_obj_set_style_bg_opa(l_stats, LV_OPA_70, 0);
	lv_obj_set_style_pad_all(l_stats, 8, 0);
	lv_obj_set_style_radius(l_stats, 6, 0);
	lv_obj_align(l_stats, LV_ALIGN_TOP_RIGHT, -UI_PAD, UI_PAD);
}

static void show(void)
{
	if (frame == NULL || l_stats == NULL) {
		return;
	}
	/* decode again on every visit so the timings are current */
	decode_poster();
	if (have_frame) {
		lv_label_set_text_fmt(l_stats, "JPEG %u x %u  %u KB\nDECODE   %u.%u ms\nCONVERT  %u.%u ms", info.width,
				      info.height, (unsigned int)(sizeof(poster_jpg) / 1024), info.decode_us / 1000,
				      (info.decode_us / 100) % 10, info.convert_us / 1000, (info.convert_us / 100) % 10);
	} else {
		lv_label_set_text(l_stats, "decode failed");
	}
	lv_obj_invalidate(view);
}

static void mini(const struct raster *r)
{
	if (!have_frame) {
		return;
	}
	/* nearest-neighbour thumbnail of the decoded frame */
	int w = lv_area_get_width(&r->box), h = lv_area_get_height(&r->box);

	for (int y = MAX(r->box.y1, r->clip.y1); y <= MIN(r->box.y2, r->clip.y2); y++) {
		const uint32_t *row = frame + ((y - r->box.y1) * GFX_H / h) * GFX_W;
		uint32_t *dst = r->base + y * r->stride;

		for (int x = MAX(r->box.x1, r->clip.x1); x <= MIN(r->box.x2, r->clip.x2); x++) {
			dst[x] = row[(x - r->box.x1) * GFX_W / w] | 0xff000000u;
		}
	}
}

const struct screen screen_cinema = {
	.title = "Cinema",
	.blurb = "Hardware JPEG decode",
	.create = create,
	.show = show,
	.mini = mini,
};
