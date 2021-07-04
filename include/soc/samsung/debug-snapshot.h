/*
 * Copyright (c) 2013 Samsung Electronics Co., Ltd.
 *		http://www.samsung.com
 *
 * Exynos-SnapShot debugging framework for Exynos SoC
 *
 * Author: Hosung Kim <Hosung0.kim@samsung.com>
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 as
 * published by the Free Software Foundation.
 */

#ifndef DEBUG_SNAPSHOT_H
#define DEBUG_SNAPSHOT_H

#include <dt-bindings/soc/samsung/debug-snapshot-def.h>
#if IS_ENABLED(CONFIG_DEBUG_SNAPSHOT)
#include <linux/sched/clock.h>

struct clk;
struct dbg_snapshot_helper_ops;
struct notifier_block;

extern int dbg_snapshot_get_enable(void);
extern void dbg_snapshot_set_item_enable(const char *name, int en);
extern int dbg_snapshot_get_item_enable(const char *name);
extern unsigned int dbg_snapshot_get_item_size(const char *name);
extern unsigned int dbg_snapshot_get_item_paddr(const char *name);
extern unsigned long dbg_snapshot_get_item_vaddr(const char *name);
extern void dbg_snapshot_output(void);
extern void dbg_snapshot_log_output(void);
extern int dbg_snapshot_add_bl_item_info(const char *name,
				unsigned long paddr, unsigned long size);
extern void dbg_snapshot_scratch_clear(void);
extern bool dbg_snapshot_is_scratch(void);
/* debug-snapshot-dpm functions */
extern bool dbg_snapshot_get_dpm_status(void);
extern void dbg_snapshot_do_dpm_policy(unsigned int policy);
/* debug-snapshot-qd functions */
extern int dbg_snapshot_qd_add_region(void *v_entry, u32 attr);
extern bool dbg_snapshot_qd_enabled(void);
extern void dbg_snapshot_qd_dump_stack(u64 sp);
extern void dbg_snapshot_panic_backtrace_stop(void);
extern void dbg_snapshot_panic_backtrace_log(int cpu, unsigned long where);
/*debug-snapshot-utils functions */
extern int dbg_snapshot_get_sjtag_status(void);
extern void dbg_snapshot_ecc_dump(void);
extern int dbg_snapshot_start_watchdog(int sec);
extern int dbg_snapshot_expire_watchdog(void);
extern int dbg_snapshot_kick_watchdog(void);
extern unsigned int dbg_snapshot_get_val_offset(unsigned int offset);
extern void dbg_snapshot_set_val_offset(unsigned int val, unsigned int offset);
extern void register_dump_one_task_notifier(struct notifier_block *nb);
extern void dbg_snapshot_register_wdt_ops(void *start, void *expire, void *stop);
extern void dbg_snapshot_register_debug_ops(void *halt, void *arraydump, void *scandump);

/* debug-snapshot-log functions */
extern int dbg_snapshot_get_freq_idx(const char *name);

#define dbg_snapshot_irq_var(v) do { v = local_clock(); } while (0)
extern void dbg_snapshot_task(int cpu, void *v_task);
extern void dbg_snapshot_work(void *worker, void *v_task, void *fn, int en);
extern void dbg_snapshot_cpuidle(char *modes, unsigned int state, s64 diff, int en);
extern void dbg_snapshot_cpuidle_mod(char *modes, unsigned int state, s64 diff, int en);
extern void dbg_snapshot_irq(int irq, void *fn, void *val, unsigned long long time,
				int en);
extern void dbg_snapshot_clk(void *clock, const char *func_name, unsigned long arg,
				int mode);
extern void dbg_snapshot_regulator(unsigned long long timestamp, char *f_name,
				unsigned int addr, unsigned int volt,
				unsigned int rvolt, int en);
extern void dbg_snapshot_acpm(unsigned long long timestamp, const char *log,
				unsigned int data);
extern void dbg_snapshot_thermal(void *data, unsigned int temp, char *name,
				unsigned int max_cooling);
extern void dbg_snapshot_hrtimer(void *timer, s64 *now, void *fn, int en);
extern void dbg_snapshot_pmu(int id, const char *func_name, int mode);
extern void dbg_snapshot_freq(int type, unsigned int old_freq,
				unsigned int target_freq, int en);
extern void dbg_snapshot_dm(int type, unsigned int min, unsigned int max,
				s32 wait_t, s32 t);
extern void dbg_snapshot_spinlock(void *lock, int en);
extern void dbg_snapshot_printk(const char *fmt, ...);

#define dss_extern_get_log_by_cpu(item)					\
extern long dss_get_len_##item##_log(void);				\
extern long dss_get_len_##item##_log_by_cpu(int cpu);			\
extern long dss_get_last_##item##_log_idx(int cpu);			\
extern long dss_get_first_##item##_log_idx(int cpu);			\
extern struct item##_log *dss_get_last_##item##_log(int cpu);		\
extern struct item##_log *dss_get_first_##item##_log(int cpu);		\
extern struct item##_log *dss_get_##item##_log_by_idx(int cpu, int idx);\
extern struct item##_log *dss_get_##item##_log_by_cpu_iter(int cpu, int idx);\
extern unsigned long dss_get_vaddr_##item##_log_by_cpu(int cpu)

#define dss_extern_get_log(item)					\
extern long dss_get_len_##item##_log(void);				\
extern long dss_get_last_##item##_log_idx(void);			\
extern long dss_get_first_##item##_log_idx(void);			\
extern struct item##_log *dss_get_last_##item##_log(void);		\
extern struct item##_log *dss_get_first_##item##_log(void);		\
extern struct item##_log *dss_get_##item##_log_by_idx(int idx);		\
extern struct item##_log *dss_get_##item##_log_iter(int idx);		\
extern unsigned long dss_get_vaddr_##item##_log(void)

static inline void dbg_snapshot_spin_func(void) { do { wfi(); } while (1); }
#else /* CONFIG_DEBUG_SNAPSHOT */
#define dbg_snapshot_irq_var(v)			v = 0
#define dbg_snapshot_task(a, b)			do { } while (0)
#define dbg_snapshot_work(a, b, c, d)		do { } while (0)
#define dbg_snapshot_cpuidle(a, b, c, d)	do { } while (0)
#define dbg_snapshot_cpuidle_mod(a, b, c, d)	do { } while (0)
#define dbg_snapshot_acpm(a, b, c)		do { } while (0)
#define dbg_snapshot_regulator(a, b, c, d, e, f)	do { } while (0)
#define dbg_snapshot_thermal(a, b, c, d)	do { } while (0)
#define dbg_snapshot_irq(a, b, c, d, e)		do { } while (0)
#define dbg_snapshot_spinlock(a, b)		do { } while (0)
#define dbg_snapshot_clk(a, b, c, d)		do { } while (0)
#define dbg_snapshot_pmu(a, b, c)		do { } while (0)
#define dbg_snapshot_freq(a, b, c, d)		do { } while (0)
#define dbg_snapshot_hrtimer(a, b, c, d)	do { } while (0)
#define dbg_snapshot_dm(a, b, c, d, e)		do { } while (0)
#define dbg_snapshot_printk(...)		do { } while (0)

#define dbg_snapshot_set_item_enable(a, b)	do { } while (0)
#define dbg_snapshot_output()			do { } while (0)
#define dbg_snapshot_log_output()		do { } while (0)
#define dbg_snapshot_scratch_clear()		do { } while (0)

#define dbg_snapshot_do_dpm_policy(a)		do { } while (0)
#define dbg_snapshot_qd_dump_stack(a)		do { } while (0)
#define dbg_snapshot_panic_backtrace_stop()	do { } while (0)
#define dbg_snapshot_panic_backtrace_log(a,b)	do { } while (0)

#define dbg_snapshot_get_sjtag_status()		do { } while (0)
#define dbg_snapshot_panic_handler_safe()	do { } while (0)
#define dbg_snapshot_ecc_dump()			do { } while (0)
#define dbg_snapshot_register_wdt_ops(a, b, c)	do { } while (0)
#define dbg_snapshot_register_debug_ops(a, b, c)	do { } while (0)

#define dbg_snapshot_set_val_offset(a)		do { } while (0)
#define register_dump_one_task_notifier(a)	do { } while (0)

#define dbg_snapshot_get_enable() 		(0)
#define dbg_snapshot_get_item_enable(a) 	(0)
#define dbg_snapshot_get_item_size(a)		(0)
#define dbg_snapshot_get_item_paddr(a) 		(0)
#define dbg_snapshot_get_item_vaddr(a) 		(0)
#define dbg_snapshot_add_bl_item_info(a, b, c)	(-1)
#define dbg_snapshot_is_scratch() 		(0)

#define dbg_snapshot_get_dpm_status() 		(0)
#define dbg_snapshot_qd_add_region(a, b)	(-1)
#define dbg_snapshot_qd_enabled()		(0)

#define dbg_snapshot_start_watchdog(a)		(-1)
#define dbg_snapshot_expire_watchdog()		(-1)
#define dbg_snapshot_kick_watchdog()		(-1)
#define dbg_snapshot_get_val_offset(a)		(0)

#define dbg_snapshot_get_freq_idx(a)		(-1)

#define dss_extern_get_log_by_cpu(item)					\
static inline long dss_get_len_##item##_log(void) {			\
	return -1;							\
}									\
static inline long dss_get_len_##item##_log_by_cpu(int cpu) {		\
	return -1;							\
}									\
static inline long dss_get_last_##item##_log_idx(int cpu) {		\
	return -1;							\
}									\
static inline long dss_get_first_##item##_log_idx(int cpu) {		\
	return -1;							\
}									\
static inline struct item##_log *dss_get_last_##item##_log(int cpu) {	\
	return NULL;							\
}									\
static inline struct item##_log *dss_get_first_##item##_log(int cpu) {	\
	return NULL;							\
}									\
static inline struct item##_log *dss_get_##item##_log_by_idx(int cpu, int idx) { \
	return NULL;							\
}									\
static inline struct item##_log *dss_get_##item##_log_by_cpu_iter(int cpu, int idx) {\
	return NULL;							\
}									\
static inline unsigned long dss_get_vaddr_##item##_log_by_cpu(int cpu) {\
	return 0;							\
}

#define dss_extern_get_log(item)					\
static inline long dss_get_len_##item##_log(void) {			\
	return -1;							\
}									\
static inline long dss_get_last_##item##_log_idx(void) {		\
	return -1;							\
}									\
static inline long dss_get_first_##item##_log_idx(void) {		\
	return -1;							\
}									\
static inline struct item##_log *dss_get_last_##item##_log(void) {	\
	return NULL;							\
}									\
static inline struct item##_log *dss_get_first_##item##_log(void) {	\
	return NULL;							\
}									\
static inline struct item##_log *dss_get_##item##_log_by_idx(int idx) {	\
	return NULL;							\
}									\
static inline struct item##_log *dss_get_##item##_log_iter(int idx) {	\
	return NULL;							\
}									\
static inline unsigned long dss_get_vaddr_##item##_log(void) {		\
	return 0;							\
}

static inline void dbg_snapshot_spin_func(void) { }

#endif /* CONFIG_DEBUG_SNAPSHOT */

#define for_each_item_in_dss_by_cpu(item, cpu, start, len, direction)	\
	for (item = dss_get_##item##_log_by_cpu_iter(cpu, start);	\
		len && item;						\
		item = dss_get_##item##_log_by_cpu_iter(cpu,		\
				direction ? ++start : --start), --len)

#define dss_get_start_addr_of_log_by_cpu(cpu, item, vaddr)		\
	(vaddr ? dss_get_vaddr_##item##_log_by_cpu(cpu) :		\
		dss_get_vaddr_##item##_log_by_cpu(cpu) -		\
		dbg_snapshot_get_item_vaddr("log_kevents") +		\
		dbg_snapshot_get_item_paddr("log_kevents"))

#define for_each_item_in_dss(item, start, len, direction)		\
	for (item = dss_get_##item##_log_by_iter(start);		\
		len && item;						\
		item = dss_get_##item##_log_by_iter(			\
			direction ? ++start : --start), --len)

#define dss_get_start_addr_of_log(item, vaddr)				\
	(vaddr ? dss_get_vaddr_##item##_log() :				\
		dss_get_vaddr_##item##_log() -				\
		dbg_snapshot_get_item_vaddr("log_kevents") +		\
		dbg_snapshot_get_item_paddr("log_kevents"))

dss_extern_get_log_by_cpu(task);
dss_extern_get_log_by_cpu(work);
dss_extern_get_log_by_cpu(cpuidle);
dss_extern_get_log_by_cpu(freq);
dss_extern_get_log_by_cpu(irq);
dss_extern_get_log_by_cpu(spinlock);
dss_extern_get_log_by_cpu(hrtimer);
dss_extern_get_log(suspend);
dss_extern_get_log(clk);
dss_extern_get_log(pmu);
dss_extern_get_log(dm);
dss_extern_get_log(regulator);
dss_extern_get_log(thermal);
dss_extern_get_log(acpm);
dss_extern_get_log(print);

/**
 * dsslog_flag - added log information supported.
 * @DSS_FLAG_IN: Generally, marking into the function
 * @DSS_FLAG_ON: Generally, marking the status not in, not out
 * @DSS_FLAG_OUT: Generally, marking come out the function
 * @DSS_FLAG_SOFTIRQ: Marking to pass the softirq function
 * @DSS_FLAG_SOFTIRQ_HI_TASKLET: Marking to pass the tasklet function
 * @DSS_FLAG_SOFTIRQ_TASKLET: Marking to pass the tasklet function
 */
enum dsslog_flag {
	DSS_FLAG_IN			= 1,
	DSS_FLAG_ON			= 2,
	DSS_FLAG_OUT			= 3,
	DSS_FLAG_SOFTIRQ		= 10000,
	DSS_FLAG_CALL_TIMER_FN		= 20000,
	DSS_FLAG_SMP_CALL_FN		= 30000,
};

enum dss_item_index {
	DSS_ITEM_HEADER_ID = 0,
	DSS_ITEM_KERNEL_ID,
	DSS_ITEM_PLATFORM_ID,
	DSS_ITEM_FATAL_ID,
	DSS_ITEM_KEVENTS_ID,
	DSS_ITEM_S2D_ID,
	DSS_ITEM_ARRDUMP_RESET_ID,
	DSS_ITEM_ARRDUMP_PANIC_ID,
	DSS_ITEM_FIRST_ID,
	DSS_ITEM_INIT_TASK_ID,
};

enum dss_log_item_indx {
	DSS_LOG_TASK_ID = 0,
	DSS_LOG_WORK_ID,
	DSS_LOG_CPUIDLE_ID,
	DSS_LOG_SUSPEND_ID,
	DSS_LOG_IRQ_ID,
	DSS_LOG_SPINLOCK_ID,
	DSS_LOG_HRTIMER_ID,
	DSS_LOG_CLK_ID,
	DSS_LOG_PMU_ID,
	DSS_LOG_FREQ_ID,
	DSS_LOG_DM_ID,
	DSS_LOG_REGULATOR_ID,
	DSS_LOG_THERMAL_ID,
	DSS_LOG_ACPM_ID,
	DSS_LOG_PRINTK_ID,
};

struct dbg_snapshot_helper_ops {
	int (*start_watchdog)(bool, int, int);
	int (*expire_watchdog)(unsigned int, int);
	int (*stop_watchdog)(int);
	int (*stop_all_cpus)(void);
	int (*run_arraydump)(void);
	void (*run_scandump_mode)(void);
};
#endif
