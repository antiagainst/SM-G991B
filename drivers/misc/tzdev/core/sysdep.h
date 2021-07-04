/*
 * Copyright (C) 2012-2017, Samsung Electronics Co., Ltd.
 *
 * This software is licensed under the terms of the GNU General Public
 * License version 2, as published by the Free Software Foundation, and
 * may be copied, distributed, and modified under those terms.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 */

#ifndef __SYSDEP_H__
#define __SYSDEP_H__

#include <linux/completion.h>
#include <linux/cpumask.h>
#include <linux/fs.h>
#include <linux/kernel.h>
#include <linux/types.h>
#include <linux/version.h>

#include <asm/barrier.h>
#include <asm/cacheflush.h>

#include <crypto/hash.h>

#if LINUX_VERSION_CODE >= KERNEL_VERSION(4, 10, 0)
#include <uapi/linux/sched/types.h>
#include <linux/wait.h>

#ifndef CPU_UP_CANCELED
#define CPU_UP_CANCELED		0x0004
#endif
#ifndef CPU_DOWN_PREPARE
#define CPU_DOWN_PREPARE	0x0005
#endif

typedef struct wait_queue_entry wait_queue_t;
#endif

#if LINUX_VERSION_CODE >= KERNEL_VERSION(4, 11, 0)
#include <linux/refcount.h>
#include <linux/sched.h>
#include <linux/sched/mm.h>
#include <linux/sched/task.h>
#endif

#if LINUX_VERSION_CODE >= KERNEL_VERSION(4, 6, 0)
#include <crypto/hash.h>
#endif

/* For MAX_RT_PRIO definitions */
#if LINUX_VERSION_CODE >= KERNEL_VERSION(3, 15, 0)
#include <linux/sched/prio.h>
#else
#include <linux/sched/rt.h>
#endif

#if defined(CONFIG_ARM64)
#define outer_inv_range(s, e)
#else
#define __flush_dcache_area(s, e)	__cpuc_flush_dcache_area(s, e)
#endif

#if LINUX_VERSION_CODE <= KERNEL_VERSION(3, 13, 0)
#define U8_MAX		((u8)~0U)
#define S8_MAX		((s8)(U8_MAX>>1))
#define S8_MIN		((s8)(-S8_MAX - 1))
#define U16_MAX		((u16)~0U)
#define S16_MAX		((s16)(U16_MAX>>1))
#define S16_MIN		((s16)(-S16_MAX - 1))
#define U32_MAX		((u32)~0U)
#define S32_MAX		((s32)(U32_MAX>>1))
#define S32_MIN		((s32)(-S32_MAX - 1))
#define U64_MAX		((u64)~0ULL)
#define S64_MAX		((s64)(U64_MAX>>1))
#define S64_MIN		((s64)(-S64_MAX - 1))
#endif

#if LINUX_VERSION_CODE >= KERNEL_VERSION(4, 0, 0)
#define for_each_cpu_mask(cpu, mask)	for_each_cpu((cpu), &(mask))
#define cpu_isset(cpu, mask)		cpumask_test_cpu((cpu), &(mask))
#else
#define cpumask_pr_args(maskp)		nr_cpu_ids, cpumask_bits(maskp)
#endif

#if LINUX_VERSION_CODE >= KERNEL_VERSION(4, 13, 0)
#define sysdep_kernel_read(file, buf, size, off)	kernel_read(file, buf, size, &off)
#else
#define sysdep_kernel_read(file, buf, size, off)	\
({							\
	int ret;					\
	ret = kernel_read(file, off, buf, size);	\
	off += ret;					\
	ret;						\
})
#endif

#if LINUX_VERSION_CODE < KERNEL_VERSION(4, 5, 0)
static inline void shash_desc_zero(struct shash_desc *desc)
{
	memset(desc, 0,
		sizeof(*desc) + crypto_shash_descsize(desc->tfm));
}
#endif

#if LINUX_VERSION_CODE < KERNEL_VERSION(3, 17, 0)
#define SHASH_DESC_ON_STACK(shash, ctx)                           \
	char __##shash##_desc[sizeof(struct shash_desc) +         \
		crypto_shash_descsize(ctx)] CRYPTO_MINALIGN_ATTR; \
	struct shash_desc *shash = (struct shash_desc *)__##shash##_desc
#endif

#if LINUX_VERSION_CODE < KERNEL_VERSION(4, 7, 0)
static inline int atomic_fetch_or(int mask, atomic_t *v)
{
	int c, old;

	old = atomic_read(v);
	do {
		c = old;
		old = atomic_cmpxchg((v), c, c | mask);
	} while(old != c);

	return old;
}
#endif

#if LINUX_VERSION_CODE < KERNEL_VERSION(3, 13, 0)
static inline void reinit_completion(struct completion *x)
{
	x->done = 0;
}
#endif

#if LINUX_VERSION_CODE >= KERNEL_VERSION(4, 2, 0)
#define set_mb(var, value)		\
	do {				\
		var = value;		\
		smp_mb();		\
	} while (0)
#endif

#if LINUX_VERSION_CODE >= KERNEL_VERSION(4, 1, 0)
static inline int check_mem_region(unsigned long start, unsigned long n)
{
	(void) start;
	(void) n;

	return 0;
}
#endif

#if LINUX_VERSION_CODE >= KERNEL_VERSION(5, 4, 0)
#define sysdep_alloc_workqueue_attrs()		alloc_workqueue_attrs()
#else
#define sysdep_alloc_workqueue_attrs()		alloc_workqueue_attrs(GFP_KERNEL)
#endif

long sysdep_get_user_pages(struct task_struct *task,
		struct mm_struct *mm, unsigned long start, unsigned long nr_pages,
		int write, int force, struct page **pages,
		struct vm_area_struct **vmas);
void sysdep_register_cpu_notifier(struct notifier_block* notifier,
		int (*startup)(unsigned int cpu),
		int (*teardown)(unsigned int cpu));
void sysdep_unregister_cpu_notifier(struct notifier_block* notifier);
int sysdep_crypto_file_sha1(uint8_t *hash, struct file *file);
int sysdep_vfs_getattr(struct file *filp, struct kstat *stat);
void sysdep_shash_desc_init(struct shash_desc *desc, struct crypto_shash *tfm);
int sysdep_pid_refcount_read(struct pid *pid);

#endif /* __SYSDEP_H__ */
