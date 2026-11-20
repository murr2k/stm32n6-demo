/*
 * Home power feed. See homepower.h.
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#include <ctype.h>
#include <errno.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#include <zephyr/fs/fs.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/net/socket.h>

#include "homepower.h"
#include "media/sdcard.h"
#include "net.h"
#include "psram.h"

LOG_MODULE_REGISTER(homepower, LOG_LEVEL_INF);

#define CFG_PATH       "/SD:/power.cfg"
#define URL_MAX        128
#define RX_CAP         (64 * 1024) /* the 30-day chart is about 25 KB */
#define STACK_SIZE     6144        /* in PSRAM: internal RAM has a few KB left */
#define PRIO           13
#define POLL_SHOWN_MS  15000       /* as the monitor's own page refreshes */
#define POLL_HIDDEN_MS 60000
#define POLL_LONG_MS   60000       /* the 7- and 30-day charts */
#define RETRY_MS       30000
#define RESOLVE_MS     (10 * 60 * 1000)
#define IO_TIMEOUT_S   5

struct target {
	char host[64];
	uint16_t port;
	char prefix[48]; /* a path before /api, without its last slash */
};

static const char *const range_names[HP_RANGES] = {"1h", "6h", "24h", "7d", "30d"};

static struct k_spinlock lock;
static K_SEM_DEFINE(wake, 0, 1);
static struct k_thread thread;

/* set by ctl, taken by the thread (under lock) */
static char url[URL_MAX];
static bool url_changed, url_save;

static volatile bool visible, glance, kick, online, started;
static volatile int range = HP_24H;
static volatile const char *state = "starting";

static struct hp_stats stats; /* under lock */
static struct hp_series *ser[2];
static volatile int ser_front = -1;
static char *rx;

/* counters for ctl "power" (the thread writes, ctl reads; approximate is fine) */
static uint32_t n_ok, n_fail, last_ms, worst_ms, last_bytes;
static int last_err, last_http;

const char *homepower_range_name(int r)
{
	return r >= 0 && r < HP_RANGES ? range_names[r] : "?";
}

/* ---- URL and settings ---------------------------------------------------- */

static int parse_url(const char *u, struct target *t)
{
	memset(t, 0, sizeof(*t));
	if (strncmp(u, "http://", 7) != 0) {
		return -EINVAL; /* https needs TLS, which the board does not have */
	}
	const char *h = u + 7;
	size_t hl = strcspn(h, ":/");

	if (hl == 0 || hl >= sizeof(t->host)) {
		return -EINVAL;
	}
	memcpy(t->host, h, hl);
	t->port = 80;
	h += hl;
	if (*h == ':') {
		char *end;
		long p = strtol(h + 1, &end, 10);

		if (end == h + 1 || p <= 0 || p > 65535) {
			return -EINVAL;
		}
		t->port = (uint16_t)p;
		h = end;
	}
	if (*h == '/') {
		size_t pl = strlen(h);

		while (pl > 0 && h[pl - 1] == '/') {
			pl--;
		}
		if (pl >= sizeof(t->prefix) || strpbrk(h, " ?#\r\n")) {
			return -EINVAL;
		}
		memcpy(t->prefix, h, pl);
	} else if (*h != '\0') {
		return -EINVAL;
	}
	return 0;
}

int homepower_set_url(const char *u)
{
	struct target t;
	k_spinlock_key_t key;

	if (strcmp(u, "none") == 0) {
		u = "";
	} else if (strlen(u) >= URL_MAX || parse_url(u, &t) != 0) {
		return -EINVAL;
	}
	key = k_spin_lock(&lock);
	strcpy(url, u);
	url_changed = url_save = true;
	k_spin_unlock(&lock, key);
	k_sem_give(&wake);
	return 0;
}

int homepower_get_url(char *out, size_t len)
{
	k_spinlock_key_t key = k_spin_lock(&lock);
	int n = snprintf(out, len, "url=%s\n", url[0] ? url : "none");

	k_spin_unlock(&lock, key);
	return n;
}

static void load_cfg(void)
{
	char buf[URL_MAX + 16] = {0};

	if (sdcard_mount() != 0 || sdcard_load(CFG_PATH, buf, sizeof(buf) - 1) <= 0) {
		return;
	}
	if (strncmp(buf, "url=", 4) == 0) {
		buf[4 + strcspn(buf + 4, " \r\n")] = '\0';
		k_spinlock_key_t key = k_spin_lock(&lock);

		if (!url_changed) { /* a ctl command since boot wins */
			strncpy(url, buf + 4, URL_MAX - 1);
			url_changed = true;
		}
		k_spin_unlock(&lock, key);
	}
}

static void save_cfg(const char *u)
{
	char line[URL_MAX + 8];
	int len = snprintf(line, sizeof(line), "url=%s\n", u);

	if (sdcard_mount() != 0 || sdcard_save(CFG_PATH, line, len) != 0) {
		LOG_WRN("no card: the monitor's address is kept until power-off");
	}
}

/* ---- HTTP ---------------------------------------------------------------- */

static int64_t days_from_civil(int y, int m, int d)
{
	y -= m <= 2;
	int era = (y >= 0 ? y : y - 399) / 400;
	int yoe = y - era * 400;
	int doy = (153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1;
	int doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;

	return (int64_t)era * 146097 + doe - 719468;
}

static int64_t epoch(int y, int mo, int d, int h, int mi, int s)
{
	return days_from_civil(y, mo, d) * 86400 + h * 3600 + mi * 60 + s;
}

/* "Thu, 19 Nov 2026 22:54:35 GMT" */
static int64_t parse_http_date(const char *v)
{
	static const char months[] = "JanFebMarAprMayJunJulAugSepOctNovDec";
	int d, y, h, mi, s;
	char mon[4] = "";

	if (sscanf(v, "%*[^,], %d %3s %d %d:%d:%d", &d, mon, &y, &h, &mi, &s) != 6) {
		return -1;
	}
	const char *m = strstr(months, mon);

	return m && mon[0] ? epoch(y, (m - months) / 3 + 1, d, h, mi, s) : -1;
}

/* "2026-11-19T22:54:22.663431+00:00" */
static int64_t parse_iso(const char *v)
{
	int y, mo, d, h, mi, s, oh = 0, om = 0;

	if (sscanf(v, "%d-%d-%dT%d:%d:%d", &y, &mo, &d, &h, &mi, &s) != 6) {
		return -1;
	}
	const char *p = v + 19;

	if (*p == '.') {
		while (isdigit((unsigned char)*++p)) {
		}
	}
	int64_t t = epoch(y, mo, d, h, mi, s);

	if ((*p == '+' || *p == '-') && sscanf(p + 1, "%d:%d", &oh, &om) == 2) {
		t -= (*p == '+' ? 1 : -1) * (oh * 3600 + om * 60);
	}
	return t;
}

static const char *header(char *hdr, const char *name)
{
	size_t n = strlen(name);

	for (char *p = strstr(hdr, "\r\n"); p; p = strstr(p + 2, "\r\n")) {
		if (strncasecmp(p + 2, name, n) == 0 && p[2 + n] == ':') {
			const char *v = p + 3 + n;

			while (*v == ' ') {
				v++;
			}
			return v;
		}
	}
	return NULL;
}

/* GET prefix + path. On success the body, NUL-terminated, is in rx; returns
 * its length and the server's clock (epoch s, or -1). */
static int http_get(const struct target *t, const struct sockaddr_in *addr, const char *path,
		    const char **body, int64_t *date_s)
{
	char req[224];
	char host[72];
	uint32_t t0 = k_uptime_get_32();
	int err = 0;
	size_t got = 0;

	if (t->port == 80) {
		snprintf(host, sizeof(host), "%s", t->host);
	} else {
		snprintf(host, sizeof(host), "%s:%u", t->host, t->port);
	}
	int len = snprintf(req, sizeof(req),
			   "GET %s%s HTTP/1.0\r\nHost: %s\r\nUser-Agent: n6demo\r\n"
			   "Accept: application/json\r\nConnection: close\r\n\r\n",
			   t->prefix, path, host);
	int s = zsock_socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);

	if (s < 0) {
		return -errno;
	}
	struct zsock_timeval tv = {.tv_sec = IO_TIMEOUT_S};

	zsock_setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
	zsock_setsockopt(s, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));
	if (zsock_connect(s, (const struct sockaddr *)addr, sizeof(*addr)) < 0) {
		err = -errno;
		zsock_close(s);
		return err;
	}
	for (int off = 0; off < len;) {
		ssize_t n = zsock_send(s, req + off, len - off, 0);

		if (n <= 0) {
			err = n < 0 ? -errno : -EIO;
			break;
		}
		off += n;
	}
	while (err == 0) {
		ssize_t n = zsock_recv(s, rx + got, RX_CAP - got, 0);

		if (n < 0) {
			err = -errno;
		} else if (n == 0) {
			break;
		} else if ((got += n) == RX_CAP) {
			err = -EMSGSIZE;
		}
	}
	zsock_close(s);
	last_ms = k_uptime_get_32() - t0;
	worst_ms = MAX(worst_ms, last_ms);
	last_bytes = got;
	if (err) {
		return err;
	}
	rx[got] = '\0';
	if (got < 12 || strncmp(rx, "HTTP/1.", 7) != 0) {
		return -EBADMSG;
	}
	last_http = atoi(rx + 9);
	if (last_http != 200) {
		return -EPROTO;
	}
	char *end = strstr(rx, "\r\n\r\n");

	if (end == NULL) {
		return -EBADMSG;
	}
	*end = '\0';
	const char *cl = header(rx, "content-length");
	const char *date = header(rx, "date");
	size_t blen = got - (end + 4 - rx);

	if (cl && (size_t)atoi(cl) != blen) {
		return -EBADMSG; /* cut short */
	}
	*date_s = date ? parse_http_date(date) : -1;
	*body = end + 4;
	return blen;
}

/* ---- JSON: just enough to pick members out of the monitor's answers ------ */

static const char *ws(const char *p, const char *e)
{
	while (p < e && (*p == ' ' || *p == '\n' || *p == '\r' || *p == '\t')) {
		p++;
	}
	return p;
}

static const char *skip_str(const char *p, const char *e)
{
	for (p++; p < e; p++) {
		if (*p == '\\') {
			p++;
		} else if (*p == '"') {
			return p + 1;
		}
	}
	return e;
}

static const char *skip_val(const char *p, const char *e)
{
	p = ws(p, e);
	if (p < e && *p == '"') {
		return skip_str(p, e);
	}
	if (p < e && (*p == '{' || *p == '[')) {
		int depth = 0;

		while (p < e) {
			if (*p == '"') {
				p = skip_str(p, e);
				continue;
			}
			if (*p == '{' || *p == '[') {
				depth++;
			} else if ((*p == '}' || *p == ']') && --depth == 0) {
				return p + 1;
			}
			p++;
		}
		return e;
	}
	while (p < e && !strchr(",}] \t\r\n", *p)) {
		p++;
	}
	return p;
}

/* The value of member `key` of the object at p, or NULL. */
static const char *jget(const char *p, const char *e, const char *key)
{
	size_t kl = strlen(key);

	if (p == NULL) {
		return NULL;
	}
	p = ws(p, e);
	if (p >= e || *p != '{') {
		return NULL;
	}
	p++;
	for (;;) {
		p = ws(p, e);
		if (p >= e || *p != '"') {
			return NULL;
		}
		const char *k = p + 1;
		const char *ke = skip_str(p, e);
		bool match = (size_t)(ke - 1 - k) == kl && memcmp(k, key, kl) == 0;

		p = ws(ke, e);
		if (p >= e || *p != ':') {
			return NULL;
		}
		p = ws(p + 1, e);
		if (match) {
			return p;
		}
		p = ws(skip_val(p, e), e);
		if (p >= e || *p != ',') {
			return NULL;
		}
		p++;
	}
}

/* A number (not null, not a string). */
static bool jnum(const char *v, double *out)
{
	char *end;

	if (v == NULL) {
		return false;
	}
	double d = strtod(v, &end);

	if (end == v) {
		return false;
	}
	*out = d;
	return true;
}

static float jf(const char *obj, const char *e, const char *key, bool *ok)
{
	double d;

	if (jnum(jget(obj, e, key), &d)) {
		return (float)d;
	}
	if (ok) {
		*ok = false;
	}
	return 0.0f;
}

/* [[t_ms, mean, min, max], ...]; a bucket without data (null) is left out */
static int parse_series(const char *v, const char *e, struct hp_series *s)
{
	int n = 0;

	v = ws(v, e);
	if (v >= e || *v != '[') {
		return -EBADMSG;
	}
	v++;
	for (;;) {
		v = ws(v, e);
		if (v >= e || *v == ']') {
			break;
		}
		if (*v == ',') {
			v++;
			continue;
		}
		if (*v != '[') {
			return -EBADMSG;
		}
		const char *pe = skip_val(v, e);
		const char *q = v + 1;
		double f[4];
		int k = 0;

		while (q < pe && k < 4) {
			char *end;

			q = ws(q, pe);
			if (*q == ']') {
				break;
			}
			f[k] = strtod(q, &end);
			if (end == q) {
				f[k] = NAN;
				end = (char *)skip_val(q, pe);
			}
			k++;
			q = ws(end, pe);
			if (q < pe && *q == ',') {
				q++;
			}
		}
		v = pe;
		if (k >= 2 && !isnan(f[0]) && !isnan(f[1]) && n < HP_MAX_POINTS) {
			s->t_s[n] = (int32_t)(((int64_t)f[0] - s->start_ms) / 1000);
			s->avg[n] = (float)f[1];
			s->lo[n] = k > 2 && !isnan(f[2]) ? (float)f[2] : (float)f[1];
			s->hi[n] = k > 3 && !isnan(f[3]) ? (float)f[3] : (float)f[1];
			n++;
		}
	}
	s->n = n;
	return 0;
}

/* ---- the two requests ---------------------------------------------------- */

static int fetch_stats(const struct target *t, const struct sockaddr_in *addr)
{
	const char *b;
	int64_t date_s;
	int len = http_get(t, addr, "/api/stats", &b, &date_s);

	if (len < 0) {
		return len;
	}
	const char *e = b + len;
	struct hp_stats st = {0};
	bool ok = true;
	double d;

	st.now_w = jf(b, e, "current_power", &ok);
	st.min_w = jf(b, e, "min_24h", &ok);
	st.avg_w = jf(b, e, "avg_24h", &ok);
	st.max_w = jf(b, e, "max_24h", &ok);
	st.cost_24h = jf(b, e, "cost_24h", &ok);
	if (!ok) {
		return -EBADMSG;
	}
	const char *lu = jget(b, e, "last_update");
	int64_t lu_s = lu && *lu == '"' ? parse_iso(lu + 1) : -1;

	st.age_s = date_s > 0 && lu_s > 0 ? (int32_t)MAX(date_s - lu_s, 0) : -1;

	const char *bp = jget(b, e, "billing_period");

	if (bp) {
		bool bok = true;

		st.cycle_days = (int)jf(bp, e, "cycle_days", &bok);
		st.days = (int)jf(bp, e, "days", &bok);
		st.energy_kwh = jf(bp, e, "energy_kwh", &bok);
		st.bill_total = jf(jget(bp, e, "tiered_cost"), e, "total_cost", &bok);
		st.have_bill = bok;

		const char *tr = jget(bp, e, "trend");
		double proj;

		if (jnum(jget(tr, e, "slope_per_day"), &d) && jnum(jget(tr, e, "projected_total"), &proj)) {
			st.slope_per_day = (float)d;
			st.projected = (float)proj;
			st.have_trend = true;
		}
	}
	st.fetched_ms = k_uptime_get_32();

	k_spinlock_key_t key = k_spin_lock(&lock);

	st.seq = stats.seq + 1;
	stats = st;
	k_spin_unlock(&lock, key);
	return 0;
}

static int fetch_dash(const struct target *t, const struct sockaddr_in *addr, int r,
		      struct hp_series *const pair[2], volatile int *front, uint32_t *seq)
{
	char path[40];
	const char *b;
	int64_t date_s;

	snprintf(path, sizeof(path), "/api/dashboard?range=%s", range_names[r]);
	int len = http_get(t, addr, path, &b, &date_s);

	if (len < 0) {
		return len;
	}
	const char *e = b + len;
	struct hp_series *s = pair[*front == 0 ? 1 : 0];
	bool ok = true;
	double start, end;

	if (!jnum(jget(b, e, "start"), &start) || !jnum(jget(b, e, "end"), &end)) {
		return -EBADMSG;
	}
	s->range = r;
	s->start_ms = (int64_t)start;
	s->end_ms = (int64_t)end;
	const char *pw = jget(b, e, "power");

	s->min_w = jf(pw, e, "min", &ok);
	s->mean_w = jf(pw, e, "mean", &ok);
	s->max_w = jf(pw, e, "max", &ok);
	s->energy_wh = jf(b, e, "energy_wh", NULL);
	s->meter_kwh = jf(b, e, "meter_kwh", NULL);
	s->rate = jf(b, e, "price_per_kwh", NULL);
	s->cost_hour = jf(b, e, "cost_per_hour", NULL);
	s->est_cost = jf(b, e, "estimated_cost", NULL);
	if (parse_series(jget(b, e, "series"), e, s) != 0) {
		return -EBADMSG;
	}
	if (!ok && s->n > 0) {
		return -EBADMSG;
	}
	s->seq = ++*seq;
	*front = s == pair[0] ? 0 : 1;
	return 0;
}

/* ---- the thread ---------------------------------------------------------- */

static const char *why(int err)
{
	switch (err) {
	case -ETIMEDOUT:
	case -EAGAIN:
		return "the monitor did not answer in time";
	case -ECONNREFUSED:
		return "the monitor refused the connection";
	case -ENETUNREACH:
	case -EHOSTUNREACH:
		return "the network does not reach the monitor";
	case -EPROTO:
		return "the monitor answered with an error";
	case -EMSGSIZE:
		return "the answer was too large";
	case -EBADMSG:
		return "the answer was not what was expected";
	default:
		return "the request failed";
	}
}

static void forget(void)
{
	k_spinlock_key_t key = k_spin_lock(&lock);

	memset(&stats, 0, sizeof(stats));
	ser_front = -1;
	k_spin_unlock(&lock, key);
}

static void hp_thread(void *p1, void *p2, void *p3)
{
	struct target tgt;
	struct sockaddr_in addr;
	bool have_tgt = false, have_addr = false;
	uint32_t resolved_ms = 0, next_stats = 0, next_dash = 0, stats_ok_ms = 0;
	uint32_t ser_seq = 0;
	int dash_range = -1, fails = 0;

	ARG_UNUSED(p1);
	ARG_UNUSED(p2);
	ARG_UNUSED(p3);
	net_wait_ip();
	load_cfg();
	for (;;) {
		uint32_t now = k_uptime_get_32();
		char u[URL_MAX];
		bool changed, save;
		k_spinlock_key_t key = k_spin_lock(&lock);

		changed = url_changed;
		save = url_save;
		url_changed = url_save = false;
		strcpy(u, url);
		k_spin_unlock(&lock, key);

		if (changed) {
			have_tgt = u[0] && parse_url(u, &tgt) == 0;
			have_addr = false;
			online = false;
			fails = 0;
			dash_range = -1;
			next_stats = next_dash = now;
			forget();
			if (save) {
				save_cfg(u);
			}
		}
		if (!have_tgt) {
			state = u[0] ? "the monitor's address is not usable" : "no monitor address set";
			online = false;
			k_sem_take(&wake, K_FOREVER);
			continue;
		}
		if (kick) { /* the panel or the Home tile came on screen */
			kick = false;
			if (now - stats_ok_ms > POLL_SHOWN_MS) {
				next_stats = now;
			}
			next_dash = now;
		}
		if (!have_addr || now - resolved_ms > RESOLVE_MS) {
			struct zsock_addrinfo hints = {.ai_family = AF_INET, .ai_socktype = SOCK_STREAM};
			struct zsock_addrinfo *res = NULL;
			char port[8];

			snprintf(port, sizeof(port), "%u", tgt.port);
			if (zsock_getaddrinfo(tgt.host, port, &hints, &res) != 0 || res == NULL) {
				state = "the monitor's name does not resolve";
				n_fail++;
				if (++fails >= 2) {
					online = false;
				}
				k_sem_take(&wake, K_MSEC(RETRY_MS));
				continue;
			}
			memcpy(&addr, res->ai_addr, sizeof(addr));
			zsock_freeaddrinfo(res);
			have_addr = true;
			resolved_ms = now;
		}

		bool shown = visible;
		bool live = shown || glance; /* the gauge is on screen: the panel's or the tile's */
		int err = 0, requests = 0;

		if ((int32_t)(now - next_stats) >= 0) {
			err = fetch_stats(&tgt, &addr);
			requests++;
			if (err == 0) {
				stats_ok_ms = now;
			}
			next_stats = now + (err ? RETRY_MS : live ? POLL_SHOWN_MS : POLL_HIDDEN_MS);
		}
		/* the chart only once the monitor answers */
		bool answers = err == 0 && (online || stats_ok_ms == now);
		int want = range;

		if (answers && err == 0 && shown && (want != dash_range || (int32_t)(now - next_dash) >= 0)) {
			err = fetch_dash(&tgt, &addr, want, ser, &ser_front, &ser_seq);
			requests++;
			dash_range = want;
			next_dash = now + (err ? RETRY_MS : want <= HP_24H ? POLL_SHOWN_MS : POLL_LONG_MS);
		}
		bool was = online;

		if (requests > 0 && err == 0) {
			fails = 0;
			online = true;
			state = "online";
			n_ok += requests;
		} else if (err != 0) {
			n_ok += requests - 1;
			n_fail++;
			last_err = err;
			state = why(err);
			have_addr = false; /* the name may point elsewhere now */
			if (++fails >= 2) {
				online = false;
			}
		}
		if (online != was) {
			if (online) {
				LOG_INF("energy monitor online");
			} else {
				LOG_INF("energy monitor offline: %s (%d)", (const char *)state, err);
			}
		}

		uint32_t next = next_stats;

		if (online && shown && (int32_t)(next_dash - next) < 0) {
			next = next_dash;
		}
		int32_t wait = (int32_t)(next - k_uptime_get_32());

		if (wait > 0) {
			k_sem_take(&wake, K_MSEC(wait));
		}
	}
}

void homepower_start(void)
{
	k_thread_stack_t *stack = psram_alloc(K_KERNEL_STACK_LEN(STACK_SIZE), 64);

	rx = psram_alloc(RX_CAP + 1, 32);
	for (int i = 0; i < 2; i++) {
		ser[i] = psram_alloc(sizeof(struct hp_series), 32);
	}
	if (stack == NULL || rx == NULL || ser[1] == NULL) {
		state = "no memory";
		return;
	}
	k_thread_create(&thread, stack, STACK_SIZE, hp_thread, NULL, NULL, NULL, PRIO, 0, K_NO_WAIT);
	k_thread_name_set(&thread, "homepower");
	started = true;
}

/* ---- readers ------------------------------------------------------------- */

bool homepower_online(void)
{
	return online;
}

bool homepower_configured(void)
{
	return url[0] != '\0';
}

const char *homepower_state(void)
{
	return (const char *)state;
}

void homepower_get_stats(struct hp_stats *out)
{
	k_spinlock_key_t key = k_spin_lock(&lock);

	*out = stats;
	k_spin_unlock(&lock, key);
}

const struct hp_series *homepower_series(void)
{
	int f = ser_front;

	return f < 0 ? NULL : ser[f];
}

void homepower_set_visible(bool on)
{
	visible = on;
	if (on) {
		kick = true;
		k_sem_give(&wake);
	}
}

void homepower_set_glance(bool on)
{
	if (on && !glance) {
		kick = true;
		k_sem_give(&wake);
	}
	glance = on;
}

void homepower_set_range(int r)
{
	if (r >= 0 && r < HP_RANGES && r != range) {
		range = r;
		k_sem_give(&wake);
	}
}

int homepower_range(void)
{
	return range;
}

int homepower_status(char *out, size_t len)
{
	struct hp_stats st;
	const struct hp_series *s = homepower_series();

	homepower_get_stats(&st);
	return snprintf(out, len,
			"configured=%d online=%d state=\"%s\" visible=%d glance=%d range=%s ok=%u fail=%u "
			"last_err=%d http=%d last_ms=%u worst_ms=%u bytes=%u seq=%u age_s=%d since_s=%u now_w=%d "
			"min_w=%d avg_w=%d max_w=%d bill_c=%d proj_c=%d series=%s/%d\n",
			homepower_configured(), online, homepower_state(), visible, glance, range_names[range],
			n_ok, n_fail, last_err, last_http, last_ms, worst_ms, last_bytes, st.seq, st.age_s,
			st.seq ? (k_uptime_get_32() - st.fetched_ms) / 1000 : 0, (int)st.now_w, (int)st.min_w,
			(int)st.avg_w, (int)st.max_w, (int)(st.bill_total * 100), (int)(st.projected * 100),
			s ? range_names[s->range] : "-", s ? s->n : 0);
}
