/*
 * sec_argos.c
 *
 * Copyright (c) 2012 Samsung Electronics Co., Ltd
 *              http://www.samsung.com
 *
 *  This program is free software; you can redistribute  it and/or modify it
 *  under  the terms of  the GNU General  Public License as published by the
 *  Free Software Foundation;  either version 2 of the  License, or (at your
 *  option) any later version.
 *
 */

#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/device.h>
#include <linux/pm_qos.h>
#include <linux/reboot.h>
#include <linux/of.h>
#include <linux/gfp.h>
#include <linux/platform_device.h>
#include <linux/delay.h>
#include <linux/sched.h>
#include <linux/slab.h>
#include <linux/list.h>
#include <linux/cpumask.h>
#include <linux/cpufreq.h>
#include <linux/interrupt.h>
#include <linux/sec_argos.h>
#include <soc/samsung/exynos_pm_qos.h>
//#include <linux/ologk.h>

#if defined(CONFIG_SCHED_EMS)
#include <linux/ems.h>
#if defined(CONFIG_SCHED_EMS_TUNE)
static struct emstune_mode_request emstune_req;
#else
static struct gb_qos_request gb_req = {
	.name = "argos_global_boost",
};
#endif
#endif

#define ARGOS_NAME "argos"
#define TYPE_SHIFT 4
#define TYPE_MASK_BIT ((1 << TYPE_SHIFT) - 1)

#define IS_FREQ_QOS_ACTIVE(x) ((x) && ((unsigned long)(void *)(x) < (unsigned long)(-4095)))

static DEFINE_SPINLOCK(argos_irq_lock);
static DEFINE_SPINLOCK(argos_task_lock);

enum {
	THRESHOLD,
#if (CONFIG_ARGOS_CLUSTER_NUM > 1)
	BIG_MIN_FREQ,
	BIG_MAX_FREQ,
#endif
#if (CONFIG_ARGOS_CLUSTER_NUM > 2)
	MID_MIN_FREQ,
	MID_MAX_FREQ,
#endif
	LIT_MIN_FREQ,
	LIT_MAX_FREQ,
	MIF_FREQ,
	INT_FREQ,
	TASK_AFFINITY_EN,
	IRQ_AFFINITY_EN,
	HMP_BOOST_EN,
	ITEM_MAX,
};

struct boost_table {
	unsigned int items[ITEM_MAX];
};

struct argos_task_affinity {
	struct task_struct *p;
	struct cpumask *affinity_cpu_mask;
	struct cpumask *default_cpu_mask;
	struct list_head entry;
};

struct argos_irq_affinity {
	unsigned int irq;
	struct cpumask *affinity_cpu_mask;
	struct cpumask *default_cpu_mask;
	struct list_head entry;
};

struct argos_pm_qos {
#if (CONFIG_ARGOS_CLUSTER_NUM > 1)
	struct freq_qos_request big_min_qos_req;
	struct freq_qos_request big_max_qos_req;
#endif
#if (CONFIG_ARGOS_CLUSTER_NUM > 2)
	struct freq_qos_request mid_min_qos_req;
	struct freq_qos_request mid_max_qos_req;
#endif
	struct freq_qos_request lit_min_qos_req;
	struct freq_qos_request lit_max_qos_req;
	struct exynos_pm_qos_request mif_qos_req;
	struct exynos_pm_qos_request int_qos_req;
	struct exynos_pm_qos_request hotplug_min_qos_req;
};

struct argos {
	const char *desc;
	struct platform_device *pdev;
	struct boost_table *tables;
	int ntables;
	int prev_level;
	struct argos_pm_qos *qos;
	struct list_head task_affinity_list;
	bool task_hotplug_disable;
	struct list_head irq_affinity_list;
	bool irq_hotplug_disable;
	bool hmpboost_enable;
	bool slowdown;
	bool argos_block;
	struct blocking_notifier_head argos_notifier;
	/* protect prev_level, qos, task/irq_hotplug_disable, hmpboost_enable */
	struct mutex level_mutex;
};

struct argos_platform_data {
	struct argos *devices;
	int ndevice;
	struct notifier_block pm_qos_nfb;
};

static struct argos_platform_data *argos_pdata;

static inline void UPDATE_PM_QOS(struct exynos_pm_qos_request *req, int class_id, int arg)
{
	if (arg) {
		if (exynos_pm_qos_request_active(req))
			exynos_pm_qos_update_request(req, arg);
		else
			exynos_pm_qos_add_request(req, class_id, arg);
	}
}

static inline void REMOVE_PM_QOS(struct exynos_pm_qos_request *req)
{
	if (exynos_pm_qos_request_active(req))
		exynos_pm_qos_remove_request(req);
}

static inline void UPDATE_FREQ_PM_QOS(struct freq_qos_request *req, int class_id, int arg)
{
	if (arg) {
		if (IS_FREQ_QOS_ACTIVE(req->qos))
			freq_qos_update_request(req, arg);
		else {
			struct cpufreq_policy* policy;
			int type, cpu;

			/* TODO: this is a temporary solution(hard coding). */
			switch (class_id) {
			case PM_QOS_CLUSTER0_FREQ_MIN:
				cpu = 0;
				type = FREQ_QOS_MIN;
				break;
			case PM_QOS_CLUSTER0_FREQ_MAX:
				cpu = 0;
				type = FREQ_QOS_MAX;
				break;
			case PM_QOS_CLUSTER1_FREQ_MIN:
				cpu = 4;
				type = FREQ_QOS_MIN;
				break;
			case PM_QOS_CLUSTER1_FREQ_MAX:
				cpu = 4;
				type = FREQ_QOS_MAX;
				break;
			case PM_QOS_CLUSTER2_FREQ_MIN:
				cpu = 7;
				type = FREQ_QOS_MIN;
				break;
			case PM_QOS_CLUSTER2_FREQ_MAX:
				cpu = 7;
				type = FREQ_QOS_MAX;
				break;
			default:
				pr_err("%s class id %d is invalid.\n", __func__, class_id);
				return;
			}

			policy = cpufreq_cpu_get(cpu);

			if (policy)
				freq_qos_add_request(&policy->constraints, req, type, arg);
			else
				pr_err("%s cpu%d policy is invalid\n", __func__, cpu);
		}
	}
}

static inline void REMOVE_FREQ_PM_QOS(struct freq_qos_request *req)
{
	if (IS_FREQ_QOS_ACTIVE(req->qos))
		freq_qos_remove_request(req);
}

static int argos_find_index(const char *label)
{
	int i;
	int dev_num = -1;

	if (!argos_pdata) {
		pr_err("%s argos not initialized\n", __func__);
		return -1;
	}

	for (i = 0; i < argos_pdata->ndevice; i++)
		if (strcmp(argos_pdata->devices[i].desc, label) == 0)
			dev_num = i;
	return dev_num;
}

int sec_argos_register_notifier(struct notifier_block *n, char *label)
{
	struct blocking_notifier_head *cnotifier;
	int dev_num;

	dev_num = argos_find_index(label);

	if (dev_num < 0) {
		pr_err("%s: No match found for label: %d", __func__, dev_num);
		return -ENODEV;
	}

	cnotifier = &argos_pdata->devices[dev_num].argos_notifier;

	if (!cnotifier) {
		pr_err("%s argos notifier not found(dev_num:%d)\n", __func__, dev_num);
		return -ENXIO;
	}

	pr_info("%s: %ps(dev_num:%d)\n", __func__, n->notifier_call, dev_num);

	return blocking_notifier_chain_register(cnotifier, n);
}
EXPORT_SYMBOL(sec_argos_register_notifier);

int sec_argos_unregister_notifier(struct notifier_block *n, char *label)
{
	struct blocking_notifier_head *cnotifier;
	int dev_num;

	dev_num = argos_find_index(label);

	if (dev_num < 0) {
		pr_err("%s: No match found for label: %d", __func__, dev_num);
		return -ENODEV;
	}

	cnotifier = &argos_pdata->devices[dev_num].argos_notifier;

	if (!cnotifier) {
		pr_err("%s argos notifier not found(dev_num:%d)\n", __func__, dev_num);
		return -ENXIO;
	}

	pr_info("%s: %ps(dev_num:%d)\n", __func__, n->notifier_call, dev_num);

	return blocking_notifier_chain_unregister(cnotifier, n);
}
EXPORT_SYMBOL(sec_argos_unregister_notifier);

static int argos_task_affinity_setup(struct task_struct *p, int dev_num,
				     struct cpumask *affinity_cpu_mask,
				     struct cpumask *default_cpu_mask)
{
	struct argos_task_affinity *this;
	struct list_head *head;

	if (!argos_pdata) {
		pr_err("%s argos not initialized\n", __func__);
		return -ENXIO;
	}

	if (dev_num < 0 || dev_num >= argos_pdata->ndevice) {
		pr_err("%s dev_num:%d should be dev_num:0 ~ %d in boundary\n",
		       __func__, dev_num, argos_pdata->ndevice - 1);
		return -EINVAL;
	}

	head = &argos_pdata->devices[dev_num].task_affinity_list;

	this = kzalloc(sizeof(*this), GFP_ATOMIC);
	if (!this)
		return -ENOMEM;

	this->p = p;
	this->affinity_cpu_mask = affinity_cpu_mask;
	this->default_cpu_mask = default_cpu_mask;

	spin_lock(&argos_task_lock);
	list_add(&this->entry, head);
	spin_unlock(&argos_task_lock);

	return 0;
}

int argos_task_affinity_setup_label(struct task_struct *p, const char *label,
				    struct cpumask *affinity_cpu_mask,
				    struct cpumask *default_cpu_mask)
{
	int dev_num;

	dev_num = argos_find_index(label);

	return argos_task_affinity_setup(p, dev_num, affinity_cpu_mask,
					 default_cpu_mask);
}

static int argos_irq_affinity_setup(unsigned int irq, int dev_num,
				    struct cpumask *affinity_cpu_mask,
				    struct cpumask *default_cpu_mask)
{
	struct argos_irq_affinity *this;
	struct list_head *head;

	if (!argos_pdata) {
		pr_err("%s argos not initialized\n", __func__);
		return -ENXIO;
	}

	if (dev_num < 0 || dev_num >= argos_pdata->ndevice) {
		pr_err("%s dev_num:%d should be dev_num:0 ~ %d in boundary\n",
		       __func__, dev_num, argos_pdata->ndevice - 1);
		return -EINVAL;
	}

	head = &argos_pdata->devices[dev_num].irq_affinity_list;

	this = kzalloc(sizeof(*this), GFP_ATOMIC);
	if (!this)
		return -ENOMEM;

	this->irq = irq;
	this->affinity_cpu_mask = affinity_cpu_mask;
	this->default_cpu_mask = default_cpu_mask;

	spin_lock(&argos_irq_lock);
	list_add(&this->entry, head);
	spin_unlock(&argos_irq_lock);

	return 0;
}

int argos_irq_affinity_setup_label(unsigned int irq, const char *label,
				   struct cpumask *affinity_cpu_mask,
				   struct cpumask *default_cpu_mask)
{
	int dev_num;

	dev_num = argos_find_index(label);

	return argos_irq_affinity_setup(irq, dev_num, affinity_cpu_mask,
			default_cpu_mask);
}

int argos_task_affinity_apply(int dev_num, bool enable)
{
	struct argos_task_affinity *this;
	struct list_head *head;
	int result = 0;
	struct cpumask *mask;
	bool *hotplug_disable;
	struct exynos_pm_qos_request *hotplug_min_qos_req;

	head = &argos_pdata->devices[dev_num].task_affinity_list;
	hotplug_disable = &argos_pdata->devices[dev_num].task_hotplug_disable;
	hotplug_min_qos_req =
		&argos_pdata->devices[dev_num].qos->hotplug_min_qos_req;

	if (list_empty(head)) {
		pr_debug("%s: task_affinity_list is empty\n", __func__);
		return result;
	}

	list_for_each_entry(this, head, entry) {
		if (enable) {
			if (!*hotplug_disable) {
				UPDATE_PM_QOS(hotplug_min_qos_req,
					      PM_QOS_CPU_ONLINE_MIN, num_possible_cpus());
				*hotplug_disable = true;
			}
			mask = this->affinity_cpu_mask;
		} else {
			if (*hotplug_disable) {
				REMOVE_PM_QOS(hotplug_min_qos_req);
				*hotplug_disable = false;
			}
			mask = this->default_cpu_mask;
		}

		result = set_cpus_allowed_ptr(this->p, mask);

		pr_info("%s: %s affinity %s to cpu_mask:0x%X\n",
			__func__, this->p->comm,
			(enable ? "enable" : "disable"),
			(int)*mask->bits);
	}

	return result;
}

int argos_irq_affinity_apply(int dev_num, bool enable)
{
	struct argos_irq_affinity *this;
	struct list_head *head;
	int result = 0;
	struct cpumask *mask;
	bool *hotplug_disable;
	struct exynos_pm_qos_request *hotplug_min_qos_req;

	head = &argos_pdata->devices[dev_num].irq_affinity_list;
	hotplug_disable = &argos_pdata->devices[dev_num].irq_hotplug_disable;
	hotplug_min_qos_req =
		&argos_pdata->devices[dev_num].qos->hotplug_min_qos_req;

	if (list_empty(head)) {
		pr_debug("%s: irq_affinity_list is empty\n", __func__);
		return result;
	}

	list_for_each_entry(this, head, entry) {
		if (enable) {
			if (!*hotplug_disable) {
				UPDATE_PM_QOS(hotplug_min_qos_req,
					      PM_QOS_CPU_ONLINE_MIN, num_possible_cpus());
				*hotplug_disable = true;
			}
			mask = this->affinity_cpu_mask;
		} else {
			if (*hotplug_disable) {
				REMOVE_PM_QOS(hotplug_min_qos_req);
				*hotplug_disable = false;
			}
			mask = this->default_cpu_mask;
		}

		result = irq_set_affinity_hint(this->irq, mask);

		pr_info("%s: irq%d affinity %s to cpu_mask:0x%X\n",
			__func__, this->irq, (enable ? "enable" : "disable"),
			(int)*mask->bits);
	}

	return result;
}

int argos_hmpboost_apply(int dev_num, bool enable)
{
	bool *hmpboost_enable;

	hmpboost_enable = &argos_pdata->devices[dev_num].hmpboost_enable;

	if (enable) {
		/* disable -> enable */
		if (!*hmpboost_enable) {
			/* set global boost */
#if defined(CONFIG_SCHED_EMS)
#if defined(CONFIG_SCHED_EMS_TUNE)
			emstune_boost(&emstune_req, 1);
#else
			gb_qos_update_request(&gb_req, 100);
#endif
#endif
			*hmpboost_enable = true;
			pr_info("%s: hmp boost enable [%d]\n", __func__, dev_num);
		}
	} else {
		/* enable -> disable */
		if (*hmpboost_enable) {
			/* unset global boost */
#if defined(CONFIG_SCHED_EMS)
#if defined(CONFIG_SCHED_EMS_TUNE)
			emstune_boost(&emstune_req, 0);
#else
			gb_qos_update_request(&gb_req, 0);
#endif
#endif
			*hmpboost_enable = false;
			pr_info("%s: hmp boost disable [%d]\n", __func__, dev_num);
		}
	}

	return 0;
}

static void argos_freq_unlock(int type)
{
	struct argos_pm_qos *qos = argos_pdata->devices[type].qos;
	const char *cname;

	cname = argos_pdata->devices[type].desc;

#if (CONFIG_ARGOS_CLUSTER_NUM > 1)
	REMOVE_FREQ_PM_QOS(&qos->big_min_qos_req);
	REMOVE_FREQ_PM_QOS(&qos->big_max_qos_req);
#endif
#if (CONFIG_ARGOS_CLUSTER_NUM > 2)
	REMOVE_FREQ_PM_QOS(&qos->mid_min_qos_req);
	REMOVE_FREQ_PM_QOS(&qos->mid_max_qos_req);
#endif
	REMOVE_FREQ_PM_QOS(&qos->lit_min_qos_req);
	REMOVE_FREQ_PM_QOS(&qos->lit_max_qos_req);
	REMOVE_PM_QOS(&qos->mif_qos_req);
	REMOVE_PM_QOS(&qos->int_qos_req);

	pr_info("%s name:%s\n", __func__, cname);
}

static void argos_freq_lock(int type, int level)
{
#if (CONFIG_ARGOS_CLUSTER_NUM > 1)
	unsigned int big_min_freq, big_max_freq;
#endif
#if (CONFIG_ARGOS_CLUSTER_NUM > 2)
	unsigned int mid_min_freq, mid_max_freq;
#endif
	unsigned int lit_min_freq, lit_max_freq;
	unsigned int mif_freq, int_freq;
	struct boost_table *t = &argos_pdata->devices[type].tables[level];
	struct argos_pm_qos *qos = argos_pdata->devices[type].qos;
	const char *cname;

	cname = argos_pdata->devices[type].desc;

#if (CONFIG_ARGOS_CLUSTER_NUM > 1)
	big_min_freq = t->items[BIG_MIN_FREQ];
	big_max_freq = t->items[BIG_MAX_FREQ];
#endif
#if (CONFIG_ARGOS_CLUSTER_NUM > 2)
	mid_min_freq = t->items[MID_MIN_FREQ];
	mid_max_freq = t->items[MID_MAX_FREQ];
#endif
	lit_min_freq = t->items[LIT_MIN_FREQ];
	lit_max_freq = t->items[LIT_MAX_FREQ];
	mif_freq = t->items[MIF_FREQ];
	int_freq = t->items[INT_FREQ];

#if (CONFIG_ARGOS_CLUSTER_NUM == 3)
	if (big_min_freq)
		UPDATE_FREQ_PM_QOS(&qos->big_min_qos_req,
			      PM_QOS_CLUSTER2_FREQ_MIN, big_min_freq);
	else
		REMOVE_FREQ_PM_QOS(&qos->big_min_qos_req);

	if (big_max_freq)
		UPDATE_FREQ_PM_QOS(&qos->big_max_qos_req,
			      PM_QOS_CLUSTER2_FREQ_MAX, big_max_freq);
	else
		REMOVE_FREQ_PM_QOS(&qos->big_max_qos_req);

	if (mid_min_freq)
		UPDATE_FREQ_PM_QOS(&qos->mid_min_qos_req,
			      PM_QOS_CLUSTER1_FREQ_MIN, mid_min_freq);
	else
		REMOVE_FREQ_PM_QOS(&qos->mid_min_qos_req);

	if (mid_max_freq)
		UPDATE_FREQ_PM_QOS(&qos->mid_max_qos_req,
			      PM_QOS_CLUSTER1_FREQ_MAX, mid_max_freq);
	else
		REMOVE_FREQ_PM_QOS(&qos->mid_max_qos_req);

#elif (CONFIG_ARGOS_CLUSTER_NUM == 2)
	if (big_min_freq)
		UPDATE_FREQ_PM_QOS(&qos->big_min_qos_req,
			      PM_QOS_CLUSTER1_FREQ_MIN, big_min_freq);
	else
		REMOVE_FREQ_PM_QOS(&qos->big_min_qos_req);

	if (big_max_freq)
		UPDATE_FREQ_PM_QOS(&qos->big_max_qos_req,
			      PM_QOS_CLUSTER1_FREQ_MAX, big_max_freq);
	else
		REMOVE_FREQ_PM_QOS(&qos->big_max_qos_req);
#endif

	if (lit_min_freq)
		UPDATE_FREQ_PM_QOS(&qos->lit_min_qos_req,
			      PM_QOS_CLUSTER0_FREQ_MIN, lit_min_freq);
	else
		REMOVE_FREQ_PM_QOS(&qos->lit_min_qos_req);

	if (lit_max_freq)
		UPDATE_FREQ_PM_QOS(&qos->lit_max_qos_req,
			      PM_QOS_CLUSTER0_FREQ_MAX, lit_max_freq);
	else
		REMOVE_FREQ_PM_QOS(&qos->lit_max_qos_req);

	if (mif_freq)
		UPDATE_PM_QOS(&qos->mif_qos_req,
			      PM_QOS_BUS_THROUGHPUT, mif_freq);
	else
		REMOVE_PM_QOS(&qos->mif_qos_req);

	if (int_freq)
		UPDATE_PM_QOS(&qos->int_qos_req,
			      PM_QOS_DEVICE_THROUGHPUT, int_freq);
	else
		REMOVE_PM_QOS(&qos->int_qos_req);

	pr_info("%s name:%s, "
#if (CONFIG_ARGOS_CLUSTER_NUM > 1)
		"BIG_MIN=%d, BIG_MAX=%d, "
#endif
#if (CONFIG_ARGOS_CLUSTER_NUM > 2)
		"MID_MIN=%d, MID_MAX=%d, "
#endif
		"LIT_MIN=%d, LIT_MAX=%d, MIF=%d, INT=%d\n",
		__func__, cname,
#if (CONFIG_ARGOS_CLUSTER_NUM > 1)
		big_min_freq, big_max_freq,
#endif
#if (CONFIG_ARGOS_CLUSTER_NUM > 2)
		mid_min_freq, mid_max_freq,
#endif
		lit_min_freq, lit_max_freq, mif_freq, int_freq);
}

void argos_block_enable(char *req_name, bool set)
{
	int dev_num;
	struct argos *cnode;

	dev_num = argos_find_index(req_name);

	if (dev_num < 0) {
		pr_err("%s: No match found for label: %s", __func__, req_name);
		return;
	}

	cnode = &argos_pdata->devices[dev_num];

	if (set) {
		cnode->argos_block = true;
		mutex_lock(&cnode->level_mutex);
		argos_freq_unlock(dev_num);
		argos_task_affinity_apply(dev_num, 0);
		argos_irq_affinity_apply(dev_num, 0);
		argos_hmpboost_apply(dev_num, 0);
		cnode->prev_level = -1;
		mutex_unlock(&cnode->level_mutex);
	} else {
		cnode->argos_block = false;
	}
	pr_info("%s req_name:%s block:%d\n",
		__func__, req_name, cnode->argos_block);
}

static int argos_cpuidle_reboot_notifier(struct notifier_block *this,
					 unsigned long event, void *_cmd)
{
	switch (event) {
	case SYSTEM_POWER_OFF:
	case SYS_RESTART:
		pr_info("%s called\n", __func__);
		exynos_pm_qos_remove_notifier(PM_QOS_NETWORK_THROUGHPUT,
				       &argos_pdata->pm_qos_nfb);
		break;
	}

	return NOTIFY_OK;
}

static struct notifier_block argos_cpuidle_reboot_nb = {
	.notifier_call = argos_cpuidle_reboot_notifier,
};

static int argos_pm_qos_notify(struct notifier_block *nfb,
			       unsigned long speedtype, void *arg)

{
	int type, level, prev_level, change_level;
	unsigned long speed;
	bool argos_blocked;
	struct argos *cnode;

	type = (speedtype & TYPE_MASK_BIT) - 1;
	if (type < 0 || type > argos_pdata->ndevice) {
		pr_err("There is no type for devices type[%d], ndevice[%d]\n",
		       type, argos_pdata->ndevice);
		return NOTIFY_BAD;
	}

	speed = speedtype >> TYPE_SHIFT;
	cnode = &argos_pdata->devices[type];

	prev_level = cnode->prev_level;
#if 0
	pr_debug("%s name:%s, speed:%ldMbps\n", __func__, cnode->desc, speed);
	if (speed >= 300)
		perflog(PERFLOG_ARGOS, "name:%s, speed:%ldMbps", cnode->desc, speed);
#endif

	argos_blocked = cnode->argos_block;

	/* Find proper level */
	for (level = 0; level < cnode->ntables; level++)
		if (speed < cnode->tables[level].items[THRESHOLD])
			break;

	/* decrease 1 level to match proper table */
	level--;
	if (!argos_blocked) {
		if (level != prev_level) {
			if (mutex_trylock(&cnode->level_mutex) == 0) {
			/*
			 * If the mutex is already locked, it means this argos
			 * is being blocked or is handling another change.
			 * We don't need to wait.
			 */
				pr_warn("%s: skip name:%s, speed:%ldMbps, prev level:%d, request level:%d\n",
					__func__, cnode->desc, speed, prev_level, level);
				goto out;
			}
			pr_info("%s: name:%s, speed:%ldMbps, prev level:%d, request level:%d\n",
				__func__, cnode->desc, speed, prev_level, level);

			change_level = level;

			if (level == -1) {
				if (cnode->argos_notifier.head) {
					pr_debug("%s: Call argos notifier(%s lev:%d)\n",
						 __func__, cnode->desc, level);
					blocking_notifier_call_chain(&cnode->argos_notifier,
								     speed, &level);
				}
				argos_freq_unlock(type);
				argos_task_affinity_apply(type, 0);
				argos_irq_affinity_apply(type, 0);
				argos_hmpboost_apply(type, 0);
			} else {
				unsigned int enable_flag;
				struct boost_table *plevel;

				if (cnode->slowdown) {
					if (prev_level - level == 1) {
						pr_info("%s: skip! apply slowdown scheme. prev level:%d, request level:%d\n",
							__func__, prev_level, level);
						mutex_unlock(&cnode->level_mutex);
						goto out;
					} else if (prev_level - level > 1) {
						change_level = level + 1;
						pr_info("%s: slowdown! request level:%d, change level:%d\n",
							__func__, level, change_level);
					}
				}

				plevel = &argos_pdata->devices[type].tables[change_level];

				argos_freq_lock(type, change_level);

				enable_flag = plevel->items[TASK_AFFINITY_EN];
				argos_task_affinity_apply(type, enable_flag);
				enable_flag = plevel->items[IRQ_AFFINITY_EN];
				argos_irq_affinity_apply(type, enable_flag);
				enable_flag = plevel->items[HMP_BOOST_EN];
				argos_hmpboost_apply(type, enable_flag);

				if (cnode->argos_notifier.head) {
					pr_debug("%s: Call argos notifier(%s lev:%d)\n",
						 __func__, cnode->desc, change_level);
					blocking_notifier_call_chain(&cnode->argos_notifier,
								     speed, &level);
				}
			}

			cnode->prev_level = change_level;
			mutex_unlock(&cnode->level_mutex);
		} else {
			pr_debug("%s:same level (%d) is requested", __func__, level);
		}
	}
out:
	return NOTIFY_OK;
}

#ifdef CONFIG_OF
static int load_table_items(struct device_node *np, struct boost_table *t)
{
	int ret = 0;
	int len = 0;
	const char *status;

	ret = of_property_read_u32(np, "threshold", &t->items[THRESHOLD]);
	if (ret) {
		pr_err("Failed to get speed property\n");
		return -EINVAL;
	}

#if (CONFIG_ARGOS_CLUSTER_NUM > 1)
	ret = of_property_read_u32(np, "big_min", &t->items[BIG_MIN_FREQ]);
	/* If not exist, set to default 0 */
	if (ret == -EINVAL) {
		t->items[BIG_MIN_FREQ] = 0;
	} else if (ret) {
		pr_err("Failed to get big_min\n");
		return ret;
	}

	ret = of_property_read_u32(np, "big_max", &t->items[BIG_MAX_FREQ]);
	/* If not exist, set to default 0 */
	if (ret == -EINVAL) {
		t->items[BIG_MAX_FREQ] = 0;
	} else if (ret) {
		pr_err("Failed to get big_max\n");
		return ret;
	}
#endif

#if (CONFIG_ARGOS_CLUSTER_NUM > 2)
	ret = of_property_read_u32(np, "mid_min", &t->items[MID_MIN_FREQ]);
	/* If not exist, set to default 0 */
	if (ret == -EINVAL) {
		t->items[MID_MIN_FREQ] = 0;
	} else if (ret) {
		pr_err("Failed to get mid_min\n");
		return ret;
	}

	ret = of_property_read_u32(np, "mid_max", &t->items[MID_MAX_FREQ]);
	/* If not exist, set to default 0 */
	if (ret == -EINVAL) {
		t->items[MID_MAX_FREQ] = 0;
	} else if (ret) {
		pr_err("Failed to get mid_max\n");
		return ret;
	}
#endif

	ret = of_property_read_u32(np, "lit_min", &t->items[LIT_MIN_FREQ]);
	/* If not exist, set to default 0 */
	if (ret == -EINVAL) {
		t->items[LIT_MIN_FREQ] = 0;
	} else if (ret) {
		pr_err("Failed to get lit_min\n");
		return ret;
	}

	ret = of_property_read_u32(np, "lit_max", &t->items[LIT_MAX_FREQ]);
	/* If not exist, set to default 0 */
	if (ret == -EINVAL) {
		t->items[LIT_MAX_FREQ] = 0;
	} else if (ret) {
		pr_err("Failed to get lit_max\n");
		return ret;
	}

	ret = of_property_read_u32(np, "mif", &t->items[MIF_FREQ]);
	if (ret == -EINVAL) {
		t->items[MIF_FREQ] = 0;
	} else if (ret) {
		pr_err("Failed to get mif\n");
		return ret;
	}

	ret = of_property_read_u32(np, "int", &t->items[INT_FREQ]);
	if (ret == -EINVAL) {
		t->items[INT_FREQ] = 0;
	} else if (ret) {
		pr_err("Failed to get int\n");
		return ret;
	}

	status = of_get_property(np, "task_affinity", &len);
	if (status && len > 0 && !strcmp(status, "enable"))
		t->items[TASK_AFFINITY_EN] = 1;
	else
		t->items[TASK_AFFINITY_EN] = 0;

	status = of_get_property(np, "irq_affinity", &len);
	if (status && len > 0 && !strcmp(status, "enable"))
		t->items[IRQ_AFFINITY_EN] = 1;
	else
		t->items[IRQ_AFFINITY_EN] = 0;

	status = of_get_property(np, "hmp_boost", &len);
	if (status && len > 0 && !strcmp(status, "enable"))
		t->items[HMP_BOOST_EN] = 1;
	else
		t->items[HMP_BOOST_EN] = 0;

	return 0;
}

static int argos_set_device_node(struct device *dev, struct device_node *np, struct argos *node)
{
	struct device_node *np_table, *np_level;
	int ret = 0;

	if (of_get_property(np, "net_boost,slowdown", NULL))
		node->slowdown = true;
	else
		node->slowdown = false;

	node->desc = of_get_property(np, "net_boost,label", NULL);
	node->qos = devm_kzalloc(dev, sizeof(struct argos_pm_qos), GFP_KERNEL);
	if (!node->qos)
		return -ENOMEM;

	np_table = of_get_child_by_name(np, "net_boost,table");
	if (!of_device_is_available(np_table)) {
		ret = -EINVAL;
		goto err_out;
	}

	/* Allocation for freq and time table */
	node->tables = devm_kzalloc(dev, sizeof(struct boost_table) * of_get_child_count(np_table), GFP_KERNEL);
	if (!node->tables) {
		ret = -ENOMEM;
		goto err_out;
	}

	/* Get and add frequency and time table */
	for_each_child_of_node(np_table, np_level) {
		ret = load_table_items(np_level, &node->tables[node->ntables]);
		if (ret)
			goto err_out;
		node->ntables++;
	}

	INIT_LIST_HEAD(&node->task_affinity_list);
	INIT_LIST_HEAD(&node->irq_affinity_list);
	node->task_hotplug_disable = false;
	node->irq_hotplug_disable = false;
	node->hmpboost_enable = false;
	node->argos_block = false;
	node->prev_level = -1;
	mutex_init(&node->level_mutex);
	BLOCKING_INIT_NOTIFIER_HEAD(&node->argos_notifier);

	return 0;

err_out:
	if (node->tables)
		devm_kfree(dev, node->tables);
	if (node->qos)
		devm_kfree(dev, node->qos);

	return ret;
}

static int argos_parse_dt(struct device *dev)
{
	struct argos_platform_data *pdata = dev->platform_data;
	struct argos *device_node;
	struct device_node *root_np, *device_np;
	int device_count = 0;
	int ret = 0;

	root_np = dev->of_node;
	for_each_child_of_node(root_np, device_np)
		if (!strncmp(device_np->name, "boot_device", 11))
			pdata->ndevice++;

	if (!pdata->ndevice) {
		dev_err(dev, "Failed to get child count\n");
		return -ENODEV;
	}

	pdata->devices = devm_kzalloc(dev, sizeof(struct argos) * pdata->ndevice, GFP_KERNEL);
	if (!pdata->devices)
		return -ENOMEM;

	for_each_child_of_node(root_np, device_np) {
		device_node = &pdata->devices[device_count];
		ret = argos_set_device_node(dev, device_np, device_node);
		if (ret)
			goto err_out;

		device_count++;
	}

	return 0;

err_out:
	if (pdata->devices)
		devm_kfree(dev, pdata->devices);

	return ret;
}
#endif

static int argos_probe(struct platform_device *pdev)
{
	int ret = 0;
	struct argos_platform_data *pdata;

	pr_info("%s+++\n", __func__);
	if (pdev->dev.of_node) {
		pdata = devm_kzalloc(&pdev->dev,
				     sizeof(struct argos_platform_data),
				     GFP_KERNEL);

		if (!pdata) {
			dev_err(&pdev->dev, "Failed to allocate platform data\n");
			return -ENOMEM;
		}

		pdev->dev.platform_data = pdata;
		ret = argos_parse_dt(&pdev->dev);
		if (ret) {
			dev_err(&pdev->dev, "Failed to parse dt data\n");
			goto err_out;
		}
		pr_info("%s: parse dt done\n", __func__);
	} else {
		pdata = pdev->dev.platform_data;
	}

	if (!pdata) {
		dev_err(&pdev->dev, "There are no platform data\n");
		ret = -EINVAL;
		goto err_out;
	}

	if (!pdata->ndevice || !pdata->devices) {
		dev_err(&pdev->dev, "There are no devices\n");
		ret = -EINVAL;
		goto err_out;
	}

	pdata->pm_qos_nfb.notifier_call = argos_pm_qos_notify;
	exynos_pm_qos_add_notifier(PM_QOS_NETWORK_THROUGHPUT, &pdata->pm_qos_nfb);
	register_reboot_notifier(&argos_cpuidle_reboot_nb);
	argos_pdata = pdata;
	platform_set_drvdata(pdev, pdata);

	pr_info("%s---\n", __func__);

	return 0;

err_out:
	if (pdev->dev.of_node && pdata)
		devm_kfree(&pdev->dev, pdata);

	return ret;
}

static int argos_remove(struct platform_device *pdev)
{
	struct argos_platform_data *pdata = platform_get_drvdata(pdev);

	if (!pdata || !argos_pdata)
		return 0;
	exynos_pm_qos_remove_notifier(PM_QOS_NETWORK_THROUGHPUT, &pdata->pm_qos_nfb);
	unregister_reboot_notifier(&argos_cpuidle_reboot_nb);

	return 0;
}

#ifdef CONFIG_OF
static const struct of_device_id argos_dt_ids[] = {
	{ .compatible = "samsung,argos"},
	{ }
};
#endif

static struct platform_driver argos_driver = {
	.driver = {
		.name = ARGOS_NAME,
		.owner = THIS_MODULE,
#ifdef CONFIG_OF
		.of_match_table	= of_match_ptr(argos_dt_ids),
#endif
	},
	.probe = argos_probe,
	.remove = argos_remove
};

module_platform_driver(argos_driver);

MODULE_DESCRIPTION("ARGOS DEVICE");
MODULE_LICENSE("GPL v2");
