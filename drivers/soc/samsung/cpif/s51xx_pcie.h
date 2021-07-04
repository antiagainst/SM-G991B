/* SPDX-License-Identifier: GPL-2.0 */
/*
 * Copyright (C) 2010 Samsung Electronics.
 *
 * This software is licensed under the terms of the GNU General Public
 * License version 2, as published by the Free Software Foundation, and
 * may be copied, distributed, and modified under those terms.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 */

#ifndef __S51xx_PCIE_H__
#define __S51xx_PCIE_H__

#include <linux/exynos-pci-noti.h>

#define MAX_MSI_NUM	(16)

extern void first_save_s51xx_status(struct pci_dev *pdev);
extern int s51xx_pcie_init(struct modem_ctl *mc);
extern int exynos_pcie_host_v1_register_event(struct exynos_pcie_register_event *reg);
/* not used: extern int exynos_pcie_host_v1_deregister_event(struct exynos_pcie_register_event *reg); */
extern void exynos_pcie_rc_register_dump(int ch_num);
extern int exynos_pcie_rc_set_outbound_atu(int ch_num, u32 target_addr, u32 offset, u32 size);
extern bool exynos_pcie_rc_get_cpl_timeout_state(int ch_num);
extern void exynos_pcie_rc_set_cpl_timeout_state(int ch_num, bool recovery);

struct s51xx_pcie {
	unsigned int busdev_num;
	int pcie_channel_num;
	struct pci_dev *s51xx_pdev;
	int irq_num_base;
	u32 __iomem *doorbell_addr;
	u32 __iomem *reg_base;
	u64 dbaddr_base;
	u32 dbaddr_offset;

	u32 link_status;
	bool suspend_try;

	struct exynos_pcie_register_event pcie_event;
	struct exynos_pcie_register_event pcie_cpl_timeout_event;
	struct pci_saved_state *pci_saved_configs;
};

//extern struct s51xx_pcie s5100pcie;

extern int exynos_pcie_host_v1_poweron(int ch_num);
extern int exynos_pcie_host_v1_poweroff(int ch_num);
extern void exynos_pcie_set_perst_gpio(int ch_num, bool on);
extern void exynos_pcie_set_ready_cto_recovery(int ch_num);
/* not used: extern int exynos_pcie_gpio_onoff(int ch_num, int val); */
/* not used(comment out): extern void exynos_pcie_msi_init_ext(int ch_num); */
extern int exynos_pcie_rc_set_affinity(int ch_num, int affinity);
extern int pci_alloc_irq_vectors_affinity(struct pci_dev *dev, unsigned int min_vecs,
					unsigned int max_vecs, unsigned int flags,
					const struct irq_affinity *affd);

#define AUTOSUSPEND_TIMEOUT	200

int s51xx_pcie_request_msi_int(struct pci_dev *pdev, int int_num);
void __iomem *s51xx_pcie_get_doorbell_address(void);
int s51xx_pcie_send_doorbell_int(struct pci_dev *pdev, int int_num);
void s51xx_pcie_save_state(struct pci_dev *pdev);
void s51xx_pcie_restore_state(struct pci_dev *pdev);
int s51xx_check_pcie_link_status(int ch_num);
void s51xx_pcie_l1ss_ctrl(int enable);
void disable_msi_int(struct pci_dev *pdev);
void print_msi_register(struct pci_dev *pdev);
int s5100_force_crash_exit_ext(void);
int s5100_poweron_pcie(struct modem_ctl *mc);
int s5100_poweroff_pcie(struct modem_ctl *mc, bool force_off);
int s5100_try_gpio_cp_wakeup(struct modem_ctl *mc);
int s5100_send_panic_noti_ext(void);
int s5100_set_outbound_atu(struct modem_ctl *mc, struct cp_btl *btl, loff_t *pos, u32 map_size);

int exynos_pcie_rc_lanechange(int ch_num, int lane);
int exynos_pcie_rc_speedchange(int ch_num, int spd);

#endif /* __S51xx_PCIE_H__ */
