// SPDX-License-Identifier: GPL-2.0
/*
 * Frame Aware Scaling (FAS)
 * Copyright (c) 2026, deutereum <fawwazzuladhim700@gmail.com>.
 *
 * FAS is a short-term CPU frequency booster derived from the
 * original cpu-boost driver. It reacts to two signals that correlate
 * with an incoming frame needing to be produced quickly:
 *
 *  - touch input events (finger down / key press)
 *  - KGSL cmdbatch retirement (a GPU frame just finished, another is
 *    likely queued right behind it)
 *
 * Aware with the panel's current refresh rate so we don't
 * waste power boosting for low refresh rates.
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
#include <drm/drm_refresh_rate.h>

struct fas_cpu_sync {
	int cpu;
	unsigned int boost_min;
};

/* How long a boost floor is held before being released, in ms. */
static unsigned int fas_boost_ms = 50;

static DEFINE_PER_CPU(struct fas_cpu_sync, fas_sync_info);
static struct workqueue_struct *fas_wq;
static struct work_struct fas_boost_work;
static struct delayed_work fas_boost_rem;

static u64 fas_last_input_time;
static unsigned int fas_active_fps;
static unsigned long fas_next_boost;

#define FAS_MIN_INPUT_INTERVAL (150 * USEC_PER_MSEC)

static int fas_adjust_notify(struct notifier_block *nb, unsigned long val,
			     void *data)
{
	struct cpufreq_policy *policy = data;
	unsigned int cpu = policy->cpu;
	unsigned int boost_min = per_cpu(fas_sync_info, cpu).boost_min;

	if (val != CPUFREQ_ADJUST || !boost_min)
		return NOTIFY_OK;

	if (!policy->governor ||
	    strcmp(policy->governor->name, "schedutil"))
		return NOTIFY_OK;

	cpufreq_verify_within_limits(policy, boost_min, UINT_MAX);

	return NOTIFY_OK;
}

static struct notifier_block fas_adjust_nb = {
	.notifier_call = fas_adjust_notify,
};

static inline void fas_update_policy_online(void)
{
	unsigned int i;

	get_online_cpus();
	for_each_online_cpu(i)
		cpufreq_update_policy(i);
	put_online_cpus();
}

static void fas_do_boost_rem(struct work_struct *work)
{
	unsigned int i;

	for_each_possible_cpu(i)
		per_cpu(fas_sync_info, i).boost_min = 0;

	fas_active_fps = 0;
	fas_update_policy_online();
}

static void fas_do_boost(struct work_struct *work)
{
	unsigned int fps = dsi_panel_get_refresh_rate();
	unsigned int i;

	if (fps <= 60)
		return;

	if (fps == fas_active_fps) {
		mod_delayed_work(fas_wq, &fas_boost_rem,
				 msecs_to_jiffies(fas_boost_ms));
		return;
	}

	fas_active_fps = fps;

	cancel_delayed_work(&fas_boost_rem);

	for_each_possible_cpu(i) {
		struct fas_cpu_sync *s = &per_cpu(fas_sync_info, i);

		if (fps <= 90) {
			s->boost_min = (i <= 3) ? 1401600 :
				       (i <= 6) ? 748800 : 0;
		} else {
			s->boost_min = (i <= 3) ? 1804800 :
				       (i <= 6) ? 1056000 : 0;
		}
	}

	fas_update_policy_online();

	queue_delayed_work(fas_wq, &fas_boost_rem,
			   msecs_to_jiffies(fas_boost_ms));
}

static inline void fas_queue_boost(void)
{
	if (work_pending(&fas_boost_work))
		return;

	queue_work(fas_wq, &fas_boost_work);
}

static void fas_input_event(struct input_handle *handle,
			    unsigned int type, unsigned int code, int value)
{
	u64 now = ktime_to_us(ktime_get());

	if (now - fas_last_input_time < FAS_MIN_INPUT_INTERVAL)
		return;

	fas_last_input_time = now;
	fas_queue_boost();
}

static int fas_input_connect(struct input_handler *handler,
			     struct input_dev *dev,
			     const struct input_device_id *id)
{
	struct input_handle *handle;
	int error;

	handle = kzalloc(sizeof(struct input_handle), GFP_KERNEL);
	if (!handle)
		return -ENOMEM;

	handle->dev = dev;
	handle->handler = handler;
	handle->name = "fas";

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
	.event		= fas_input_event,
	.connect	= fas_input_connect,
	.disconnect	= fas_input_disconnect,
	.name		= "fas",
	.id_table	= fas_ids,
};

void kgsl_cmdbatch_retired_hook(void)
{
	if (dsi_panel_get_refresh_rate() <= 60)
		return;

	if (time_before(jiffies, fas_next_boost))
		return;

	fas_next_boost = jiffies + msecs_to_jiffies(fas_boost_ms);
	fas_queue_boost();
}

static int fas_init(void)
{
	fas_wq = alloc_workqueue("fas_wq", WQ_HIGHPRI, 0);
	if (!fas_wq)
		return -EFAULT;

	INIT_WORK(&fas_boost_work, fas_do_boost);
	INIT_DELAYED_WORK(&fas_boost_rem, fas_do_boost_rem);

	cpufreq_register_notifier(&fas_adjust_nb, CPUFREQ_POLICY_NOTIFIER);

	return input_register_handler(&fas_input_handler);
}
late_initcall(fas_init);
