// SPDX-License-Identifier: GPL-2.0-only
/*
 * coretemp.c - Linux kernel module for hardware monitoring
 *
 * Copyright (C) 2007 Rudolf Marek <r.marek@assembler.cz>
 *
 * Inspired from many hwmon drivers
 */

#define pr_fmt(fmt) KBUILD_MODNAME ": " fmt

#include <linux/module.h>
#include <linux/init.h>
#include <linux/slab.h>
#include <linux/jiffies.h>
#include <linux/hwmon.h>
#include <linux/sysfs.h>
#include <linux/hwmon-sysfs.h>
#include <linux/err.h>
#include <linux/mutex.h>
#include <linux/list.h>
#include <linux/platform_device.h>
#include <linux/cpu.h>
#include <linux/smp.h>
#include <linux/moduleparam.h>
#include <linux/pci.h>
#include <asm/msr.h>
#include <asm/processor.h>
#include <asm/cpu_device_id.h>
#include <linux/sched/isolation.h>
#include <linux/cpufreq.h>
#include <linux/kthread.h>
#include <linux/delay.h>
#include <linux/reboot.h>
#include <linux/pm_qos.h>

#define DRVNAME	"coretemp"

/*
 * force_tjmax only matters when TjMax can't be read from the CPU itself.
 * When set, it replaces the driver's suboptimal heuristic.
 */
static int force_tjmax;
module_param_named(tjmax, force_tjmax, int, 0444);
MODULE_PARM_DESC(tjmax, "TjMax value in degrees Celsius");

#define NUM_REAL_CORES		2	/* Number of Real cores per cpu */
#define CORETEMP_NAME_LENGTH	28	/* String Length of attrs */

/* custom CPU DVFS for Intel Core 2 Duo */
#define MCELSIUS			(1000)
static unsigned int user_cpu_dvfs_max_temp = 80 * MCELSIUS;
static unsigned int cpu_dvfs_max_temp = 0;
static int cpu_dvfs_peak_temp = 0;
static int cpu_temp = 0;
static unsigned int cpu_dvfs_sleep_time = 6;	/* ms */
static unsigned int cpu_dvfs_limit = 0;
static unsigned int cpu_dvfs_min_temp = 0;
static struct task_struct *cpu_dvfs_thread = NULL;
static inline int get_cpu_temp(int cpu);

#define CPU_DVFS_RANGE_TEMP_MIN		(45 * MCELSIUS)
#define CPU_DVFS_RANGE_TEMP_MAX		(90 * MCELSIUS)
#define CPU_DVFS_AVOID_SHUTDOWN_TEMP	(95 * MCELSIUS)
#define CPU_DVFS_SHUTDOWN_TEMP		(100 * MCELSIUS)
#define CPU_DVFS_MARGIN_TEMP		(10 * MCELSIUS)
#define CPU_DVFS_STEP_DOWN_TEMP		(5 * MCELSIUS)
#define CPU_DVFS_DEBUG			(0)

#define FREQ_STEP_0		(1200000)
#define FREQ_STEP_1		(1600000)
#define FREQ_STEP_2		(2000000)

enum coretemp_attr_index {
	ATTR_LABEL,
	ATTR_CRIT_ALARM,
	ATTR_TEMP,
	ATTR_TJMAX,
	ATTR_TTARGET,
	MAX_CORE_ATTRS = ATTR_TJMAX + 1,	/* Maximum no of basic attrs */
	TOTAL_ATTRS = ATTR_TTARGET + 1		/* Maximum no of possible attrs */
};

#ifdef CONFIG_SMP
#define for_each_sibling(i, cpu) \
	for_each_cpu(i, topology_sibling_cpumask(cpu))
#else
#define for_each_sibling(i, cpu)	for (i = 0; false; )
#endif

/*
 * Per-Core Temperature Data
 * @tjmax: The static tjmax value when tjmax cannot be retrieved from
 *		IA32_TEMPERATURE_TARGET MSR.
 * @last_updated: The time when the current temperature value was updated
 *		earlier (in jiffies).
 * @cpu_core_id: The CPU Core from which temperature values should be read
 *		This value is passed as "id" field to rdmsr/wrmsr functions.
 * @status_reg: One of IA32_THERM_STATUS or IA32_PACKAGE_THERM_STATUS,
 *		from where the temperature values should be read.
 * @attr_size:  Total number of pre-core attrs displayed in the sysfs.
 */
struct temp_data {
	int temp;
	int tjmax;
	unsigned long last_updated;
	unsigned int cpu;
	int index;
	u32 cpu_core_id;
	u32 status_reg;
	int attr_size;
	struct device_attribute sd_attrs[TOTAL_ATTRS];
	char attr_name[TOTAL_ATTRS][CORETEMP_NAME_LENGTH];
	struct attribute *attrs[TOTAL_ATTRS + 1];
	struct attribute_group attr_group;
	struct mutex update_lock;
};

/* Platform Data per Physical CPU */
struct platform_data {
	struct device		*hwmon_dev;
	u16			pkg_id;
	int			nr_cores;
	struct ida		ida;
	struct cpumask		cpumask;
	struct temp_data	*pkg_data;
	struct temp_data	**core_data;
	struct device_attribute name_attr;
};

struct tjmax_pci {
	unsigned int device;
	int tjmax;
};

static const struct tjmax_pci tjmax_pci_table[] = {
	{ 0x0708, 110000 },	/* CE41x0 (Sodaville ) */
	{ 0x0c72, 102000 },	/* Atom S1240 (Centerton) */
	{ 0x0c73, 95000 },	/* Atom S1220 (Centerton) */
	{ 0x0c75, 95000 },	/* Atom S1260 (Centerton) */
};

struct tjmax {
	char const *id;
	int tjmax;
};

static const struct tjmax tjmax_table[] = {
	{ "CPU  230", 100000 },		/* Model 0x1c, stepping 2	*/
	{ "CPU  330", 125000 },		/* Model 0x1c, stepping 2	*/
};

struct tjmax_model {
	u8 model;
	u8 mask;
	int tjmax;
};

#define ANY 0xff

static const struct tjmax_model tjmax_model_table[] = {
	{ 0x1c, 10, 100000 },	/* D4xx, K4xx, N4xx, D5xx, K5xx, N5xx */
	{ 0x1c, ANY, 90000 },	/* Z5xx, N2xx, possibly others
				 * Note: Also matches 230 and 330,
				 * which are covered by tjmax_table
				 */
	{ 0x26, ANY, 90000 },	/* Atom Tunnel Creek (Exx), Lincroft (Z6xx)
				 * Note: TjMax for E6xxT is 110C, but CPU type
				 * is undetectable by software
				 */
	{ 0x27, ANY, 90000 },	/* Atom Medfield (Z2460) */
	{ 0x35, ANY, 90000 },	/* Atom Clover Trail/Cloverview (Z27x0) */
	{ 0x36, ANY, 100000 },	/* Atom Cedar Trail/Cedarview (N2xxx, D2xxx)
				 * Also matches S12x0 (stepping 9), covered by
				 * PCI table
				 */
};

static bool is_pkg_temp_data(struct temp_data *tdata)
{
	return tdata->index < 0;
}

static int adjust_tjmax(struct cpuinfo_x86 *c, u32 id, struct device *dev)
{
	/* The 100C is default for both mobile and non mobile CPUs */

	int tjmax = 100000;
	int tjmax_ee = 85000;
	int usemsr_ee = 1;
	int err;
	u32 eax, edx;
	int i;
	u16 devfn = PCI_DEVFN(0, 0);
	struct pci_dev *host_bridge = pci_get_domain_bus_and_slot(0, 0, devfn);

	/*
	 * Explicit tjmax table entries override heuristics.
	 * First try PCI host bridge IDs, followed by model ID strings
	 * and model/stepping information.
	 */
	if (host_bridge && host_bridge->vendor == PCI_VENDOR_ID_INTEL) {
		for (i = 0; i < ARRAY_SIZE(tjmax_pci_table); i++) {
			if (host_bridge->device == tjmax_pci_table[i].device) {
				pci_dev_put(host_bridge);
				return tjmax_pci_table[i].tjmax;
			}
		}
	}
	pci_dev_put(host_bridge);

	for (i = 0; i < ARRAY_SIZE(tjmax_table); i++) {
		if (strstr(c->x86_model_id, tjmax_table[i].id))
			return tjmax_table[i].tjmax;
	}

	for (i = 0; i < ARRAY_SIZE(tjmax_model_table); i++) {
		const struct tjmax_model *tm = &tjmax_model_table[i];
		if (c->x86_model == tm->model &&
		    (tm->mask == ANY || c->x86_stepping == tm->mask))
			return tm->tjmax;
	}

	/* Early chips have no MSR for TjMax */

	if (c->x86_model == 0xf && c->x86_stepping < 4)
		usemsr_ee = 0;

	if (c->x86_model > 0xe && usemsr_ee) {
		u8 platform_id;

		/*
		 * Now we can detect the mobile CPU using Intel provided table
		 * http://softwarecommunity.intel.com/Wiki/Mobility/720.htm
		 * For Core2 cores, check MSR 0x17, bit 28 1 = Mobile CPU
		 */
		err = rdmsr_safe_on_cpu(id, 0x17, &eax, &edx);
		if (err) {
			dev_warn(dev,
				 "Unable to access MSR 0x17, assuming desktop"
				 " CPU\n");
			usemsr_ee = 0;
		} else if (c->x86_model < 0x17 && !(eax & 0x10000000)) {
			/*
			 * Trust bit 28 up to Penryn, I could not find any
			 * documentation on that; if you happen to know
			 * someone at Intel please ask
			 */
			usemsr_ee = 0;
		} else {
			/* Platform ID bits 52:50 (EDX starts at bit 32) */
			platform_id = (edx >> 18) & 0x7;

			/*
			 * Mobile Penryn CPU seems to be platform ID 7 or 5
			 * (guesswork)
			 */
			if (c->x86_model == 0x17 &&
			    (platform_id == 5 || platform_id == 7)) {
				/*
				 * If MSR EE bit is set, set it to 90 degrees C,
				 * otherwise 105 degrees C
				 */
				tjmax_ee = 90000;
				tjmax = 105000;
			}
		}
	}

	if (usemsr_ee) {
		err = rdmsr_safe_on_cpu(id, 0xee, &eax, &edx);
		if (err) {
			dev_warn(dev,
				 "Unable to access MSR 0xEE, for Tjmax, left"
				 " at default\n");
		} else if (eax & 0x40000000) {
			tjmax = tjmax_ee;
		}
	} else if (tjmax == 100000) {
		/*
		 * If we don't use msr EE it means we are desktop CPU
		 * (with exeception of Atom)
		 */
		dev_warn(dev, "Using relative temperature scale!\n");
	}

	return tjmax;
}

static bool cpu_has_tjmax(struct cpuinfo_x86 *c)
{
	u8 model = c->x86_model;

	return model > 0xe &&
	       model != 0x1c &&
	       model != 0x26 &&
	       model != 0x27 &&
	       model != 0x35 &&
	       model != 0x36;
}

static int get_tjmax(struct temp_data *tdata, struct device *dev)
{
	struct cpuinfo_x86 *c = &cpu_data(tdata->cpu);
	int err;
	u32 eax, edx;
	u32 val;

	/* use static tjmax once it is set */
	if (tdata->tjmax)
		return tdata->tjmax;

	/*
	 * A new feature of current Intel(R) processors, the
	 * IA32_TEMPERATURE_TARGET contains the TjMax value
	 */
	err = rdmsr_safe_on_cpu(tdata->cpu, MSR_IA32_TEMPERATURE_TARGET, &eax, &edx);
	if (err) {
		if (cpu_has_tjmax(c))
			dev_warn(dev, "Unable to read TjMax from CPU %u\n", tdata->cpu);
	} else {
		val = (eax >> 16) & 0xff;
		if (val)
			return val * 1000;
	}

	if (force_tjmax) {
		dev_notice(dev, "TjMax forced to %d degrees C by user\n",
			   force_tjmax);
		tdata->tjmax = force_tjmax * 1000;
	} else {
		/*
		 * An assumption is made for early CPUs and unreadable MSR.
		 * NOTE: the calculated value may not be correct.
		 */
		tdata->tjmax = adjust_tjmax(c, tdata->cpu, dev);
	}
	return tdata->tjmax;
}

static int get_ttarget(struct temp_data *tdata, struct device *dev)
{
	u32 eax, edx;
	int tjmax, ttarget_offset, ret;

	/*
	 * ttarget is valid only if tjmax can be retrieved from
	 * MSR_IA32_TEMPERATURE_TARGET
	 */
	if (tdata->tjmax)
		return -ENODEV;

	ret = rdmsr_safe_on_cpu(tdata->cpu, MSR_IA32_TEMPERATURE_TARGET, &eax, &edx);
	if (ret)
		return ret;

	tjmax = (eax >> 16) & 0xff;

	/* Read the still undocumented bits 8:15 of IA32_TEMPERATURE_TARGET. */
	ttarget_offset = (eax >> 8) & 0xff;

	return (tjmax - ttarget_offset) * 1000;
}

/* Keep track of how many zone pointers we allocated in init() */
static int max_zones __read_mostly;
/* Array of zone pointers. Serialized by cpu hotplug lock */
static struct platform_device **zone_devices;

static ssize_t show_label(struct device *dev,
				struct device_attribute *devattr, char *buf)
{
	struct platform_data *pdata = dev_get_drvdata(dev);
	struct temp_data *tdata = container_of(devattr, struct temp_data, sd_attrs[ATTR_LABEL]);

	if (is_pkg_temp_data(tdata))
		return sprintf(buf, "Package id %u\n", pdata->pkg_id);

	return sprintf(buf, "Core %u\n", tdata->cpu_core_id);
}

static ssize_t show_crit_alarm(struct device *dev,
				struct device_attribute *devattr, char *buf)
{
	u32 eax, edx;
	struct temp_data *tdata = container_of(devattr, struct temp_data,
						sd_attrs[ATTR_CRIT_ALARM]);

	mutex_lock(&tdata->update_lock);
	rdmsr_on_cpu(tdata->cpu, tdata->status_reg, &eax, &edx);
	mutex_unlock(&tdata->update_lock);

	return sprintf(buf, "%d\n", (eax >> 5) & 1);
}

static ssize_t show_tjmax(struct device *dev,
			struct device_attribute *devattr, char *buf)
{
	struct temp_data *tdata = container_of(devattr, struct temp_data, sd_attrs[ATTR_TJMAX]);
	int tjmax;

	mutex_lock(&tdata->update_lock);
	tjmax = get_tjmax(tdata, dev);
	mutex_unlock(&tdata->update_lock);

	return sprintf(buf, "%d\n", tjmax);
}

static ssize_t show_ttarget(struct device *dev,
				struct device_attribute *devattr, char *buf)
{
	struct temp_data *tdata = container_of(devattr, struct temp_data, sd_attrs[ATTR_TTARGET]);
	int ttarget;

	mutex_lock(&tdata->update_lock);
	ttarget = get_ttarget(tdata, dev);
	mutex_unlock(&tdata->update_lock);

	if (ttarget < 0)
		return ttarget;
	return sprintf(buf, "%d\n", ttarget);
}

static ssize_t show_temp(struct device *dev,
			struct device_attribute *devattr, char *buf)
{
	u32 eax, edx;
	struct temp_data *tdata = container_of(devattr, struct temp_data, sd_attrs[ATTR_TEMP]);
	int tjmax;

	mutex_lock(&tdata->update_lock);

	tjmax = get_tjmax(tdata, dev);
	/* Check whether the time interval has elapsed */
	if (time_after(jiffies, tdata->last_updated + HZ)) {
		rdmsr_on_cpu(tdata->cpu, tdata->status_reg, &eax, &edx);
		/*
		 * Ignore the valid bit. In all observed cases the register
		 * value is either low or zero if the valid bit is 0.
		 * Return it instead of reporting an error which doesn't
		 * really help at all.
		 */
		tdata->temp = tjmax - ((eax >> 16) & 0xff) * 1000;
		tdata->last_updated = jiffies;
	}

	mutex_unlock(&tdata->update_lock);
	return sprintf(buf, "%d\n", tdata->temp);
}

static int create_core_attrs(struct temp_data *tdata, struct device *dev)
{
	int i;
	static ssize_t (*const rd_ptr[TOTAL_ATTRS]) (struct device *dev,
			struct device_attribute *devattr, char *buf) = {
			show_label, show_crit_alarm, show_temp, show_tjmax,
			show_ttarget };
	static const char *const suffixes[TOTAL_ATTRS] = {
		"label", "crit_alarm", "input", "crit", "max"
	};

	for (i = 0; i < tdata->attr_size; i++) {
		/*
		 * We map the attr number to core id of the CPU
		 * The attr number is always core id + 2
		 * The Pkgtemp will always show up as temp1_*, if available
		 */
		int attr_no = is_pkg_temp_data(tdata) ? 1 : tdata->cpu_core_id + 2;

		snprintf(tdata->attr_name[i], CORETEMP_NAME_LENGTH,
			 "temp%d_%s", attr_no, suffixes[i]);
		sysfs_attr_init(&tdata->sd_attrs[i].attr);
		tdata->sd_attrs[i].attr.name = tdata->attr_name[i];
		tdata->sd_attrs[i].attr.mode = 0444;
		tdata->sd_attrs[i].show = rd_ptr[i];
		tdata->attrs[i] = &tdata->sd_attrs[i].attr;
	}
	tdata->attr_group.attrs = tdata->attrs;
	return sysfs_create_group(&dev->kobj, &tdata->attr_group);
}

#define ATTR_RW(_name) \
	static struct kobj_attribute _name##_attr = \
		__ATTR(_name, 0644, _name##_show, _name##_store)

static ssize_t cpu_dvfs_max_temp_show(struct kobject *kobj, struct kobj_attribute *attr, char *buf)
{
	unsigned int offset = 0;

	offset += sprintf(buf + offset, "[cpu0_temp]\t%d °C\n", (get_cpu_temp(0) / MCELSIUS));
	offset += sprintf(buf + offset, "[cpu1_temp]\t%d °C\n", (get_cpu_temp(1) / MCELSIUS));
	offset += sprintf(buf + offset, "[peak_temp]\t%d °C\n", (cpu_dvfs_peak_temp / MCELSIUS));
	offset += sprintf(buf + offset, "[user_max_temp]\t%u °C\n", (user_cpu_dvfs_max_temp / MCELSIUS));
	offset += sprintf(buf + offset, "[cal_max_temp]\t%u °C\n", (cpu_dvfs_max_temp / MCELSIUS));
	offset += sprintf(buf + offset, "[dvfs_avoid_shutdown_temp]\t%d °C\n", ((int)CPU_DVFS_AVOID_SHUTDOWN_TEMP / MCELSIUS));
	offset += sprintf(buf + offset, "[dvfs_shutdown_temp]\t%d °C\n", ((int)CPU_DVFS_SHUTDOWN_TEMP / MCELSIUS));
	offset += sprintf(buf + offset, "[cpu_max_freq]\t%u KHz\n", cpu_max_freq);
	offset += sprintf(buf + offset, "[cpu_dvfs_limit]\t%u KHz\n", cpu_dvfs_limit);

	return strlen(buf);
}

static ssize_t cpu_dvfs_max_temp_store(struct kobject *kobj, struct kobj_attribute *attr, const char *buf, size_t count)
{
	unsigned int tmp = 0;

	if (sscanf(buf, "%u", &tmp)) {
		if ((tmp < CPU_DVFS_RANGE_TEMP_MIN / MCELSIUS) || (tmp > CPU_DVFS_RANGE_TEMP_MAX / MCELSIUS)) {
			pr_err("%s: CPU DVFS: out of range %d - %d C\n", __func__, (int)CPU_DVFS_RANGE_TEMP_MIN / MCELSIUS, (int)CPU_DVFS_RANGE_TEMP_MAX / MCELSIUS);
			return -EINVAL;
		}
		user_cpu_dvfs_max_temp = tmp * MCELSIUS;
		sanitize_cpu_dvfs(false);
		return count;
	}

	pr_err("%s: CPU DVFS: invalid input\n", __func__);
	return -EINVAL;
}
ATTR_RW(cpu_dvfs_max_temp);

static struct attribute *coretemp_attrs[] = {
	&cpu_dvfs_max_temp_attr.attr,
	NULL,
};

static struct attribute_group coretemp_attr_group = {
	.attrs = coretemp_attrs,
};

static struct kobject *coretemp_kobject;

static inline void set_cpu_dvfs_limit(unsigned int freq)
{
	struct cpufreq_policy *policy;

	if (freq > cpu_max_freq)
		freq = cpu_max_freq;

	if (cpu_dvfs_limit == freq)
		return;

	policy = cpufreq_cpu_get(0);
	if (!policy) {
		pr_warn("%s: CPU DVFS: cpufreq policy is not ready!\n", __func__);
		return;
	}

	freq_qos_update_request(policy->max_freq_req, freq);
	cpufreq_cpu_put(policy);
	cpu_dvfs_limit = freq;
}

inline void sanitize_cpu_dvfs(bool sanitize)
{
	if (!sanitize) {
		cpu_dvfs_max_temp = user_cpu_dvfs_max_temp;
		cpu_dvfs_peak_temp = 0;
		set_cpu_dvfs_limit(cpu_max_freq);
	} else {
		cpu_dvfs_max_temp -= CPU_DVFS_STEP_DOWN_TEMP;
	}

	cpu_dvfs_min_temp = (cpu_dvfs_max_temp - CPU_DVFS_MARGIN_TEMP);
}

static inline int cpu_dvfs_check_thread(void *null)
{
	unsigned int freq = 0;
	static int prev_temp = 0;

	while (!kthread_should_stop()) {
		if (!cpu_max_freq) {
			pr_warn("%s: CPU DVFS: waiting for cpufreq driver ...\n", __func__);
			msleep(500);
			continue;
		}
		break;
	}

	sanitize_cpu_dvfs(false);
	cpu_dvfs_limit = cpu_max_freq;
	pr_info("%s: CPU DVFS: kthread started successfully.\n", __func__);

	while (!kthread_should_stop()) {

		cpu_temp = max(get_cpu_temp(0), get_cpu_temp(1));

		if (cpu_temp == prev_temp) {
			msleep(cpu_dvfs_sleep_time);
			continue;
		}

		if (cpu_temp > cpu_dvfs_peak_temp) {
			cpu_dvfs_peak_temp = cpu_temp;
#if CPU_DVFS_DEBUG
			pr_info("%s: CPU DVFS: peak_temp: %d C\n", __func__, cpu_dvfs_peak_temp / MCELSIUS);
#endif
		}

		if (cpu_temp >= CPU_DVFS_SHUTDOWN_TEMP) {
			pr_err("%s: CPU DVFS: CPU_DVFS_SHUTDOWN_TEMP %u C reached! - CURR_TEMP: %d C! - cal cpu_dvfs_max_temp: %u C - cpu_dvfs_limit: %u KHz\n", 
					__func__, CPU_DVFS_SHUTDOWN_TEMP / MCELSIUS, cpu_temp / MCELSIUS, cpu_dvfs_max_temp / MCELSIUS, cpu_dvfs_limit);
			sanitize_cpu_dvfs(true);
			freq = FREQ_STEP_0;
			set_cpu_dvfs_limit(freq);
			pr_err("%s: CPU DVFS: shutting down ...\n", __func__);
			emergency_sync();
			kernel_power_off();
			break;
		}

		if (cpu_temp >= CPU_DVFS_AVOID_SHUTDOWN_TEMP) {
			pr_warn("%s: CPU DVFS: CPU_DVFS_AVOID_SHUTDOWN_TEMP %u C reached! - CURR_TEMP: %d C! - cal cpu_dvfs_max_temp: %u C, calibrating to: %u C ... - cpu_dvfs_limit: %u KHz\n", 
					__func__, CPU_DVFS_AVOID_SHUTDOWN_TEMP / MCELSIUS, cpu_temp / MCELSIUS, cpu_dvfs_max_temp / MCELSIUS, ((cpu_dvfs_max_temp / MCELSIUS) - (CPU_DVFS_STEP_DOWN_TEMP / MCELSIUS)), cpu_dvfs_limit);
			sanitize_cpu_dvfs(true);
			freq = FREQ_STEP_0;

		} else if (cpu_temp >= cpu_dvfs_max_temp) {
			if (cpu_dvfs_limit == FREQ_STEP_2)
				freq = FREQ_STEP_1;
			else
				freq = FREQ_STEP_0;

		} else if (cpu_temp <= cpu_dvfs_min_temp) {
			if (cpu_dvfs_limit == FREQ_STEP_0)
				freq = FREQ_STEP_1;
			else
				freq = FREQ_STEP_2;
		}

		prev_temp = cpu_temp;
		set_cpu_dvfs_limit(freq);
		msleep(cpu_dvfs_sleep_time);
		continue;
	}

	return 0;
}

static int chk_ucode_version(unsigned int cpu)
{
	struct cpuinfo_x86 *c = &cpu_data(cpu);

	/*
	 * Check if we have problem with errata AE18 of Core processors:
	 * Readings might stop update when processor visited too deep sleep,
	 * fixed for stepping D0 (6EC).
	 */
	if (c->x86_model == 0xe && c->x86_stepping < 0xc && c->microcode < 0x39) {
		pr_err("Errata AE18 not fixed, update BIOS or microcode of the CPU!\n");
		return -ENODEV;
	}
	return 0;
}

static struct platform_device *coretemp_get_pdev(unsigned int cpu)
{
	int id = topology_logical_die_id(cpu);

	if (id >= 0 && id < max_zones)
		return zone_devices[id];
	return NULL;
}

static struct temp_data *
init_temp_data(struct platform_data *pdata, unsigned int cpu, int pkg_flag)
{
	struct temp_data *tdata;

	if (!pdata->core_data) {
		/*
		 * TODO:
		 * The information of actual possible cores in a package is broken for now.
		 * Will replace hardcoded NUM_REAL_CORES with actual per package core count
		 * when this information becomes available.
		 */
		pdata->nr_cores = NUM_REAL_CORES;
		pdata->core_data = kcalloc(pdata->nr_cores, sizeof(struct temp_data *),
					   GFP_KERNEL);
		if (!pdata->core_data)
			return NULL;
	}

	tdata = kzalloc(sizeof(struct temp_data), GFP_KERNEL);
	if (!tdata)
		return NULL;

	if (pkg_flag) {
		pdata->pkg_data = tdata;
		/* Use tdata->index as indicator of package temp data */
		tdata->index = -1;
	} else {
		tdata->index = ida_alloc_max(&pdata->ida, pdata->nr_cores - 1, GFP_KERNEL);
		if (tdata->index < 0) {
			kfree(tdata);
			return NULL;
		}
		pdata->core_data[tdata->index] = tdata;
	}

	tdata->status_reg = pkg_flag ? MSR_IA32_PACKAGE_THERM_STATUS :
							MSR_IA32_THERM_STATUS;
	tdata->cpu = cpu;
	tdata->cpu_core_id = topology_core_id(cpu);
	tdata->attr_size = MAX_CORE_ATTRS;
	mutex_init(&tdata->update_lock);
	return tdata;
}

static void destroy_temp_data(struct platform_data *pdata, struct temp_data *tdata)
{
	if (is_pkg_temp_data(tdata)) {
		pdata->pkg_data = NULL;
		kfree(pdata->core_data);
		pdata->core_data = NULL;
		pdata->nr_cores = 0;
	} else {
		pdata->core_data[tdata->index] = NULL;
		ida_free(&pdata->ida, tdata->index);
	}
	kfree(tdata);
}

static struct temp_data *get_temp_data(struct platform_data *pdata, int cpu)
{
	int i;

	/* cpu < 0 means get pkg temp_data */
	if (cpu < 0)
		return pdata->pkg_data;

	for (i = 0; i < pdata->nr_cores; i++) {
		if (pdata->core_data[i] &&
		    pdata->core_data[i]->cpu_core_id == topology_core_id(cpu))
			return pdata->core_data[i];
	}
	return NULL;
}

static inline int get_cpu_temp(int cpu)
{
	struct platform_device *pdev = coretemp_get_pdev(cpu);
	struct platform_data *pd;
	struct temp_data *tdata;
	u32 eax, edx;

	pd = platform_get_drvdata(pdev);
	tdata = get_temp_data(pd, cpu);

	mutex_lock(&tdata->update_lock);

	rdmsr_on_cpu(tdata->cpu, tdata->status_reg, &eax, &edx);
	tdata->temp = tdata->tjmax - ((eax >> 16) & 0xff) * MCELSIUS;

	mutex_unlock(&tdata->update_lock);

	return (tdata->temp);
}

static int create_core_data(struct platform_device *pdev, unsigned int cpu,
			    int pkg_flag)
{
	struct temp_data *tdata;
	struct platform_data *pdata = platform_get_drvdata(pdev);
	struct cpuinfo_x86 *c = &cpu_data(cpu);
	u32 eax, edx;
	int err;

	if (!housekeeping_cpu(cpu, HK_TYPE_MISC))
		return 0;

	tdata = init_temp_data(pdata, cpu, pkg_flag);
	if (!tdata)
		return -ENOMEM;

	/* Test if we can access the status register */
	err = rdmsr_safe_on_cpu(cpu, tdata->status_reg, &eax, &edx);
	if (err)
		goto err;

	/* Make sure tdata->tjmax is a valid indicator for dynamic/static tjmax */
	get_tjmax(tdata, &pdev->dev);

	/*
	 * The target temperature is available on older CPUs but not in the
	 * MSR_IA32_TEMPERATURE_TARGET register. Atoms don't have the register
	 * at all.
	 */
	if (c->x86_model > 0xe && c->x86_model != 0x1c)
		if (get_ttarget(tdata, &pdev->dev) >= 0)
			tdata->attr_size++;

	/* Create sysfs interfaces */
	err = create_core_attrs(tdata, pdata->hwmon_dev);
	if (err)
		goto err;

	return 0;

err:
	destroy_temp_data(pdata, tdata);
	return err;
}

static void
coretemp_add_core(struct platform_device *pdev, unsigned int cpu, int pkg_flag)
{
	if (create_core_data(pdev, cpu, pkg_flag))
		dev_err(&pdev->dev, "Adding Core %u failed\n", cpu);
}

static void coretemp_remove_core(struct platform_data *pdata, struct temp_data *tdata)
{
	/* if we errored on add then this is already gone */
	if (!tdata)
		return;

	/* Remove the sysfs attributes */
	sysfs_remove_group(&pdata->hwmon_dev->kobj, &tdata->attr_group);

	destroy_temp_data(pdata, tdata);
}

static int coretemp_device_add(int zoneid)
{
	struct platform_device *pdev;
	struct platform_data *pdata;
	int err;

	/* Initialize the per-zone data structures */
	pdata = kzalloc(sizeof(*pdata), GFP_KERNEL);
	if (!pdata)
		return -ENOMEM;

	pdata->pkg_id = zoneid;
	ida_init(&pdata->ida);

	pdev = platform_device_alloc(DRVNAME, zoneid);
	if (!pdev) {
		err = -ENOMEM;
		goto err_free_pdata;
	}

	err = platform_device_add(pdev);
	if (err)
		goto err_put_dev;

	platform_set_drvdata(pdev, pdata);
	zone_devices[zoneid] = pdev;
	return 0;

err_put_dev:
	platform_device_put(pdev);
err_free_pdata:
	kfree(pdata);
	return err;
}

static void coretemp_device_remove(int zoneid)
{
	struct platform_device *pdev = zone_devices[zoneid];
	struct platform_data *pdata = platform_get_drvdata(pdev);

	ida_destroy(&pdata->ida);
	kfree(pdata);
	platform_device_unregister(pdev);
}

static int coretemp_cpu_online(unsigned int cpu)
{
	struct platform_device *pdev = coretemp_get_pdev(cpu);
	struct cpuinfo_x86 *c = &cpu_data(cpu);
	struct platform_data *pdata;

	/*
	 * Don't execute this on resume as the offline callback did
	 * not get executed on suspend.
	 */
	if (cpuhp_tasks_frozen)
		return 0;

	/*
	 * CPUID.06H.EAX[0] indicates whether the CPU has thermal
	 * sensors. We check this bit only, all the early CPUs
	 * without thermal sensors will be filtered out.
	 */
	if (!cpu_has(c, X86_FEATURE_DTHERM))
		return -ENODEV;

	pdata = platform_get_drvdata(pdev);
	if (!pdata->hwmon_dev) {
		struct device *hwmon;

		/* Check the microcode version of the CPU */
		if (chk_ucode_version(cpu))
			return -EINVAL;

		/*
		 * Alright, we have DTS support.
		 * We are bringing the _first_ core in this pkg
		 * online. So, initialize per-pkg data structures and
		 * then bring this core online.
		 */
		hwmon = hwmon_device_register_with_groups(&pdev->dev, DRVNAME,
							  pdata, NULL);
		if (IS_ERR(hwmon))
			return PTR_ERR(hwmon);
		pdata->hwmon_dev = hwmon;

		/*
		 * Check whether pkgtemp support is available.
		 * If so, add interfaces for pkgtemp.
		 */
		if (cpu_has(c, X86_FEATURE_PTS))
			coretemp_add_core(pdev, cpu, 1);
	}

	/*
	 * Check whether a thread sibling is already online. If not add the
	 * interface for this CPU core.
	 */
	if (!cpumask_intersects(&pdata->cpumask, topology_sibling_cpumask(cpu)))
		coretemp_add_core(pdev, cpu, 0);

	cpumask_set_cpu(cpu, &pdata->cpumask);

	return 0;
}

static int coretemp_cpu_offline(unsigned int cpu)
{
	struct platform_device *pdev = coretemp_get_pdev(cpu);
	struct platform_data *pd;
	struct temp_data *tdata;
	int target;

	/* No need to tear down any interfaces for suspend */
	if (cpuhp_tasks_frozen)
		return 0;

	/* If the physical CPU device does not exist, just return */
	pd = platform_get_drvdata(pdev);
	if (!pd->hwmon_dev)
		return 0;

	tdata = get_temp_data(pd, cpu);

	cpumask_clear_cpu(cpu, &pd->cpumask);

	/*
	 * If this is the last thread sibling, remove the CPU core
	 * interface, If there is still a sibling online, transfer the
	 * target cpu of that core interface to it.
	 */
	target = cpumask_any_and(&pd->cpumask, topology_sibling_cpumask(cpu));
	if (target >= nr_cpu_ids) {
		coretemp_remove_core(pd, tdata);
	} else if (tdata && tdata->cpu == cpu) {
		mutex_lock(&tdata->update_lock);
		tdata->cpu = target;
		mutex_unlock(&tdata->update_lock);
	}

	/*
	 * If all cores in this pkg are offline, remove the interface.
	 */
	tdata = get_temp_data(pd, -1);
	if (cpumask_empty(&pd->cpumask)) {
		if (tdata)
			coretemp_remove_core(pd, tdata);
		hwmon_device_unregister(pd->hwmon_dev);
		pd->hwmon_dev = NULL;
		return 0;
	}

	/*
	 * Check whether this core is the target for the package
	 * interface. We need to assign it to some other cpu.
	 */
	if (tdata && tdata->cpu == cpu) {
		target = cpumask_first(&pd->cpumask);
		mutex_lock(&tdata->update_lock);
		tdata->cpu = target;
		mutex_unlock(&tdata->update_lock);
	}
	return 0;
}
static const struct x86_cpu_id __initconst coretemp_ids[] = {
	X86_MATCH_VENDOR_FEATURE(INTEL, X86_FEATURE_DTHERM, NULL),
	{}
};
MODULE_DEVICE_TABLE(x86cpu, coretemp_ids);

static enum cpuhp_state coretemp_hp_online;

static int __init coretemp_init(void)
{
	int i, err;

	/*
	 * CPUID.06H.EAX[0] indicates whether the CPU has thermal
	 * sensors. We check this bit only, all the early CPUs
	 * without thermal sensors will be filtered out.
	 */
	if (!x86_match_cpu(coretemp_ids))
		return -ENODEV;

	max_zones = topology_max_packages() * topology_max_dies_per_package();
	zone_devices = kcalloc(max_zones, sizeof(struct platform_device *),
			      GFP_KERNEL);
	if (!zone_devices)
		return -ENOMEM;

	for (i = 0; i < max_zones; i++) {
		err = coretemp_device_add(i);
		if (err)
			goto outzone;
	}

	err = cpuhp_setup_state(CPUHP_AP_ONLINE_DYN, "hwmon/coretemp:online",
				coretemp_cpu_online, coretemp_cpu_offline);
	if (err < 0)
		goto outzone;
	coretemp_hp_online = err;

	coretemp_kobject = kobject_create_and_add("coretemp", kernel_kobj);

	if (!coretemp_kobject)
		pr_err("%s: CPU DVFS: Failed to create kobjects\n", __func__);

	err = sysfs_create_group(coretemp_kobject, &coretemp_attr_group);
	if (err) {
		pr_err("%s: CPU DVFS: Failed to register sysfs\n", __func__);
		kobject_put(coretemp_kobject);
	}

	cpu_dvfs_thread = kthread_run(cpu_dvfs_check_thread, NULL, "cpu_dvfsd");
	if (IS_ERR(cpu_dvfs_thread))
		pr_err("%s: CPU DVFS: failed to create and start kthread.", __func__);

	set_cpus_allowed_ptr(cpu_dvfs_thread, cpu_all_mask);
	set_user_nice(cpu_dvfs_thread, MIN_NICE);

	return 0;

outzone:
	while (i--)
		coretemp_device_remove(i);
	kfree(zone_devices);
	return err;
}
module_init(coretemp_init)

static void __exit coretemp_exit(void)
{
	int i;

	cpuhp_remove_state(coretemp_hp_online);
	for (i = 0; i < max_zones; i++)
		coretemp_device_remove(i);
	kfree(zone_devices);
}
module_exit(coretemp_exit)

MODULE_AUTHOR("Rudolf Marek <r.marek@assembler.cz>");
MODULE_DESCRIPTION("Intel Core temperature monitor");
MODULE_LICENSE("GPL");
