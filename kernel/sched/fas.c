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
 * aware with the panel's current refresh rate so we don't
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

/* Tracks last applied fps to avoid redundant policy updates. */
static unsigned int fas_active_fps;

/* Debounce window for touch-triggered boosts. */
#define FAS_MIN_INPUT_INTERVAL (150 * USEC_PER_MSEC)

/*
 * The CPUFREQ_ADJUST notifier overrides the current policy min to make
 * sure policy min >= boost_min. The cpufreq framework then enforces it.
 */
static int fas_adjust_notify(struct notifier_block *nb, unsigned long val,
			      void *data)
{
	struct cpufreq_policy *policy = data;
	unsigned int cpu = policy->cpu;
	struct fas_cpu_sync *s = &per_cpu(fas_sync_info, cpu);
	unsigned int boost_min = s->boost_min;

	switch (val) {
	case CPUFREQ_ADJUST:
		if (!boost_min)
			break;

		/* Only apply boost under schedutil */
		if (!policy->governor ||
		    strcmp(policy->governor->name, "schedutil"))
			break;

		pr_debug("CPU%u policy min before boost: %u kHz\n",
			 cpu, policy->min);
		pr_debug("CPU%u boost min: %u kHz\n", cpu, boost_min);

		cpufreq_verify_within_limits(policy, boost_min, UINT_MAX);

		pr_debug("CPU%u policy min after boost: %u kHz\n",
			 cpu, policy->min);
		break;
	}

	return NOTIFY_OK;
}

static struct notifier_block fas_adjust_nb = {
	.notifier_call = fas_adjust_notify,
};

static void fas_update_policy_online(void)
{
	unsigned int i;

	/* Re-evaluate policy to trigger the adjust notifier for online CPUs */
	get_online_cpus();
	for_each_online_cpu(i) {
		pr_debug("Updating policy for CPU%d\n", i);
		cpufreq_update_policy(i);
	}
	put_online_cpus();
}

static void fas_do_boost_rem(struct work_struct *work)
{
	unsigned int i;
	struct fas_cpu_sync *s;

	pr_debug("Resetting boost min for all CPUs\n");
	for_each_possible_cpu(i) {
		s = &per_cpu(fas_sync_info, i);
		s->boost_min = 0;
	}

	fas_active_fps = 0;
	fas_update_policy_online();
}

static void fas_do_boost(struct work_struct *work)
{
	unsigned int fps = dsi_panel_get_refresh_rate();
	unsigned int i;
	struct fas_cpu_sync *s;

	if (fps <= 60)
		return;

	/*
	 * If same fps tier is already active, just re-arm the expiry
	 * timer — no need to walk per-cpu data or update policies again.
	 */
	if (fps == fas_active_fps) {
		mod_delayed_work(fas_wq, &fas_boost_rem,
				 msecs_to_jiffies(fas_boost_ms));
		return;
	}

	fas_active_fps = fps;

	/* Non-blocking cancel — rem work only zeros boost_min, no harm if
	 * it races and runs once more; next boost will overwrite anyway. */
	cancel_delayed_work(&fas_boost_rem);

	/* Set boost_min per-CPU based on current refresh rate. */
	pr_debug("Setting boost min for all CPUs (fps=%u)\n", fps);
	for_each_possible_cpu(i) {
		s = &per_cpu(fas_sync_info, i);
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

static void fas_input_event(struct input_handle *handle,
		unsigned int type, unsigned int code, int value)
{
	u64 now;

	now = ktime_to_us(ktime_get());
	if (now - fas_last_input_time < FAS_MIN_INPUT_INTERVAL)
		return;

	fas_last_input_time = now;

	if (work_pending(&fas_boost_work))
		return;

	queue_work(fas_wq, &fas_boost_work);
}

static int fas_input_connect(struct input_handler *handler,
		struct input_dev *dev, const struct input_device_id *id)
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
	/* multi-touch touchscreen */
	{
		.flags = INPUT_DEVICE_ID_MATCH_EVBIT |
			INPUT_DEVICE_ID_MATCH_ABSBIT,
		.evbit = { BIT_MASK(EV_ABS) },
		.absbit = { [BIT_WORD(ABS_MT_POSITION_X)] =
			BIT_MASK(ABS_MT_POSITION_X) |
			BIT_MASK(ABS_MT_POSITION_Y) },
	},
	/* touchpad */
	{
		.flags = INPUT_DEVICE_ID_MATCH_KEYBIT |
			INPUT_DEVICE_ID_MATCH_ABSBIT,
		.keybit = { [BIT_WORD(BTN_TOUCH)] = BIT_MASK(BTN_TOUCH) },
		.absbit = { [BIT_WORD(ABS_X)] =
			BIT_MASK(ABS_X) | BIT_MASK(ABS_Y) },
	},
	/* keypad */
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

/*
 * Called from adreno_dispatch.c's retire_cmdobj() whenever a KGSL
 * cmdbatch retires, i.e. a GPU-rendered frame just completed. Treated
 * the same as a touch event for boost purposes.
 */
void kgsl_cmdbatch_retired_hook(void)
{
	unsigned int fps = dsi_panel_get_refresh_rate();

	if (fps <= 60)
		return;

	if (work_pending(&fas_boost_work))
		return;

	queue_work(fas_wq, &fas_boost_work);
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
