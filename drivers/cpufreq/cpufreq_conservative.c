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

static unsigned int down_threshold;

struct cs_policy_dbs_info {
	struct policy_dbs_info policy_dbs;
	unsigned int down_skip;
};

static inline struct cs_policy_dbs_info *to_dbs_info(struct policy_dbs_info *policy_dbs)
{
	return container_of(policy_dbs, struct cs_policy_dbs_info, policy_dbs);
}

struct cs_dbs_tuners {
	bool boost;
};

/* Conservative governor macros */
#define DEF_FREQUENCY_UP_THRESHOLD		(75)	/* min 40, max 100 */
#define DOWN_THRESHOLD_MARGIN			(25)
#define DEF_SAMPLING_DOWN_FACTOR		(1)
#define MAX_SAMPLING_DOWN_FACTOR		(10)
#define DEF_BOOST				(1)

#define DEF_FREQUENCY_STEP_MHZ_0			(1200)
#define DEF_FREQUENCY_STEP_MHZ_1			(1600)

#define DEF_FREQUENCY_STEP_0			(1200000)
#define DEF_FREQUENCY_STEP_1			(1600000)
#define DEF_FREQUENCY_STEP_2			(2000000)

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
	struct dbs_data *dbs_data = policy_dbs->dbs_data;
	struct cs_dbs_tuners *cs_tuners = dbs_data->tuners;
	unsigned int load = dbs_update(policy);
	unsigned int requested_freq = 0;

	/* Check for frequency increase */
	if (load >= dbs_data->up_threshold) {
		dbs_info->down_skip = 0;

		/* if we are already at full speed then break out early */
		if (policy->cur == policy->max)
			goto out;

		if (!cs_tuners->boost) {
			if (policy->cur == DEF_FREQUENCY_STEP_0)
				requested_freq = DEF_FREQUENCY_STEP_1;
			else if (policy->cur == DEF_FREQUENCY_STEP_1)
				requested_freq = DEF_FREQUENCY_STEP_2;
			else
				goto out;

			if (requested_freq > policy->max)
				requested_freq = policy->max;
		} else {
			/* Boost */
			requested_freq = policy->max;
		}

		__cpufreq_driver_target(policy, requested_freq, CPUFREQ_RELATION_H);
		goto out;
	}

	/*
	 * if we cannot reduce the frequency anymore, break out early
	 */
	if (policy->cur == policy->min)
		goto out;

	/* if sampling_down_factor is active break out early */
	if (++dbs_info->down_skip < dbs_data->sampling_down_factor)
		goto out;
	dbs_info->down_skip = 0;

	/* Check for frequency decrease */
	if (load <= down_threshold) {
		if (policy->cur == DEF_FREQUENCY_STEP_2)
			requested_freq = DEF_FREQUENCY_STEP_1;
		else if (policy->cur == DEF_FREQUENCY_STEP_1)
			requested_freq = DEF_FREQUENCY_STEP_0;
		else
			goto out;

		if (requested_freq < policy->min)
			requested_freq = policy->min;

		__cpufreq_driver_target(policy, requested_freq, CPUFREQ_RELATION_L);
	}

 out:
	return dbs_data->sampling_rate;
}

static void update_down_threshold(struct dbs_data *dbs_data)
{
	down_threshold = ((dbs_data->up_threshold * DEF_FREQUENCY_STEP_MHZ_0 / DEF_FREQUENCY_STEP_MHZ_1) - DOWN_THRESHOLD_MARGIN);
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

	if (ret != 1 || input > 100 || input < 40)
		return -EINVAL;

	dbs_data->up_threshold = input;

	/* update down_threshold */
	update_down_threshold(dbs_data);

	return count;
}

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

static ssize_t store_boost(struct gov_attr_set *attr_set,
					  const char *buf, size_t count)
{
	struct dbs_data *dbs_data = to_dbs_data(attr_set);
	struct cs_dbs_tuners *cs_tuners = dbs_data->tuners;
	unsigned int input;
	int ret;

	ret = sscanf(buf, "%u", &input);
	if (ret != 1)
		return -EINVAL;

	if (input > 1)
		input = 1;

	cs_tuners->boost = input;
	return count;
}

gov_show_one_common(sampling_rate);
gov_show_one_common(sampling_down_factor);
gov_show_one_common(up_threshold);
gov_show_one_common(ignore_nice_load);
gov_show_one(cs, boost);

gov_attr_rw(sampling_rate);
gov_attr_rw(sampling_down_factor);
gov_attr_rw(up_threshold);
gov_attr_rw(ignore_nice_load);
gov_attr_rw(boost);

static struct attribute *cs_attributes[] = {
	&sampling_rate.attr,
	&sampling_down_factor.attr,
	&up_threshold.attr,
	&ignore_nice_load.attr,
	&boost.attr,
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

	tuners->boost = DEF_BOOST;
	dbs_data->up_threshold = DEF_FREQUENCY_UP_THRESHOLD;
	dbs_data->sampling_down_factor = DEF_SAMPLING_DOWN_FACTOR;
	dbs_data->ignore_nice_load = 0;
	dbs_data->tuners = tuners;

	// init down_threshold
	update_down_threshold(dbs_data);

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
