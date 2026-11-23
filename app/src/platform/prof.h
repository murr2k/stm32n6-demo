/* zprof's marks, plots and trigger when zprof is in the build (every build since
 * 2026-11-22: app/CMakeLists.txt; n6.cmd --no-zprof leaves it out); nothing
 * otherwise. Port hooks and metrics: prof.c.
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
#define ZPROF_TRIGGER(why)           do { (void)(why); } while (0)
#endif

#endif /* PROF_H_ */
