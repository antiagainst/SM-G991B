/* linux/arch/arm64/mach-exynos/include/mach/exynos-devfreq.h
 *
 * Copyright (c) 2015 Samsung Electronics Co., Ltd.
 *              http://www.samsung.com
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 as
 * published by the Free Software Foundation.
 */
#ifndef __EXYNOS_DEVFREQ_H_
#define __EXYNOS_DEVFREQ_H_

#include <linux/devfreq.h>
#include <soc/samsung/exynos_pm_qos.h>
#include <linux/clk.h>
#include <soc/samsung/exynos-devfreq-dep.h>
#if defined(CONFIG_EXYNOS_DVFS_MANAGER) || defined(CONFIG_EXYNOS_DVFS_MANAGER_MODULE)
#include <soc/samsung/exynos-dm.h>
#endif

#define EXYNOS_DEVFREQ_MODULE_NAME	"exynos-devfreq"
#define VOLT_STEP			25000
#define MAX_NR_CONSTRAINT		DM_TYPE_END
#define DATA_INIT			5
#define SET_CONST			1
#define RELEASE				2

/* DEVFREQ GOV TYPE */
#define SIMPLE_INTERACTIVE 0

int devfreq_simple_interactive_init(void);
#if defined(CONFIG_EXYNOS_ALT_DVFS) || defined(CONFIG_EXYNOS_ALT_DVFS_MODULE)
#define LOAD_BUFFER_MAX			10
#if defined(CONFIG_EXYNOS_ALT_DVFS_DEBUG) || defined(CONFIG_EXYNOS_ALT_DVFS_DEBUG_MODULE)
#define MAX_LOG_TIME 300
#endif
struct devfreq_alt_load {
	unsigned long long	delta;
	unsigned int		load;
#if defined(CONFIG_EXYNOS_ALT_DVFS_DEBUG) || defined(CONFIG_EXYNOS_ALT_DVFS_DEBUG_MODULE)
	unsigned long long clock;
	unsigned int alt_freq;
#endif
};

#define ALTDVFS_MIN_SAMPLE_TIME 	15
#define ALTDVFS_HOLD_SAMPLE_TIME	100
#define ALTDVFS_TARGET_LOAD		75
#define ALTDVFS_NUM_TARGET_LOAD 	1
#define ALTDVFS_HISPEED_LOAD		99
#define ALTDVFS_HISPEED_FREQ		1000000
#define ALTDVFS_TOLERANCE		1

struct devfreq_alt_dvfs_data {
	struct devfreq_alt_load	buffer[LOAD_BUFFER_MAX];
	struct devfreq_alt_load	*front;
	struct devfreq_alt_load	*rear;

	unsigned long long	busy;
	unsigned long long	total;
	unsigned int		min_load;
	unsigned int		max_load;
	unsigned long long	max_spent;

	struct devfreq_alt_dvfs_param *alt_param;
	struct devfreq_alt_dvfs_param *alt_param_set;
	struct devfreq_alt_dvfs_param *alt_user_mode;
	int default_mode, current_mode, num_modes;
#if defined(CONFIG_EXYNOS_ALT_DVFS_DEBUG) || defined(CONFIG_EXYNOS_ALT_DVFS_DEBUG_MODULE)
	bool				load_track;
	unsigned int		log_top;
	struct devfreq_alt_load *log;
#endif
};

struct devfreq_alt_dvfs_param {
	/* ALT-DVFS parameter */
	unsigned int		*target_load;
	unsigned int		num_target_load;
	unsigned int		min_sample_time;
	unsigned int		hold_sample_time;
	unsigned int		hispeed_load;
	unsigned int		hispeed_freq;
	unsigned int		tolerance;
};
#endif /* ALT_DVFS */

#define DEFAULT_DELAY_TIME		10 /* msec */
#define DEFAULT_NDELAY_TIME		1
#define DELAY_TIME_RANGE		10
#define BOUND_CPU_NUM			0

struct devfreq_notifier_block {
       struct notifier_block nb;
       struct devfreq *df;
};

struct devfreq_simple_interactive_data {
	bool use_delay_time;
	int *delay_time;
	int ndelay_time;
	unsigned long prev_freq;
	u64 changed_time;
	struct timer_list freq_timer;
	struct timer_list freq_slack_timer;
	struct task_struct *change_freq_task;
	int pm_qos_class;
	int pm_qos_class_max;
	struct devfreq_notifier_block nb;
	struct devfreq_notifier_block nb_max;

#if defined(CONFIG_EXYNOS_ALT_DVFS) || defined(CONFIG_EXYNOS_ALT_DVFS_MODULE)
	struct devfreq_alt_dvfs_data alt_data;
	unsigned int governor_freq;
#endif
};

struct exynos_devfreq_opp_table {
	u32 idx;
	u32 freq;
	u32 volt;
};

struct um_exynos {
	struct list_head node;
	void __iomem **va_base;
	u32 *pa_base;
	u32 *mask_v;
	u32 *mask_a;
	u32 *channel;
	unsigned int um_count;
	u64 val_ccnt;
	u64 val_pmcnt;
};

struct exynos_devfreq_freq_infos {
	/* Basic freq infos */
	// min/max frequency
	u32 max_freq;
	u32 min_freq;
	// cur_freq pointer
	const u32 *cur_freq;
	// min/max pm_qos node
	u32 pm_qos_class;
	u32 pm_qos_class_max;
	// num of freqs
	u32 max_state;
};

struct exynos_devfreq_profile {
	int num_stats;
	/* Total time in state */
	ktime_t *time_in_state;
	ktime_t last_time_in_state;

	/* Active time in state */
	ktime_t *active_time_in_state;
	ktime_t last_active_time_in_state;

	u64 **freq_stats;
	u64 *last_freq_stats;
};

struct exynos_devfreq_data {
	struct device				*dev;
	struct devfreq				*devfreq;
	struct mutex				lock;
	struct clk				*clk;

	bool					devfreq_disabled;

	u32		devfreq_type;

	struct exynos_devfreq_opp_table		*opp_list;

	u32					default_qos;

	u32					max_state;
	struct devfreq_dev_profile		devfreq_profile;

	u32		gov_type;
	const char				*governor_name;
	u32					cal_qos_max;
	void					*governor_data;
	struct devfreq_simple_interactive_data	simple_interactive_data;
	u32					dfs_id;
	s32					old_idx;
	s32					new_idx;
	u32					old_freq;
	u32					new_freq;
	u32					min_freq;
	u32					max_freq;
	u32					reboot_freq;
	u32					boot_freq;
	u64					suspend_freq;
	bool					suspend_flag;

	u32					old_volt;
	u32					new_volt;

	u32					pm_qos_class;
	u32					pm_qos_class_max;
	struct exynos_pm_qos_request		sys_pm_qos_min;
#if defined(CONFIG_ARM_EXYNOS_DEVFREQ_DEBUG) || defined(CONFIG_ARM_EXYNOS_DEVFREQ_DEBUG_MODULE)
	struct exynos_pm_qos_request		debug_pm_qos_min;
	struct exynos_pm_qos_request		debug_pm_qos_max;
#endif
	struct exynos_pm_qos_request		default_pm_qos_min;
	struct exynos_pm_qos_request		default_pm_qos_max;
	struct exynos_pm_qos_request		boot_pm_qos;
	u32					boot_qos_timeout;

	struct notifier_block			reboot_notifier;

	u32					ess_flag;

	s32					target_delay;

#if defined(CONFIG_EXYNOS_DVFS_MANAGER) || defined(CONFIG_EXYNOS_DVFS_MANAGER_MODULE)
	u32		dm_type;
	u32		nr_constraint;
	struct exynos_dm_constraint		**constraint;
#endif
	void					*private_data;
	bool					use_acpm;
	bool					bts_update;
	bool					update_fvp;
	bool					use_get_dev;
	bool					use_dtm;

	struct devfreq_notifier_block		*um_nb;
	struct um_exynos			um_data;
	u64					last_monitor_period;
	u64					last_monitor_time;
	u32					last_um_usage_rate;

	struct exynos_pm_domain *pm_domain;
	unsigned long previous_freq;
	unsigned long *time_in_state;
	unsigned long last_stat_updated;
	struct exynos_devfreq_profile		*profile;
};

struct exynos_profile_data {
	unsigned long long total_time;
	unsigned long long busy_time;
	unsigned long long delta_time;
};

s32 exynos_devfreq_get_opp_idx(struct exynos_devfreq_opp_table *table,
				unsigned int size, u32 freq);
#if (defined(CONFIG_ARM_EXYNOS_DEVFREQ) && defined(CONFIG_EXYNOS_DVFS_MANAGER)) &&		\
	(defined(CONFIG_ARM_EXYNOS_DEVFREQ_MODULE) && defined(CONFIG_EXYNOS_DVFS_MANAGER_MODULE))
u32 exynos_devfreq_get_dm_type(u32 devfreq_type);
u32 exynos_devfreq_get_devfreq_type(int dm_type);
struct devfreq *find_exynos_devfreq_device(void *devdata);
int find_exynos_devfreq_dm_type(struct device *dev, int *dm_type);
#endif

#if defined(CONFIG_EXYNOS_ALT_DVFS) || defined(CONFIG_EXYNOS_ALT_DVFS_MODULE)
extern exynos_devfreq_alt_mode_change(unsigned int devfreq_type, int new_mode);
extern void exynos_alt_call_chain(void);
#endif

#if defined(CONFIG_ARM_EXYNOS_DEVFREQ) || defined(CONFIG_ARM_EXYNOS_DEVFREQ_MODULE)
extern unsigned long exynos_devfreq_get_domain_freq(unsigned int devfreq_type);
extern void exynos_devfreq_get_freq_infos(unsigned int devfreq_type, struct exynos_devfreq_freq_infos *infos);
extern void exynos_devfreq_get_profile(unsigned int devfreq_type, ktime_t **time_in_state, u64 **tables);
#else
static inline unsigned long exynos_devfreq_get_domain_freq(unsigned int devfreq_type)
{
	return 0;
}
#endif
#endif	/* __EXYNOS_DEVFREQ_H_ */
