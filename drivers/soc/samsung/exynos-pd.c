/*
 * Exynos PM domain support for PMUCAL 3.0 interface.
 *
 * Copyright (c) 2020 Samsung Electronics Co., Ltd.
 *		http://www.samsung.com
 *
 * Implementation of Exynos specific power domain control which is used in
 * conjunction with runtime-pm.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 as
 * published by the Free Software Foundation.
 */

#include <soc/samsung/exynos-pd.h>
#include <soc/samsung/bts.h>
#include <soc/samsung/cal-if.h>
#include <linux/module.h>
#include <linux/sec_debug.h>

struct exynos_pm_domain *exynos_pd_lookup_name(const char *domain_name)
{
	struct exynos_pm_domain *exypd = NULL;
	struct device_node *np;

	if (IS_ERR_OR_NULL(domain_name))
		return NULL;

	for_each_compatible_node(np, NULL, "samsung,exynos-pd") {
		struct platform_device *pdev;
		struct exynos_pm_domain *pd;

		if (of_device_is_available(np)) {
			pdev = of_find_device_by_node(np);
			if (!pdev)
				continue;
			pd = platform_get_drvdata(pdev);
			if (!strcmp(pd->name, domain_name)) {
				exypd = pd;
				break;
			}
		}
	}
	return exypd;
}
EXPORT_SYMBOL(exynos_pd_lookup_name);

void *exynos_pd_lookup_cmu_id(u32 cmu_id)
{
	struct exynos_pm_domain *exypd = NULL;
	struct device_node *np;

	for_each_compatible_node(np, NULL, "samsung,exynos-pd") {
		struct platform_device *pdev;
		struct exynos_pm_domain *pd;

		if (of_device_is_available(np)) {
			pdev = of_find_device_by_node(np);
			if (!pdev)
				continue;
			pd = platform_get_drvdata(pdev);
			if (pd->cmu_id == cmu_id) {
				exypd = pd;
				break;
			}
		}
	}
	return exypd;
}
EXPORT_SYMBOL(exynos_pd_lookup_cmu_id);

int exynos_pd_status(struct exynos_pm_domain *pd)
{
	int status;

	if (unlikely(!pd))
		return -EINVAL;

	mutex_lock(&pd->access_lock);
	status = cal_pd_status(pd->cal_pdid);
	mutex_unlock(&pd->access_lock);

	return status;
}
EXPORT_SYMBOL(exynos_pd_status);


int exynos_pd_booton_rel(const char *pd_name)
{
	struct exynos_pm_domain *pd = NULL;
	pd = exynos_pd_lookup_name(pd_name);

	if (unlikely(!pd)) {
		pr_info("%s     :%s not found\n",__func__, pd_name);
		return -EINVAL;
	}

	if (of_property_read_bool(pd->of_node, "pd-boot-on")) {
		pd->genpd.flags &= ~GENPD_FLAG_ALWAYS_ON;
		pr_info(EXYNOS_PD_PREFIX "    %-9s flag set to %d\n",
				pd->genpd.name, pd->genpd.flags);
	}
	return 0;
}
EXPORT_SYMBOL(exynos_pd_booton_rel);

/* Power domain on sequence.
 * on_pre, on_post functions are registered as notification handler at CAL code.
 */
static void exynos_pd_power_on_pre(struct exynos_pm_domain *pd)
{
	if (!pd->skip_idle_ip)
		exynos_update_ip_idle_status(pd->idle_ip_index, 0);

	if (pd->devfreq_index >= 0) {
		exynos_bts_scitoken_setting(true);
	}
}

static void exynos_pd_power_on_post(struct exynos_pm_domain *pd)
{
	if (cal_pd_status(pd->cal_pdid)) {
#if defined(CONFIG_EXYNOS_BCM)
		if (pd->bcm)
			bcm_pd_sync(pd->bcm, true);
#endif
	}
}

static void exynos_pd_power_off_pre(struct exynos_pm_domain *pd)
{
#ifdef CONFIG_EXYNOS_CL_DVFS_G3D
	if (!strcmp(pd->name, "pd-g3d")) {
		exynos_g3d_power_down_noti_apm();
	}
#endif /* CONFIG_EXYNOS_CL_DVFS_G3D */
	if (cal_pd_status(pd->cal_pdid)) {
#if defined(CONFIG_EXYNOS_BCM)
		if (pd->bcm)
			bcm_pd_sync(pd->bcm, false);
#endif
	}
}

static void exynos_pd_power_off_post(struct exynos_pm_domain *pd)
{
	if (!pd->skip_idle_ip)
		exynos_update_ip_idle_status(pd->idle_ip_index, 1);

	if (pd->devfreq_index >= 0) {
		exynos_bts_scitoken_setting(false);
	}
}

static void exynos_pd_prepare_forced_off(struct exynos_pm_domain *pd)
{
}

static int exynos_pd_power_on(struct generic_pm_domain *genpd)
{
	struct exynos_pm_domain *pd = container_of(genpd, struct exynos_pm_domain, genpd);
	int ret = 0;

	mutex_lock(&pd->access_lock);

	DEBUG_PRINT_INFO("%s(%s)+\n", __func__, pd->name);

	if (unlikely(!pd->pd_control)) {
		pr_debug(EXYNOS_PD_PREFIX "%s is Logical sub power domain, dose not have to power on control\n", pd->name);
		goto acc_unlock;
	}

	if (pd->power_down_skipped) {
		pr_info(EXYNOS_PD_PREFIX "%s power-on is skipped.\n", pd->name);
		goto acc_unlock;
	}

	exynos_pd_power_on_pre(pd);

	ret = pd->pd_control(pd->cal_pdid, 1);
	if (ret) {
		pr_err(EXYNOS_PD_PREFIX "%s cannot be powered on\n", pd->name);
		exynos_pd_power_off_post(pd);
		ret = -EAGAIN;
		goto acc_unlock;
	}

	exynos_pd_power_on_post(pd);

acc_unlock:
	DEBUG_PRINT_INFO("%s(%s)-, ret = %d\n", __func__, pd->name, ret);
	mutex_unlock(&pd->access_lock);

	return ret;
}

static int exynos_pd_power_off(struct generic_pm_domain *genpd)
{
	struct exynos_pm_domain *pd = container_of(genpd, struct exynos_pm_domain, genpd);
	int ret = 0;

	mutex_lock(&pd->access_lock);

	DEBUG_PRINT_INFO("%s(%s)+\n", __func__, pd->name);

	if (unlikely(!pd->pd_control)) {
		pr_debug(EXYNOS_PD_PREFIX "%s is Logical sub power domain, dose not have to power off control\n", genpd->name);
		goto acc_unlock;
	}

	if (pd->power_down_ok && !pd->power_down_ok()) {
		pr_info(EXYNOS_PD_PREFIX "%s power-off is skipped.\n", pd->name);
		pd->power_down_skipped = true;
		goto acc_unlock;
	}

	exynos_pd_power_off_pre(pd);

	ret = pd->pd_control(pd->cal_pdid, 0);
	if (unlikely(ret)) {
		if (ret == -4) {
			pr_err(EXYNOS_PD_PREFIX "Timed out during %s  power off! -> forced power off\n", genpd->name);
			exynos_pd_prepare_forced_off(pd);
			ret = pd->pd_control(pd->cal_pdid, 0);
			if (unlikely(ret)) {
				pr_auto(ASL1, EXYNOS_PD_PREFIX "%s occur error at power off!\n", genpd->name);
				secdbg_exin_set_epd(genpd->name);
				goto acc_unlock;
			}
		} else {
			pr_auto(ASL1, EXYNOS_PD_PREFIX "%s occur error at power off!\n", genpd->name);
			secdbg_exin_set_epd(genpd->name);
			goto acc_unlock;
		}
	}

	exynos_pd_power_off_post(pd);
	pd->power_down_skipped = false;

acc_unlock:
	DEBUG_PRINT_INFO("%s(%s)-, ret = %d\n", __func__, pd->name, ret);
	mutex_unlock(&pd->access_lock);

	return ret;
}

/**
 *  of_get_devfreq_sync_volt_idx - check devfreq sync voltage idx
 *
 *  Returns the index if the "devfreq-sync-voltage" is described in DT pd node,
 *  -ENOENT otherwise.
 */
static int of_get_devfreq_sync_volt_idx(const struct device_node *device)
{
	int ret;
	u32 val;

	ret = of_property_read_u32(device, "devfreq-sync-voltage", &val);
	if (ret)
		return -ENOENT;

	return val;
}

static bool exynos_pd_power_down_ok_aud(void)
{
#if defined(CONFIG_SND_SOC_SAMSUNG_ABOX) || defined(CONFIG_SND_SOC_SAMSUNG_ABOX_MODULE)
	return !abox_is_on();
#else
	return true;
#endif
}

static bool exynos_pd_power_down_ok_vts(void)
{
#if defined(CONFIG_SND_SOC_SAMSUNG_VTS) || defined(CONFIG_SND_SOC_SAMSUNG_VTS_MODULE)

	return !vts_is_on();
#else
	return true;
#endif
}

static bool exynos_pd_power_down_ok_usb(void)
{
#ifdef CONFIG_USB_DWC3_EXYNOS
	return !otg_is_connect();
#else
	return true;
#endif
}

static void of_get_power_down_ok(struct exynos_pm_domain *pd)
{
	int ret;
	u32 val;
	struct device_node *device = pd->of_node;

	ret = of_property_read_u32(device, "power-down-ok", &val);
	if (ret)
		return ;
	else {
		switch (val) {
		case PD_OK_AUD:
			pd->power_down_ok = exynos_pd_power_down_ok_aud;
			break;
		case PD_OK_VTS:
			pd->power_down_ok = exynos_pd_power_down_ok_vts;
			break;
		case PD_OK_USB:
			pd->power_down_ok = exynos_pd_power_down_ok_usb;
			break;
		default:
			break;
		}
	}
}


static int exynos_pd_genpd_init(struct exynos_pm_domain *pd, int state)
{
	pd->genpd.name = pd->name;
	pd->genpd.power_off = exynos_pd_power_off;
	pd->genpd.power_on = exynos_pd_power_on;

	/* pd power on/off latency is less than 1ms */
	pm_genpd_init(&pd->genpd, NULL, state ? false : true);

	pd->genpd.states = kzalloc(sizeof(struct genpd_power_state), GFP_KERNEL);

	if (!pd->genpd.states)
		return -ENOMEM;

	pd->genpd.states[0].power_on_latency_ns = 1000000;
	pd->genpd.states[0].power_off_latency_ns = 1000000;

	return 0;
}

static int exynos_pd_suspend_late(struct device *dev)
{
	struct platform_device *pdev = container_of(dev,
					struct platform_device, dev);
	struct exynos_pm_domain *data = platform_get_drvdata(pdev);

	if (IS_ERR(&data->genpd))
		return 0;
	/* Suspend callback function might be registered if necessary */
	if (of_property_read_bool(dev->of_node, "pd-always-on")) {
		data->genpd.flags = 0;
		dev_info(dev, EXYNOS_PD_PREFIX "    %-9s flag set to 0\n", data->genpd.name);
	}
	return 0;
}

static int exynos_pd_resume_early(struct device *dev)
{
	struct platform_device *pdev = container_of(dev,
					struct platform_device, dev);
	struct exynos_pm_domain *data = (struct exynos_pm_domain *)platform_get_drvdata(pdev);

	if (IS_ERR(&data->genpd))
		return 0;
	/* Resume callback function might be registered if necessary */
	if (of_property_read_bool(dev->of_node, "pd-always-on")) {
		data->genpd.flags |= GENPD_FLAG_ALWAYS_ON;
		dev_info(dev, EXYNOS_PD_PREFIX "    %-9s - %s\n", data->genpd.name,
				"on,  always");
	}

	return 0;
}

static int exynos_pd_probe(struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;
	struct device_node *np = dev->of_node;
	struct exynos_pm_domain *pd;

	unsigned int index = 0;

	struct exynos_pm_domain *parent_pd;
	struct device_node *parent;
	struct platform_device *parent_pd_pdev;

	int initial_state;
	int ret;

	if (!of_have_populated_dt()) {
		dev_err(dev, EXYNOS_PD_PREFIX "PM Domain works along with Device Tree\n");
		return -EPERM;
	}

	/* skip unmanaged power domain */
	if (!of_device_is_available(np))
		return -EINVAL;

	pd = kzalloc(sizeof(*pd), GFP_KERNEL);
	if (!pd) {
		dev_err(dev, EXYNOS_PD_PREFIX "%s: failed to allocate memory for domain\n",
				__func__);
		return -ENOMEM;
	}

	/* init exynos_pm_domain's members  */
	pd->name = kstrdup(np->name, GFP_KERNEL);
	ret = of_property_read_u32(np, "cal_id", (u32 *)&pd->cal_pdid);
	if (ret) {
		dev_err(dev, EXYNOS_PD_PREFIX "%s: failed to get cal_pdid  from of %s\n",
				__func__, pd->name);
		return -ENODEV;
	}

	pd->of_node = np;
	pd->pd_control = cal_pd_control;
	pd->check_status = exynos_pd_status;
	pd->devfreq_index = of_get_devfreq_sync_volt_idx(pd->of_node);
	of_get_power_down_ok(pd);
	pd->power_down_skipped = false;

	ret = of_property_read_u32(np, "need_smc", (u32 *)&pd->need_smc);
	if (ret) {
		pd->need_smc = 0x0;
	} else {
		cal_pd_set_smc_id(pd->cal_pdid, pd->need_smc);
		dev_info(dev, EXYNOS_PD_PREFIX "%s: %s read need_smc 0x%x successfully.!\n",
				__func__, pd->name, pd->need_smc);
	}

	ret = of_property_read_u32(np, "cmu_id", (u32 *)&pd->cmu_id);
	if (ret) {
		pd->cmu_id = 0x0;
	} else {
		pr_info("%s read cmu_id 0x%x successfully.!\n",
			pd->name, pd->cmu_id);
	}

	initial_state = cal_pd_status(pd->cal_pdid);
	if (initial_state == -1) {
		dev_err(dev, EXYNOS_PD_PREFIX "%s: %s is in unknown state\n",
				__func__, pd->name);
		return -EINVAL;
	}

	if (of_property_read_bool(np, "skip-idle-ip"))
		pd->skip_idle_ip = true;
	else
		pd->idle_ip_index = exynos_get_idle_ip_index(pd->name, 1);

	mutex_init(&pd->access_lock);
	platform_set_drvdata(pdev, pd);

	ret = exynos_pd_genpd_init(pd, initial_state);
	if (ret) {
		dev_err(dev, EXYNOS_PD_PREFIX "%s: exynos_pd_genpd_init fail: %s, ret:%d\n",
				__func__, pd->name, ret);
		return ret;
	}

	of_genpd_add_provider_simple(np, &pd->genpd);

	if (of_property_read_bool(np, "pd-always-on") ||
			(of_property_read_bool(np, "pd-boot-on"))) {
		pd->genpd.flags |= GENPD_FLAG_ALWAYS_ON;
		dev_info(dev, EXYNOS_PD_PREFIX "    %-9s - %s\n", pd->genpd.name,
				"on,  always");
	} else {
		dev_info(dev, EXYNOS_PD_PREFIX "    %-9s - %-3s\n", pd->genpd.name,
				cal_pd_status(pd->cal_pdid) ? "on" : "off");
	}

	parent = of_parse_phandle(np, "parent", index);
	if (parent) {
		parent_pd_pdev = of_find_device_by_node(parent);
		if (!parent_pd_pdev) {
			dev_info(dev, EXYNOS_PD_PREFIX "parent pd pdev not found");
		} else {
			parent_pd = platform_get_drvdata(parent_pd_pdev);
			if (!parent_pd) {
				dev_info(dev, EXYNOS_PD_PREFIX "parent pd not found");
			} else {
				if (pm_genpd_add_subdomain(&parent_pd->genpd, &pd->genpd))
					pr_err(EXYNOS_PD_PREFIX "%s cannot add subdomain %s\n",
							parent_pd->name, pd->name);
				else
					pr_err(EXYNOS_PD_PREFIX "%s have new subdomain %s\n",
							parent_pd->name, pd->name);
			}
		}
	}

#ifdef CONFIG_DEBUG_FS
	cal_register_pd_lookup_cmu_id(exynos_pd_lookup_cmu_id);
#endif
	dev_info(dev, EXYNOS_PD_PREFIX "PM Domain Initialize\n");
	return ret;
}

static const struct of_device_id of_exynos_pd_match[] = {
	{ .compatible = "samsung,exynos-pd", },
	{ },
};
MODULE_DEVICE_TABLE(of, of_exynos_pd_match);

static const struct dev_pm_ops exynos_pd_pm_ops = {
	.suspend_late	= exynos_pd_suspend_late,
	.resume_early		= exynos_pd_resume_early,
};

static const struct platform_device_id exynos_pd_ids[] = {
	{ "exynos-pd", },
	{ }
};

static struct platform_driver exynos_pd_driver = {
	.driver = {
		.name = "exynos-pd",
		.of_match_table = of_exynos_pd_match,
		.pm = &exynos_pd_pm_ops
	},
	.probe		= exynos_pd_probe,
	.id_table	= exynos_pd_ids,
};

static int exynos_pd_init(void)
{
	return platform_driver_register(&exynos_pd_driver);
}
subsys_initcall(exynos_pd_init);

static void exynos_pd_exit(void)
{
	return platform_driver_unregister(&exynos_pd_driver);
}
module_exit(exynos_pd_exit);

MODULE_SOFTDEP("pre: clk_exynos");
MODULE_LICENSE("GPL");
