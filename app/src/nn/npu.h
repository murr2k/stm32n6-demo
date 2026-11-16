/*
 * The NPU: ST's runtime and the networks tools/nn.py converts.
 *
 * Present only when the build found build-nn/ and the ST Edge AI install
 * (APP_HAS_NPU); otherwise the status says so and nothing runs. Networks run
 * one at a time on the nn thread: their working memory is one shared block at
 * the top of PSRAM (PSRAM_NPU_BASE), their weights are in the flash where
 * `nn.py flash` wrote them and are checked against the converter's CRC before
 * the first run.
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#ifndef NN_NPU_H
#define NN_NPU_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

enum npu_net {
	NPU_PALM,     /* palm detector, 192 x 192 RGB in */
	NPU_HAND,     /* hand landmarks, 224 x 224 RGB in */
	NPU_NETS,
};

struct npu_status {
	bool present;      /* runtime and networks built in */
	bool ready;        /* initialised, weights checked */
	const char *error; /* why it is not ready, NULL if it is or has not tried yet */
	uint32_t runs[NPU_NETS];
	uint32_t last_us[NPU_NETS], best_us[NPU_NETS], worst_us[NPU_NETS];
};

void npu_get_status(struct npu_status *st);

/* Control port: "nn" (status), "nn test [net]", "nn bench [runs] [net]", "nn trace [net]".
 * They need the NPU's memory, which the display lends only while the Camera
 * screen is shown. */
int npu_ctl(const char *line, char *out, size_t out_len);

/* For the pipelines on the nn thread (hands.c), only between gfx_borrow_begin()
 * and gfx_borrow_end(): */
int npu_prepare(void);       /* once: weights checked, runtime and networks started */
const char *npu_error(void); /* why npu_prepare() failed */
uint8_t *npu_input(enum npu_net net);
const float *npu_output(enum npu_net net, int i, uint32_t *count);
int npu_infer(enum npu_net net, uint32_t *us);
/* The built-in test input of a network (tools/nn.py testimg), or NULL. */
const uint8_t *npu_test_input(enum npu_net net);

#endif
