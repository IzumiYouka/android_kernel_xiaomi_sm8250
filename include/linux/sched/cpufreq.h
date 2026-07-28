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
	unsigned int headroom_pct;

	if (util >= cap)
		return freq;

	/*
	 * Select per-cluster headroom percentage based on CPU capacity.
	 * Little cores get more headroom to stay responsive at low freq.
	 * Prime core gets less headroom since top-end boosts are expensive.
	 * Thresholds derived from runtime cpu_capacity values on sm8250:
	 *   little=313, big=777, prime=1024.
	 */
	if (cap <= 544)
		headroom_pct = 28;		/* little: ~28% max headroom */
	else if (cap >= 900)
		headroom_pct = 12;		/* prime: ~12% max headroom */
	else
		headroom_pct = 20;		/* big: ~20% max headroom */

	/*
	 * Quadratic taper: headroom is proportional to (delta^2 / cap),
	 * giving large boost at low util and near-zero boost near capacity.
	 */
	delta = cap - util;
	headroom = (delta * delta * headroom_pct) / (cap * 100);

	/*
	 * Suppress boosting at very low util (below ~16% of cap) to avoid
	 * unnecessary frequency ramping for small background work.
	 * Interpolates headroom smoothly from 0 up to full value at min_util.
	 */
	min_util = cap / 6;
	if (min_util && util < min_util)
		headroom = (headroom * util * util) / (min_util * min_util);

	util += headroom;
	return freq * util / cap;
}
#endif /* CONFIG_CPU_FREQ */

#endif /* _LINUX_SCHED_CPUFREQ_H */
