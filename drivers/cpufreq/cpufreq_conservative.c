/*
 *  drivers/cpufreq/cpufreq_conservative.c
 *
 *  Copyright (C)  2001 Russell King
 *            (C)  2003 Venkatesh Pallipadi <venkatesh.pallipadi@intel.com>.
 *                      Jun Nakajima <jun.nakajima@intel.com>
 *            (C)  2009 Alexander Clouter <alex@digriz.org.uk>
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 as
 * published by the Free Software Foundation.
 */

#include <linux/slab.h>
#include "cpufreq_governor.h"

struct cs_policy_dbs_info {
	struct policy_dbs_info policy_dbs;
	unsigned int down_skip;
	unsigned int requested_freq;
};

static inline struct cs_policy_dbs_info *to_dbs_info(struct policy_dbs_info *policy_dbs)
{
	return container_of(policy_dbs, struct cs_policy_dbs_info, policy_dbs);
}

struct cs_dbs_tuners {
	unsigned int dynamic_down_threshold;
	unsigned int freq_step;
};

#define DEF_FREQUENCY_UP_THRESHOLD	(65) /* min 30, max 100 */
#define DOWN_THRESHOLD_MARGIN			(15)
#define DEF_FREQUENCY_STEP			(19)

#define DEF_FREQUENCY_STEP_0			(1200000)
#define DEF_FREQUENCY_STEP_1			(1600000)
#define DEF_FREQUENCY_STEP_2			(2000000)

/* Conservative governor macros */
#define DEF_SAMPLING_DOWN_FACTOR		(1)
#define MAX_SAMPLING_DOWN_FACTOR		(10)
#define DEF_FREQUENCY_SAMPLE_RATE		(100000)

static unsigned int up_threshold_0 = DEF_FREQUENCY_UP_THRESHOLD;

static unsigned int down_threshold_1 = 0;
//static unsigned int down_threshold_2 = 0;

static inline unsigned int get_freq_step(struct cs_dbs_tuners *cs_tuners,
					 struct cpufreq_policy *policy)
{
	unsigned int freq_step = (cs_tuners->freq_step * policy->max) / 100;

	/* max freq cannot be less than 100. But who knows... */
	if (unlikely(freq_step == 0))
		freq_step = DEF_FREQUENCY_STEP;

	return freq_step;
}

/*
 * Every sampling_rate, we check, if current idle time is less than 20%
 * (default), then we try to increase frequency. Every sampling_rate *
 * sampling_down_factor, we check, if current idle time is more than 80%
 * (default), then we try to decrease frequency
 *
 * Frequency updates happen at minimum steps of 5% (default) of maximum
 * frequency
 */
static unsigned int cs_dbs_update(struct cpufreq_policy *policy)
{
	struct policy_dbs_info *policy_dbs = policy->governor_data;
	struct cs_policy_dbs_info *dbs_info = to_dbs_info(policy_dbs);
	unsigned int requested_freq = dbs_info->requested_freq;
	struct dbs_data *dbs_data = policy_dbs->dbs_data;
	struct cs_dbs_tuners *cs_tuners = dbs_data->tuners;
	unsigned int load = dbs_update(policy);
	unsigned int freq_step;

	/*
	 * break out if we 'cannot' reduce the speed as the user might
	 * want freq_step to be zero
	 */
	if (cs_tuners->freq_step == 0)
		goto out;

	/*
	 * If requested_freq is out of range, it is likely that the limits
	 * changed in the meantime, so fall back to current frequency in that
	 * case.
	 */
	if (requested_freq > policy->max || requested_freq < policy->min) {
		requested_freq = policy->cur;
		dbs_info->requested_freq = requested_freq;
	}

	freq_step = get_freq_step(cs_tuners, policy);

	/*
	 * Decrease requested_freq one freq_step for each idle period that
	 * we didn't update the frequency.
	 */
	if (policy_dbs->idle_periods < UINT_MAX) {
		unsigned int freq_steps = policy_dbs->idle_periods * freq_step;

		if (requested_freq > policy->min + freq_steps)
			requested_freq -= freq_steps;
		else
			requested_freq = policy->min;

		policy_dbs->idle_periods = UINT_MAX;
	}

	/* Check for frequency increase */
	if (load > dbs_data->up_threshold) {
		dbs_info->down_skip = 0;

		/* if we are already at full speed then break out early */
		if (requested_freq == policy->max)
			goto out;

		requested_freq += freq_step;
		if (requested_freq > policy->max)
			requested_freq = policy->max;

		__cpufreq_driver_target(policy, requested_freq, CPUFREQ_RELATION_H);
		dbs_info->requested_freq = requested_freq;
		goto out;
	}

	/* if sampling_down_factor is active break out early */
	if (++dbs_info->down_skip < dbs_data->sampling_down_factor)
		goto out;
	dbs_info->down_skip = 0;
/*
	// Get dynamic_down_threshold
	if (policy->cur == DEF_FREQUENCY_STEP_1) {
		cs_tuners->dynamic_down_threshold = down_threshold_1;
		goto check;
	}

	if (policy->cur == DEF_FREQUENCY_STEP_2)
		cs_tuners->dynamic_down_threshold = down_threshold_2;

check:
*/
	/* Check for frequency decrease */
	if (load < cs_tuners->dynamic_down_threshold) {
		/*
		 * if we cannot reduce the frequency anymore, break out early
		 */
		if (requested_freq == policy->min)
			goto out;

		if (requested_freq > freq_step)
			requested_freq -= freq_step;
		else
			requested_freq = policy->min;

		__cpufreq_driver_target(policy, requested_freq, CPUFREQ_RELATION_L);
		dbs_info->requested_freq = requested_freq;
	}

 out:
	return dbs_data->sampling_rate;
}

static void recalculate_down_threshold(struct dbs_data *dbs_data)
{
	struct cs_dbs_tuners *cs_tuners = dbs_data->tuners;

	down_threshold_1 = ((up_threshold_0 * DEF_FREQUENCY_STEP_0 / DEF_FREQUENCY_STEP_1) - DOWN_THRESHOLD_MARGIN);
	//down_threshold_2 = ((up_threshold_0 * DEF_FREQUENCY_STEP_1 / DEF_FREQUENCY_STEP_2) - DOWN_THRESHOLD_MARGIN);

	cs_tuners->dynamic_down_threshold = down_threshold_1;
}

/************************** sysfs interface ************************/

static ssize_t store_sampling_down_factor(struct gov_attr_set *attr_set,
					  const char *buf, size_t count)
{
	struct dbs_data *dbs_data = to_dbs_data(attr_set);
	unsigned int input;
	int ret;
	ret = sscanf(buf, "%u", &input);

	if (ret != 1 || input > MAX_SAMPLING_DOWN_FACTOR || input < 1)
		return -EINVAL;

	dbs_data->sampling_down_factor = input;
	return count;
}

static ssize_t store_up_threshold(struct gov_attr_set *attr_set,
				  const char *buf, size_t count)
{
	struct dbs_data *dbs_data = to_dbs_data(attr_set);
	unsigned int input;
	int ret;
	ret = sscanf(buf, "%u", &input);

	if (ret != 1 || input > 100 || input < 30)
		return -EINVAL;

	dbs_data->up_threshold = input;
	up_threshold_0 = input;

	recalculate_down_threshold(dbs_data);

	return count;
}
/*
static ssize_t store_down_threshold(struct gov_attr_set *attr_set,
				    const char *buf, size_t count)
{
	struct dbs_data *dbs_data = to_dbs_data(attr_set);
	struct cs_dbs_tuners *cs_tuners = dbs_data->tuners;
	unsigned int input;
	int ret;
	ret = sscanf(buf, "%u", &input);

	if (ret != 1 || input < 1 || input > 100 ||
			input >= dbs_data->up_threshold)
		return -EINVAL;

	cs_tuners->down_threshold = input;
	return count;
}
*/
static ssize_t store_ignore_nice_load(struct gov_attr_set *attr_set,
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

	if (input == dbs_data->ignore_nice_load) /* nothing to do */
		return count;

	dbs_data->ignore_nice_load = input;

	/* we need to re-evaluate prev_cpu_idle */
	gov_update_cpu_data(dbs_data);

	return count;
}

static ssize_t store_freq_step(struct gov_attr_set *attr_set, const char *buf,
			       size_t count)
{
	struct dbs_data *dbs_data = to_dbs_data(attr_set);
	struct cs_dbs_tuners *cs_tuners = dbs_data->tuners;
	unsigned int input;
	int ret;
	ret = sscanf(buf, "%u", &input);

	if (ret != 1)
		return -EINVAL;

	if (input > 100)
		input = 100;

	/*
	 * no need to test here if freq_step is zero as the user might actually
	 * want this, they would be crazy though :)
	 */
	cs_tuners->freq_step = input;
	return count;
}

gov_show_one_common(sampling_rate);
gov_show_one_common(sampling_down_factor);
gov_show_one_common(up_threshold);
gov_show_one_common(ignore_nice_load);
gov_show_one(cs, dynamic_down_threshold);
gov_show_one(cs, freq_step);

gov_attr_rw(sampling_rate);
gov_attr_rw(sampling_down_factor);
gov_attr_rw(up_threshold);
gov_attr_rw(ignore_nice_load);
gov_attr_ro(dynamic_down_threshold);
gov_attr_rw(freq_step);

static struct attribute *cs_attributes[] = {
	&sampling_rate.attr,
	&sampling_down_factor.attr,
	&up_threshold.attr,
	&dynamic_down_threshold.attr,
	&ignore_nice_load.attr,
	&freq_step.attr,
	NULL
};

/************************** sysfs end ************************/

static struct policy_dbs_info *cs_alloc(void)
{
	struct cs_policy_dbs_info *dbs_info;

	dbs_info = kzalloc(sizeof(*dbs_info), GFP_KERNEL);
	return dbs_info ? &dbs_info->policy_dbs : NULL;
}

static void cs_free(struct policy_dbs_info *policy_dbs)
{
	kfree(to_dbs_info(policy_dbs));
}

static int cs_init(struct dbs_data *dbs_data)
{
	struct cs_dbs_tuners *tuners;

	tuners = kzalloc(sizeof(*tuners), GFP_KERNEL);
	if (!tuners)
		return -ENOMEM;

	tuners->dynamic_down_threshold = 0;
	tuners->freq_step = DEF_FREQUENCY_STEP;
	dbs_data->up_threshold = DEF_FREQUENCY_UP_THRESHOLD;
	dbs_data->sampling_down_factor = DEF_SAMPLING_DOWN_FACTOR;
	dbs_data->ignore_nice_load = 0;
	dbs_data->tuners = tuners;
	dbs_data->sampling_rate = DEF_FREQUENCY_SAMPLE_RATE;

	recalculate_down_threshold(dbs_data);

	return 0;
}

static void cs_exit(struct dbs_data *dbs_data)
{
	kfree(dbs_data->tuners);
}

static void cs_start(struct cpufreq_policy *policy)
{
	struct cs_policy_dbs_info *dbs_info = to_dbs_info(policy->governor_data);

	dbs_info->down_skip = 0;
	dbs_info->requested_freq = policy->cur;
}

static struct dbs_governor cs_governor = {
	.gov = CPUFREQ_DBS_GOVERNOR_INITIALIZER("conservative"),
	.kobj_type = { .default_attrs = cs_attributes },
	.gov_dbs_update = cs_dbs_update,
	.alloc = cs_alloc,
	.free = cs_free,
	.init = cs_init,
	.exit = cs_exit,
	.start = cs_start,
};

#define CPU_FREQ_GOV_CONSERVATIVE	(&cs_governor.gov)

static int __init cpufreq_gov_dbs_init(void)
{
	return cpufreq_register_governor(CPU_FREQ_GOV_CONSERVATIVE);
}

static void __exit cpufreq_gov_dbs_exit(void)
{
	cpufreq_unregister_governor(CPU_FREQ_GOV_CONSERVATIVE);
}

MODULE_AUTHOR("Alexander Clouter <alex@digriz.org.uk>");
MODULE_DESCRIPTION("'cpufreq_conservative' - A dynamic cpufreq governor for "
		"Low Latency Frequency Transition capable processors "
		"optimised for use in a battery environment");
MODULE_LICENSE("GPL");

#ifdef CONFIG_CPU_FREQ_DEFAULT_GOV_CONSERVATIVE
struct cpufreq_governor *cpufreq_default_governor(void)
{
	return CPU_FREQ_GOV_CONSERVATIVE;
}

fs_initcall(cpufreq_gov_dbs_init);
#else
module_init(cpufreq_gov_dbs_init);
#endif
module_exit(cpufreq_gov_dbs_exit);
