/* SPDX-License-Identifier: GPL-2.0 */
#ifndef _LINUX_SCHED_CPUFREQ_H
#define _LINUX_SCHED_CPUFREQ_H

#include <linux/types.h>

/*
 * Interface between cpufreq drivers and the scheduler:
 */

#define SCHED_CPUFREQ_IOWAIT	(1U << 0)
#define SCHED_CPUFREQ_MIGRATION	(1U << 1)
#define SCHED_CPUFREQ_INTERCLUSTER_MIG (1U << 3)
#define SCHED_CPUFREQ_WALT (1U << 4)
#define SCHED_CPUFREQ_PL        (1U << 5)
#define SCHED_CPUFREQ_EARLY_DET (1U << 6)
#define SCHED_CPUFREQ_CONTINUE (1U << 8)

#ifdef CONFIG_CPU_FREQ
struct cpufreq_policy;

struct update_util_data {
       void (*func)(struct update_util_data *data, u64 time, unsigned int flags);
};

void cpufreq_add_update_util_hook(int cpu, struct update_util_data *data,
                       void (*func)(struct update_util_data *data, u64 time,
				    unsigned int flags));
void cpufreq_remove_update_util_hook(int cpu);
bool cpufreq_this_cpu_can_update(struct cpufreq_policy *policy);

static inline unsigned long map_util_freq(unsigned long util,
					  unsigned long freq,
					  unsigned long cap)
{
	unsigned long delta, headroom, min_util;

	if (util >= cap)
		return freq;

	/*
	 * Quadratic tapered DVFS headroom:
	 * provide extra headroom at low-mid util while tapering it near max
	 * capacity to avoid over-aggressive top-end frequency boosting.
	 */
	delta = cap - util;
	headroom = (delta * delta) / (cap << 2);

	/*
	 * Suppress boosting at very low util to avoid unnecessary frequency
	 * ramping for tiny background work.
	 */
	min_util = cap / 10;
	if (min_util && util < min_util)
		headroom = (headroom * util * util) / (min_util * min_util);

	util += headroom;

	return freq * util / cap;
}
#endif /* CONFIG_CPU_FREQ */

#endif /* _LINUX_SCHED_CPUFREQ_H */
