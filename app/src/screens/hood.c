/*
 * Under the Hood: the firmware itself, measured live.
 *
 * Left, an interactive 3D plot: one ridge per thread, the share of the CPU it
 * took in each 0.4 s over the last twelve, the newest at the right
 * end of each ridge. Drag to orbit, pinch to zoom, tap a ridge to read
 * it, double tap to reset. The height is logarithmic by default, 0.01 % to
 * 100 %, so a thread at a twentieth of a per cent shows as clearly as the UI
 * thread at forty; tap SCALE for linear ones. On the right, every thread with its CPU share and stack
 * high-water mark, and the memory pools.
 *
 * The numbers are Zephyr's own: thread runtime statistics (cycles per
 * thread; interrupt time counts to the thread it interrupted), stack
 * painting (CONFIG_INIT_STACKS), heap runtime statistics, the PSRAM bump
 * allocator and the network statistics. Nothing is estimated.
 *
 * Drawn like Core Map: projected in floating point, filled as anti-aliased
 * convex quads straight into the frame buffer, ridges far to near. Each ridge
 * is a ribbon: a band under its top edge and a line on the floor. The ridges
 * stand in parallel planes, so that order is exact.
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#include <math.h>
#include <stdio.h>
#include <string.h>

#include <zephyr/kernel.h>
#include <zephyr/net/net_if.h>
#include <zephyr/net/net_mgmt.h>
#include <zephyr/net/net_stats.h>
#include <zephyr/sys/mem_stats.h>
#include <zephyr/sys/sys_heap.h>

#include "platform/psram.h"
#include "platform/touch.h"
#include "screens.h"
#include "ui/raster.h"
#include "ui/theme.h"

#define CH_Y          46 /* chart area inside the screen root */
#define CH_W          532
#define CH_H          (UI_CONTENT_H - CH_Y)
#define SIDE_X        (CH_W + 4)
#define SIDE_W        (GFX_W - SIDE_X - UI_PAD)

#define HIST          30      /* samples per ridge */
#define SAMPLE_MS     400     /* 12 s of history */
#define MAXT          40      /* threads tracked */
#define ROWS          8       /* ridges shown: the busiest threads */
#define W_X           12.0f   /* world width of the time axis */
#define ROW_DZ        1.0f    /* world distance between ridges */
#define BOX_H         4.5f    /* world height of 100 % */
#define RIBBON        0.09f   /* depth of the band under each ridge, share of BOX_H: a full
                                * curtain down to the floor overdrew the view several times */

#define DEG           0.017453293f
#define PI_F          3.14159265f
#define YAW0          (-24.0f * DEG)
#define PITCH0        (24.0f * DEG)
#define PITCH_MIN     (6.0f * DEG)
#define PITCH_MAX     (80.0f * DEG)
#define ZOOM_MIN      0.6f
#define ZOOM_MAX      2.5f
#define ORBIT_K       0.0075f
#define IDLE_ORBIT_MS 8000
#define ORBIT_RATE    0.08f   /* rad/s, swinging, not turning round */

extern char _image_ram_start[], _image_ram_end[];
extern struct k_heap _system_heap;
void lvgl_heap_stats(struct sys_memory_stats *stats);

struct thr {
	const struct k_thread *tid;
	const char *name;
	uint64_t last;            /* execution cycles at the last sample */
	uint16_t hist[HIST];      /* hundredths of a per cent of the CPU (10000 = all of it), ring like h_head */
	uint32_t mean;            /* the same unit, over the history */
	size_t stack_size, stack_unused;
	bool alive, seen;
};

struct cam {
	float r[3], u[3], b[3];
	float pos[3];
	float tgt_y, dist, focal, cx, cy;
};

static lv_obj_t *chart, *side, *l_cpu, *l_frame, *l_threads, *l_up, *l_sel, *l_draw, *l_scale;

/* The height axis. The UI thread takes tens of per cent and most others well
 * under one, so a linear axis shows one wall and flat lines: the default is
 * logarithmic, 0.01 % on the floor to 100 % at the top, a decade per grid
 * line. The linear scales (full height = 100, 25, 5 or 1 %, ridges cut at the
 * top) are a tap away. Unit: hundredths of a per cent; 0 means logarithmic.
 */
static const uint16_t scales[] = {0, 10000, 2500, 500, 100};
static int scale_i;

static float level(uint32_t u)
{
	if (scales[scale_i] == 0) {
		return u == 0 ? 0.0f : CLAMP(log10f((float)u) * 0.25f, 0.0f, 1.0f);
	}
	return MIN(u / (float)scales[scale_i], 1.0f);
}

/* data */
static struct thr th[MAXT];
static int n_th;
static int h_head;             /* index of the newest sample */
static int n_samples;
static uint16_t busy_hist[HIST];
static uint32_t sample_ms;
static int rows[ROWS], n_rows;  /* indices into th[], nearest ridge first */
static int stack_rr;
static uint64_t net_rx, net_tx;
static uint32_t net_rx_bps, net_tx_bps, net_ms;
static uint32_t lut_row[ROWS];

/* view */
static float v_yaw = YAW0, v_pitch = PITCH0, v_zoom = 1.0f;
static float v_panx, v_pany, view_cx, view_cy;
static bool resetting;
static int selected = -1; /* index into th[] */
static int g_prev;
static float g_lx, g_ly, g_ld, g_lmx, g_lmy, g_vyaw, g_moved;
static uint32_t g_down_ms, g_tap_ms, g_touch_ms, g_last_ms;
static uint32_t draw_us;

/* what the last frame drew, for hit testing: each ridge's outline on screen */
static float hit_tx[ROWS][HIST + 1], hit_ty[ROWS][HIST + 1], hit_by[ROWS][HIST + 1];
static int hit_n[ROWS];

/* ---- sampling ------------------------------------------------------------ */

static void collect(const struct k_thread *t, void *user)
{
	int i;

	ARG_UNUSED(user);
	for (i = 0; i < n_th; i++) {
		if (th[i].tid == t) {
			break;
		}
	}
	if (i == n_th) {
		if (n_th == MAXT) {
			return;
		}
		memset(&th[i], 0, sizeof(th[i]));
		th[i].tid = t;
		th[i].last = UINT64_MAX; /* no delta from the first sample */
		n_th++;
	}
	th[i].seen = true;
}

static bool is_idle(const struct thr *t)
{
	return t->name && strcmp(t->name, "idle") == 0;
}

static void pick_rows(void)
{
	int idx[MAXT], n = 0;

	for (int i = 0; i < n_th; i++) {
		if (th[i].alive && !is_idle(&th[i])) {
			idx[n++] = i;
		}
	}
	/* busiest first */
	for (int a = 1; a < n; a++) {
		int v = idx[a], b = a;

		for (; b > 0 && th[idx[b - 1]].mean < th[v].mean; b--) {
			idx[b] = idx[b - 1];
		}
		idx[b] = v;
	}
	n_rows = MIN(n, ROWS);
	/* nearest ridge first: the quiet ones in front, so the busy ones behind do not hide them */
	for (int k = 0; k < n_rows; k++) {
		rows[k] = idx[n_rows - 1 - k];
	}
}

static uint32_t t_sample_max, t_stack_max, t_net_max, t_side_max;

void hood_timing(uint32_t out[4], bool clear)
{
	out[0] = t_sample_max, out[1] = t_stack_max, out[2] = t_net_max, out[3] = t_side_max;
	if (clear) {
		t_sample_max = t_stack_max = t_net_max = t_side_max = 0;
	}
}

static void sample(uint32_t now_ms)
{
	uint32_t t0 = gfx_now_us(), t1;
	uint64_t total = 0;
	uint64_t delta[MAXT];

	for (int i = 0; i < n_th; i++) {
		th[i].seen = false;
	}
	k_thread_foreach(collect, NULL);
	for (int i = 0; i < n_th; i++) {
		k_thread_runtime_stats_t st;

		delta[i] = 0;
		th[i].alive = th[i].seen;
		if (!th[i].seen || k_thread_runtime_stats_get((k_tid_t)th[i].tid, &st) != 0) {
			continue;
		}
		if (th[i].name == NULL) {
			th[i].name = k_thread_name_get((k_tid_t)th[i].tid);
		}
		if (th[i].last != UINT64_MAX && st.execution_cycles >= th[i].last) {
			delta[i] = st.execution_cycles - th[i].last;
		}
		th[i].last = st.execution_cycles;
		total += delta[i];
	}
	if (total == 0) {
		return;
	}
	h_head = (h_head + 1) % HIST;
	n_samples = MIN(n_samples + 1, HIST);
	sample_ms = now_ms;
	for (int i = 0; i < n_th; i++) {
		uint32_t pm = (uint32_t)(delta[i] * 10000 / total); /* hundredths of a per cent */
		uint32_t sum = 0;

		th[i].hist[h_head] = (uint16_t)pm;
		if (is_idle(&th[i])) {
			busy_hist[h_head] = (uint16_t)(10000 - MIN(pm, 10000u));
		}
		for (int k = 0; k < HIST; k++) {
			sum += th[i].hist[k];
		}
		th[i].mean = sum / HIST;
	}

	t1 = gfx_now_us();
	t_sample_max = MAX(t_sample_max, t1 - t0);
	/* one stack a sample: painting is read back word by word */
	for (int k = 0; k < n_th; k++) {
		struct thr *t = &th[(stack_rr + k) % n_th];

		if (t->alive) {
			size_t unused;

			if (k_thread_stack_space_get(t->tid, &unused) == 0) {
				t->stack_unused = unused;
				t->stack_size = t->tid->stack_info.size;
			}
			stack_rr = (stack_rr + k + 1) % n_th;
			break;
		}
	}

	t_stack_max = MAX(t_stack_max, gfx_now_us() - t1);
	t1 = gfx_now_us();
	/* network throughput, once a second */
	if (now_ms - net_ms >= 1000) {
		struct net_stats ns;

		if (net_mgmt(NET_REQUEST_STATS_GET_ALL, NULL, &ns, sizeof(ns)) == 0) {
			if (net_ms) {
				uint32_t dt = now_ms - net_ms;

				net_rx_bps = (uint32_t)((ns.bytes.received - net_rx) * 1000 / dt);
				net_tx_bps = (uint32_t)((ns.bytes.sent - net_tx) * 1000 / dt);
			}
			net_rx = ns.bytes.received;
			net_tx = ns.bytes.sent;
		}
		net_ms = now_ms;
	}
	t_net_max = MAX(t_net_max, gfx_now_us() - t1);

	static uint32_t pick_ms;

	if (now_ms - pick_ms >= 4000 || n_rows == 0) {
		pick_ms = now_ms;
		pick_rows();
	}
}

/* sample k (0 = newest) of thread i, per mille */
static inline uint32_t hist_at(int i, int k)
{
	return th[i].hist[(h_head - k + HIST) % HIST];
}

static const char *nice_name(const struct thr *t)
{
	if (t->name == NULL || t->name[0] == '\0') {
		return "(no name)";
	}
	if (strcmp(t->name, "main") == 0) {
		return "main (UI)";
	}
	return t->name;
}

/* ---- camera ------------------------------------------------------------- */

static void cam_aim(struct cam *c, float yaw, float pitch)
{
	float sy = sinf(yaw), cy = cosf(yaw), sp = sinf(pitch), cp = cosf(pitch);

	c->b[0] = sy * cp, c->b[1] = sp, c->b[2] = cy * cp;
	c->r[0] = cy, c->r[1] = 0.0f, c->r[2] = -sy;
	c->u[0] = -sy * sp, c->u[1] = cp, c->u[2] = -cy * sp;
	c->pos[0] = c->dist * c->b[0];
	c->pos[1] = c->tgt_y + c->dist * c->b[1];
	c->pos[2] = c->dist * c->b[2];
}

static inline void proj(const struct cam *c, float x, float y, float z, float *sx, float *sy)
{
	float qy = y - c->tgt_y;
	float k = c->focal / (c->dist - (x * c->b[0] + qy * c->b[1] + z * c->b[2]));

	*sx = c->cx + k * (x * c->r[0] + z * c->r[2]);
	*sy = c->cy - k * (x * c->u[0] + qy * c->u[1] + z * c->u[2]);
}

static float depth(void)
{
	return MAX(n_rows, 1) * ROW_DZ;
}

/* Fixed scale: the box fills the view from the default angle and does not refit as it turns. */
static void cam_make(struct cam *c, const lv_area_t *view, float yaw, float pitch, float zoom, float panx,
		     float pany, bool remember)
{
	float hx = W_X * 0.5f, hz = depth() * 0.5f;
	float x0 = 1e9f, x1 = -1e9f, y0 = 1e9f, y1 = -1e9f;

	c->tgt_y = BOX_H * 0.35f;
	c->dist = 1.6f * sqrtf(W_X * W_X + hz * hz * 4.0f + BOX_H * BOX_H);
	c->focal = 1.0f;
	c->cx = c->cy = 0.0f;
	cam_aim(c, YAW0, PITCH0);
	for (int i = 0; i < 8; i++) {
		float sx, sy;

		proj(c, i & 1 ? hx : -hx, i & 2 ? BOX_H : 0.0f, i & 4 ? hz : -hz, &sx, &sy);
		x0 = MIN(x0, sx), x1 = MAX(x1, sx);
		y0 = MIN(y0, sy), y1 = MAX(y1, sy);
	}
	/* room for the scale labels on the left and the time labels below, at either end of the idle swing */
	float fit = MIN(0.74f * lv_area_get_width(view) / (x1 - x0), 0.72f * lv_area_get_height(view) / (y1 - y0));

	c->cx = 0.5f * (view->x1 + view->x2 + 1) - fit * 0.5f * (x0 + x1);
	c->cx += 0.05f * lv_area_get_width(view); /* the scale labels hang off the left edge of the box */
	c->cy = 0.5f * (view->y1 + view->y2 + 1) - fit * 0.5f * (y0 + y1) - 4.0f;
	if (remember) {
		view_cx = c->cx;
		view_cy = c->cy;
	}
	c->cx += panx;
	c->cy += pany;
	c->focal = fit * zoom;
	cam_aim(c, yaw, pitch);
}

/* ---- drawing ------------------------------------------------------------ */

static inline uint32_t shade(uint32_t rgb, uint32_t k)
{
	return ((((rgb & 0xff00ff) * k) >> 8) & 0xff00ff) | ((((rgb & 0x00ff00) * k) >> 8) & 0x00ff00);
}

static void line3(const struct raster *r, const struct cam *c, float xa, float ya, float za, float xb, float yb,
		  float zb, uint32_t rgb, uint8_t a)
{
	float ax, ay, bx, by;

	proj(c, xa, ya, za, &ax, &ay);
	proj(c, xb, yb, zb, &bx, &by);
	raster_line(r, (int)(ax * RQ_ONE), (int)(ay * RQ_ONE), (int)(bx * RQ_ONE), (int)(by * RQ_ONE), rgb, 8, a);
}

/* A filled quad, pushed out by about half a pixel all round. Neighbouring
 * pieces of one ridge have the same colour and lie in one plane, so the
 * overlap is invisible, and it covers the hairline that two anti-aliased
 * edges meeting exactly would leave.
 */
static void quad_fill(const struct raster *r, float *x, float *y, uint32_t rgb)
{
	float mx = 0.25f * (x[0] + x[1] + x[2] + x[3]), my = 0.25f * (y[0] + y[1] + y[2] + y[3]);

	for (int i = 0; i < 4; i++) {
		float dx = x[i] - mx, dy = y[i] - my, d = sqrtf(dx * dx + dy * dy);

		if (d > 0.01f) {
			x[i] += 0.6f * dx / d;
			y[i] += 0.6f * dy / d;
		}
	}
	raster_poly(r, x, y, 4, rgb, 255);
}

static void scene_draw(const struct raster *r, lv_layer_t *layer, const lv_area_t *view, float yaw, float pitch,
		       float zoom, uint32_t now_ms)
{
	float hx = W_X * 0.5f, hz = depth() * 0.5f;
	float frac = n_samples ? CLAMP((now_ms - sample_ms) / (float)SAMPLE_MS, 0.0f, 1.0f) : 0.0f;
	float dx = W_X / (HIST - 2);
	struct cam c;
	char buf[40];

	cam_make(&c, view, yaw, pitch, zoom, v_panx, v_pany, true);

	/* floor and the back wall with the percentage lines */
	{
		float qx[4], qy[4];

		proj(&c, -hx, 0, -hz, &qx[0], &qy[0]);
		proj(&c, hx, 0, -hz, &qx[1], &qy[1]);
		proj(&c, hx, 0, hz, &qx[2], &qy[2]);
		proj(&c, -hx, 0, hz, &qx[3], &qy[3]);
		raster_poly(r, qx, qy, 4, C_BG1, 255);
		float zb = c.pos[2] > 0 ? -hz : hz;

		proj(&c, -hx, 0, zb, &qx[0], &qy[0]);
		proj(&c, hx, 0, zb, &qx[1], &qy[1]);
		proj(&c, hx, BOX_H, zb, &qx[2], &qy[2]);
		proj(&c, -hx, BOX_H, zb, &qx[3], &qy[3]);
		float face = CLAMP(fabsf(c.b[2]) * 3.0f, 0.0f, 1.0f);

		if (face > 0.0f) {
			raster_poly(r, qx, qy, 4, lv_color_to_u32(ui_mix(C_BG0, C_BG1, (uint8_t)(170.0f * face))) & 0xffffff,
				    255);
			for (int k = 1; k <= 4; k++) {
				line3(r, &c, -hx, BOX_H * k / 4, zb, hx, BOX_H * k / 4, zb, k == 4 ? C_LINE2 : C_LINE,
				      (uint8_t)(255.0f * face));
			}
		}
		/* time grid on the floor: one line a second, moving with the data */
		for (int s = 0; s <= 6; s++) {
			float x = hx - (s * 5 + frac) * dx; /* every 2 s */

			if (x >= -hx) {
				line3(r, &c, x, 0, -hz, x, 0, hz, s % 3 == 0 ? C_LINE2 : C_LINE, 255);
			}
		}
		line3(r, &c, -hx, 0, -hz, hx, 0, -hz, C_LINE2, 255);
		line3(r, &c, -hx, 0, hz, hx, 0, hz, C_LINE2, 255);
	}

	/* ridges, far to near */
	bool from_front = c.pos[2] > 0;

	for (int n = 0; n < n_rows; n++) {
		int row = from_front ? n_rows - 1 - n : n; /* rows[0] is at the front (largest z) */
		int i = rows[row];
		float z = hz - (row + 0.5f) * ROW_DZ;
		uint32_t col = lut_row[row % ROWS];
		uint32_t fill = shade(col, selected == i ? 140 : 88);
		float px[HIST + 1], py[HIST + 1], pb[HIST + 1], xs[HIST + 1], vv[HIST + 1];
		int np = 0;

		/* points from the newest (right) to the oldest (left), scrolling by frac */
		for (int k = -1; k < n_samples && np <= HIST; k++) {
			float x = k < 0 ? hx : hx - (k + frac) * dx;
			float v = level(hist_at(i, MAX(k, 0)));

			if (x < -hx) {
				/* cut the last segment at the left edge */
				float xp = hx - (k - 1 + frac) * dx;
				float vp = level(hist_at(i, MAX(k - 1, 0)));
				float t = (xp - (-hx)) / (xp - x);

				x = -hx;
				v = vp + t * (v - vp);
			}
			float bx;

			proj(&c, x, v * BOX_H, z, &px[np], &py[np]);
			proj(&c, x, 0, z, &bx, &pb[np]);
			xs[np] = x;
			vv[np] = v;
			hit_tx[row][np] = px[np];
			hit_ty[row][np] = py[np];
			hit_by[row][np] = pb[np];
			np++;
			if (x <= -hx) {
				break;
			}
		}
		hit_n[row] = np;

		/* where the ridge stands: a line on the floor */
		line3(r, &c, -hx, 0, z, hx, 0, z, shade(col, 110), 255);

		/* A run of equal samples is one straight piece of wall: one polygon and
		 * one line instead of one for each sample. Each polygon costs tens of
		 * microseconds whatever its size, and most threads idle at a steady
		 * value, or at the top of a stretched scale.
		 */
		for (int k = 0; k + 1 < np;) {
			int e = k + 1;

			while (e + 1 < np && vv[e] == vv[k] && vv[e + 1] == vv[k]) {
				e++;
			}
			if (vv[k] > 0.0f || vv[e] > 0.0f) {
				float qx[4], qy[4];

				proj(&c, xs[k], MAX(vv[k] - RIBBON, 0.0f) * BOX_H, z, &qx[0], &qy[0]);
				proj(&c, xs[e], MAX(vv[e] - RIBBON, 0.0f) * BOX_H, z, &qx[1], &qy[1]);
				qx[2] = px[e], qy[2] = py[e];
				qx[3] = px[k], qy[3] = py[k];
				quad_fill(r, qx, qy, fill);
			}
			k = e;
		}
		for (int k = 0; k + 1 < np;) {
			int e = k + 1;

			while (e + 1 < np && vv[e] == vv[k] && vv[e + 1] == vv[k]) {
				e++;
			}
			raster_line(r, (int)(px[k] * RQ_ONE), (int)(py[k] * RQ_ONE), (int)(px[e] * RQ_ONE), (int)(py[e] * RQ_ONE),
				    col, selected == i ? 14 : 9, 255);
			k = e;
		}
	}	if (layer == NULL) {
		return;
	}

	/* labels: thread names at the newest end, the scale on the left wall edge, time on the front edge */
	float side_x = c.r[0] > 0 ? hx : -hx; /* the end of the ridges on the right of the screen */
	float last_y = -1e9f;

	for (int n = 0; n < n_rows && fabsf(c.r[0]) > 0.5f; n++) {
		int row = c.pos[2] > 0 ? n_rows - 1 - n : n; /* the busiest first: they keep their names */
		float tx, ty;

		proj(&c, side_x, 0, hz - (row + 0.5f) * ROW_DZ, &tx, &ty);
		if (fabsf(ty - last_y) < 13.0f) {
			continue;
		}
		last_y = ty;
		ui_draw_text(layer, &font_mono_12, selected == rows[row] ? C_TEXT : lut_row[row % ROWS],
			     c.r[0] > 0 ? (int)tx + 8 : (int)tx - 8 - ui_text_width(&font_mono_12, nice_name(&th[rows[row]])),
			     (int)ty - 7, nice_name(&th[rows[row]]));
	}
	for (int k = 0; k <= 4; k++) {
		float tx, ty;

		proj(&c, c.r[0] > 0 ? -hx : hx, BOX_H * k / 4, c.pos[2] > 0 ? -hz : hz, &tx, &ty);
		static const char *const decade[] = {"0.01", "0.1", "1", "10", "100 %"};
		uint32_t u = scales[scale_i] * k / 4; /* hundredths of a per cent */

		if (scales[scale_i] == 0) {
			snprintf(buf, sizeof(buf), "%s", decade[k]);
		} else if (u % 100 == 0) {
			snprintf(buf, sizeof(buf), k == 4 ? "%u %%" : "%u", u / 100);
		} else {
			snprintf(buf, sizeof(buf), k == 4 ? "%u.%02u %%" : "%u.%02u", u / 100, u % 100);
		}
		ui_draw_text(layer, &font_mono_12, C_TEXT3, (int)tx - 8 - ui_text_width(&font_mono_12, buf), (int)ty - 7, buf);
	}
	{
		float tx, ty, fz = c.pos[2] > 0 ? hz : -hz;

		proj(&c, hx, 0, fz, &tx, &ty);
		ui_draw_text(layer, &font_mono_12, C_TEXT3, (int)tx - 12, (int)ty + 6, "NOW");
		proj(&c, -hx, 0, fz, &tx, &ty);
		ui_draw_text(layer, &font_mono_12, C_TEXT3, (int)tx - 16, (int)ty + 6, "-12 S");
	}
}

static void chart_draw(lv_event_t *e)
{
	struct raster r;
	uint32_t t0 = gfx_now_us();

	if (!raster_begin(&r, e)) {
		return;
	}
	scene_draw(&r, lv_event_get_layer(e), &r.box, v_yaw, v_pitch, v_zoom, k_uptime_get_32());
	draw_us = gfx_now_us() - t0;
}

/* ---- the side panel ------------------------------------------------------ */

static void fmt_bytes(char *buf, size_t n, uint64_t b)
{
	if (b >= 10u * 1024 * 1024) {
		snprintf(buf, n, "%u MB", (unsigned int)(b >> 20));
	} else if (b >= 10u * 1024) {
		snprintf(buf, n, "%u KB", (unsigned int)(b >> 10));
	} else {
		snprintf(buf, n, "%u B", (unsigned int)b);
	}
}

static void bar(const struct raster *r, lv_layer_t *layer, int y, const char *what, uint64_t used, uint64_t total,
		uint32_t col)
{
	char a[16], b[16], buf[48];
	int x0 = r->box.x1, w = lv_area_get_width(&r->box);
	int fillw = total ? (int)((used * (uint64_t)w) / total) : 0;

	fmt_bytes(a, sizeof(a), used);
	fmt_bytes(b, sizeof(b), total);
	snprintf(buf, sizeof(buf), "%s / %s", a, b);
	ui_draw_text(layer, &font_mono_12, C_TEXT2, x0, y, what);
	ui_draw_text(layer, &font_mono_12, C_TEXT, r->box.x2 - ui_text_width(&font_mono_12, buf), y, buf);
	raster_fill(r, x0, y + 17, r->box.x2, y + 20, C_BG2);
	raster_fill(r, x0, y + 17, x0 + MIN(fillw, w) - 1, y + 20, col);
}

static void side_draw(lv_event_t *e)
{
	lv_layer_t *layer = lv_event_get_layer(e);
	struct raster r;
	int idx[MAXT], n = 0;
	char buf[48];

	if (!raster_begin(&r, e)) {
		return;
	}
	uint32_t ts0 = gfx_now_us();
	int x0 = r.box.x1, y = r.box.y1;

	ui_draw_text(layer, &font_mono_12, C_TEXT3, x0, y, "THREAD");
	ui_draw_text(layer, &font_mono_12, C_TEXT3, x0 + 132, y, "CPU");
	ui_draw_text(layer, &font_mono_12, C_TEXT3, x0 + 178, y, "STACK");
	y += 18;
	raster_fill(&r, x0, y - 3, r.box.x2, y - 3, C_LINE);

	for (int i = 0; i < n_th; i++) {
		if (th[i].alive) {
			idx[n++] = i;
		}
	}
	for (int a = 1; a < n; a++) {
		int v = idx[a], b = a;

		for (; b > 0 && th[idx[b - 1]].mean < th[v].mean; b--) {
			idx[b] = idx[b - 1];
		}
		idx[b] = v;
	}
	for (int k = 0; k < n && y < r.box.y1 + 212; k++) {
		const struct thr *t = &th[idx[k]];
		uint32_t pm = hist_at(idx[k], 0);
		uint32_t col = selected == idx[k] ? C_AZURE : is_idle(t) ? C_TEXT3 : C_TEXT;

		snprintf(buf, sizeof(buf), "%.15s", nice_name(t));
		ui_draw_text(layer, &font_mono_12, col, x0, y, buf);
		snprintf(buf, sizeof(buf), "%u.%02u", pm / 100, pm % 100);
		ui_draw_text(layer, &font_mono_12, col, x0 + 168 - ui_text_width(&font_mono_12, buf), y, buf);
		if (t->stack_size) {
			uint32_t used = (uint32_t)(t->stack_size - t->stack_unused);
			uint32_t pct = used * 100 / t->stack_size;

			snprintf(buf, sizeof(buf), "%u%%", pct);
			ui_draw_text(layer, &font_mono_12, pct >= 85 ? C_AMBER : C_TEXT2,
				     r.box.x2 - ui_text_width(&font_mono_12, buf), y, buf);
			/* a small gauge of the high-water mark */
			raster_fill(&r, x0 + 178, y + 6, x0 + 210, y + 8, C_BG2);
			raster_fill(&r, x0 + 178, y + 6, x0 + 178 + (int)(32 * pct / 100), y + 8,
				    pct >= 85 ? C_AMBER : C_LINE2);
		}
		y += 16;
	}

	y = r.box.y1 + 238;
	raster_fill(&r, x0, y - 6, r.box.x2, y - 6, C_LINE);
	struct sys_memory_stats s;

	bar(&r, layer, y, "STATIC RAM", (uint64_t)(_image_ram_end - _image_ram_start), (uint64_t)CONFIG_SRAM_SIZE * 1024,
	    C_AZURE);
	y += 28;
	lvgl_heap_stats(&s);
	bar(&r, layer, y, "LVGL POOL", s.allocated_bytes, s.allocated_bytes + s.free_bytes, C_VIOLET);
	y += 28;
	sys_heap_runtime_stats_get(&_system_heap.heap, &s);
	bar(&r, layer, y, "KERNEL HEAP", s.allocated_bytes, s.allocated_bytes + s.free_bytes, C_MINT);
	y += 28;
	bar(&r, layer, y, "PSRAM", psram_used(), 32u * 1024 * 1024, C_AMBER);
	y += 28;
	snprintf(buf, sizeof(buf), "NET  IN %u KB/s  OUT %u KB/s", net_rx_bps / 1024, net_tx_bps / 1024);
	ui_draw_text(layer, &font_mono_12, C_TEXT2, x0, y, buf);
	t_side_max = MAX(t_side_max, gfx_now_us() - ts0);
}

/* ---- interaction ---------------------------------------------------------- */

static bool in_quad(const float *qx, const float *qy, float x, float y)
{
	int pos = 0, neg = 0;

	for (int i = 0; i < 4; i++) {
		int j = (i + 1) & 3;
		float cross = (qx[j] - qx[i]) * (y - qy[i]) - (qy[j] - qy[i]) * (x - qx[i]);

		pos += cross > 0;
		neg += cross < 0;
	}
	return pos == 0 || neg == 0;
}

/* The thread whose ridge is under (x, y), nearest first, or -1. */
static int hit_test(float x, float y)
{
	struct cam c;
	lv_area_t a;

	lv_obj_get_coords(chart, &a);
	cam_make(&c, &a, v_yaw, v_pitch, v_zoom, v_panx, v_pany, false);
	for (int n = 0; n < n_rows; n++) {
		int row = c.pos[2] > 0 ? n : n_rows - 1 - n; /* nearest first */

		for (int k = 0; k + 1 < hit_n[row]; k++) {
			float qx[4] = {hit_tx[row][k], hit_tx[row][k + 1], hit_tx[row][k + 1], hit_tx[row][k]};
			float qy[4] = {hit_ty[row][k] - 4, hit_ty[row][k + 1] - 4, hit_by[row][k + 1], hit_by[row][k]};

			if (in_quad(qx, qy, x, y)) {
				return rows[row];
			}
		}
	}
	return -1;
}

static bool gestures(uint32_t now_ms)
{
	struct touch_state ts;
	float x[2] = {0}, y[2] = {0};
	float dt = MIN((now_ms - g_last_ms) * 0.001f, 0.1f);
	float yaw0 = v_yaw, pitch0 = v_pitch, zoom0 = v_zoom, panx0 = v_panx, pany0 = v_pany;
	int n = 0;

	g_last_ms = now_ms;
	touch_get(&ts);
	for (int s = 0; s < TOUCH_MAX_POINTS && n < 2; s++) {
		bool on_button = ts.pt[s].x < 150 && ts.pt[s].y < UI_CONTENT_Y + CH_Y + 34;

		if (ts.pt[s].down && ts.pt[s].y >= UI_CONTENT_Y + CH_Y && ts.pt[s].x < CH_W && !on_button) {
			x[n] = ts.pt[s].x;
			y[n++] = ts.pt[s].y;
		}
	}
	if (shell_gesture_active()) {
		n = 0;
	}
	if (n > 0) {
		g_touch_ms = now_ms;
		resetting = false;
	}
	if (n == 1) {
		if (g_prev != 1) {
			if (g_prev == 0) {
				g_down_ms = now_ms;
				g_moved = 0;
			}
			g_lx = x[0];
			g_ly = y[0];
			g_vyaw = 0;
		}
		float ddx = x[0] - g_lx, ddy = y[0] - g_ly;

		v_yaw -= ddx * ORBIT_K;
		v_pitch += ddy * ORBIT_K;
		g_moved += fabsf(ddx) + fabsf(ddy);
		if (dt > 0.0f) {
			g_vyaw = 0.6f * g_vyaw + 0.4f * (ddx / dt);
		}
		g_lx = x[0];
		g_ly = y[0];
	} else if (n == 2) {
		float d = MAX(hypotf(x[1] - x[0], y[1] - y[0]), 24.0f);
		float mx = 0.5f * (x[0] + x[1]), my = 0.5f * (y[0] + y[1]);

		if (g_prev == 2) {
			float z = CLAMP(v_zoom * d / g_ld, ZOOM_MIN, ZOOM_MAX);
			float k = z / v_zoom;

			v_panx = (mx - view_cx) - k * (g_lmx - view_cx - v_panx);
			v_pany = (my - view_cy) - k * (g_lmy - view_cy - v_pany);
			v_zoom = z;
		}
		g_ld = d;
		g_lmx = mx;
		g_lmy = my;
		g_moved = 100;
		g_vyaw = 0;
	} else {
		if (g_prev == 1 && g_moved < 10 && now_ms - g_down_ms < 260) {
			if (now_ms - g_tap_ms < 320) {
				resetting = true;
				g_tap_ms = 0;
			} else {
				int hit = hit_test(g_lx, g_ly);

				selected = hit == selected ? -1 : hit;
				g_tap_ms = now_ms;
			}
			g_vyaw = 0;
		}
		if (fabsf(g_vyaw) > 20.0f) {
			v_yaw -= g_vyaw * dt * ORBIT_K;
			g_vyaw *= MAX(1.0f - dt * 2.5f, 0.0f);
		} else {
			g_vyaw = 0;
		}
		if (resetting) {
			float k = 1.0f - expf(-dt / 0.12f);

			v_yaw += (YAW0 - v_yaw) * k;
			v_pitch += (PITCH0 - v_pitch) * k;
			v_zoom += (1.0f - v_zoom) * k;
			v_panx -= v_panx * k;
			v_pany -= v_pany * k;
			if (fabsf(YAW0 - v_yaw) < 0.003f && fabsf(PITCH0 - v_pitch) < 0.003f && fabsf(1.0f - v_zoom) < 0.003f &&
			    fabsf(v_panx) < 0.5f && fabsf(v_pany) < 0.5f) {
				v_yaw = YAW0, v_pitch = PITCH0, v_zoom = 1.0f;
				v_panx = v_pany = 0.0f;
				resetting = false;
				g_touch_ms = now_ms;
			}
		} else if (shell_in_attract() || now_ms - g_touch_ms > IDLE_ORBIT_MS) {
			/* swing gently between two views instead of turning round to the back */
			v_yaw = YAW0 + 0.30f * sinf((now_ms - g_touch_ms) * 0.001f * ORBIT_RATE * 2.0f);
		}
	}
	g_prev = n;
	v_pitch = CLAMP(v_pitch, PITCH_MIN, PITCH_MAX);
	v_zoom = CLAMP(v_zoom, ZOOM_MIN, ZOOM_MAX);
	v_panx = CLAMP(v_panx, -0.45f * CH_W, 0.45f * CH_W);
	v_pany = CLAMP(v_pany, -0.45f * CH_H, 0.45f * CH_H);
	if (v_yaw > PI_F) {
		v_yaw -= 2.0f * PI_F;
	} else if (v_yaw < -PI_F) {
		v_yaw += 2.0f * PI_F;
	}
	return v_yaw != yaw0 || v_pitch != pitch0 || v_zoom != zoom0 || v_panx != panx0 || v_pany != pany0;
}

/* ---- build ----------------------------------------------------------------- */

static void scale_click(lv_event_t *e)
{
	uint32_t pm;

	ARG_UNUSED(e);
	scale_i = (scale_i + 1) % ARRAY_SIZE(scales);
	pm = scales[scale_i];
	if (pm == 0) {
		lv_label_set_text(l_scale, "SCALE  LOG");
	} else {
		lv_label_set_text_fmt(l_scale, "SCALE  %u %%", pm / 100);
	}
}

static lv_obj_t *field(lv_obj_t *root, int x, const char *cap, uint32_t color)
{
	lv_obj_t *l;

	lv_obj_set_pos(ui_caption(root, cap), x, 8);
	l = ui_label(root, &font_mono_14, color, "-");
	lv_obj_set_pos(l, x, 22);
	return l;
}

static void set_text(lv_obj_t *label, const char *text)
{
	if (strcmp(lv_label_get_text(label), text) != 0) {
		lv_label_set_text(label, text);
	}
}

static void create(lv_obj_t *root)
{
	static const uint32_t pal[] = {C_AZURE, C_AMBER, C_VIOLET, C_MINT, C_CORAL};

	for (int i = 0; i < ROWS; i++) {
		lut_row[i] = pal[i % ARRAY_SIZE(pal)];
	}
	l_cpu = field(root, 16, "CPU BUSY", C_TEXT);
	l_frame = field(root, 118, "UI FRAME", C_TEXT);
	l_threads = field(root, 220, "THREADS", C_TEXT);
	l_up = field(root, 300, "UPTIME", C_TEXT);
	l_sel = field(root, 400, "SELECTED", C_AZURE);
	l_draw = field(root, 716, "DRAW", C_TEXT);

	chart = ui_box(root, 0, CH_Y, CH_W, CH_H);
	lv_obj_add_event_cb(chart, chart_draw, LV_EVENT_DRAW_MAIN, NULL);
	l_scale = ui_label(root, &font_mono_12, C_AZURE, "SCALE  LOG");
	lv_obj_set_style_bg_color(l_scale, ui_c(C_BG2), 0);
	lv_obj_set_style_bg_opa(l_scale, LV_OPA_COVER, 0);
	lv_obj_set_style_pad_hor(l_scale, 8, 0);
	lv_obj_set_style_pad_ver(l_scale, 5, 0);
	lv_obj_set_style_radius(l_scale, 4, 0);
	lv_obj_set_pos(l_scale, 16, CH_Y + 6);
	lv_obj_add_flag(l_scale, LV_OBJ_FLAG_CLICKABLE);
	lv_obj_set_ext_click_area(l_scale, 10);
	lv_obj_add_event_cb(l_scale, scale_click, LV_EVENT_CLICKED, NULL);
	side = ui_box(root, SIDE_X, CH_Y + 2, SIDE_W, CH_H - 30);
	lv_obj_add_event_cb(side, side_draw, LV_EVENT_DRAW_MAIN, NULL);

	lv_obj_t *hint = ui_caption(root, "MEASURED: ZEPHYR THREAD STATS   DRAG  PINCH  TAP A RIDGE  DOUBLE TAP RESETS");

	lv_obj_align(hint, LV_ALIGN_BOTTOM_RIGHT, -UI_PAD, -8);
}

/* Samples whether visible or not: the history is there when the screen opens. */
static void service(uint32_t now_ms)
{
	if (now_ms - sample_ms >= SAMPLE_MS || n_samples == 0) {
		sample(now_ms);
	}
}

static void tick(uint32_t now_ms)
{
	static uint32_t text_t0, side_t0;
	char buf[48];

	gestures(now_ms);
	lv_obj_invalidate(chart); /* the ridges scroll continuously */
	if (now_ms - side_t0 >= 500) {
		side_t0 = now_ms;
		lv_obj_invalidate(side);
	}
	if (now_ms - text_t0 >= 250) {
		const struct gfx_stats *g = gfx_stats();
		uint32_t up = now_ms / 1000;
		int alive = 0;

		text_t0 = now_ms;
		snprintf(buf, sizeof(buf), "%u.%u %%", busy_hist[h_head] / 100, (busy_hist[h_head] % 100) / 10);
		set_text(l_cpu, buf);
		snprintf(buf, sizeof(buf), "%u.%02u ms", g->avg_render_us / 1000, (g->avg_render_us % 1000) / 10);
		set_text(l_frame, buf);
		for (int i = 0; i < n_th; i++) {
			alive += th[i].alive;
		}
		snprintf(buf, sizeof(buf), "%d", alive);
		set_text(l_threads, buf);
		snprintf(buf, sizeof(buf), "%u:%02u:%02u", up / 3600, (up / 60) % 60, up % 60);
		set_text(l_up, buf);
		if (selected >= 0) {
			uint32_t pm = hist_at(selected, 0);

			snprintf(buf, sizeof(buf), "%.14s  %u.%02u %%", nice_name(&th[selected]), pm / 100, pm % 100);
		} else {
			snprintf(buf, sizeof(buf), "tap a ridge");
		}
		set_text(l_sel, buf);
		snprintf(buf, sizeof(buf), "%u us", draw_us);
		set_text(l_draw, buf);
	}
}

static void show(void)
{
	g_touch_ms = k_uptime_get_32() - IDLE_ORBIT_MS + 3000;
	g_prev = 0;
	lv_obj_invalidate(side);
}

/* Home tile: the busiest ridges as stacked lines, newest at the right. */
static void mini(const struct raster *r)
{
	int w = lv_area_get_width(&r->box), h = lv_area_get_height(&r->box);
	int nr = MIN(n_rows, 6);

	for (int n = nr - 1; n >= 0; n--) {
		int i = rows[n_rows - nr + n];
		int base = r->box.y2 - 4 - (nr - 1 - n) * (h / (nr + 2));
		int prev_x = 0, prev_y = 0;

		for (int k = 0; k < n_samples; k += 2) {
			int x = r->box.x2 - k * (w - 24) / HIST - (nr - 1 - n) * 3;
			int y = base - (int)(log10f((float)MAX(hist_at(i, k), 1u)) * 0.25f * (h / 2));

			if (k) {
				raster_line(r, prev_x * RQ_ONE, prev_y * RQ_ONE, x * RQ_ONE, y * RQ_ONE, lut_row[n % ROWS], 9, 255);
			}
			prev_x = x;
			prev_y = y;
		}
	}
}

const struct screen screen_hood = {
	.title = "Under the Hood",
	.blurb = "This firmware, live",
	.create = create,
	.tick = tick,
	.show = show,
	.service = service,
	.mini = mini,
	.attract_secs = 14,
};
