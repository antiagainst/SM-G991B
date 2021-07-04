/*
 * Copyright (c) 2013 Samsung Electronics Co., Ltd.
 *		http://www.samsung.com
 *
 * Debug-SnapShot: Debug Framework for Ramdump based debugging method
 * The original code is Exynos-Snapshot for Exynos SoC
 *
 * Author: Hosung Kim <hosung0.kim@samsung.com>
 * Author: Changki Kim <changki.kim@samsung.com>
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 as
 * published by the Free Software Foundation.
 */

#ifndef DEBUG_SNAPSHOT_LOCAL_H
#define DEBUG_SNAPSHOT_LOCAL_H
#include <linux/device.h>
#include <soc/samsung/debug-snapshot-log.h>
#include <dt-bindings/soc/samsung/debug-snapshot-def.h>

struct dbg_snapshot_param {
	void *dss_log_misc;
	void *dss_items;
	void *dss_log_items;
	void *dss_log;
	void *hook_func;
};

struct dbg_snapshot_info {
	size_t size;
	size_t vaddr;
	size_t paddr;
	unsigned int enabled;
};

struct dbg_snapshot_base {
	size_t size;
	size_t vaddr;
	size_t paddr;
	unsigned int enabled;
	struct dbg_snapshot_param *param;
};

struct dbg_snapshot_item {
	char *name;
	struct dbg_snapshot_info entry;
	unsigned int persist;
	unsigned char *head_ptr;
	unsigned char *curr_ptr;
};

struct dbg_snapshot_log_item {
	char *name;
	struct dbg_snapshot_info entry;
};

struct dbg_snapshot_desc {
	struct device *dev;
	raw_spinlock_t ctrl_lock;
	int sjtag_status;
	bool panic_to_wdt;
};

struct dbg_snapshot_dpm {
	bool enabled;
	unsigned int version;
	bool enabled_debug;
	unsigned int dump_mode;
	int dump_mode_none;
	bool dump_mode_file;

	unsigned int pre_log;
	unsigned int p_el1_da;
	unsigned int p_el1_sp_pc;
	unsigned int p_el1_ia;
	unsigned int p_el1_undef;
	unsigned int p_el1_inv;
	unsigned int p_el1_serror;
};

extern void dbg_snapshot_init_log(void);
extern void dbg_snapshot_init_dpm(void);
extern void dbg_snapshot_init_utils(void);
extern int dbg_snapshot_dt_scan_dpm(void);
extern int dbg_snapshot_get_enable(void);
extern void __iomem *dbg_snapshot_get_header_vaddr(void);
extern unsigned long dbg_snapshot_get_header_paddr(void);
extern void dbg_snapshot_scratch_reg(unsigned int val);
extern void dbg_snapshot_print_log_report(void);
extern void dbg_snapshot_set_debug_test_buffer_addr(u64 paddr, unsigned int cpu);
extern unsigned int dbg_snapshot_get_debug_test_buffer_addr(unsigned int cpu);
extern unsigned long dbg_snapshot_get_last_pc(unsigned int cpu);
extern unsigned long dbg_snapshot_get_last_pc_paddr(void);
extern void dbg_snapshot_set_qd_entry(unsigned long address);
extern int dbg_snapshot_get_num_items(void);
extern int dbg_snapshot_log_get_num_items(void);
extern void *dbg_snapshot_get_item_by_index(int index);
extern void *dbg_snapshot_log_get_item_by_index(int index);
extern void dbg_snapshot_set_enable_log_item(const char *name, int en);
extern int dbg_snapshot_get_dpm_none_dump_mode(void);
extern void dbg_snapshot_set_dpm_none_dump_mode(unsigned int mode);
extern void cache_flush_all(void);

extern struct dbg_snapshot_log *dss_log;
extern struct dbg_snapshot_desc dss_desc;
extern struct dbg_snapshot_item dss_items[];
extern struct dbg_snapshot_dpm dss_dpm;
extern struct dbg_snapshot_log_item dss_log_items[];
extern struct dbg_snapshot_log_misc dss_log_misc;

/*  Size domain */
#define DSS_KEEP_HEADER_SZ		(SZ_256 * 3)
#define DSS_HEADER_SZ			SZ_4K //0x0 -- 0x1000
#define DSS_MMU_REG_SZ			SZ_4K //0x1000 -- 0x2000
#define DSS_CORE_REG_SZ			SZ_4K //0x2000 -- 0x3000
#define DSS_DBGC_SRAM_LOG_SZ		SZ_8K //0x3000 -- 0x5000
#define DSS_DPM_DT_SZ			SZ_8K //0x5000 -- 0x7000
#define DSS_DBGC_DRAM_LOG_SZ		SZ_16K //0x7000 -- 0xB000

#define DSS_MMU_REG_OFFSET		SZ_512
#define DSS_CORE_REG_OFFSET		SZ_512
#define DSS_MAX_BL_SIZE			(24)

/* Sign domain */
#define DSS_SIGN_RESET			0x0
#define DSS_SIGN_RESERVED		0x1
#define DSS_SIGN_SCRATCH		0xD
#define DSS_SIGN_ALIVE			0xFACE
#define DSS_SIGN_DEAD			0xDEAD
#define DSS_SIGN_PANIC			0xBABA
#define DSS_SIGN_SAFE_FAULT		0xFAFA
#define DSS_SIGN_NORMAL_REBOOT		0xCAFE
#define DSS_SIGN_LOCKUP			0xDEADBEEF
#define DSS_SIGN_MAGIC			(0xDB9 << 16)
#define DSS_BOOT_CNT_MAGIC		0xFACEDB90

/*  Specific Address Information */
#define DSS_OFFSET_SCRATCH		(0x100)
#define DSS_OFFSET_NONE_DPM_DUMP_MODE	(0x108)
#define DSS_OFFSET_DEBUG_TEST_BUFFER(n)	(0x190 + (0x8 * n))
#define DSS_OFFSET_LAST_LOGBUF		(0x200)
#define DSS_OFFSET_LAST_PLATFORM_LOGBUF	(0x204)
#define DSS_OFFSET_CHECK_EOB		(0x208)
#define DSS_OFFSET_BL_BOOT_CNT_MAGIC	(0x210)
#define DSS_OFFSET_BL_BOOT_CNT		(0x214)
#define DSS_OFFSET_KERNEL_BOOT_CNT_MAGIC (0x218)
#define DSS_OFFSET_KERNEL_BOOT_CNT	(0x21C)
#define DSS_OFFSET_MEMLOG_BL_BASE	DSS_MEMLOG_BL_BASE /* 0x290 */
#define DSS_OFFSET_MEMLOG_BL_MAGIC	(DSS_MEMLOG_BL_BASE + 0x8) /*0x298 */
#define DSS_OFFSET_EMERGENCY_REASON	(0x300)
#define DSS_OFFSET_WDT_CALLER		(0x310)
#define DSS_OFFSET_CORE_POWER_STAT	(0x400)
#define DSS_OFFSET_PANIC_STAT		(0x500)
#define DSS_OFFSET_CORE_LAST_PC		(0x600)
#define DSS_OFFSET_QD_ENTRY		(0x660)

/* DSS_BASE + 0x700 -- 0x8FF is reserved */
#define DSS_OFFSET_LINUX_BANNER		(0x700)
/* DSS_BASE + 0x900 -- 0xDFF is reserved */
#define DSS_OFFSET_ITEM_INFO		(0x900)
/* DSS_BASE + 0xE00 -- 0xFFF is reserved */
#define DSS_OFFSET_PANIC_STRING		(0xE00)
#endif
