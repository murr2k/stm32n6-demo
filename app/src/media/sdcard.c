/*
 * microSD card: mount, sector-aligned reads, upload service. See sdcard.h.
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/net/socket.h>
#include <zephyr/sys/crc.h>

#include <ff.h>
#include <soc.h>
#include <stm32n6xx_hal.h>

#include "platform/net.h"
#include "player.h"
#include "sdcard.h"

LOG_MODULE_REGISTER(sdcard, LOG_LEVEL_INF);

#define SRV_PORT 5558
#define TMP_PATH SD_CLIPS_DIR "/upload.tmp"
#define PATH_MAX_LEN (sizeof(SD_CLIPS_DIR) + 1 + SD_NAME_MAX)

static FATFS fat;
static struct fs_mount_t mnt = {.type = FS_FATFS, .mnt_point = "/SD:", .fs_data = &fat};
static volatile bool mounted;
static struct sd_status status;
static struct k_spinlock st_lock;
static K_MUTEX_DEFINE(mount_lock);
static K_MUTEX_DEFINE(io_lock);
static K_MUTEX_DEFINE(lib_lock);
static uint8_t io_buf[SD_IO_BLOCK] __aligned(32);
static uint8_t rx_buf[SD_IO_BLOCK] __aligned(32);

/* Let the SDMMC2 DMA master act with the CPU's rights (secure, privileged,
 * compartment 1). Out of reset it is non-secure and its writes to the RAM
 * this application runs in are refused.
 */
static void rif_open(void)
{
	RIMC_MasterConfig_t rimc = {
		.MasterCID = RIF_CID_1,
		.SecPriv = RIF_ATTRIBUTE_SEC | RIF_ATTRIBUTE_PRIV,
	};

	HAL_RIF_RIMC_ConfigMasterAttributes(RIF_MASTER_INDEX_SDMMC2, &rimc);
	HAL_RIF_RISC_SetSlaveSecureAttributes(RIF_RISC_PERIPH_INDEX_SDMMC2,
					      RIF_ATTRIBUTE_SEC | RIF_ATTRIBUTE_PRIV);
}

/* The card's bus at 48 MHz, in High Speed mode. Zephyr's driver calls
 * HAL_SD_Init(), which brings the card up and sets the 1-bit bus to the
 * clock of hsd->Init.ClockDiv, then HAL_SD_ConfigWideBusOperation() for 4
 * bits, which takes the clock from it again. Neither ever sends CMD6, and
 * without it the card stays in Default Speed signalling, specified to
 * 25 MHz. So: initialise at the overlay's clk-div (24 MHz), switch the card
 * to High Speed, and only if it agrees clear the divider (the kernel clock,
 * 48 MHz) for the 4-bit setup that follows. A card that refuses stays at
 * 24 MHz. Linked with -Wl,--wrap=HAL_SD_Init (app/CMakeLists.txt).
 */
static volatile bool high_speed;

HAL_StatusTypeDef __real_HAL_SD_Init(SD_HandleTypeDef *hsd);

HAL_StatusTypeDef __wrap_HAL_SD_Init(SD_HandleTypeDef *hsd)
{
	hsd->Init.ClockDiv = MAX(hsd->Init.ClockDiv, 1U); /* again 24 MHz on a re-init */
	high_speed = false;

	HAL_StatusTypeDef ret = __real_HAL_SD_Init(hsd);

	if (ret != HAL_OK) {
		return ret;
	}
	if (HAL_SD_ConfigSpeedBusOperation(hsd, SDMMC_SPEED_MODE_HIGH) == HAL_OK) {
		hsd->Init.ClockDiv = 0;
		high_speed = true;
	} else {
		hsd->ErrorCode = HAL_SD_ERROR_NONE;
		hsd->State = HAL_SD_STATE_READY;
	}
	return HAL_OK;
}

/* Zephyr's fs layer logs an error for every call that fails, also where failing
 * is the normal case (a temporary file that is not there, a directory that
 * is): ask first. fs_stat() is quiet about a missing entry.
 */
static bool exists(const char *path)
{
	struct fs_dirent e;

	return fs_stat(path, &e) == 0;
}

static void remove_if_there(const char *path)
{
	if (exists(path)) {
		fs_unlink(path);
	}
}

void sdcard_note_change(const char *name)
{
	k_spinlock_key_t key = k_spin_lock(&st_lock);
	char *slot = status.changed[++status.generation % SD_CHANGES];

	memset(slot, 0, SD_NAME_MAX);
	strncpy(slot, name, SD_NAME_MAX - 1);
	status.receiving = false;
	k_spin_unlock(&st_lock, key);
}

int sdcard_mount(void)
{
	int ret = 0;

	k_mutex_lock(&mount_lock, K_FOREVER);
	if (!mounted) {
		rif_open(); /* without it: "fs mount error (-19)", the DMA's writes do not land */
		ret = fs_mount(&mnt);
		if (ret == 0) {
			int mk = exists(SD_CLIPS_DIR) ? 0 : fs_mkdir(SD_CLIPS_DIR);

			if (mk != 0) {
				LOG_WRN("cannot create %s (%d)", SD_CLIPS_DIR, mk);
			}
			mounted = true;
			status.mounted = true;
			sdcard_note_change("");
			status.high_speed = high_speed;
			LOG_INF("card mounted, %s, %s", fat.fs_type == FS_EXFAT ? "exFAT" : "FAT",
				high_speed ? "High Speed at 48 MHz" : "Default Speed at 24 MHz");
		} else {
			LOG_WRN("no usable card (%d)", ret);
		}
	}
	k_mutex_unlock(&mount_lock);
	return ret;
}

bool sdcard_mounted(void)
{
	return mounted;
}

void sdcard_get_status(struct sd_status *st)
{
	k_spinlock_key_t key = k_spin_lock(&st_lock);

	*st = status;
	k_spin_unlock(&st_lock, key);
}

void sdcard_library_lock(void)
{
	k_mutex_lock(&lib_lock, K_FOREVER);
}

void sdcard_library_unlock(void)
{
	k_mutex_unlock(&lib_lock);
}

int sdcard_read_at(struct fs_file_t *f, uint32_t off, void *dst, uint32_t len)
{
	uint8_t *out = dst;
	int ret = 0;

	k_mutex_lock(&io_lock, K_FOREVER);
	while (len > 0) {
		uint32_t base = off & ~511u, skip = off - base;
		uint32_t want = MIN(ROUND_UP(skip + len, 512), SD_IO_BLOCK);
		ssize_t got;

		if (fs_tell(f) != (off_t)base && fs_seek(f, base, FS_SEEK_SET) != 0) {
			ret = -EIO;
			break;
		}
		got = fs_read(f, io_buf, want);
		if (got < 0) {
			ret = (int)got;
			break;
		}
		if ((uint32_t)got <= skip) {
			break; /* end of file */
		}
		uint32_t n = MIN((uint32_t)got - skip, len);

		memcpy(out, io_buf + skip, n);
		out += n;
		off += n;
		len -= n;
		if ((uint32_t)got < want) {
			break; /* end of file */
		}
	}
	k_mutex_unlock(&io_lock);
	return ret < 0 ? ret : (int)(out - (uint8_t *)dst);
}

/* Asking a FAT32 volume for its free space can mean reading the whole
 * allocation table, so this is done here, on the service thread, and cached.
 */
static void refresh_space(void)
{
	struct fs_statvfs sv;

	if (mounted && fs_statvfs("/SD:", &sv) == 0) {
		k_spinlock_key_t key = k_spin_lock(&st_lock);

		status.total_mb = (uint32_t)(((uint64_t)sv.f_blocks * sv.f_frsize) >> 20);
		status.free_mb = (uint32_t)(((uint64_t)sv.f_bfree * sv.f_frsize) >> 20);
		status.space_known = true;
		k_spin_unlock(&st_lock, key);
	}
}

/* A clip's file name: letters, digits, dot, dash, underscore, ending in .n6v. */
static bool name_ok(const char *name)
{
	size_t n = strlen(name);

	if (n < 5 || n >= SD_NAME_MAX || name[0] == '.' || strcmp(name + n - 4, ".n6v") != 0) {
		return false;
	}
	for (size_t i = 0; i < n; i++) {
		if (!isalnum((unsigned char)name[i]) && strchr("._-", name[i]) == NULL) {
			return false;
		}
	}
	return true;
}

static void clip_path(char *path, const char *name)
{
	snprintf(path, PATH_MAX_LEN, "%s/%s", SD_CLIPS_DIR, name);
}

static int cmd_list(int s, char *out, size_t out_len)
{
	struct fs_dir_t dir;
	struct fs_dirent ent;
	int count = 0;

	fs_dir_t_init(&dir);
	if (fs_opendir(&dir, SD_CLIPS_DIR) != 0) {
		return snprintf(out, out_len, "err cannot read %s\n", SD_CLIPS_DIR);
	}
	while (fs_readdir(&dir, &ent) == 0 && ent.name[0] != '\0') {
		if (ent.type == FS_DIR_ENTRY_FILE) {
			int n = snprintf(out, out_len, "f %s %u\n", ent.name, (unsigned int)ent.size);

			zsock_send(s, out, n, 0);
			count++;
		}
	}
	fs_closedir(&dir);
	return snprintf(out, out_len, "ok %d\n", count);
}

/* Returns the reply length, negated when the connection must be dropped
 * because the byte stream is no longer in step with the protocol.
 */
static int cmd_put(int s, const char *name, uint32_t size, uint32_t crc_want, char *out, size_t out_len)
{
	char path[PATH_MAX_LEN];
	struct fs_file_t f;
	uint32_t done = 0, credited = 0, crc = 0, t0 = k_uptime_get_32(), ms, net_ms = 0, card_ms = 0;
	k_spinlock_key_t key;
	int ret;

	if (!name_ok(name)) {
		return snprintf(out, out_len, "err bad name\n");
	}
	if (player_using_file(name)) {
		return snprintf(out, out_len, "err busy: that clip is playing\n");
	}
	remove_if_there(TMP_PATH);
	fs_file_t_init(&f);
	ret = fs_open(&f, TMP_PATH, FS_O_CREATE | FS_O_WRITE);
	if (ret != 0) {
		return snprintf(out, out_len, "err cannot create the file (%d)\n", ret);
	}
	zsock_send(s, "ok go\n", 6, 0);

	key = k_spin_lock(&st_lock);
	status.receiving = true;
	status.rx_done = 0;
	status.rx_size = size;
	strncpy(status.rx_name, name, sizeof(status.rx_name) - 1);
	k_spin_unlock(&st_lock, key);

	while (done < size && ret == 0) {
		uint32_t fill = 0, want = MIN(size - done, SD_IO_BLOCK);
		uint32_t t1 = k_uptime_get_32();

		/* whole blocks from an aligned buffer: FatFs passes them straight to the card */
		while (fill < want) {
			ssize_t n = zsock_recv(s, rx_buf + fill, want - fill, 0);

			if (n <= 0) {
				ret = -ECONNRESET;
				break;
			}
			fill += n;
			/* one credit per piece taken in: the host sends the next piece on it (see sdcard.h) */
			while (done + fill - credited >= SD_PIECE) {
				credited += SD_PIECE;
				zsock_send(s, ".", 1, 0);
			}
		}
		if (ret == 0) {
			uint32_t t2 = k_uptime_get_32();
			ssize_t w = fs_write(&f, rx_buf, fill);

			net_ms += t2 - t1;
			card_ms += k_uptime_get_32() - t2;
			crc = crc32_ieee_update(crc, rx_buf, fill);
			if (w != (ssize_t)fill) {
				ret = w < 0 ? (int)w : -ENOSPC;
			}
			done += fill;
			ms = MAX(k_uptime_get_32() - t0, 1u);
			key = k_spin_lock(&st_lock);
			status.rx_done = done;
			status.rx_kbps = (uint32_t)((uint64_t)done * 8 / ms);
			k_spin_unlock(&st_lock, key);
		}
	}
	int cl = fs_close(&f);

	if (ret == 0 && cl != 0) {
		ret = cl;
	}
	ms = MAX(k_uptime_get_32() - t0, 1u);
	if (ret == 0 && crc != crc_want) {
		ret = -EBADMSG;
	}
	if (ret == 0) {
		clip_path(path, name);
		sdcard_library_lock();
		if (player_using_file(name)) {
			ret = -EBUSY;
		} else {
			remove_if_there(path);
			ret = fs_rename(TMP_PATH, path);
		}
		sdcard_library_unlock();
	}
	if (ret != 0) {
		remove_if_there(TMP_PATH);
	}
	sdcard_note_change(name);
	refresh_space();

	if (ret == 0) {
		LOG_INF("received %s: %u KB in %u ms (network %u ms, card %u ms)", name, done / 1024, ms, net_ms,
			card_ms);
		return snprintf(out, out_len, "ok %u %u %u %u\n", done, ms, net_ms, card_ms);
	}
	LOG_WRN("upload of %s failed (%d) after %u of %u bytes", name, ret, done, size);
	/* after a failure part of the file may still be on its way: start afresh */
	return -snprintf(out, out_len, "err %s (%d)\n",
			 ret == -EBADMSG ? "checksum mismatch" :
			 ret == -ENOSPC ? "card full" :
			 ret == -EBUSY ? "busy: that clip is playing" : "write failed", ret);
}

static int cmd_del(const char *name, char *out, size_t out_len)
{
	char path[PATH_MAX_LEN];
	int ret;

	if (!name_ok(name)) {
		return snprintf(out, out_len, "err bad name\n");
	}
	clip_path(path, name);
	sdcard_library_lock();
	ret = player_using_file(name) ? -EBUSY : fs_unlink(path);
	sdcard_library_unlock();
	if (ret != 0) {
		return snprintf(out, out_len, "err %s (%d)\n", ret == -EBUSY ? "busy: that clip is playing" : "cannot delete", ret);
	}
	sdcard_note_change(name);
	refresh_space();
	return snprintf(out, out_len, "ok\n");
}

static int handle(int s, char *line, char *out, size_t out_len)
{
	char cmd[8] = {0}, name[SD_NAME_MAX] = {0};
	unsigned int size = 0, crc = 0;
	int n = sscanf(line, "%7s %39s %u %x", cmd, name, &size, &crc);

	if (n < 1) {
		return snprintf(out, out_len, "err empty\n");
	}
	if (!strcmp(cmd, "mount")) {
		int ret = sdcard_mount();

		refresh_space();
		return ret == 0 ? snprintf(out, out_len, "ok\n") : snprintf(out, out_len, "err %d\n", ret);
	}
	if (!mounted) {
		return snprintf(out, out_len, "err no card\n");
	}
	if (!strcmp(cmd, "list")) {
		return cmd_list(s, out, out_len);
	}
	if (!strcmp(cmd, "put") && n == 4) {
		return cmd_put(s, name, size, crc, out, out_len);
	}
	if (!strcmp(cmd, "del") && n >= 2) {
		return cmd_del(name, out, out_len);
	}
	if (!strcmp(cmd, "df")) {
		struct sd_status st;

		refresh_space();
		sdcard_get_status(&st);
		return snprintf(out, out_len, "ok total_mb=%u free_mb=%u\n", st.total_mb, st.free_mb);
	}
	return snprintf(out, out_len, "err unknown: %s\n", cmd);
}

static void service_thread(void *p1, void *p2, void *p3)
{
	struct sockaddr_in addr = {.sin_family = AF_INET, .sin_port = htons(SRV_PORT)};
	static char line[96], out[160];
	int srv;

	net_wait_ip();
	refresh_space();
	srv = zsock_socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
	if (srv < 0 || zsock_bind(srv, (struct sockaddr *)&addr, sizeof(addr)) < 0 ||
	    zsock_listen(srv, 1) < 0) {
		LOG_ERR("socket setup failed (%d)", errno);
		return;
	}
	LOG_INF("media upload on tcp/%d", SRV_PORT);
	for (;;) {
		int s = zsock_accept(srv, NULL, NULL);
		size_t len = 0;

		if (s < 0) {
			continue;
		}
		/* one client at a time: a silent one must not hold the port (see ctl.c) */
		struct zsock_timeval idle = {.tv_sec = 10};

		zsock_setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, &idle, sizeof(idle));
		zsock_setsockopt(s, SOL_SOCKET, SO_SNDTIMEO, &idle, sizeof(idle));
		for (;;) {
			char ch;

			if (zsock_recv(s, &ch, 1, 0) <= 0) {
				break;
			}
			if (ch == '\n' || len == sizeof(line) - 1) {
				line[len] = '\0';
				len = 0;
				int n = handle(s, line, out, sizeof(out));

				if (zsock_send(s, out, abs(n), 0) < 0 || n < 0) {
					break;
				}
			} else if (ch != '\r') {
				line[len++] = ch;
			}
		}
		zsock_close(s);
	}
}
K_THREAD_DEFINE(sd_service, 6144, service_thread, NULL, NULL, NULL, 10, 0, 0);
