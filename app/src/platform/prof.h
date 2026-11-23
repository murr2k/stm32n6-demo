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

/* Threads zprof's fault handler ended (the board ran on): the alarm in the status
 * bar, the list on Settings and in /status.json. Nothing without zprof's handler. */
#include <stdint.h>

/* Faults acknowledged ("faults ack", ctl.c): no longer an alarm, still listed. */
extern int prof_faults_acked;
#ifdef CONFIG_ZPROF_FAULT
#define prof_fault_info zprof_fault_info
#define prof_faults     zprof_faults
#define prof_fault_reason zprof_fault_reason
#else
struct prof_fault_info {
	uint32_t uptime_ms;
	uint32_t reason;
	char thread[16];
};
static inline int prof_faults(struct prof_fault_info *out, int n)
{
	(void)out;
	(void)n;
	return 0;
}
static inline const char *prof_fault_reason(uint32_t reason)
{
	(void)reason;
	return "";
}
#endif

#endif /* PROF_H_ */
