/*
 * Core Map: the host PC's logical processors as posts on a floor plan of the
 * chip. Height and colour are each processor's load, pushed ten times a
 * second by host/companion.py (the same datagrams as Host Link). Drag to
 * orbit, pinch to zoom and pan, tap a post to read it, double tap to reset.
 * Without the companion the screen says so; it never invents data.
 *
 * No GPU is involved. At most 32 posts are projected in floating point,
 * sorted far to near and filled as anti-aliased convex quads straight into
 * the frame buffer (ui/raster.c). The posts stand on a grid and do not
 * intersect, so that order is exact and no depth buffer is needed.
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#include <math.h>
#include <stdio.h>
#include <string.h>

#include <zephyr/kernel.h>

#include "platform/hostlink.h"
#include "platform/touch.h"
#include "screens.h"
#include "ui/raster.h"
#include "ui/theme.h"

#define CH_Y          46 /* chart area inside the screen root */
#define CH_H          (UI_CONTENT_H - CH_Y)
#define MAXP          64 /* posts: a larger host is shown per core, or per group (see build_groups) */
#define MAX_ROWS      8
#define DEG           0.017453293f
#define PI_F          3.14159265f
#define YAW0          (28.0f * DEG)
#define PITCH0        (30.0f * DEG)
#define PITCH_MIN     (8.0f * DEG)
#define PITCH_MAX     (82.0f * DEG)
#define ZOOM_MIN      0.6f
#define ZOOM_MAX      2.5f    /* beyond this, tall posts overdraw the whole view several times */
#define POST_HALF     0.3f    /* posts are 0.6 of a floor cell */
#define ORBIT_K       0.0075f /* radians per pixel of drag */
#define IDLE_ORBIT_MS 8000    /* untouched this long, the chart turns by itself */
#define ORBIT_RATE    0.10f   /* rad/s */

struct post {
	float x, z;  /* centre on the floor, world units (one cell = 1) */
	uint8_t lp;  /* logical processor number */
	uint8_t type, thread, ord;
};

struct cam {
	float r[3], u[3], b[3]; /* right, up, back (towards the camera) */
	float pos[3];
	float tgt_y, dist, focal, cx, cy;
};

struct quad {
	float x[4], y[4];
};

static lv_obj_t *chart, *l_host, *l_cpus, *l_total, *l_sel, *l_draw, *l_wait;

/* Posts and logical processors. Up to MAXP processors, one post each. Beyond
 * that the drawing no longer fits a frame (128 posts: 17.8 ms, overlapping
 * posts overdraw each other), so a post stands for a physical core (its SMT
 * threads averaged) when the host told us the topology and that fits, or for
 * a run of consecutive processors otherwise. The screen says which.
 */
static int grp_n;
static uint8_t grp_first[MAXP], grp_cnt[MAXP];
static bool grp_by_core, grouped;

/* floor plan */
static struct post posts[MAXP];
static int n_posts, plan_cols, plan_rows;
static float box_h; /* world height of 100 % */
static uint32_t plan_sig;
static bool plan_hybrid, plan_smt;
static uint8_t row_type[MAX_ROWS], row_thread[MAX_ROWS];
static char summary[40];

/* data */
static float shown[MAXP]; /* per logical processor, percent, eased towards the latest sample */
static bool moving;
static uint32_t lut[101];

/* view */
static float v_yaw = YAW0, v_pitch = PITCH0, v_zoom = 1.0f;
static float v_panx, v_pany;   /* pixels */
static float view_cx, view_cy; /* where the middle of the box sits on screen without pan */
static bool resetting;
static int selected = -1; /* logical processor, or -1 */

/* gesture state */
static int g_prev;
static float g_lx, g_ly, g_ld, g_lmx, g_lmy, g_vyaw, g_moved;
static uint32_t g_down_ms, g_tap_ms, g_touch_ms, g_last_ms;

/* what the last frame drew, for hit testing: faces per post, nearest post last */
static struct quad hit_q[MAXP][3];
static uint8_t hit_n[MAXP], draw_order[MAXP];
static uint32_t draw_us;

/* ---- colour ------------------------------------------------------------- */

static void build_lut(void)
{
	/* idle posts are a neutral grey that sits back; load pulls them to red */
	static const struct {
		uint8_t v;
		uint32_t c;
	} steps[] = {{0, 0x5f656b}, {10, 0x6c7075}, {35, 0x9a5f59}, {60, 0xd3412f}, {85, 0xf3231a}, {100, 0xff1010}};

	for (int i = 0, k = 0; i <= 100; i++) {
		while (k < (int)ARRAY_SIZE(steps) - 2 && i > steps[k + 1].v) {
			k++;
		}
		int span = steps[k + 1].v - steps[k].v;

		lut[i] = lv_color_to_u32(ui_mix(steps[k].c, steps[k + 1].c, (i - steps[k].v) * 255 / span)) & 0xffffff;
	}
}

static inline uint32_t shade(uint32_t rgb, uint32_t k)
{
	return ((((rgb & 0xff00ff) * k) >> 8) & 0xff00ff) | ((((rgb & 0x00ff00) * k) >> 8) & 0x00ff00);
}

/* ---- floor plan --------------------------------------------------------- */

static uint32_t plan_signature(const struct hl_state *s)
{
	uint32_t h = 2166136261u ^ s->n_cores ^ (s->topo ? 0x100 : 0);

	for (int i = 0; i < MIN(s->n_cores, MAXP) && s->topo; i++) {
		h = (h ^ s->core_kind[i]) * 16777619u;
		h = (h ^ s->core_id[i]) * 16777619u;
	}
	return h | 1;
}

/* Follow the chip, as far as the host told us about it: on a hybrid CPU the
 * low-power E-cores stand in front, then the E-cores, then one row of P-cores
 * per SMT thread at the back; short rows are centred. Otherwise a plain grid
 * about twice as wide as deep.
 */
static void build_groups(const struct hl_state *s)
{
	int n = MIN(s->n_cores, HL_MAX_CORES);

	grouped = n > MAXP;
	grp_by_core = false;
	grp_n = 0;
	if (!grouped) {
		for (int i = 0; i < n; i++) {
			grp_first[grp_n] = i;
			grp_cnt[grp_n++] = 1;
		}
		return;
	}
	if (s->topo) {
		int g = 0;

		for (int i = 0; i < n; i++) {
			g += i == 0 || s->core_id[i] != s->core_id[i - 1] ||
			     HL_KIND_TYPE(s->core_kind[i]) != HL_KIND_TYPE(s->core_kind[i - 1]);
		}
		if (g <= MAXP) {
			for (int i = 0; i < n; i++) {
				if (i == 0 || s->core_id[i] != s->core_id[i - 1] ||
				    HL_KIND_TYPE(s->core_kind[i]) != HL_KIND_TYPE(s->core_kind[i - 1])) {
					grp_first[grp_n] = i;
					grp_cnt[grp_n++] = 0;
				}
				grp_cnt[grp_n - 1]++;
			}
			grp_by_core = true;
			return;
		}
	}
	int k = (n + MAXP - 1) / MAXP;

	for (int i = 0; i < n; i += k) {
		grp_first[grp_n] = i;
		grp_cnt[grp_n++] = MIN(k, n - i);
	}
}

/* "CPU 5" or "CPU 4-5": the logical processors behind post g */
static void group_label(int g, char *buf, size_t len)
{
	if (grp_cnt[g] <= 1) {
		snprintf(buf, len, "CPU %u", grp_first[g]);
	} else {
		snprintf(buf, len, "CPU %u-%u", grp_first[g], grp_first[g] + grp_cnt[g] - 1);
	}
}

static void build_plan(const struct hl_state *s)
{
	static const struct {
		uint8_t type, thread;
	} keys[] = {{HL_CORE_PLAIN, 0}, {HL_CORE_PLAIN, 1}, {HL_CORE_LPE, 0}, {HL_CORE_LPE, 1}, {HL_CORE_E, 0},
		    {HL_CORE_E, 1},     {HL_CORE_P, 0},     {HL_CORE_P, 1},   {HL_CORE_P, 2},   {HL_CORE_P, 3}};
	static uint8_t row_lp[MAX_ROWS][MAXP];
	uint8_t type[MAXP], thread[MAXP], core[MAXP], ord[MAXP], row_n[MAX_ROWS] = {0};
	int n, rows = 0, cores_of[4] = {0}, lps_of[4] = {0};

	build_groups(s);
	n = grp_n;
	plan_hybrid = false;
	plan_smt = false;
	for (int i = 0; i < n; i++) {
		int lp = grp_first[i];

		type[i] = s->topo ? HL_KIND_TYPE(s->core_kind[lp]) : HL_CORE_PLAIN;
		thread[i] = s->topo && !grouped ? HL_KIND_THREAD(s->core_kind[lp]) : 0;
		core[i] = s->topo ? s->core_id[lp] : i;
		plan_hybrid |= type[i] != HL_CORE_PLAIN;
		plan_smt |= thread[i] != 0;
	}
	/* number the physical cores of each type in order of appearance */
	for (int i = 0; i < n; i++) {
		int j;

		for (j = 0; j < i; j++) {
			if (type[j] == type[i] && core[j] == core[i]) {
				break;
			}
		}
		ord[i] = j < i ? ord[j] : cores_of[type[i]]++;
		lps_of[type[i]]++;
	}

	if (plan_hybrid) {
		for (size_t k = 0; k < ARRAY_SIZE(keys) && rows < MAX_ROWS; k++) {
			for (int i = 0; i < n; i++) {
				if (type[i] == keys[k].type && MIN(thread[i], 3) == keys[k].thread) {
					row_lp[rows][row_n[rows]++] = i;
				}
			}
			if (row_n[rows]) {
				row_type[rows] = keys[k].type;
				row_thread[rows] = keys[k].thread;
				rows++;
			}
		}
	} else {
		int w = MAX((int)ceilf(sqrtf(2.0f * n)), 1);

		for (int i = 0; i < n && i / w < MAX_ROWS; i++) {
			row_lp[i / w][row_n[i / w]++] = i;
			rows = i / w + 1;
			row_type[i / w] = HL_CORE_PLAIN;
		}
	}

	plan_cols = 1;
	for (int r = 0; r < rows; r++) {
		plan_cols = MAX(plan_cols, row_n[r]);
	}
	plan_rows = MAX(rows, 1);
	box_h = plan_cols * 0.55f;
	n_posts = 0;
	for (int r = 0; r < rows; r++) {
		for (int i = 0; i < row_n[r]; i++) {
			struct post *p = &posts[n_posts++];
			int lp = row_lp[r][i];

			p->x = (plan_cols - row_n[r]) * 0.5f + i + 0.5f - plan_cols * 0.5f;
			p->z = plan_rows * 0.5f - (r + 0.5f); /* row 0 is nearest the default camera */
			p->lp = lp;
			p->type = type[lp];
			p->thread = thread[lp];
			p->ord = ord[lp];
		}
	}

	if (plan_hybrid) {
		int len = 0;

		summary[0] = '\0';
		if (cores_of[HL_CORE_P]) {
			len += snprintf(summary + len, sizeof(summary) - len, "%d P", cores_of[HL_CORE_P]);
			if (lps_of[HL_CORE_P] > cores_of[HL_CORE_P]) {
				len += snprintf(summary + len, sizeof(summary) - len, " (%d T)", lps_of[HL_CORE_P]);
			}
		}
		if (cores_of[HL_CORE_E] && len < (int)sizeof(summary)) {
			len += snprintf(summary + len, sizeof(summary) - len, "%s%d E", len ? " + " : "", cores_of[HL_CORE_E]);
		}
		if (cores_of[HL_CORE_LPE] && len < (int)sizeof(summary)) {
			snprintf(summary + len, sizeof(summary) - len, "%s%d LP-E", len ? " + " : "", cores_of[HL_CORE_LPE]);
		}
	} else {
		snprintf(summary, sizeof(summary), "%d", n);
	}
	if (grouped) {
		if (s->n_host_cores > s->n_cores) {
			snprintf(summary, sizeof(summary), "%d: FIRST %d, %d PER POST", s->n_host_cores, s->n_cores, grp_cnt[0]);
		} else {
			snprintf(summary, sizeof(summary), "%d, A POST PER %s", s->n_cores,
				 grp_by_core ? "CORE" : grp_cnt[0] == 2 ? "PAIR" : "GROUP");
		}
	}
	if (selected >= n) {
		selected = -1;
	}
	plan_sig = plan_signature(s);
}

static void post_name(const struct post *p, char *buf, size_t len)
{
	static const char *const tag[] = {"C", "P", "E", "LP"};

	if (plan_smt && (p->type == HL_CORE_P || p->type == HL_CORE_PLAIN)) {
		snprintf(buf, len, "%s%u.%u", tag[p->type], p->ord, p->thread); /* core.thread */
	} else {
		snprintf(buf, len, "%s%u", tag[p->type & 3], p->ord);
	}
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

/* A camera that orbits the middle of the box. The scale is fixed so that the
 * box fills `view` from the default angle; it does not refit as the chart
 * turns, which would make it breathe.
 */
static void cam_make(struct cam *c, const lv_area_t *view, float yaw, float pitch, float zoom, float panx,
		     float pany, bool remember)
{
	float hx = plan_cols * 0.5f, hz = plan_rows * 0.5f;
	float x0 = 1e9f, x1 = -1e9f, y0 = 1e9f, y1 = -1e9f;

	c->tgt_y = box_h * 0.45f;
	c->dist = 1.5f * sqrtf((float)(plan_cols * plan_cols + plan_rows * plan_rows) + box_h * box_h);
	c->focal = 1.0f;
	c->cx = c->cy = 0.0f;
	cam_aim(c, YAW0, PITCH0);
	for (int i = 0; i < 8; i++) {
		float sx, sy;

		proj(c, i & 1 ? hx : -hx, i & 2 ? box_h : 0.0f, i & 4 ? hz : -hz, &sx, &sy);
		x0 = MIN(x0, sx), x1 = MAX(x1, sx);
		y0 = MIN(y0, sy), y1 = MAX(y1, sy);
	}
	float fit = MIN(0.90f * lv_area_get_width(view) / (x1 - x0), 0.84f * lv_area_get_height(view) / (y1 - y0));

	c->cx = 0.5f * (view->x1 + view->x2 + 1) - fit * 0.5f * (x0 + x1);
	c->cy = 0.5f * (view->y1 + view->y2 + 1) - fit * 0.5f * (y0 + y1);
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

static void quad_project(const struct cam *c, const float v[4][3], struct quad *q)
{
	for (int i = 0; i < 4; i++) {
		proj(c, v[i][0], v[i][1], v[i][2], &q->x[i], &q->y[i]);
	}
}

static void line3(const struct raster *r, const struct cam *c, float xa, float ya, float za, float xb, float yb,
		  float zb, uint32_t rgb, uint8_t a)
{
	float ax, ay, bx, by;

	if (a == 0) {
		return;
	}
	proj(c, xa, ya, za, &ax, &ay);
	proj(c, xb, yb, zb, &bx, &by);
	raster_line(r, (int)(ax * RQ_ONE), (int)(ay * RQ_ONE), (int)(bx * RQ_ONE), (int)(by * RQ_ONE), rgb, 8, a);
}

static void wall(const struct raster *r, const struct cam *c, const float v[4][3], float strength)
{
	struct quad q;
	uint8_t a = (uint8_t)(255.0f * strength);

	if (a == 0) {
		return;
	}
	quad_project(c, v, &q);
	/* nothing is behind a back wall but the screen colour, so mix it in here
	 * and fill opaque: no per-pixel blend
	 */
	raster_poly(r, q.x, q.y, 4, lv_color_to_u32(ui_mix(C_BG0, C_BG1, (uint8_t)(170.0f * strength))) & 0xffffff, 255);
	for (int k = 1; k <= 4; k++) { /* 25, 50, 75, 100 % */
		float y = box_h * k / 4;

		line3(r, c, v[0][0], y, v[0][2], v[1][0], y, v[1][2], k == 4 ? C_LINE2 : C_LINE, a);
	}
}

/* Convex hull of n points (n <= 8), counter-clockwise on screen; returns the
 * number of corners. A box's outline from any viewpoint has at most six.
 */
static int hull(const float *px, const float *py, int n, float *hx, float *hy)
{
	int idx[8], h[16], k = 0;

	for (int i = 0; i < n; i++) {
		int j = i;

		for (; j > 0 && (px[idx[j - 1]] > px[i] || (px[idx[j - 1]] == px[i] && py[idx[j - 1]] > py[i])); j--) {
			idx[j] = idx[j - 1];
		}
		idx[j] = i;
	}
	for (int pass = 0; pass < 2; pass++) {
		int start = k;

		for (int m = 0; m < n; m++) {
			int i = pass == 0 ? idx[m] : idx[n - 1 - m];

			while (k >= start + 2) {
				int a = h[k - 2], b = h[k - 1];
				float cross = (px[b] - px[a]) * (py[i] - py[a]) - (py[b] - py[a]) * (px[i] - px[a]);

				if (cross > 0.0f) {
					break;
				}
				k--;
			}
			h[k++] = i;
		}
		k--; /* the last point is the first of the other half */
	}
	for (int i = 0; i < k && i < 8; i++) {
		hx[i] = px[h[i]];
		hy[i] = py[h[i]];
	}
	return MIN(k, 8);
}

static void scene_draw(const struct raster *r, lv_layer_t *layer, const lv_area_t *view, float yaw, float pitch,
		       float zoom, bool full)
{
	float hx = plan_cols * 0.5f, hz = plan_rows * 0.5f;
	float dist2[MAXP];
	uint8_t thumb_order[MAXP];
	uint8_t *order = full ? draw_order : thumb_order; /* the thumbnail must not disturb hit testing */
	struct cam c;
	struct quad q;
	char buf[48];

	if (n_posts == 0) {
		return;
	}
	cam_make(&c, view, yaw, pitch, zoom, full ? v_panx : 0.0f, full ? v_pany : 0.0f, full);

	/* floor */
	const float floor_v[4][3] = {{-hx, 0, -hz}, {hx, 0, -hz}, {hx, 0, hz}, {-hx, 0, hz}};

	quad_project(&c, floor_v, &q);
	raster_poly(r, q.x, q.y, 4, C_BG1, 255);

	/* the two walls behind the posts, fading out as they turn edge-on */
	float xb = c.pos[0] > 0 ? -hx : hx, zb = c.pos[2] > 0 ? -hz : hz;
	float ax = CLAMP(fabsf(c.b[0]) * 3.0f, 0.0f, 1.0f), az = CLAMP(fabsf(c.b[2]) * 3.0f, 0.0f, 1.0f);

	if (full) {
		const float wx[4][3] = {{xb, 0, -zb}, {xb, 0, zb}, {xb, box_h, zb}, {xb, box_h, -zb}};
		const float wz[4][3] = {{-xb, 0, zb}, {xb, 0, zb}, {xb, box_h, zb}, {-xb, box_h, zb}};

		wall(r, &c, wx, ax);
		wall(r, &c, wz, az);
		line3(r, &c, xb, 0, zb, xb, box_h, zb, C_LINE2, (uint8_t)(255.0f * MAX(ax, az)));
		line3(r, &c, xb, 0, -zb, xb, box_h, -zb, C_LINE2, (uint8_t)(255.0f * ax));
		line3(r, &c, -xb, 0, zb, -xb, box_h, zb, C_LINE2, (uint8_t)(255.0f * az));
		for (int i = 0; i <= plan_cols; i++) {
			line3(r, &c, -hx + i, 0, -hz, -hx + i, 0, hz, i % plan_cols ? C_LINE : C_LINE2, 255);
		}
		for (int i = 0; i <= plan_rows; i++) {
			line3(r, &c, -hx, 0, -hz + i, hx, 0, -hz + i, i % plan_rows ? C_LINE : C_LINE2, 255);
		}
	}

	/* posts, far to near */
	for (int i = 0; i < n_posts; i++) {
		float dx = posts[i].x - c.pos[0], dz = posts[i].z - c.pos[2];
		int k = i;

		dist2[i] = dx * dx + dz * dz;
		for (; k > 0 && dist2[order[k - 1]] < dist2[i]; k--) {
			order[k] = order[k - 1];
		}
		order[k] = i;
	}
	for (int n = 0; n < n_posts; n++) {
		int i = order[n];
		const struct post *p = &posts[i];
		float v = shown[p->lp];
		float h = MAX(v, 0.8f) * 0.01f * box_h;
		float x0 = p->x - POST_HALF, x1 = p->x + POST_HALF, z0 = p->z - POST_HALF, z1 = p->z + POST_HALF;
		uint32_t rgb = lut[CLAMP((int)(v + 0.5f), 0, 100)];
		bool see_x = c.pos[0] > x1 || c.pos[0] < x0, see_z = c.pos[2] > z1 || c.pos[2] < z0;
		float xf = c.pos[0] > x1 ? x1 : x0, zf = c.pos[2] > z1 ? z1 : z0;
		const float side_x[4][3] = {{xf, 0, z0}, {xf, 0, z1}, {xf, h, z1}, {xf, h, z0}};
		const float side_z[4][3] = {{x0, 0, zf}, {x1, 0, zf}, {x1, h, zf}, {x0, h, zf}};
		const float top[4][3] = {{x0, h, z0}, {x1, h, z0}, {x1, h, z1}, {x0, h, z1}};
		struct quad qx, qz;
		int faces = 0;

		/* Many posts (a large host): the polygon filler's cost goes with the
		 * rows it covers, and two tall side faces are most of a post's rows.
		 * Fill the whole outline in the side shade instead, mark the near
		 * vertical edge with a line, and lay the top over it: about half the
		 * rows. The faces are still projected for hit testing.
		 */
		if (n_posts > 32) {
			float cx8[8], cy8[8], ox[8], oy[8];

			for (int k = 0; k < 8; k++) {
				proj(&c, k & 1 ? x1 : x0, k & 2 ? h : 0.0f, k & 4 ? z1 : z0, &cx8[k], &cy8[k]);
			}
			int no = hull(cx8, cy8, 8, ox, oy);

			raster_poly(r, ox, oy, no, shade(rgb, 165), 255);
			if (see_x && see_z) {
				float ex, ey, tx, ty;

				proj(&c, xf, 0, zf, &ex, &ey);
				proj(&c, xf, h, zf, &tx, &ty);
				raster_line(r, (int)(ex * RQ_ONE), (int)(ey * RQ_ONE), (int)(tx * RQ_ONE), (int)(ty * RQ_ONE),
					    shade(rgb, 115), 8, 255);
			}
			if (full) {
				if (see_x) {
					quad_project(&c, side_x, &qx);
					hit_q[i][faces++] = qx;
				}
				if (see_z) {
					quad_project(&c, side_z, &qz);
					hit_q[i][faces++] = qz;
				}
			}
			quad_project(&c, top, &q);
			raster_poly(r, q.x, q.y, 4, rgb, 255);
			if (full) {
				hit_q[i][faces++] = q;
				hit_n[i] = faces;
			}
			continue;
		}

		/* Faces that share an edge would each be softened against what is
		 * behind the post, leaving a hairline of background between them. So a
		 * face that is drawn first reaches one pixel under the face drawn over it.
		 */
		if (see_z) {
			quad_project(&c, side_z, &qz);
		}
		if (see_x) {
			quad_project(&c, side_x, &qx);
			if (full) {
				hit_q[i][faces++] = qx;
			}
			qx.y[2] -= 1.0f, qx.y[3] -= 1.0f; /* under the top face */
			if (see_z) {                     /* and under the other side */
				int near = zf == z0 ? 0 : 1, near_top = zf == z0 ? 3 : 2;
				float dir = 0.5f * (qz.x[0] + qz.x[1]) > qx.x[near] ? 1.0f : -1.0f;

				qx.x[near] += dir, qx.x[near_top] += dir;
			}
			raster_poly(r, qx.x, qx.y, 4, shade(rgb, 190), 255);
		}
		if (see_z) {
			if (full) {
				hit_q[i][faces++] = qz;
			}
			qz.y[2] -= 1.0f, qz.y[3] -= 1.0f;
			raster_poly(r, qz.x, qz.y, 4, shade(rgb, 144), 255);
		}
		quad_project(&c, top, &q);
		raster_poly(r, q.x, q.y, 4, rgb, 255);
		if (full) {
			hit_q[i][faces++] = q;
			hit_n[i] = faces;
		}
	}
	if (!full || layer == NULL) {
		return;
	}

	/* the selected post: outline its top */
	int sel_x = 0, sel_y = 0;
	const struct post *sel = NULL;

	for (int i = 0; i < n_posts && selected >= 0; i++) {
		if (posts[i].lp == selected) {
			const struct quad *t = &hit_q[i][hit_n[i] - 1];
			float top_y = 1e9f, mid_x = 0;

			sel = &posts[i];
			for (int k = 0; k < 4; k++) {
				int j = (k + 1) & 3;

				raster_line(r, (int)(t->x[k] * RQ_ONE), (int)(t->y[k] * RQ_ONE), (int)(t->x[j] * RQ_ONE),
					    (int)(t->y[j] * RQ_ONE), C_TEXT, 10, 255);
				top_y = MIN(top_y, t->y[k]);
				mid_x += 0.25f * t->x[k];
			}
			sel_x = (int)mid_x;
			sel_y = (int)top_y;
		}
	}

	/* colour key */
	int kx = r->box.x1 + 30, ky = r->box.y2 - 15;

	for (int i = 0; i < 120; i++) {
		raster_fill(r, kx + i, ky, kx + i, ky + 5, lut[i * 100 / 119]);
	}

	/* text last: it is drawn by LVGL after this handler's own pixels */
	ui_draw_text(layer, &font_mono_12, C_TEXT3, r->box.x1 + 16, ky - 4, "0");
	ui_draw_text(layer, &font_mono_12, C_TEXT3, kx + 126, ky - 4, "100 % BUSY");

	/* load scale on whichever wall edge is further left */
	float ex[2] = {xb, -xb}, ez[2] = {-zb, zb}, ea[2] = {ax, az}, sx[2], sy;

	proj(&c, ex[0], 0, ez[0], &sx[0], &sy);
	proj(&c, ex[1], 0, ez[1], &sx[1], &sy);
	int edge = (sx[0] < sx[1] && ea[0] > 0.3f) || ea[1] <= 0.3f ? 0 : 1;

	for (int k = 0; k <= 4; k++) {
		float tx, ty;

		proj(&c, ex[edge], box_h * k / 4, ez[edge], &tx, &ty);
		snprintf(buf, sizeof(buf), k == 4 ? "100 %%" : "%d", k * 25);
		ui_draw_text(layer, &font_mono_12, C_TEXT3, (int)tx - 8 - ui_text_width(&font_mono_12, buf), (int)ty - 7, buf);
	}

	/* what each row is, at the end that is on the right of the screen */
	if (plan_hybrid && fabsf(c.r[0]) > 0.6f) { /* not when the rows are seen end-on: the names would pile up */
		static const uint32_t type_color[] = {C_TEXT2, C_VIOLET, C_AZURE, C_MINT};
		static const char *const type_name[] = {"CORES", "P-CORES", "E-CORES", "LP E-CORES"};
		float side = c.r[0] > 0 ? 1.0f : -1.0f;
		float last_y = -1e9f;

		for (int row = 0; row < plan_rows; row++) {
			float tx, ty;

			proj(&c, side * (hx + 0.2f), 0, hz - (row + 0.5f), &tx, &ty);
			if (fabsf(ty - last_y) < 14.0f) {
				continue; /* too close to the previous name to read */
			}
			last_y = ty;
			if (row_type[row] == HL_CORE_P && plan_smt) {
				snprintf(buf, sizeof(buf), "P-CORES  THREAD %u", row_thread[row]);
			} else {
				snprintf(buf, sizeof(buf), "%s", type_name[row_type[row] & 3]);
			}
			ui_draw_text(layer, &font_mono_12, type_color[row_type[row] & 3], (int)tx + 8, (int)ty - 7, buf);
		}
	}

	if (sel) {
		char name[12];

		post_name(sel, name, sizeof(name));
		char cpu[16];

		group_label(sel->lp, cpu, sizeof(cpu));
		snprintf(buf, sizeof(buf), "%s  %s  %d %%", cpu, name, (int)(shown[sel->lp] + 0.5f));
		int w = ui_text_width(&font_mono_12, buf);
		int bx = CLAMP(sel_x - w / 2, r->box.x1 + 8, r->box.x2 - w - 8);
		int by = MAX(sel_y - 28, r->box.y1 + 4);

		raster_fill_a(r, bx - 6, by - 3, bx + w + 5, by + 16, C_BG0, 225);
		raster_fill(r, bx - 6, by + 17, bx + w + 5, by + 17, C_TEXT);
		ui_draw_text(layer, &font_mono_12, C_TEXT, bx, by, buf);
	}
}

static void chart_draw(lv_event_t *e)
{
	struct raster r;
	uint32_t t0 = gfx_now_us();

	if (!raster_begin(&r, e)) {
		return;
	}
	scene_draw(&r, lv_event_get_layer(e), &r.box, v_yaw, v_pitch, v_zoom, true);
	draw_us = gfx_now_us() - t0;
}

/* ---- interaction -------------------------------------------------------- */

static bool in_quad(const struct quad *q, float x, float y)
{
	int pos = 0, neg = 0;

	for (int i = 0; i < 4; i++) {
		int j = (i + 1) & 3;
		float cross = (q->x[j] - q->x[i]) * (y - q->y[i]) - (q->y[j] - q->y[i]) * (x - q->x[i]);

		pos += cross > 0;
		neg += cross < 0;
	}
	return pos == 0 || neg == 0;
}

/* The logical processor whose post is under (x, y), nearest first, or -1. */
static int hit_test(float x, float y)
{
	for (int n = n_posts - 1; n >= 0; n--) {
		int i = draw_order[n];

		for (int f = 0; f < hit_n[i]; f++) {
			if (in_quad(&hit_q[i][f], x, y)) {
				return posts[i].lp;
			}
		}
	}
	return -1;
}

/* Returns true if the view changed. Touch coordinates are panel coordinates. */
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
		if (ts.pt[s].down && ts.pt[s].y >= UI_CONTENT_Y + CH_Y) {
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
		float dx = x[0] - g_lx, dy = y[0] - g_ly;

		/* the chart follows the finger: right turns it right, down tips it towards you */
		v_yaw -= dx * ORBIT_K;
		v_pitch += dy * ORBIT_K;
		g_moved += fabsf(dx) + fabsf(dy);
		if (dt > 0.0f) {
			g_vyaw = 0.6f * g_vyaw + 0.4f * (dx / dt);
		}
		g_lx = x[0];
		g_ly = y[0];
	} else if (n == 2) {
		float d = MAX(hypotf(x[1] - x[0], y[1] - y[0]), 24.0f);
		float mx = 0.5f * (x[0] + x[1]), my = 0.5f * (y[0] + y[1]);

		if (g_prev == 2) {
			/* scale about the point between the fingers and follow it as it moves */
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
				resetting = true; /* double tap */
				g_tap_ms = 0;
			} else {
				selected = hit_test(g_lx, g_ly);
				g_tap_ms = now_ms;
			}
			g_vyaw = 0;
		}
		if (fabsf(g_vyaw) > 20.0f) {
			/* inertia: the spin decays with a 0.4 s time constant */
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
			v_yaw += ORBIT_RATE * dt;
		}
	}
	g_prev = n;

	v_pitch = CLAMP(v_pitch, PITCH_MIN, PITCH_MAX);
	v_zoom = CLAMP(v_zoom, ZOOM_MIN, ZOOM_MAX);
	/* the middle of the box may not leave the chart area */
	v_panx = CLAMP(v_panx, -0.45f * GFX_W, 0.45f * GFX_W);
	v_pany = CLAMP(v_pany, -0.45f * CH_H, 0.45f * CH_H);
	if (v_yaw > PI_F) {
		v_yaw -= 2.0f * PI_F;
	} else if (v_yaw < -PI_F) {
		v_yaw += 2.0f * PI_F;
	}
	return v_yaw != yaw0 || v_pitch != pitch0 || v_zoom != zoom0 || v_panx != panx0 || v_pany != pany0;
}

/* ---- build -------------------------------------------------------------- */

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
	build_lut();
	l_host = field(root, 16, "HOST", C_TEXT);
	l_cpus = field(root, 170, "LOGICAL PROCESSORS", C_TEXT);
	l_total = field(root, 420, "HOST CPU", C_TEXT);
	l_sel = field(root, 520, "SELECTED", C_AZURE);
	l_draw = field(root, 716, "DRAW", C_TEXT);

	chart = ui_box(root, 0, CH_Y, GFX_W, CH_H);
	lv_obj_add_event_cb(chart, chart_draw, LV_EVENT_DRAW_MAIN, NULL);

	lv_obj_t *hint = ui_caption(root, "DRAG TO ORBIT   PINCH TO ZOOM   TAP A POST   DOUBLE TAP TO RESET");

	lv_obj_align(hint, LV_ALIGN_BOTTOM_RIGHT, -UI_PAD, -8);

	l_wait = ui_label(root, &font_textm_20, C_TEXT2, "Waiting for the host companion");
	lv_obj_set_style_bg_color(l_wait, ui_c(C_BG0), 0);
	lv_obj_set_style_bg_opa(l_wait, LV_OPA_90, 0);
	lv_obj_set_style_pad_all(l_wait, 14, 0);
	lv_obj_set_style_radius(l_wait, 8, 0);
	lv_obj_align(l_wait, LV_ALIGN_CENTER, 0, -10);
	lv_obj_t *how = ui_label(root, &font_mono_12, C_TEXT3, "ON THE PC:  host\\run.cmd");

	lv_obj_align_to(how, l_wait, LV_ALIGN_OUT_BOTTOM_MID, 0, 6);
	lv_obj_set_user_data(l_wait, how);
}

/* Runs whether the screen is visible or not: the Home tile shows it too. */
static void service(uint32_t now_ms)
{
	static uint32_t last_ms;
	const struct hl_state *s = hostlink_state();
	bool online = hostlink_online();
	float k = 1.0f - expf(-MIN((now_ms - last_ms) * 0.001f, 0.1f) / 0.14f);

	last_ms = now_ms;
	if (s->n_cores && plan_signature(s) != plan_sig) {
		build_plan(s);
	}
	/* heights glide to each 10 Hz sample instead of stepping; with no host they sink to zero */
	moving = false;
	for (int i = 0; i < MAXP; i++) {
		float target = 0.0f;

		if (online && i < grp_n) {
			uint32_t sum = 0;

			for (int k = 0; k < grp_cnt[i]; k++) {
				sum += s->load[grp_first[i] + k];
			}
			target = (float)sum / MAX(grp_cnt[i], 1);
		}
		float d = target - shown[i];

		if (fabsf(d) > 0.05f) {
			shown[i] += d * k;
			moving = true;
		} else {
			shown[i] += d;
		}
	}
}

static void tick(uint32_t now_ms)
{
	static uint32_t text_t0;
	static bool was_online; /* the waiting note starts out visible */
	const struct hl_state *s = hostlink_state();
	bool online = hostlink_online();
	char buf[40];

	if (online != was_online) {
		lv_obj_t *how = lv_obj_get_user_data(l_wait);

		was_online = online;
		if (online) {
			lv_obj_add_flag(l_wait, LV_OBJ_FLAG_HIDDEN);
			lv_obj_add_flag(how, LV_OBJ_FLAG_HIDDEN);
		} else {
			lv_obj_remove_flag(l_wait, LV_OBJ_FLAG_HIDDEN);
			lv_obj_remove_flag(how, LV_OBJ_FLAG_HIDDEN);
		}
		lv_obj_invalidate(chart);
	}
	if (gestures(now_ms) || moving) {
		lv_obj_invalidate(chart);
	}

	if (now_ms - text_t0 >= 200) {
		text_t0 = now_ms;
		set_text(l_host, s->packets ? s->host_ip : "-");
		set_text(l_cpus, n_posts ? summary : "-");
		snprintf(buf, sizeof(buf), "%u %%", online ? (s->cpu_x10 + 5) / 10 : 0);
		set_text(l_total, online ? buf : "-");
		snprintf(buf, sizeof(buf), "tap a post");
		for (int i = 0; i < n_posts && selected >= 0; i++) {
			if (posts[i].lp == selected) {
				char name[12];

				post_name(&posts[i], name, sizeof(name));
				char cpu[16];

				group_label(posts[i].lp, cpu, sizeof(cpu));
				snprintf(buf, sizeof(buf), "%s  %s  %d %%", cpu, name, (int)(shown[selected] + 0.5f));
			}
		}
		set_text(l_sel, buf);
		snprintf(buf, sizeof(buf), "%u us", draw_us);
		set_text(l_draw, buf);
	}
}

static void show(void)
{
	/* turn by itself a few seconds after arriving, until someone takes hold */
	g_touch_ms = k_uptime_get_32() - IDLE_ORBIT_MS + 3000;
	g_prev = 0;
	lv_obj_invalidate(chart);
}

static void mini(const struct raster *r)
{
	if (!hostlink_online() || n_posts == 0) {
		/* no host: a flat dashed baseline, not made-up posts */
		raster_hline_dash(r, r->box.x1, r->box.x2, r->box.y2 - 2, C_LINE2, 255, 4, 4);
		return;
	}
	scene_draw(r, NULL, &r->box, YAW0, PITCH0, 1.5f, false); /* closer in: the tile is small */
}

const struct screen screen_coremap = {
	.title = "Core Map",
	.blurb = "A PC's cores, in 3D",
	.create = create,
	.tick = tick,
	.show = show,
	.service = service,
	.mini = mini,
	.attract_secs = 16, /* about a quarter turn */
	.available = hostlink_online,
};
