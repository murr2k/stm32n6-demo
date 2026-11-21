/*
 * Settings: the board's own address (DHCP or fixed, entered on a keypad and
 * applied without a restart), where a person or an agent can read about the
 * board (a QR code for http://<board>/), and which PC is feeding Host Link
 * and Core Map.
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#include <stdio.h>
#include <string.h>

#include <zephyr/kernel.h>
#include <zephyr/net/net_if.h>

#include "platform/hostlink.h"
#include "platform/net.h"
#include "platform/netcfg.h"
#include "screens.h"
#include "ui/theme.h"

#define FIELD_X 110
#define FIELD_W 200
#define FIELD_H 34
#define KEY_W   62
#define KEY_H   40
#define KEY_GAP 8
#define KEYS_Y  232
#define RIGHT_X 480

static lv_obj_t *l_now, *b_dhcp, *b_static, *field[3], *field_l[3], *l_result;
static lv_obj_t *qr, *l_url, *l_host, *l_store;
static struct netcfg edit;
static int sel;                 /* the field the keypad types into */
static char shown_ip[16] = "-";

static char *field_text(int i)
{
	return i == 0 ? edit.ip : i == 1 ? edit.mask : edit.gw;
}

static void style_button(lv_obj_t *b, bool on)
{
	lv_obj_set_style_bg_color(b, ui_c(on ? C_AZURE : C_BG2), 0);
	lv_obj_set_style_border_color(b, ui_c(on ? C_AZURE : C_LINE2), 0);
	lv_obj_set_style_text_color(lv_obj_get_child(b, 0), ui_c(on ? C_BG0 : C_TEXT), 0);
}

static void refresh(void)
{
	style_button(b_dhcp, !edit.fixed);
	style_button(b_static, edit.fixed);
	for (int i = 0; i < 3; i++) {
		bool active = edit.fixed && i == sel;

		lv_label_set_text(field_l[i], field_text(i)[0] ? field_text(i) : (edit.fixed ? "" : "-"));
		lv_obj_set_style_border_color(field[i], ui_c(active ? C_AZURE : C_LINE), 0);
		lv_obj_set_style_border_width(field[i], active ? 2 : 1, 0);
		lv_obj_set_style_text_color(field_l[i], ui_c(edit.fixed ? C_TEXT : C_TEXT3), 0);
	}
}

static void say(const char *text, uint32_t color)
{
	lv_label_set_text(l_result, text);
	lv_obj_set_style_text_color(l_result, ui_c(color), 0);
}

static void mode_click(lv_event_t *e)
{
	edit.fixed = lv_event_get_user_data(e) != NULL;
	if (edit.fixed && edit.ip[0] == '\0') {
		/* start from what the board has now: usually only the last number changes */
		struct net_status ns;

		net_get_status(&ns);
		strncpy(edit.ip, ns.ip, sizeof(edit.ip) - 1);
		strncpy(edit.mask, ns.mask[0] ? ns.mask : "255.255.255.0", sizeof(edit.mask) - 1);
		strncpy(edit.gw, ns.gw, sizeof(edit.gw) - 1);
	}
	sel = 0;
	say(edit.fixed ? "Type the address, then APPLY." : "APPLY to go back to DHCP.", C_TEXT2);
	refresh();
}

static void field_click(lv_event_t *e)
{
	if (edit.fixed) {
		sel = (int)(intptr_t)lv_event_get_user_data(e);
		refresh();
	}
}

static void key_click(lv_event_t *e)
{
	const char *k = lv_event_get_user_data(e);
	char *t = field_text(sel);
	size_t n = strlen(t);

	if (!strcmp(k, "APPLY")) {
		const char *why = netcfg_check(&edit);

		if (why) {
			say(why, C_AMBER);
			return;
		}
		int ret = netcfg_apply(&edit);

		if (ret) {
			char buf[48];

			snprintf(buf, sizeof(buf), "Could not apply (%d).", ret);
			say(buf, C_AMBER);
		} else {
			say(edit.fixed ? "Applied: the board uses this address now." : "Applied: asking DHCP for an address.",
			    C_MINT);
		}
		return;
	}
	if (!edit.fixed) {
		return;
	}
	if (!strcmp(k, "DEL")) {
		if (n) {
			t[n - 1] = '\0';
		}
	} else if (!strcmp(k, "CLR")) {
		t[0] = '\0';
	} else if (n < 15) {
		t[n] = k[0];
		t[n + 1] = '\0';
	}
	refresh();
}

static lv_obj_t *button(lv_obj_t *root, int x, int y, int w, int h, const char *text, lv_event_cb_t cb,
			void *user)
{
	lv_obj_t *b = ui_box(root, x, y, w, h);
	lv_obj_t *l = ui_label(b, &font_textm_16, C_TEXT, text);

	lv_obj_center(l);
	lv_obj_set_style_bg_opa(b, LV_OPA_COVER, 0);
	lv_obj_set_style_bg_color(b, ui_c(C_BG2), 0);
	lv_obj_set_style_bg_color(b, ui_c(C_LINE2), LV_STATE_PRESSED);
	lv_obj_set_style_border_color(b, ui_c(C_LINE2), 0);
	lv_obj_set_style_border_width(b, 1, 0);
	lv_obj_set_style_radius(b, 6, 0);
	lv_obj_add_flag(b, LV_OBJ_FLAG_CLICKABLE);
	lv_obj_add_event_cb(b, cb, LV_EVENT_CLICKED, user);
	return b;
}

static lv_obj_t *text(lv_obj_t *root, int x, int y, int w, const lv_font_t *font, uint32_t color, const char *s)
{
	lv_obj_t *l = ui_label(root, font, color, s);

	lv_obj_set_pos(l, x, y);
	if (w) {
		lv_obj_set_width(l, w);
		lv_label_set_long_mode(l, LV_LABEL_LONG_WRAP);
	}
	return l;
}

static void create(lv_obj_t *root)
{
	static const char *const keys[4][4] = {
		{"1", "2", "3", "DEL"}, {"4", "5", "6", "."}, {"7", "8", "9", "CLR"}, {"0", NULL, "APPLY", NULL}};
	static const char *const names[3] = {"ADDRESS", "MASK", "GATEWAY"};

	lv_obj_set_pos(ui_caption(root, "THIS BOARD"), 16, 8);
	l_now = text(root, 16, 24, 440, &font_mono_14, C_TEXT, "-");

	b_dhcp = button(root, 16, 56, 120, 36, "DHCP", mode_click, NULL);
	b_static = button(root, 144, 56, 120, 36, "FIXED", mode_click, (void *)1);

	for (int i = 0; i < 3; i++) {
		int y = 104 + i * (FIELD_H + 8);

		lv_obj_set_pos(ui_caption(root, names[i]), 16, y + 10);
		field[i] = ui_box(root, FIELD_X, y, FIELD_W, FIELD_H);
		lv_obj_set_style_bg_opa(field[i], LV_OPA_COVER, 0);
		lv_obj_set_style_bg_color(field[i], ui_c(C_BG1), 0);
		lv_obj_set_style_border_width(field[i], 1, 0);
		lv_obj_set_style_radius(field[i], 6, 0);
		lv_obj_add_flag(field[i], LV_OBJ_FLAG_CLICKABLE);
		lv_obj_add_event_cb(field[i], field_click, LV_EVENT_CLICKED, (void *)(intptr_t)i);
		field_l[i] = ui_label(field[i], &font_mono_14, C_TEXT, "");
		lv_obj_align(field_l[i], LV_ALIGN_LEFT_MID, 10, 0);
	}

	for (int r = 0; r < 4; r++) {
		for (int c = 0; c < 4; c++) {
			const char *k = keys[r][c];

			if (k == NULL) {
				continue;
			}
			bool wide = r == 3; /* "0" and "APPLY" take two columns each */

			button(root, 16 + c * (KEY_W + KEY_GAP), KEYS_Y + r * (KEY_H + KEY_GAP),
			       wide ? 2 * KEY_W + KEY_GAP : KEY_W, KEY_H, k, key_click, (void *)k);
		}
	}
	l_result = text(root, 300, KEYS_Y + 4, 160, &font_text_14, C_TEXT2, "");
	l_store = text(root, 300, KEYS_Y + 110, 160, &font_text_14, C_TEXT3, "");
	text(root, 300, KEYS_Y + 150, 160, &font_text_14, C_TEXT3,
	     "Hold the USER button while powering up to force DHCP.");

	/* right: where to read about the board */
	lv_obj_set_pos(ui_caption(root, "FOR PEOPLE AND AGENTS"), RIGHT_X, 8);
	qr = lv_qrcode_create(root);
	lv_qrcode_set_size(qr, 132);
	lv_qrcode_set_dark_color(qr, ui_c(C_BG0));
	lv_qrcode_set_light_color(qr, ui_c(C_TEXT));
	lv_qrcode_set_quiet_zone(qr, true);
	lv_obj_set_pos(qr, RIGHT_X, 30);
	l_url = text(root, RIGHT_X + 146, 34, 0, &font_mono_12, C_AZURE, "-");
	text(root, RIGHT_X + 150, 76, 154, &font_text_14, C_TEXT2,
	     "A remote for a phone: the screens, the demo, the clips. Linked from it: what this board is, "
	     "for a person or an agent.");

	lv_obj_set_pos(ui_caption(root, "DATA HOST"), RIGHT_X, 186);
	l_host = text(root, RIGHT_X, 202, 300, &font_text_14, C_TEXT, "-");
	text(root, RIGHT_X, 250, 300, &font_text_14, C_TEXT3,
	     "Host Link and Core Map show the load of a PC running the companion (run.cmd). It finds this "
	     "board by itself: it broadcasts a query on udp/5557 and the board answers.");

	netcfg_get(&edit);
	refresh();
}

static void tick(uint32_t now_ms)
{
	static uint32_t t0;
	struct net_status ns;
	char buf[96];

	if (now_ms - t0 < 500) {
		return;
	}
	t0 = now_ms;
	net_get_status(&ns);
	struct net_linkaddr *mac = net_if_get_link_addr(net_if_get_default());

	snprintf(buf, sizeof(buf), "%s  %s%s  MAC %02x:%02x:%02x:%02x:%02x:%02x", ns.have_ip ? ns.ip : "no address",
		 ns.fixed ? "FIXED" : "DHCP", netcfg_recovery() ? " (USER held)" : "", mac->addr[0], mac->addr[1],
		 mac->addr[2], mac->addr[3], mac->addr[4], mac->addr[5]);
	if (strcmp(lv_label_get_text(l_now), buf)) {
		lv_label_set_text(l_now, buf);
	}
	if (strcmp(shown_ip, ns.have_ip ? ns.ip : "-")) {
		strncpy(shown_ip, ns.have_ip ? ns.ip : "-", sizeof(shown_ip) - 1);
		snprintf(buf, sizeof(buf), "http://%s/", shown_ip);
		lv_qrcode_update(qr, buf, strlen(buf));
		lv_label_set_text(l_url, buf);
	}
	const struct hl_state *h = hostlink_state();

	if (hostlink_online()) {
		snprintf(buf, sizeof(buf), "%s, %u logical processors", h->host_ip, h->n_host_cores);
	} else if (h->packets) {
		snprintf(buf, sizeof(buf), "%s, not heard for a while", h->host_ip);
	} else {
		snprintf(buf, sizeof(buf), "none yet");
	}
	if (strcmp(lv_label_get_text(l_host), buf)) {
		lv_label_set_text(l_host, buf);
	}
	snprintf(buf, sizeof(buf), "Stored: %s", netcfg_store_state());
	if (strcmp(lv_label_get_text(l_store), buf)) {
		lv_label_set_text(l_store, buf);
	}
}

static void mini(const struct raster *r)
{
	/* a small keypad, nothing live to draw */
	int w = lv_area_get_width(&r->box), h = lv_area_get_height(&r->box);
	int kw = w / 6, kh = h / 5;

	for (int row = 0; row < 3; row++) {
		for (int col = 0; col < 3; col++) {
			int x = r->box.x1 + w / 2 - (3 * kw) / 2 + col * kw;
			int y = r->box.y1 + h / 2 - (3 * kh) / 2 + row * kh;

			raster_fill(r, x + 2, y + 2, x + kw - 3, y + kh - 3, C_BG2);
		}
	}
}

const struct screen screen_settings = {
	.title = "Settings",
	.blurb = "Address, and how to connect",
	.create = create,
	.tick = tick,
	.mini = mini,
	.attract_skip = true, /* nothing moves; and taps must not change the address */
};
