/* zprof's marks and plots in a zprof build (build-zprof: the module and CONFIG_ZPROF
 * come from the command line); nothing in any other build. Port hooks and metrics:
 * prof.c.
 * SPDX-License-Identifier: Apache-2.0
 */
#ifndef PROF_H_
#define PROF_H_

#ifdef CONFIG_ZPROF
#include <zprof/zprof.h>
#else
#define ZPROF_MARK_START(name)       do { } while (0)
#define ZPROF_MARK_STOP(name, value) do { (void)(value); } while (0)
#define ZPROF_PLOT(name, value)      do { (void)(value); } while (0)
#endif

#endif /* PROF_H_ */
