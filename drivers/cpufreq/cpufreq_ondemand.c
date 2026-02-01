// SPDX-License-Identifier: GPL-2.0-only
/*
 *  drivers/cpufreq/cpufreq_ondemand.c
 *
 *  Copyright (C)  2001 Russell King
 *            (C)  2003 Venkatesh Pallipadi <venkatesh.pallipadi@intel.com>.
 *                      Jun Nakajima <jun.nakajima@intel.com>
 */

#define pr_fmt(fmt) KBUILD_MODNAME ": " fmt

#include <linux/cpu.h>
#include <linux/percpu-defs.h>
#include <linux/slab.h>
#include <linux/tick.h>
#include <linux/sched/cpufreq.h>

#include "cpufreq_ondemand.h"

/* On-demand governor macros */
#define DEF_FREQUENCY_UP_THRESHOLD		(75)
#define DEF_SAMPLING_DOWN_FACTOR		(10)
#define MAX_SAMPLING_DOWN_FACTOR		(100000)
#define MICRO_FREQUENCY_UP_THRESHOLD		(75)
#define MIN_FREQUENCY_UP_THRESHOLD		(40)
#define MAX_FREQUENCY_UP_THRESHOLD		(100)
#define IO_IS_BUSY				(0)
#define IGNORE_NICE_LOAD			(0)
#define DOWN_THRESHOLD_MARGIN			(25)
#define DEF_BOOST				(1)

#define DEF_FREQUENCY_STEP_0			(1200000)
#define DEF_FREQUENCY_STEP_1			(1600000)
#define DEF_FREQUENCY_STEP_2			(2000000)

/*
 * Every sampling_rate, we check, if current idle time is less than 20%
 * (default), then we try to increase frequency. Else, we adjust the frequency
 * proportional to load.
 */
static void od_update(struct cpufreq_policy *policy)
{
	struct policy_dbs_info *policy_dbs = policy->governor_data;
	struct dbs_data *dbs_data = policy_dbs->dbs_data;
	unsigned int load = dbs_update(policy);
	unsigned int requested_freq = 0;

	/* Check for frequency increase */
	if (load >= dbs_data->up_threshold) {

		/* if we are already at full speed then break out early */
		if (policy->cur == policy->max)
			return;

		if (!dbs_data->boost) {
			if (policy->cur == DEF_FREQUENCY_STEP_0)
				requested_freq = DEF_FREQUENCY_STEP_1;
			else
				requested_freq = DEF_FREQUENCY_STEP_2;

			if (requested_freq > policy->max)
				requested_freq = policy->max;
		} else {
			/* Boost */
			requested_freq = policy->max;
		}

		__cpufreq_driver_target(policy, requested_freq,
			CPUFREQ_RELATION_H);

		/* If we are at max speed, apply sampling_down_factor */
		if (policy->cur == policy->max)
			policy_dbs->rate_mult = dbs_data->sampling_down_factor;

		return;
	}

	/* No longer fully busy, reset rate_mult */
	policy_dbs->rate_mult = 1;

	/*
	 * if we cannot reduce the frequency anymore, break out early
	 */
	if (policy->cur == policy->min)
		return;

	/* Check for frequency decrease */
	if (load < dbs_data->down_threshold) {

		if (policy->cur >= DEF_FREQUENCY_STEP_2)
			requested_freq = DEF_FREQUENCY_STEP_1;
		else
			requested_freq = DEF_FREQUENCY_STEP_0;

		if (requested_freq < policy->min)
			requested_freq = policy->min;

		__cpufreq_driver_target(policy, requested_freq,
				CPUFREQ_RELATION_L);
	}
}

static unsigned int od_dbs_update(struct cpufreq_policy *policy)
{
	struct policy_dbs_info *policy_dbs = policy->governor_data;
	struct dbs_data *dbs_data = policy_dbs->dbs_data;

	od_update(policy);

	return dbs_data->sampling_rate * policy_dbs->rate_mult;
}

static void update_down_threshold(struct dbs_data *dbs_data)
{
	dbs_data->down_threshold = ((dbs_data->up_threshold * DEF_FREQUENCY_STEP_0 / DEF_FREQUENCY_STEP_1) - DOWN_THRESHOLD_MARGIN);

	pr_info("[%s] for CPU - new value: %u\n",__func__, dbs_data->down_threshold);
}

/************************** sysfs interface ************************/
static struct dbs_governor od_dbs_gov;

static ssize_t io_is_busy_store(struct gov_attr_set *attr_set, const char *buf,
				size_t count)
{
	struct dbs_data *dbs_data = to_dbs_data(attr_set);
	unsigned int input;
	int ret;

	ret = sscanf(buf, "%u", &input);
	if (ret != 1)
		return -EINVAL;
	dbs_data->io_is_busy = !!input;

	/* we need to re-evaluate prev_cpu_idle */
	gov_update_cpu_data(dbs_data);

	return count;
}

static ssize_t up_threshold_store(struct gov_attr_set *attr_set,
				  const char *buf, size_t count)
{
	struct dbs_data *dbs_data = to_dbs_data(attr_set);
	unsigned int input;
	int ret;
	ret = sscanf(buf, "%u", &input);

	if (ret != 1 || input > MAX_FREQUENCY_UP_THRESHOLD ||
			input < MIN_FREQUENCY_UP_THRESHOLD) {
		return -EINVAL;
	}

	dbs_data->up_threshold = input;

	update_down_threshold(dbs_data);

	return count;
}

static ssize_t sampling_down_factor_store(struct gov_attr_set *attr_set,
					  const char *buf, size_t count)
{
	struct dbs_data *dbs_data = to_dbs_data(attr_set);
	struct policy_dbs_info *policy_dbs;
	unsigned int input;
	int ret;
	ret = sscanf(buf, "%u", &input);

	if (ret != 1 || input > MAX_SAMPLING_DOWN_FACTOR || input < 1)
		return -EINVAL;

	dbs_data->sampling_down_factor = input;

	/* Reset down sampling multiplier in case it was active */
	list_for_each_entry(policy_dbs, &attr_set->policy_list, list) {
		/*
		 * Doing this without locking might lead to using different
		 * rate_mult values in od_update() and od_dbs_update().
		 */
		mutex_lock(&policy_dbs->update_mutex);
		policy_dbs->rate_mult = 1;
		mutex_unlock(&policy_dbs->update_mutex);
	}

	return count;
}

static ssize_t ignore_nice_load_store(struct gov_attr_set *attr_set,
				      const char *buf, size_t count)
{
	struct dbs_data *dbs_data = to_dbs_data(attr_set);
	unsigned int input;
	int ret;

	ret = sscanf(buf, "%u", &input);
	if (ret != 1)
		return -EINVAL;

	if (input > 1)
		input = 1;

	if (input == dbs_data->ignore_nice_load) { /* nothing to do */
		return count;
	}
	dbs_data->ignore_nice_load = input;

	/* we need to re-evaluate prev_cpu_idle */
	gov_update_cpu_data(dbs_data);

	return count;
}

static ssize_t boost_store(struct gov_attr_set *attr_set, const char *buf,
				size_t count)
{
	struct dbs_data *dbs_data = to_dbs_data(attr_set);
	unsigned int input;
	int ret;

	ret = sscanf(buf, "%u", &input);
	if (ret != 1)
		return -EINVAL;

	if (input > 1)
		input = 1;

	dbs_data->boost = input;

	return count;
}


gov_show_one_common(sampling_rate);
gov_show_one_common(up_threshold);
gov_show_one_common(sampling_down_factor);
gov_show_one_common(ignore_nice_load);
gov_show_one_common(io_is_busy);
gov_show_one_common(boost);

gov_attr_rw(sampling_rate);
gov_attr_rw(io_is_busy);
gov_attr_rw(up_threshold);
gov_attr_rw(sampling_down_factor);
gov_attr_rw(ignore_nice_load);
gov_attr_rw(boost);

static struct attribute *od_attrs[] = {
	&sampling_rate.attr,
	&up_threshold.attr,
	&sampling_down_factor.attr,
	&ignore_nice_load.attr,
	&io_is_busy.attr,
	&boost.attr,
	NULL
};
ATTRIBUTE_GROUPS(od);

/************************** sysfs end ************************/

static struct policy_dbs_info *od_alloc(void)
{
	struct od_policy_dbs_info *dbs_info;

	dbs_info = kzalloc(sizeof(*dbs_info), GFP_KERNEL);
	return dbs_info ? &dbs_info->policy_dbs : NULL;
}

static void od_free(struct policy_dbs_info *policy_dbs)
{
	kfree(to_dbs_info(policy_dbs));
}

static int od_init(struct dbs_data *dbs_data)
{
	u64 idle_time;
	int cpu;

	cpu = get_cpu();
	idle_time = get_cpu_idle_time_us(cpu, NULL);
	put_cpu();
	if (idle_time != -1ULL) {
		/* Idle micro accounting is supported. Use finer thresholds */
		dbs_data->up_threshold = MICRO_FREQUENCY_UP_THRESHOLD;
	} else {
		dbs_data->up_threshold = DEF_FREQUENCY_UP_THRESHOLD;
	}

	dbs_data->sampling_down_factor = DEF_SAMPLING_DOWN_FACTOR;
	dbs_data->ignore_nice_load = IGNORE_NICE_LOAD;
	dbs_data->io_is_busy = IO_IS_BUSY;
	dbs_data->boost = DEF_BOOST;

	update_down_threshold(dbs_data);

	return 0;
}

static void od_exit(struct dbs_data *dbs_data)
{
	if (dbs_data->tuners)
		kfree(dbs_data->tuners);
}

static void od_start(struct cpufreq_policy *policy)
{
	struct od_policy_dbs_info *dbs_info = to_dbs_info(policy->governor_data);

	dbs_info->sample_type = OD_NORMAL_SAMPLE;
}

static struct dbs_governor od_dbs_gov = {
	.gov = CPUFREQ_DBS_GOVERNOR_INITIALIZER("ondemand"),
	.kobj_type = { .default_groups = od_groups },
	.gov_dbs_update = od_dbs_update,
	.alloc = od_alloc,
	.free = od_free,
	.init = od_init,
	.exit = od_exit,
	.start = od_start,
};

#define CPU_FREQ_GOV_ONDEMAND	(od_dbs_gov.gov)

MODULE_AUTHOR("Venkatesh Pallipadi <venkatesh.pallipadi@intel.com>");
MODULE_AUTHOR("Alexey Starikovskiy <alexey.y.starikovskiy@intel.com>");
MODULE_DESCRIPTION("'cpufreq_ondemand' - A dynamic cpufreq governor for "
	"Low Latency Frequency Transition capable processors");
MODULE_LICENSE("GPL");

#ifdef CONFIG_CPU_FREQ_DEFAULT_GOV_ONDEMAND
struct cpufreq_governor *cpufreq_default_governor(void)
{
	return &CPU_FREQ_GOV_ONDEMAND;
}
#endif

cpufreq_governor_init(CPU_FREQ_GOV_ONDEMAND);
cpufreq_governor_exit(CPU_FREQ_GOV_ONDEMAND);
