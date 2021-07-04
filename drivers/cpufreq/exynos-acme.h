/*
 * Copyright (c) 2016 Park Bumgyu, Samsung Electronics Co., Ltd <bumgyu.park@samsung.com>
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 as
 * published by the Free Software Foundation.
 *
 * Exynos ACME(A Cpufreq that Meets Every chipset) driver implementation
 */

#include <soc/samsung/exynos-dm.h>
#include <linux/fs.h>
#include <linux/miscdevice.h>

struct exynos_cpufreq_dm {
	struct list_head		list;
	struct exynos_dm_constraint	c;
	int				master_cal_id;
	int				slave_cal_id;
};

typedef int (*target_fn)(struct cpufreq_policy *policy,
			        unsigned int target_freq,
			        unsigned int relation);

struct exynos_cpufreq_ready_block {
	struct list_head		list;

	/* callback function to update policy-dependant data */
	int (*update)(struct cpufreq_policy *policy);
	int (*get_target)(struct cpufreq_policy *policy, target_fn target);
};

struct exynos_cpufreq_file_operations {
	struct file_operations		fops;
	struct miscdevice		miscdev;
	struct freq_constraints		*freq_constraints;
	enum				freq_qos_req_type req_type;
	unsigned int			default_value;
};

struct exynos_cpufreq_domain {
	/* list of domain */
	struct list_head		list;

	/* lock */
	struct mutex			lock;

	/* dt node */
	struct device_node		*dn;

	/* domain identity */
	unsigned int			id;
	struct cpumask			cpus;
	unsigned int			cal_id;
	int				dm_type;

	/* frequency scaling */
	bool				enabled;

	unsigned int			table_size;
	struct cpufreq_frequency_table	*freq_table;

	unsigned int			max_freq;
	unsigned int			min_freq;
	unsigned int			max_freq_qos;
	unsigned int			min_freq_qos;
	unsigned int			boot_freq;
	unsigned int			resume_freq;
	unsigned int			old;
	unsigned int			clipped_freq;

	/* freq qos */
	struct freq_qos_request		min_qos_req;
	struct freq_qos_request		max_qos_req;
	struct freq_qos_request		user_min_qos_req;
	struct freq_qos_request		user_max_qos_req;
	struct delayed_work		work;

	/* fops node */
	struct exynos_cpufreq_file_operations	min_qos_fops;
	struct exynos_cpufreq_file_operations	max_qos_fops;

	/* list head of DVFS Manager constraints */
	struct list_head		dm_list;

	bool				need_awake;

	/* per-domain sysfs support */
	struct kobject			kobj;
};

/*
 * the time it takes on this CPU to switch between
 * two frequencies in nanoseconds
 */
#define TRANSITION_LATENCY	5000000

/*
 * Exynos CPUFreq API
 */
#if IS_ENABLED(CONFIG_ARM_EXYNOS_ACME)
extern void exynos_cpufreq_ready_list_add(struct exynos_cpufreq_ready_block *rb);
#else
static inline void exynos_cpufreq_ready_list_add(struct exynos_cpufreq_ready_block *rb) { }
#endif
