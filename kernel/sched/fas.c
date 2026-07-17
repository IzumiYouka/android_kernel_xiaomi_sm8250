// SPDX-License-Identifier: GPL-2.0
/*
 * Frame Aware Scaling (FAS)
 * Copyright (c) 2026, deutereum <fawwazzuladhim700@gmail.com>.
 *
 * FAS is a short-term CPU frequency booster that acts as a schedutil helper
 * to react with sudden frame changes.
 *
 * FAS is aware of the panel's current refresh rate so it won't
 * waste power boosting on low refresh rates.
 */

#define pr_fmt(fmt) "fas: " fmt

#include <linux/kernel.h>
#include <linux/init.h>
#include <linux/cpufreq.h>
#include <linux/cpu.h>
#include <linux/ktime.h>
#include <linux/slab.h>
#include <linux/input.h>
#include <linux/atomic.h>
#include <linux/kthread.h>
#include <drm/drm_notifier_mi.h>
#include <uapi/linux/sched/types.h>
#include <drm/drm_refresh_rate.h>

/*
 * Silver cluster CPUs (0-3) boost frequencies by FPS tier.
 * Gold/Prime are intentionally unboosted — FAS targets the little cluster only.
 */
#define FAS_BOOST_FREQ_LO	1401600	/* <=90 Hz */
#define FAS_BOOST_FREQ_HI	1804800	/* >90 Hz  */

#define FAS_INPUT_BOOST_MS	50
#define FAS_ADAPTIVE_MULT	14
#define FAS_ADAPTIVE_MIN_MS	50U
#define FAS_ADAPTIVE_MAX_MS	200U
#define FAS_MIN_INPUT_INTERVAL	(150 * USEC_PER_MSEC)

enum {
	SCREEN_OFF,
	INPUT_BOOST,
	CMDBATCH_BOOST,
};

struct fas_drv {
	struct delayed_work		input_unboost;
	struct delayed_work		cmdbatch_unboost;
	struct notifier_block		cpu_notif;
	struct notifier_block		drm_notif;
	wait_queue_head_t		boost_waitq;
	atomic_long_t			input_boost_expires;
	atomic_long_t			cmdbatch_boost_expires;
	unsigned long			state;
	struct task_struct		*thread;
};

static void input_unboost_worker(struct work_struct *work);
static void cmdbatch_unboost_worker(struct work_struct *work);

static struct fas_drv fas_drv_g = {
	.input_unboost    = __DELAYED_WORK_INITIALIZER(fas_drv_g.input_unboost,
						       input_unboost_worker, 0),
	.cmdbatch_unboost = __DELAYED_WORK_INITIALIZER(fas_drv_g.cmdbatch_unboost,
						       cmdbatch_unboost_worker, 0),
	.boost_waitq      = __WAIT_QUEUE_HEAD_INITIALIZER(fas_drv_g.boost_waitq),
};

static u64 fas_last_input_time;
static u64 fas_last_cmdbatch_time;

/* Update one CPU per cluster — policy is shared, siblings follow */
static void fas_update_online_cpu_policy(void)
{
	unsigned int cpu;

	get_online_cpus();
	/* Silver: CPUs 0-3 */
	for_each_online_cpu(cpu) {
		if (cpu <= 3) {
			cpufreq_update_policy(cpu);
			break;
		}
	}
	put_online_cpus();
}

static void __fas_kick_input(struct fas_drv *b, unsigned int duration_ms)
{
	unsigned long boost_jiffies = msecs_to_jiffies(duration_ms);
	unsigned long curr_expires, new_expires;

	if (test_bit(SCREEN_OFF, &b->state))
		return;

	do {
		curr_expires = atomic_long_read(&b->input_boost_expires);
		new_expires  = jiffies + boost_jiffies;

		if (time_after(curr_expires, new_expires))
			return;
	} while (atomic_long_cmpxchg(&b->input_boost_expires,
				      curr_expires, new_expires) != curr_expires);

	set_bit(INPUT_BOOST, &b->state);
	if (!mod_delayed_work(system_unbound_wq, &b->input_unboost, boost_jiffies))
		wake_up(&b->boost_waitq);
}

static void __fas_kick_cmdbatch(struct fas_drv *b, unsigned int duration_ms)
{
	unsigned long boost_jiffies = msecs_to_jiffies(duration_ms);
	unsigned long curr_expires, new_expires;

	if (test_bit(SCREEN_OFF, &b->state))
		return;

	do {
		curr_expires = atomic_long_read(&b->cmdbatch_boost_expires);
		new_expires  = jiffies + boost_jiffies;

		if (time_after(curr_expires, new_expires))
			return;
	} while (atomic_long_cmpxchg(&b->cmdbatch_boost_expires,
				      curr_expires, new_expires) != curr_expires);

	set_bit(CMDBATCH_BOOST, &b->state);
	if (!mod_delayed_work(system_unbound_wq, &b->cmdbatch_unboost, boost_jiffies))
		wake_up(&b->boost_waitq);
}

static void input_unboost_worker(struct work_struct *work)
{
	struct fas_drv *b = container_of(to_delayed_work(work),
					 typeof(*b), input_unboost);

	clear_bit(INPUT_BOOST, &b->state);
	wake_up(&b->boost_waitq);
}

static void cmdbatch_unboost_worker(struct work_struct *work)
{
	struct fas_drv *b = container_of(to_delayed_work(work),
					 typeof(*b), cmdbatch_unboost);

	clear_bit(CMDBATCH_BOOST, &b->state);
	fas_last_cmdbatch_time = 0;
	wake_up(&b->boost_waitq);
}

static int fas_boost_thread(void *data)
{
	static const struct sched_param sched_max_rt_prio = {
		.sched_priority = MAX_RT_PRIO - 1
	};
	struct fas_drv *b = data;
	unsigned long old_state = 0;

	sched_setscheduler_nocheck(current, SCHED_FIFO, &sched_max_rt_prio);

	while (1) {
		bool should_stop = false;
		unsigned long curr_state;

		wait_event(b->boost_waitq,
			(curr_state = READ_ONCE(b->state)) != old_state ||
			(should_stop = kthread_should_stop()));

		if (should_stop)
			break;

		old_state = curr_state;
		fas_update_online_cpu_policy();
	}

	return 0;
}

static int fas_cpu_notifier_cb(struct notifier_block *nb, unsigned long action,
			       void *data)
{
	struct fas_drv *b = container_of(nb, typeof(*b), cpu_notif);
	struct cpufreq_policy *policy = data;

	if (action != CPUFREQ_ADJUST)
		return NOTIFY_OK;

	/* Only touch the Silver cluster */
	if (policy->cpu > 3)
		return NOTIFY_OK;

	if (test_bit(SCREEN_OFF, &b->state))
		return NOTIFY_OK;

	if (test_bit(INPUT_BOOST, &b->state) ||
	    test_bit(CMDBATCH_BOOST, &b->state)) {
		unsigned int fps = dsi_panel_get_refresh_rate();

		if (fps > 60) {
			unsigned int freq = (fps <= 90) ?
					    FAS_BOOST_FREQ_LO : FAS_BOOST_FREQ_HI;
			cpufreq_verify_within_limits(policy, freq, UINT_MAX);
		}
	}

	return NOTIFY_OK;
}

static int fas_drm_notifier_cb(struct notifier_block *nb, unsigned long action,
			       void *data)
{
	struct fas_drv *b = container_of(nb, typeof(*b), drm_notif);
	int *blank = ((struct mi_drm_notifier *)data)->data;

	if (action != MI_DRM_EARLY_EVENT_BLANK)
		return NOTIFY_OK;

	if (*blank == MI_DRM_BLANK_UNBLANK) {
		clear_bit(SCREEN_OFF, &b->state);
	} else {
		set_bit(SCREEN_OFF, &b->state);
		wake_up(&b->boost_waitq);
	}

	return NOTIFY_OK;
}

/* Input handler */

static void fas_input_event(struct input_handle *handle,
			    unsigned int type, unsigned int code, int value)
{
	u64 now = ktime_to_us(ktime_get());

	if (now - fas_last_input_time < FAS_MIN_INPUT_INTERVAL)
		return;

	fas_last_input_time = now;
	__fas_kick_input(&fas_drv_g, FAS_INPUT_BOOST_MS);
}

static int fas_input_connect(struct input_handler *handler,
			     struct input_dev *dev,
			     const struct input_device_id *id)
{
	struct input_handle *handle;
	int error;

	handle = kzalloc(sizeof(*handle), GFP_KERNEL);
	if (!handle)
		return -ENOMEM;

	handle->dev     = dev;
	handle->handler = handler;
	handle->name    = "fas";

	error = input_register_handle(handle);
	if (error)
		goto err2;

	error = input_open_device(handle);
	if (error)
		goto err1;

	return 0;
err1:
	input_unregister_handle(handle);
err2:
	kfree(handle);
	return error;
}

static void fas_input_disconnect(struct input_handle *handle)
{
	input_close_device(handle);
	input_unregister_handle(handle);
	kfree(handle);
}

static const struct input_device_id fas_ids[] = {
	{
		.flags = INPUT_DEVICE_ID_MATCH_EVBIT |
			 INPUT_DEVICE_ID_MATCH_ABSBIT,
		.evbit = { BIT_MASK(EV_ABS) },
		.absbit = { [BIT_WORD(ABS_MT_POSITION_X)] =
			BIT_MASK(ABS_MT_POSITION_X) |
			BIT_MASK(ABS_MT_POSITION_Y) },
	},
	{
		.flags = INPUT_DEVICE_ID_MATCH_KEYBIT |
			 INPUT_DEVICE_ID_MATCH_ABSBIT,
		.keybit = { [BIT_WORD(BTN_TOUCH)] = BIT_MASK(BTN_TOUCH) },
		.absbit = { [BIT_WORD(ABS_X)] =
			BIT_MASK(ABS_X) | BIT_MASK(ABS_Y) },
	},
	{
		.flags = INPUT_DEVICE_ID_MATCH_EVBIT,
		.evbit = { BIT_MASK(EV_KEY) },
	},
	{ },
};

static struct input_handler fas_input_handler = {
	.event      = fas_input_event,
	.connect    = fas_input_connect,
	.disconnect = fas_input_disconnect,
	.name       = "fas",
	.id_table   = fas_ids,
};

/* KGSL cmdbatch hook — called from KGSL retirement path */

void fas_do_cmdbatch_boost(void)
{
	struct fas_drv *b = &fas_drv_g;
	unsigned int fps = dsi_panel_get_refresh_rate();
	u64 now = ktime_to_ms(ktime_get());
	u64 interval;
	unsigned int window_ms;

	if (fps <= 60)
		return;

	interval = fas_last_cmdbatch_time ?
		   now - fas_last_cmdbatch_time : 0;
	fas_last_cmdbatch_time = now;

	if (interval > 0 && interval < FAS_ADAPTIVE_MAX_MS) {
		window_ms = (unsigned int)(interval * FAS_ADAPTIVE_MULT / 10);
		window_ms = clamp(window_ms,
				  FAS_ADAPTIVE_MIN_MS,
				  FAS_ADAPTIVE_MAX_MS);
	} else {
		window_ms = FAS_ADAPTIVE_MIN_MS;
	}

	__fas_kick_cmdbatch(b, window_ms);
}

static int __init fas_init(void)
{
	struct fas_drv *b = &fas_drv_g;
	struct task_struct *thread;
	int ret;

	b->cpu_notif.notifier_call = fas_cpu_notifier_cb;
	ret = cpufreq_register_notifier(&b->cpu_notif, CPUFREQ_POLICY_NOTIFIER);
	if (ret) {
		pr_err("Failed to register cpufreq notifier: %d\n", ret);
		return ret;
	}

	b->drm_notif.notifier_call = fas_drm_notifier_cb;
	b->drm_notif.priority      = INT_MAX;
	ret = mi_drm_register_client(&b->drm_notif);
	if (ret) {
		pr_err("Failed to register DRM notifier: %d\n", ret);
		goto unregister_cpu_notif;
	}

	thread = kthread_run(fas_boost_thread, b, "fas_boostd");
	if (IS_ERR(thread)) {
		ret = PTR_ERR(thread);
		pr_err("Failed to start boost thread: %d\n", ret);
		goto unregister_drm_notif;
	}
	b->thread = thread;

	ret = input_register_handler(&fas_input_handler);
	if (ret) {
		pr_err("Failed to register input handler: %d\n", ret);
		goto stop_thread;
	}

	return 0;

stop_thread:
	kthread_stop(b->thread);
unregister_drm_notif:
	mi_drm_unregister_client(&b->drm_notif);
unregister_cpu_notif:
	cpufreq_unregister_notifier(&b->cpu_notif, CPUFREQ_POLICY_NOTIFIER);
	return ret;
}
late_initcall(fas_init);
