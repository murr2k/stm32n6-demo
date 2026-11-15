/*
 * Phase 0 bring-up firmware for the STM32N6570-DK demo.
 *
 * Proves, and measures, the pieces everything else stands on:
 *   - memory bandwidth of internal SRAM vs HexaSPI PSRAM (frame buffer placement)
 *   - LTDC with app-owned double buffer, swapped in the vertical blanking
 *   - GT911 multi-touch events
 *   - Ethernet + DHCP
 *   - frame buffer readback over TCP (the host's eyes)
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#include <string.h>

#include <zephyr/cache.h>
#include <zephyr/device.h>
#include <zephyr/drivers/display.h>
#include <zephyr/input/input.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/net/net_if.h>
#include <zephyr/net/net_mgmt.h>
#include <zephyr/net/socket.h>

LOG_MODULE_REGISTER(bringup, LOG_LEVEL_INF);

#define W 800
#define H 480
#define FB_BYTES (W * H * 4)

/* AXISRAM3..6 are four contiguous 448 KB banks: 1.75 MB at 0x3420_0000. */
#define FB_INTERNAL ((uint32_t *)0x34200000)
#define FB_PSRAM_A  ((uint32_t *)0x90000000)
#define FB_PSRAM_B  ((uint32_t *)(0x90000000 + 0x200000))

extern uint32_t SystemCoreClock;

static const struct device *const disp = DEVICE_DT_GET(DT_CHOSEN(zephyr_display));
static const uint32_t *volatile front_fb;

/* ---- memory benchmark ---------------------------------------------------- */

static uint32_t us_since(uint64_t t0)
{
	return (uint32_t)k_cyc_to_us_floor64(k_cycle_get_64() - t0);
}

static void bench(const char *name, uint32_t *p)
{
	const size_t words = FB_BYTES / 4;
	uint64_t t0;
	uint32_t fill_us, copy_us, read_us, sum = 0;

	t0 = k_cycle_get_64();
	for (size_t i = 0; i < words; i++) {
		p[i] = 0xff000000u | i;
	}
	sys_cache_data_flush_range(p, FB_BYTES);
	fill_us = us_since(t0);

	t0 = k_cycle_get_64();
	memcpy(p + words / 2, p, FB_BYTES / 2);
	sys_cache_data_flush_range(p, FB_BYTES);
	copy_us = us_since(t0);

	sys_cache_data_invd_range(p, FB_BYTES);
	t0 = k_cycle_get_64();
	for (size_t i = 0; i < words; i++) {
		sum += p[i];
	}
	read_us = us_since(t0);

	printk("BENCH %-10s fill %5u us (%4u MB/s)  copy-half %5u us (%4u MB/s)  read %5u us (%4u MB/s)  sum %08x\n",
	       name, fill_us, (uint32_t)(FB_BYTES / (fill_us ? fill_us : 1)),
	       copy_us, (uint32_t)(FB_BYTES / 2 / (copy_us ? copy_us : 1)),
	       read_us, (uint32_t)(FB_BYTES / (read_us ? read_us : 1)), sum);
}

/* ---- test pattern -------------------------------------------------------- */

static inline uint32_t rgb(uint32_t r, uint32_t g, uint32_t b)
{
	return 0xff000000u | (r << 16) | (g << 8) | b;
}

static void draw_pattern(uint32_t *fb)
{
	static const uint32_t bars[8] = {
		0xffffffff, 0xffffff00, 0xff00ffff, 0xff00ff00,
		0xffff00ff, 0xffff0000, 0xff0000ff, 0xff000000,
	};

	for (int y = 0; y < H; y++) {
		uint32_t *row = fb + y * W;

		for (int x = 0; x < W; x++) {
			uint32_t c;
			uint32_t v = (x * 255) / (W - 1);

			if (y < 160) {
				c = bars[x / 100];
			} else if (y < 200) {
				c = rgb(v, 0, 0);
			} else if (y < 240) {
				c = rgb(0, v, 0);
			} else if (y < 280) {
				c = rgb(0, 0, v);
			} else if (y < 320) {
				c = rgb(v, v, v);
			} else if (y < 400) {
				/* slow two-colour gradient: banding shows here first */
				c = rgb(16 + (x * 24) / W, 24 + (x * 40) / W, 40 + ((y - 320) * 60) / 80);
			} else {
				c = ((x ^ y) & 1) ? 0xffffffff : 0xff000000; /* 1 px checker */
			}
			row[x] = c;
		}
	}
	/* 1 px border proves the active area is not shifted or cropped */
	for (int x = 0; x < W; x++) {
		fb[x] = fb[(H - 1) * W + x] = 0xffff8000;
	}
	for (int y = 0; y < H; y++) {
		fb[y * W] = fb[y * W + W - 1] = 0xffff8000;
	}
}

static int present(const uint32_t *fb)
{
	const struct display_buffer_descriptor desc = {
		.buf_size = FB_BYTES, .width = W, .height = H, .pitch = W,
	};
	int ret = display_write(disp, 0, 0, &desc, fb);

	if (ret == 0) {
		front_fb = fb;
	}
	return ret;
}

/* Sweep a bar across both buffers and time the swaps: gives the refresh rate
 * and, to a human, shows whether the swap tears.
 */
static void swap_test(const char *name, uint32_t *a, uint32_t *b, int frames)
{
	uint32_t *buf[2] = {a, b};
	int prev_x[2] = {-1, -1};
	uint32_t worst = 0;
	uint64_t t_all = k_cycle_get_64();

	draw_pattern(a);
	draw_pattern(b);
	for (int f = 0; f < frames; f++) {
		uint32_t *fb = buf[f & 1];
		int x = (f * 8) % (W - 16);
		uint64_t t0 = k_cycle_get_64();

		if (prev_x[f & 1] >= 0) { /* restore what the bar covered */
			for (int y = 330; y < 390; y++) {
				for (int i = 0; i < 16; i++) {
					int px = prev_x[f & 1] + i;

					fb[y * W + px] = rgb(16 + (px * 24) / W, 24 + (px * 40) / W,
							     40 + ((y - 320) * 60) / 80);
				}
			}
		}
		for (int y = 330; y < 390; y++) {
			for (int i = 0; i < 16; i++) {
				fb[y * W + x + i] = 0xffffffff;
			}
		}
		prev_x[f & 1] = x;
		present(fb);

		uint32_t us = us_since(t0);

		if (f > 1 && us > worst) {
			worst = us;
		}
	}
	uint32_t total = us_since(t_all);

	printk("SWAP  %-18s %d frames in %u us: %u.%02u fps, worst frame %u us\n", name, frames, total,
	       (uint32_t)((uint64_t)frames * 1000000 / total),
	       (uint32_t)(((uint64_t)frames * 100000000 / total) % 100), worst);
}

/* ---- touch --------------------------------------------------------------- */

static void touch_cb(struct input_event *evt, void *user_data)
{
	static int slot, x, y;
	static int64_t last;

	ARG_UNUSED(user_data);
	switch (evt->code) {
	case INPUT_ABS_MT_SLOT:
		slot = evt->value;
		break;
	case INPUT_ABS_X:
		x = evt->value;
		break;
	case INPUT_ABS_Y:
		y = evt->value;
		break;
	case INPUT_BTN_TOUCH:
		if (evt->value == 0 || k_uptime_get() - last > 100) {
			last = k_uptime_get();
			printk("TOUCH slot %d %s x=%d y=%d\n", slot, evt->value ? "down" : "up", x, y);
		}
		break;
	default:
		break;
	}
}
INPUT_CALLBACK_DEFINE(DEVICE_DT_GET(DT_CHOSEN(zephyr_touch)), touch_cb, NULL);

/* ---- network ------------------------------------------------------------- */

static struct net_mgmt_event_callback ipv4_cb;
static K_SEM_DEFINE(have_ip, 0, 1);

static void ipv4_event(struct net_mgmt_event_callback *cb, uint64_t event, struct net_if *iface)
{
	char buf[NET_IPV4_ADDR_LEN];

	if (event != NET_EVENT_IPV4_ADDR_ADD) {
		return;
	}
	for (int i = 0; i < NET_IF_MAX_IPV4_ADDR; i++) {
		struct net_if_addr *a = &iface->config.ip.ipv4->unicast[i].ipv4;

		if (a->is_used && a->addr_type == NET_ADDR_DHCP) {
			printk("NET   ip %s\n", net_addr_ntop(AF_INET, &a->address.in_addr, buf, sizeof(buf)));
			k_sem_give(&have_ip);
		}
	}
}

/* Frame buffer dump: connect to TCP 5555, receive a 16-byte header then the
 * raw front buffer. tools/shot.py turns it into a PNG.
 */
static void fbdump_thread(void *a, void *b, void *c)
{
	struct sockaddr_in addr = {.sin_family = AF_INET, .sin_port = htons(5555)};
	int srv;

	k_sem_take(&have_ip, K_FOREVER);
	srv = zsock_socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
	if (srv < 0 || zsock_bind(srv, (struct sockaddr *)&addr, sizeof(addr)) < 0 || zsock_listen(srv, 1) < 0) {
		LOG_ERR("fbdump: socket setup failed (%d)", errno);
		return;
	}
	printk("NET   fbdump listening on tcp/5555\n");
	for (;;) {
		int s = zsock_accept(srv, NULL, NULL);

		if (s < 0) {
			continue;
		}
		const uint8_t *p = (const uint8_t *)front_fb;
		uint32_t hdr[4] = {0x4246364e /* "N6FB" */, W, H, 32};
		size_t left = p ? FB_BYTES : 0;
		uint64_t t0 = k_cycle_get_64();

		zsock_send(s, hdr, sizeof(hdr), 0);
		while (left) {
			ssize_t n = zsock_send(s, p, MIN(left, 16384), 0);

			if (n <= 0) {
				break;
			}
			p += n;
			left -= n;
		}
		zsock_close(s);
		printk("NET   fbdump sent %u bytes in %u ms\n", (uint32_t)(FB_BYTES - left), us_since(t0) / 1000);
	}
}
K_THREAD_DEFINE(fbdump, 4096, fbdump_thread, NULL, NULL, NULL, 8, 0, 0);

/* ---- main ---------------------------------------------------------------- */

int main(void)
{
	static uint32_t scratch[FB_BYTES / 16]; /* small internal-RAM reference */

	printk("\nN6 demo bring-up: core clock %u Hz, sys ticks %u Hz\n", SystemCoreClock,
	       sys_clock_hw_cycles_per_sec());

	net_mgmt_init_event_callback(&ipv4_cb, ipv4_event, NET_EVENT_IPV4_ADDR_ADD);
	net_mgmt_add_event_callback(&ipv4_cb);
	net_dhcpv4_start(net_if_get_default());

	ARG_UNUSED(scratch);
	bench("AXISRAM3-6", FB_INTERNAL);
	bench("PSRAM", FB_PSRAM_A);

	if (!device_is_ready(disp)) {
		printk("DISP  device not ready\n");
		return 0;
	}
	struct display_capabilities caps;

	display_get_capabilities(disp, &caps);
	printk("DISP  %ux%u fmt 0x%x\n", caps.x_resolution, caps.y_resolution, caps.current_pixel_format);

	draw_pattern(FB_INTERNAL);
	present(FB_INTERNAL);
	display_blanking_off(disp);
	printk("DISP  pattern up\n");

	swap_test("internal+psram", FB_INTERNAL, FB_PSRAM_A, 300);
	swap_test("psram+psram", FB_PSRAM_A, FB_PSRAM_B, 300);

	for (;;) {
		swap_test("internal+psram", FB_INTERNAL, FB_PSRAM_A, 600);
	}
	return 0;
}
