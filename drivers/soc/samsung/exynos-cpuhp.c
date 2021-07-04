/*
 * Copyright (c) 2018 Samsung Electronics Co., Ltd.
 *
 * CPU Part
 *
 * CPU Hotplug driver for Exynos
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 as
 * published by the Free Software Foundation.
 */

#define pr_fmt(fmt)	KBUILD_MODNAME ": " fmt

#include <linux/cpu.h>
#include <linux/fb.h>
#include <linux/kthread.h>
#include <linux/pm_qos.h>
#include <linux/suspend.h>
#include <linux/module.h>
#include <linux/moduleparam.h>
#include <linux/platform_device.h>

#include <soc/samsung/debug-snapshot.h>
#include <soc/samsung/exynos-cpuhp.h>

#define CPUHP_USER_NAME_LEN	16

struct cpuhp_user {
	char			name[CPUHP_USER_NAME_LEN];
	struct cpumask		online_cpus;
	struct list_head	list;
};

static struct {
	/* Control cpu hotplug operation */
	bool			enabled;

	/* flag for suspend */
	bool			suspended;

	/* flag for debug print */
	bool			debug;

	/* list head for requester */
	struct list_head	users;

	/* user for system */
	struct cpuhp_user	system_user;

	/* user for sysfs */
	struct cpuhp_user	sysfs_user;

	/* Synchronizes accesses to refcount and cpumask */
	struct mutex		lock;

	/* user request mask */
	struct cpumask		online_cpus;

	/* available mask */
	struct cpumask		available_cpus;

	/* cpuhp kobject */
	struct kobject		*kobj;
} cpuhp = {
	.lock = __MUTEX_INITIALIZER(cpuhp.lock),
};

/******************************************************************************/
/*                             Helper functions                               */
/******************************************************************************/
static int cpuhp_do(void);

/*
 * Update pm_suspend status.
 * During suspend-resume, cpuhp driver is stop
 */
static inline void cpuhp_suspend(bool enable)
{
	/* This lock guarantees completion of cpuhp_do() */
	mutex_lock(&cpuhp.lock);
	cpuhp.suspended = enable;
	mutex_unlock(&cpuhp.lock);
}

/*
 * Update cpuhp enablestatus.
 * cpuhp driver is working when enabled big is TRUE
 */
static inline void cpuhp_enable(bool enable)
{
	mutex_lock(&cpuhp.lock);
	cpuhp.enabled = enable;
	mutex_unlock(&cpuhp.lock);
}

/* find user matched name. if return NULL, there is no user matched name */
static struct cpuhp_user *cpuhp_find_user(char *name)
{
	struct cpuhp_user *user;

	list_for_each_entry(user, &cpuhp.users, list)
		if (!strcmp(user->name, name))
			return user;

	return NULL;
}

/* update user's requesting  cpu mask */
static int cpuhp_update_user(char *name, struct cpumask mask)
{
	struct cpuhp_user *user = cpuhp_find_user(name);

	if (!user)
		return -EINVAL;

	cpumask_copy(&user->online_cpus, &mask);

	return 0;
}

/******************************************************************************/
/*                               External APIs                                */
/******************************************************************************/
/* remove user from hotplug requesting user list */
int exynos_cpuhp_unregister(char *name, struct cpumask mask)
{
	return 0;
}
EXPORT_SYMBOL_GPL(exynos_cpuhp_unregister);

/*
 * Register cpu-hp user
 * Users and IPs that want to use cpu-hp should register through this function.
 *	name: Must have a unique value, and panic will occur if you use an already
 *	      registered name.
 *	mask: The cpu mask that user wants to ONLINE and cpu OFF bits has more HIGH
 *	      priority than ONLINE bit. This mask is default cpu mask at registration
 *	      and it is reflected immediately after registration.
 */
int exynos_cpuhp_register(char *name, struct cpumask mask)
{
	struct cpuhp_user *user;
	char buf[CPUHP_USER_NAME_LEN];

	mutex_lock(&cpuhp.lock);

	/* check wether name is already register or not */
	if (cpuhp_find_user(name))
		panic("Failed to register cpuhp user! already existed\n");

	/* allocate memory for new user */
	user = kzalloc(sizeof(struct cpuhp_user), GFP_KERNEL);
	if (!user) {
		mutex_unlock(&cpuhp.lock);
		return -ENOMEM;
	}

	/* init new user's information */
	cpumask_copy(&user->online_cpus, &mask);
	strcpy(user->name, name);

	/* register user list */
	list_add(&user->list, &cpuhp.users);

	scnprintf(buf, sizeof(buf), "%*pbl", cpumask_pr_args(&user->online_cpus));
	pr_info("Register new user [name:%s, mask:%s]\n", user->name, buf);

	mutex_unlock(&cpuhp.lock);

	/* applying new user's request */
	return cpuhp_do();
}
EXPORT_SYMBOL_GPL(exynos_cpuhp_register);

/*
 * User requests cpu-hp.
 * The mask contains the requested cpu mask.
 * The INTERSECTIONS of other user's request masks is determined by the final cpu-mask.
 */
int exynos_cpuhp_request(char *name, struct cpumask mask)
{
	if (cpuhp_update_user(name, mask))
		return 0;

	return cpuhp_do();
}
EXPORT_SYMBOL_GPL(exynos_cpuhp_request);

/******************************************************************************/
/*                               CPUHP handlers                               */
/******************************************************************************/
/* legacy hotplug in */
static int cpuhp_in(const struct cpumask *mask)
{
	int cpu, ret = 0;

	for_each_cpu(cpu, mask) {
		ret = cpu_up(cpu);
		if (ret) {
			/*
			 * If it fails to enable cpu,
			 * it cancels cpu hotplug request and retries later.
			 */
			pr_err("Failed to hotplug in CPU%d with error %d\n",
					cpu, ret);
			break;
		}
	}

	return ret;
}

/* legacy hotplug out */
static int cpuhp_out(const struct cpumask *mask)
{
	int cpu, ret = 0;

	/*
	 * Reverse order of cpu,
	 * explore cpu7, cpu6, cpu5, ... cpu1
	 */
	for (cpu = nr_cpu_ids - 1; cpu > 0; cpu--) {
		if (!cpumask_test_cpu(cpu, mask))
			continue;

		ret = cpu_down(cpu);
		if (ret) {
			pr_err("Failed to hotplug out CPU%d with error %d\n",
					cpu, ret);
			break;
		}
	}

	return ret;
}

/*
 * Return last target online cpu mask
 * Returns the cpu_mask INTERSECTIONS of all users in the user list.
 */
static struct cpumask cpuhp_get_online_cpus(void)
{
	struct cpumask mask;
	struct cpuhp_user *user;
	char buf[10];

	cpumask_setall(&mask);

	list_for_each_entry(user, &cpuhp.users, list)
		cpumask_and(&mask, &mask, &user->online_cpus);

	if (cpumask_empty(&mask) || !cpumask_test_cpu(0, &mask)) {
		scnprintf(buf, sizeof(buf), "%*pbl", cpumask_pr_args(&mask));
		panic("Online mask(%s) is wrong\n", buf);
	}

	cpumask_and(&mask, &mask, &cpuhp.available_cpus);

	return mask;
}

/*
 * Executes cpu_up
 */
static int cpuhp_cpu_up(struct cpumask enable_cpus)
{
	if (cpumask_empty(&enable_cpus))
		return 0;

	return cpuhp_in(&enable_cpus);
}

/*
 * Executes cpu_down
 */
static int cpuhp_cpu_down(struct cpumask disable_cpus)
{
	if (cpumask_empty(&disable_cpus))
		return 0;

	return cpuhp_out(&disable_cpus);
}

/* print cpu control informatoin for deubgging */
static void cpuhp_print_debug_info(struct cpumask online_cpus)
{
	char new_buf[10], pre_buf[10];

	scnprintf(pre_buf, sizeof(pre_buf), "%*pbl", cpumask_pr_args(&cpuhp.online_cpus));
	scnprintf(new_buf, sizeof(new_buf), "%*pbl", cpumask_pr_args(&online_cpus));
	dbg_snapshot_printk("%s: %s -> %s\n", __func__, pre_buf, new_buf);

	/* print cpu control information */
	if (cpuhp.debug)
		pr_info("%s: %s -> %s\n", __func__, pre_buf, new_buf);
}

/*
 * cpuhp_do() is the main function for cpu hotplug. Only this function
 * enables or disables cpus, so all APIs in this driver call cpuhp_do()
 * eventually.
 */
static int cpuhp_do(void)
{
	struct cpumask online_cpus, enable_cpus, disable_cpus;
	int ret = 0;

	mutex_lock(&cpuhp.lock);
	/*
	 * If cpu hotplug is disabled or suspended,
	 * cpuhp_do() do nothing.
	 */
	if (!cpuhp.enabled || cpuhp.suspended) {
		mutex_unlock(&cpuhp.lock);
		return 0;
	}

	online_cpus = cpuhp_get_online_cpus();
	cpuhp_print_debug_info(online_cpus);

	/* if there is no mask change, skip */
	if (cpumask_equal(&cpuhp.online_cpus, &online_cpus))
		goto out;

	/* get the enable cpu  mask for new online cpu */
	cpumask_andnot(&enable_cpus, &online_cpus, &cpuhp.online_cpus);
	/* get the disable cpu mask for new offline cpu */
	cpumask_andnot(&disable_cpus, &cpuhp.online_cpus, &online_cpus);

	if (!cpumask_empty(&enable_cpus))
		ret = cpuhp_cpu_up(enable_cpus);
	if (ret)
		goto out;

	if (!cpumask_empty(&disable_cpus))
		ret = cpuhp_cpu_down(disable_cpus);

	cpumask_copy(&cpuhp.online_cpus, &online_cpus);

out:
	mutex_unlock(&cpuhp.lock);

	return ret;
}

static int cpuhp_control(bool enable)
{
	struct cpumask mask;
	int ret = 0;

	if (enable) {
		cpuhp_enable(true);
		cpuhp_do();
	} else {
		mutex_lock(&cpuhp.lock);

		cpumask_setall(&mask);
		cpumask_andnot(&mask, &mask, cpu_online_mask);

		/*
		 * If it success to enable all CPUs, clear cpuhp.enabled flag.
		 * Since then all hotplug requests are ignored.
		 */
		ret = cpuhp_in(&mask);
		if (!ret) {
			/*
			 * In this position, can't use cpuhp_enable()
			 * because already taken cpuhp.lock
			 */
			cpuhp.enabled = false;
		} else {
			pr_err("Failed to disable cpu hotplug, please try again\n");
		}

		mutex_unlock(&cpuhp.lock);
	}

	return ret;
}

/******************************************************************************/
/*                             SYSFS Interface                                */
/******************************************************************************/
/*
 * User can change the number of online cpu by using min_online_cpu and
 * max_online_cpu sysfs node. User input minimum and maxinum online cpu
 * to this node as below:
 *
 * #echo mask > /sys/power/cpuhp/set_online_cpu
 */
#define STR_LEN 6
static ssize_t set_online_cpu_show(struct device *dev,
		struct device_attribute *attr, char *buf)
{
	unsigned int online_cpus;

	online_cpus = *(unsigned int *)cpumask_bits(&cpuhp.sysfs_user.online_cpus);

	return snprintf(buf, 30, "set online cpu : 0x%x\n", online_cpus);
}

static ssize_t set_online_cpu_store(struct device *dev,
		struct device_attribute *attr, const char *buf, size_t count)
{
	struct cpumask online_cpus;
	char str[STR_LEN];

	if (strlen(buf) >= STR_LEN)
		return -EINVAL;

	if (!sscanf(buf, "%s", str))
		return -EINVAL;

	if (str[0] == '0' && str[1] == 'x')
		/* Move str pointer to remove "0x" */
		cpumask_parse(str + 2, &online_cpus);
	else
		cpumask_parse(str, &online_cpus);

	if (!cpumask_test_cpu(0, &online_cpus)) {
		pr_warn("wrong format\n");
		return -EINVAL;
	}

	cpumask_copy(&cpuhp.sysfs_user.online_cpus, &online_cpus);
	cpuhp_do();

	return count;
}
DEVICE_ATTR_RW(set_online_cpu);

/*
 * It shows cpuhp driver requested online_cpu
 *
 * #cat /sys/power/cpuhp/online_cpu
 */
static ssize_t online_cpu_show(struct device *dev,
		struct device_attribute *attr, char *buf)
{
	unsigned int online_cpus;

	online_cpus = *(unsigned int *)cpumask_bits(&cpuhp.online_cpus);

	return snprintf(buf, 30, "online cpu: 0x%x\n", online_cpus);
}
DEVICE_ATTR_RO(online_cpu);

/*
 * It shows users information(name, requesting cpu_mask)
 * registered in cpu-hp user_list
 *
 * #cat /sys/power/cpuhp/users
 */
static ssize_t users_show(struct device *dev,
		struct device_attribute *attr, char *buf)
{
	struct cpuhp_user *user;
	unsigned int online_cpus;
	ssize_t ret = 0;

	list_for_each_entry(user, &cpuhp.users, list) {
		online_cpus = *(unsigned int *)cpumask_bits(&user->online_cpus);
		ret += scnprintf(&buf[ret], 30, "%s: (0x%x)\n",
				user->name, online_cpus);
	}

	return ret;
}
DEVICE_ATTR_RO(users);

/*
 * User can control the cpu hotplug operation as below:
 *
 * #echo 1 > /sys/power/cpuhp/enabled => enable
 * #echo 0 > /sys/power/cpuhp/enabled => disable
 *
 * If enabled become 0, hotplug driver enable the all cpus and no hotplug
 * operation happen from hotplug driver.
 */
static ssize_t enabled_show(struct device *dev,
		struct device_attribute *attr, char *buf)
{
	return snprintf(buf, 10, "%d\n", cpuhp.enabled);
}

static ssize_t enabled_store(struct device *dev,
		struct device_attribute *attr, const char *buf, size_t count)
{
	int input;

	if (!sscanf(buf, "%d", &input))
		return -EINVAL;

	cpuhp_control(!!input);

	return count;
}
DEVICE_ATTR_RW(enabled);

/*
 * User can control en/disable debug mode
 *
 * #echo 1 > /sys/power/cpuhp/debug => enable
 * #echo 0 > /sys/power/cpuhp/debug => disable
 *
 * When it is enabled, information is printed every time there is a cpu control
 */
static ssize_t debug_show(struct device *dev,
		struct device_attribute *attr, char *buf)
{
	return snprintf(buf, 10, "%d\n", cpuhp.debug);
}

static ssize_t debug_store(struct device *dev,
		struct device_attribute *attr, const char *buf, size_t count)
{
	int input;

	if (!sscanf(buf, "%d", &input))
		return -EINVAL;

	cpuhp.debug = !!input;

	return count;
}
DEVICE_ATTR_RW(debug);

static struct attribute *exynos_cpuhp_attrs[] = {
	&dev_attr_set_online_cpu.attr,
	&dev_attr_online_cpu.attr,
	&dev_attr_users.attr,
	&dev_attr_enabled.attr,
	&dev_attr_debug.attr,
	NULL,
};

static struct attribute_group exynos_cpuhp_group = {
	.name = "cpuhp",
	.attrs = exynos_cpuhp_attrs,
};

/******************************************************************************/
/*                            Initialize Driver                               */
/******************************************************************************/
static void cpuhp_user_init(void)
{
	struct cpumask mask;

	/* init user list */
	INIT_LIST_HEAD(&cpuhp.users);

	cpumask_copy(&mask, cpu_possible_mask);

	/* register user for SYSFS */
	cpumask_copy(&cpuhp.system_user.online_cpus, &mask);
	strcpy(cpuhp.system_user.name, "SYSTEM");
	list_add(&cpuhp.system_user.list, &cpuhp.users);

	/* register user for SYSTEM */
	cpumask_copy(&cpuhp.sysfs_user.online_cpus, &mask);
	strcpy(cpuhp.sysfs_user.name, "SYSFS");
	list_add(&cpuhp.sysfs_user.list, &cpuhp.users);

	cpumask_copy(&cpuhp.online_cpus, cpu_online_mask);
}

static int max_cpus = CONFIG_VENDOR_NR_CPUS;
module_param(max_cpus, int, 0);

static int exynos_cpuhp_probe(struct platform_device *pdev)
{
	int cpu;

	/* Initialize pm_qos request and handler */
	cpuhp_user_init();

	/* Initialize available cpus  */
	for_each_possible_cpu(cpu) {
		if (max_cpus > 0 && cpu >= max_cpus)
			break;
		cpumask_set_cpu(cpu, &cpuhp.available_cpus);
	}

	/* Create CPUHP sysfs */
	if (sysfs_create_group(&pdev->dev.kobj, &exynos_cpuhp_group))
		pr_err("Failed to create sysfs for CPUHP\n");

	/* Link CPUHP sysfs to /sys/devices/system/cpu/cpuhp */
	if (sysfs_create_link(&cpu_subsys.dev_root->kobj,
				&pdev->dev.kobj, "cpuhp"))
		pr_err("Failed to link CPUHP sysfs to cpuctrl\n");

	/* Enable cpuhp */
	cpuhp_enable(true);

	pr_info("Exynos CPUHP driver probe done!\n");

	return 0;
}

static const struct of_device_id of_exynos_cpuhp_match[] = {
	{ .compatible = "samsung,exynos-cpuhp", },
	{ },
};
MODULE_DEVICE_TABLE(of, of_exynos_cpuhp_match);

static struct platform_driver exynos_cpuhp_driver = {
	.driver = {
		.name = "exynos-cpuhp",
		.owner = THIS_MODULE,
		.of_match_table = of_exynos_cpuhp_match,
	},
	.probe		= exynos_cpuhp_probe,
};

static int __init exynos_cpuhp_init(void)
{
	return platform_driver_register(&exynos_cpuhp_driver);
}
arch_initcall(exynos_cpuhp_init);

static void __exit exynos_cpuhp_exit(void)
{
	platform_driver_unregister(&exynos_cpuhp_driver);
}
module_exit(exynos_cpuhp_exit);

MODULE_DESCRIPTION("Exynos CPUHP driver");
MODULE_LICENSE("GPL");
