/*
 * The NPU. See npu.h.
 *
 * Every run happens on the nn thread: the runtime's software layers run on
 * the CPU and need more stack than the threads that ask, and an NPU that
 * never finishes must not take the control port down with it (the asker
 * times out, the thread stays busy and later requests are refused).
 *
 * Caches: the input buffer is written by the CPU and read by the NPU, so it
 * is flushed before a run. The outputs are flushed and invalidated before
 * and after: no dirty line of the CPU's can land on what the NPU wrote, and
 * what the CPU's own software layers wrote reaches memory.
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#include <stdio.h>
#include <string.h>

#include <zephyr/cache.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/sys/crc.h>

#include "hands.h"
#include "media/camera.h"
#include "npu.h"
#include "platform/gfx.h"
#include "platform/psram.h"

LOG_MODULE_REGISTER(npu, LOG_LEVEL_INF);

static struct npu_status status;

#if defined(APP_HAS_NPU)

#include "ll_aton_osal.h"
#include "ll_aton_rt_user_api.h"
#include "nn_weights.h"
#include "npu_cache.h"
#include "stai.h"
#include "stai_hand_landmark.h"
#include "stai_palm_detector.h"

BUILD_ASSERT(NN_ACT_BASE == PSRAM_NPU_BASE, "psram.h and tools/nn.py place the NPU's memory differently");
BUILD_ASSERT(NN_NPU_RAM_BASE == GFX_FB_B && NN_NPU_RAM_END == GFX_FB_B + GFX_FB_BYTES,
	     "the networks' working memory is not frame buffer B");

#if defined(APP_NN_TEST_INPUT)
/* build-nn/test: a MediaPipe test picture letterboxed for the palm detector, and
 * the hand cut out of it for the landmark network (tools/nn.py testimg) */
static const uint8_t test_palm[] = {
#include "nn_palm_in.inc"
};
static const uint8_t test_hand[] = {
#include "nn_hand_in.inc"
};
BUILD_ASSERT(sizeof(test_palm) == STAI_PALM_DETECTOR_IN_1_SIZE_BYTES, "test input does not fit the network");
BUILD_ASSERT(sizeof(test_hand) == STAI_HAND_LANDMARK_IN_1_SIZE_BYTES, "test input does not fit the network");
static const uint8_t *const test_input[NPU_NETS] = {test_palm, test_hand};
#endif

struct net {
	const char *name;
	stai_network *ctx;
	stai_return_code (*init)(stai_network *);
	stai_return_code (*get_inputs)(stai_network *, stai_ptr *, stai_size *);
	stai_return_code (*get_outputs)(stai_network *, stai_ptr *, stai_size *);
	stai_return_code (*run)(stai_network *, const stai_run_mode);
	stai_return_code (*new_inference)(stai_network *);
	stai_return_code (*set_callback)(stai_network *, const stai_event_cb, void *);
	size_t in_size;
	const uint8_t *weights;
	uint32_t weights_size, weights_crc;
	uint8_t *in;
	stai_ptr out[4];
	uint32_t out_size[4];
	stai_size n_out;
};

static uint8_t palm_ctx[STAI_PALM_DETECTOR_CONTEXT_SIZE] __aligned(8);
static uint8_t hand_ctx[STAI_HAND_LANDMARK_CONTEXT_SIZE] __aligned(8);

static struct net nets[NPU_NETS] = {
	[NPU_PALM] = {
		.name = "palm detector",
		.ctx = palm_ctx,
		.init = stai_palm_detector_init,
		.get_inputs = stai_palm_detector_get_inputs,
		.get_outputs = stai_palm_detector_get_outputs,
		.run = stai_palm_detector_run,
		.new_inference = stai_ext_palm_detector_new_inference,
		.set_callback = stai_palm_detector_set_callback,
		.in_size = STAI_PALM_DETECTOR_IN_1_SIZE_BYTES,
		.weights = (const uint8_t *)NN_PALM_DETECTOR_WEIGHTS_AT,
		.weights_size = NN_PALM_DETECTOR_WEIGHTS_SIZE,
		.weights_crc = NN_PALM_DETECTOR_WEIGHTS_CRC,
		.out_size = {STAI_PALM_DETECTOR_OUT_1_SIZE_BYTES, STAI_PALM_DETECTOR_OUT_2_SIZE_BYTES},
	},
	[NPU_HAND] = {
		.name = "hand landmarks",
		.ctx = hand_ctx,
		.init = stai_hand_landmark_init,
		.get_inputs = stai_hand_landmark_get_inputs,
		.get_outputs = stai_hand_landmark_get_outputs,
		.run = stai_hand_landmark_run,
		.new_inference = stai_ext_hand_landmark_new_inference,
		.set_callback = stai_hand_landmark_set_callback,
		.in_size = STAI_HAND_LANDMARK_IN_1_SIZE_BYTES,
		.weights = (const uint8_t *)NN_HAND_LANDMARK_WEIGHTS_AT,
		.weights_size = NN_HAND_LANDMARK_WEIGHTS_SIZE,
		.weights_crc = NN_HAND_LANDMARK_WEIGHTS_CRC,
		.out_size = {STAI_HAND_LANDMARK_OUT_1_SIZE_BYTES, STAI_HAND_LANDMARK_OUT_2_SIZE_BYTES,
			     STAI_HAND_LANDMARK_OUT_3_SIZE_BYTES, STAI_HAND_LANDMARK_OUT_4_SIZE_BYTES},
	},
};

int npu_prepare(void)
{
	if (status.ready) {
		return 0;
	}
	if (status.error) {
		return -EIO;
	}
	for (int i = 0; i < NPU_NETS; i++) {
		struct net *n = &nets[i];

		if (crc32_ieee(n->weights, n->weights_size) != n->weights_crc) {
			status.error = "weights missing from the flash (tools/nn.py flash)";
			LOG_ERR("%s: %s", n->name, status.error);
			return -ENOENT;
		}
	}
	npu_cache_enable(); /* Zephyr's driver enabled it; this gives the runtime its handle */
	if (stai_runtime_init() != STAI_SUCCESS) {
		status.error = "the NPU runtime did not start";
		return -EIO;
	}
	for (int i = 0; i < NPU_NETS; i++) {
		struct net *n = &nets[i];

		/* the input is the converter's, in the lent memory beside the NPU */
		stai_ptr in[1];
		stai_size n_in;

		if (n->init(n->ctx) != STAI_SUCCESS || n->get_outputs(n->ctx, n->out, &n->n_out) != STAI_SUCCESS ||
		    n->get_inputs(n->ctx, in, &n_in) != STAI_SUCCESS || n_in != 1) {
			status.error = "a network did not initialise";
			LOG_ERR("%s: %s", n->name, status.error);
			return -EIO;
		}
		n->in = in[0];
	}
	status.ready = true;
	LOG_INF("NPU ready: %d networks, working memory at 0x%08x", NPU_NETS, NN_ACT_BASE);
	return 0;
}

static void outputs_sync(struct net *n)
{
	for (int i = 0; i < n->n_out; i++) {
		sys_cache_data_flush_and_invd_range(n->out[i], n->out_size[i]);
	}
}

const char *npu_error(void)
{
	return status.error;
}

const uint8_t *npu_test_input(enum npu_net net)
{
#if defined(APP_NN_TEST_INPUT)
	return test_input[net];
#else
	return NULL;
#endif
}

uint8_t *npu_input(enum npu_net net)
{
	return nets[net].in;
}

const float *npu_output(enum npu_net net, int i, uint32_t *count)
{
	if (i >= nets[net].n_out) {
		return NULL;
	}
	*count = nets[net].out_size[i] / sizeof(float);
	return (const float *)nets[net].out[i];
}

/* One inference on whatever is in the network's input buffer. */
int npu_infer(enum npu_net which, uint32_t *us)
{
	struct net *n = &nets[which];
	stai_return_code r;

	sys_cache_data_flush_range(n->in, n->in_size);
	outputs_sync(n);
	uint32_t t0 = k_cycle_get_32();

	do {
		r = n->run(n->ctx, STAI_MODE_ASYNC);
		if (r == STAI_RUNNING_WFE) {
			LL_ATON_OSAL_WFE();
		}
	} while (r == STAI_RUNNING_WFE || r == STAI_RUNNING_NO_WFE);
	uint32_t t = (uint32_t)k_cyc_to_us_floor64(k_cycle_get_32() - t0);

	outputs_sync(n);
	if (n->new_inference(n->ctx) != STAI_SUCCESS) {
		return -EIO;
	}
	status.runs[which]++;
	status.last_us[which] = t;
	if (status.best_us[which] == 0 || t < status.best_us[which]) {
		status.best_us[which] = t;
	}
	status.worst_us[which] = MAX(status.worst_us[which], t);
	*us = t;
	return 0;
}

/* --- epoch trace ----------------------------------------------------------
 * "nn trace [net]": one run with the runtime's epoch callback recording the
 * start and end of every epoch block, with the converter's estimate for it.
 * The log is in PSRAM; tools/nn.py trace reads it with "dump".
 */
struct trace_ev {
	uint32_t cycles; /* k_cycle_get_32() */
	uint16_t type;   /* LL_ATON_RT_Callbacktype_t */
	uint16_t flags;  /* EpochBlock_Flags_t */
	int16_t ep0, ep1;
	uint32_t est_npu, est_tot; /* converter's estimate, NPU cycles without / with memory waits */
};
#define TRACE_MAX 512

static struct trace_ev *trace_log;
static int trace_n;

/* the stai layer passes the runtime's epoch events through: type and epoch block */
static void trace_cb(void *cookie, const stai_event_type type, const void *payload)
{
	const LL_ATON_RT_EpochBlockItem_t *eb = payload;

	ARG_UNUSED(cookie);
	if (trace_n >= TRACE_MAX || eb == NULL) {
		return;
	}
	struct trace_ev *e = &trace_log[trace_n++];

	e->cycles = k_cycle_get_32();
	e->type = type;
	e->flags = eb->flags;
	e->ep0 = eb->epoch_num;
	e->ep1 = eb->last_epoch_num;
	e->est_npu = (uint32_t)eb->estimated_npu_cycles;
	e->est_tot = (uint32_t)eb->estimated_tot_cycles;
}

/* --- requests from other threads ---------------------------------------- */

enum job_kind { JOB_TEST, JOB_BENCH, JOB_TRACE };

static struct {
	enum job_kind kind;
	int net, count;
	int ret;
	uint32_t us, min_us, max_us, sum_us, in_crc;
} job;

static K_SEM_DEFINE(job_go, 0, 1);
static K_SEM_DEFINE(job_done, 0, 1);
static atomic_t busy;

static void do_job(void)
{
	job.ret = npu_prepare();
	if (job.ret != 0) {
		return;
	}
	if (job.kind == JOB_TEST) {
#if defined(APP_NN_TEST_INPUT)
		struct net *n = &nets[job.net];

		memcpy(n->in, test_input[job.net], n->in_size);
		/* before the run: the network reuses its input's memory as it goes */
		job.in_crc = crc32_ieee(n->in, n->in_size);
		job.ret = npu_infer(job.net, &job.us);
#else
		job.ret = -ENOENT;
#endif
		return;
	}
	if (job.kind == JOB_TRACE) {
		struct net *n = &nets[job.net];

		if (trace_log == NULL) {
			trace_log = psram_alloc(TRACE_MAX * sizeof(*trace_log), 64);
		}
		if (trace_log == NULL) {
			job.ret = -ENOMEM;
			return;
		}
		memset(n->in, 0x80, n->in_size);
		trace_n = 0;
		n->set_callback(n->ctx, trace_cb, NULL);
		job.ret = npu_infer(job.net, &job.us);
		n->set_callback(n->ctx, NULL, NULL);
		sys_cache_data_flush_range(trace_log, trace_n * sizeof(*trace_log));
		return;
	}
	job.min_us = UINT32_MAX, job.max_us = 0, job.sum_us = 0;
	memset(nets[job.net].in, 0x80, nets[job.net].in_size);
	for (int i = 0; i < job.count && job.ret == 0; i++) {
		uint32_t t;

		job.ret = npu_infer(job.net, &t);
		job.min_us = MIN(job.min_us, t);
		job.max_us = MAX(job.max_us, t);
		job.sum_us += t;
	}
}

static void nn_thread(void *p1, void *p2, void *p3)
{
	for (;;) {
		/* requests from the control port first, between camera frames */
		if (k_sem_take(&job_go, hands_enabled() ? K_NO_WAIT : K_MSEC(100)) == 0) {
			if (gfx_borrow_begin()) {
				do_job();
				gfx_borrow_end();
			} else {
				job.ret = -EAGAIN;
			}
			k_sem_give(&job_done);
			atomic_clear(&busy);
			continue;
		}
		if (hands_enabled()) {
			hands_step();
		}
	}
}
/* Below the UI, the player and the camera: the NPU does the work, this thread mostly waits. */
K_THREAD_DEFINE(nn_tid, 8192, nn_thread, NULL, NULL, NULL, 5, 0, 0);

static int submit(int seconds)
{
	if (!atomic_cas(&busy, 0, 1)) {
		return -EBUSY;
	}
	k_sem_reset(&job_done);
	k_sem_give(&job_go);
	if (k_sem_take(&job_done, K_SECONDS(seconds)) != 0) {
		return -ETIMEDOUT; /* still busy: later requests are refused */
	}
	return job.ret;
}

static int reply_error(int r, char *out, size_t out_len)
{
	if (r == -EAGAIN) {
		return snprintf(out, out_len, "err %d the NPU's memory is lent only while the Camera screen shows\n", r);
	}
	return snprintf(out, out_len, "err %d %s\n", r, status.error ? status.error : "");
}

int npu_ctl(const char *line, char *out, size_t out_len)
{
	char sub[12] = {0};
	int a1 = -1, a2 = -1;

	sscanf(line, "%*s %11s %d %d", sub, &a1, &a2);
	if (!strcmp(sub, "test")) {
		/* "nn test [net]": the network on its built-in test input; the outputs stay
		 * where they are for the host to read with "dump" (tools/nn.py check) */
		job.kind = JOB_TEST;
		job.net = CLAMP(a1 < 0 ? NPU_PALM : a1, 0, NPU_NETS - 1);
		int r = submit(10);

		if (r != 0) {
			return reply_error(r, out, out_len);
		}
		struct net *n = &nets[job.net];
		int len = snprintf(out, out_len, "ok net=%d us=%u in_crc=%08x", job.net, job.us, job.in_crc);

		for (int i = 0; i < n->n_out; i++) {
			len += snprintf(out + len, out_len - len, " out%d=0x%08x n%d=%u", i,
					(unsigned int)(uintptr_t)n->out[i], i, n->out_size[i] / 4);
		}
		return len + snprintf(out + len, out_len - len, "\n");
	}
	if (!strcmp(sub, "hands")) {
		/* "nn hands": the tracker's state; "nn hands 0" pauses it, "1" resumes,
		 * "2" runs it on the built-in test picture instead of the camera */
		struct hands_result h;

		if (a1 >= 0) {
			hands_simulate(a1 == 2);
			hands_enable(a1 != 0);
		}
		hands_get(&h);
		return snprintf(out, out_len,
				"ok on=%d valid=%d gesture=%d fingers=%d presence_pct=%d fps_x10=%u palm_us=%u "
				"hand_us=%u prep_us=%u searches=%u palm_pct=%d palms=%u confirmed=%u lost=%u "
				"right_pct=%d lent=%d in0=0x%08x in1=0x%08x error=%s\n",
				hands_enabled(), h.valid, h.gesture, h.fingers, (int)(h.presence * 100), h.fps_x10,
				h.palm_us, h.hand_us, h.prep_us, h.searches, (int)(h.palm_score * 100), h.palms,
				h.confirmed, h.lost, (int)(h.handedness * 100), gfx_back_buffer_lent(),
				(unsigned int)(uintptr_t)nets[NPU_PALM].in, (unsigned int)(uintptr_t)nets[NPU_HAND].in,
				h.error ? h.error : "-");
	}
	if (!strcmp(sub, "thr")) {
		/* "nn thr [palm_pct presence_pct]": the tracker's two thresholds */
		float palm, presence;

		if (a1 >= 0 && a2 >= 0) {
			hands_set_thresholds(a1 / 100.0f, a2 / 100.0f);
		}
		hands_get_thresholds(&palm, &presence);
		return snprintf(out, out_len, "ok palm_pct=%d presence_pct=%d\n", (int)(palm * 100 + 0.5f),
				(int)(presence * 100 + 0.5f));
	}
	if (!strcmp(sub, "grab")) {
		/* "nn grab 1" asks for the next frame the tracker works on to be copied
		 * (camera picture and network inputs); "nn grab" says where they are */
		struct hands_grab g;

		if (a1 > 0) {
			hands_grab_arm();
		}
		hands_grab_get(&g);
		return snprintf(out, out_len,
				"ok n=%u frame=0x%08x palm_in=0x%08x hand_in=0x%08x has_palm=%d has_hand=%d "
				"palm_pct=%d presence_pct=%d cx=%d cy=%d size=%d rot_mdeg=%d w=%d h=%d\n",
				g.n, (unsigned int)(uintptr_t)g.frame, (unsigned int)(uintptr_t)g.palm_in,
				(unsigned int)(uintptr_t)g.hand_in, g.has_palm, g.has_hand, (int)(g.palm_score * 100),
				(int)(g.presence * 100), (int)g.cx, (int)g.cy, (int)g.size,
				(int)(g.rot * 57295.78f), CAM_W, CAM_H);
	}
	if (!strcmp(sub, "trace")) {
		job.kind = JOB_TRACE;
		job.net = CLAMP(a1 < 0 ? NPU_PALM : a1, 0, NPU_NETS - 1);
		int r = submit(10);

		if (r != 0) {
			return reply_error(r, out, out_len);
		}
		return snprintf(out, out_len, "ok net=%d us=%u log=0x%08x events=%d size=%u hz=%u\n", job.net, job.us,
				(unsigned int)(uintptr_t)trace_log, trace_n, (unsigned int)sizeof(struct trace_ev),
				sys_clock_hw_cycles_per_sec());
	}
	if (!strcmp(sub, "bench")) {
		job.kind = JOB_BENCH;
		job.count = CLAMP(a1 < 0 ? 10 : a1, 1, 200);
		job.net = CLAMP(a2 < 0 ? NPU_PALM : a2, 0, NPU_NETS - 1);
		int r = submit(30);

		if (r != 0) {
			return reply_error(r, out, out_len);
		}
		return snprintf(out, out_len, "ok net=%s runs=%d min_us=%u avg_us=%u max_us=%u\n",
				nets[job.net].name, job.count, job.min_us, job.sum_us / job.count, job.max_us);
	}
	size_t unused = 0;

	k_thread_stack_space_get(nn_tid, &unused);
	int len = snprintf(out, out_len, "present=1 ready=%d busy=%d stack_unused=%u error=%s", status.ready,
			   (int)atomic_get(&busy), (unsigned int)unused, status.error ? status.error : "-");

	for (int i = 0; i < NPU_NETS; i++) {
		len += snprintf(out + len, out_len - len, " | %s runs=%u last_us=%u best_us=%u worst_us=%u",
				nets[i].name, status.runs[i], status.last_us[i], status.best_us[i],
				status.worst_us[i]);
	}
	return len + snprintf(out + len, out_len - len, "\n");
}

void npu_get_status(struct npu_status *st)
{
	*st = status;
	st->present = true;
}

#else /* !APP_HAS_NPU */

int npu_ctl(const char *line, char *out, size_t out_len)
{
	ARG_UNUSED(line);
	return snprintf(out, out_len, "err no NPU in this build (tools/nn.py fetch, generate)\n");
}

void npu_get_status(struct npu_status *st)
{
	*st = status;
	st->error = "not in this build";
}

int npu_prepare(void)
{
	return -ENODEV;
}

const char *npu_error(void)
{
	return "no NPU in this build";
}

uint8_t *npu_input(enum npu_net net)
{
	return NULL;
}

const float *npu_output(enum npu_net net, int i, uint32_t *count)
{
	return NULL;
}

int npu_infer(enum npu_net net, uint32_t *us)
{
	return -ENODEV;
}

const uint8_t *npu_test_input(enum npu_net net)
{
	return NULL;
}

#endif
