/*
 * Placement of the generated networks' data, forced into their sources by
 * app/CMakeLists.txt. The epoch controller's programs are patched at
 * initialisation (their "runtime" copies, 16 KB for the hand networks): in
 * PSRAM with the rest of the NPU's data, not in the 840 KB of internal RAM.
 * The constant programs stay in the image (flash), read in place.
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#define ECBLOB_RUNTIME_SECTION __attribute__((section("PSRAM.nn_blobs")))
