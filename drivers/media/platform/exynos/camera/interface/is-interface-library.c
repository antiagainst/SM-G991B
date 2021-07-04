/*
 * Samsung EXYNOS FIMC-IS (Imaging Subsystem) driver
 *
 * Copyright (C) 2014 Samsung Electronics Co., Ltd.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 as
 * published by the Free Software Foundation.
 */

#include <linux/random.h>
#include <linux/firmware.h>
#include <linux/mm.h>
#include <linux/kallsyms.h>
#include <linux/stacktrace.h>
#include <linux/file.h>

#include <asm/cacheflush.h>
#include <asm/pgtable.h>
#include <asm/tlbflush.h>
#include <asm/neon.h>

#include "is-interface-library.h"
#ifdef CONFIG_PABLO_V8_20_0
#include "is-interface-vra-v2.h"
#else
#include "is-interface-vra.h"
#endif
#include "../is-device-ischain.h"
#include "is-vender.h"
#include "votf/camerapp-votf.h"
#ifdef CONFIG_RKP
#include <linux/rkp.h>
#endif
#if IS_ENABLED(CONFIG_EXYNOS_MEMORY_LOGGER)
#include <soc/samsung/memlogger.h>
#endif

int debug_ddk;
module_param(debug_ddk, int, 0644);
char *attr_ddk[4];
module_param_array(attr_ddk, charp, NULL, 0644);
int debug_rta;
module_param(debug_rta, int, 0644);

struct is_lib_support gPtr_lib_support;
struct mutex gPtr_bin_load_ctrl;
extern struct is_lib_vra *g_lib_vra;

/*
 * Log write
 */

int is_log_write_console(char *str)
{
	is_info("[@][LIB] %s", str);
	return 0;
}

int is_log_write(const char *str, ...)
{
	struct is_lib_support *lib = &gPtr_lib_support;
	int size = 0;
	int cpu = raw_smp_processor_id();
	unsigned long long t;
	unsigned long usec;
	va_list args;
	ulong debug_kva, ctrl_kva;
	unsigned long flag;

	va_start(args, str);
	size += vscnprintf(lib->string + size,
			  sizeof(lib->string) - size, str, args);
	va_end(args);

	t = local_clock();
	usec = do_div(t, NSEC_PER_SEC) / NSEC_PER_USEC;
	size = snprintf(lib->log_buf, sizeof(lib->log_buf),
		"[%5lu.%06lu] [%d] %s",
		(unsigned long)t, usec, cpu, lib->string);

	if (size > MAX_LIB_LOG_BUF)
		size = MAX_LIB_LOG_BUF;

	spin_lock_irqsave(&lib->slock_debug, flag);

	debug_kva = lib->minfo->kvaddr_debug;
	ctrl_kva  = lib->minfo->kvaddr_debug_cnt;

	if (lib->log_ptr == 0) {
		lib->log_ptr = debug_kva;
	} else if (lib->log_ptr >= (ctrl_kva - size)) {
		memcpy((void *)lib->log_ptr, (void *)&lib->log_buf,
			(ctrl_kva - lib->log_ptr));
		size -= (debug_kva + DEBUG_REGION_SIZE - (u32)lib->log_ptr);
		lib->log_ptr = debug_kva;
	}

	memcpy((void *)lib->log_ptr, (void *)&lib->log_buf, size);
	lib->log_ptr += size;

	*((int *)(ctrl_kva)) = (lib->log_ptr - debug_kva);

#if IS_ENABLED(CONFIG_EXYNOS_MEMORY_LOGGER)
	memlog_write_printf(is_debug.mobj_printf_ddk, MEMLOG_LEVEL_INFO, "[%d] %s", cpu, lib->string);
#endif
	spin_unlock_irqrestore(&lib->slock_debug, flag);

	return 0;
}

int is_event_write(const char *str, ...)
{
	struct is_lib_support *lib = &gPtr_lib_support;
	int size = 0;
	int cpu = raw_smp_processor_id();
	unsigned long long t;
	unsigned long usec;
	va_list args;
	ulong debug_kva = lib->minfo->kvaddr_event;
	ulong ctrl_kva  = lib->minfo->kvaddr_event_cnt;

	va_start(args, str);
	size += vscnprintf(lib->event_string + size,
			  sizeof(lib->event_string) - size, str, args);
	va_end(args);

	t = local_clock();
	usec = do_div(t, NSEC_PER_SEC) / NSEC_PER_USEC;
	size = snprintf(lib->event_buf, sizeof(lib->event_buf),
		"[%5lu.%06lu] [%d] %s",
		(unsigned long)t, usec, cpu, lib->event_string);

	if (size > 256)
		return -ENOMEM;

	if (lib->event_ptr == 0) {
		lib->event_ptr = debug_kva;
	} else if (lib->event_ptr >= (ctrl_kva - size)) {
		memcpy((void *)lib->event_ptr, (void *)&lib->event_buf,
			(ctrl_kva - lib->event_ptr));
		size -= (debug_kva + DEBUG_REGION_SIZE - (u32)lib->event_ptr);
		lib->event_ptr = debug_kva;
	}

	memcpy((void *)lib->event_ptr, (void *)&lib->event_buf, size);
	lib->event_ptr += size;

	*((int *)(ctrl_kva)) = (lib->event_ptr - debug_kva);

	return 0;
}

#ifdef LIB_MEM_TRACK
#ifdef LIB_MEM_TRACK_CALLSTACK
static void save_callstack(ulong *addrs)
{
	struct stack_trace trace;
	int i;

	trace.nr_entries = 0;
	trace.max_entries = MEM_TRACK_ADDRS_COUNT;
	trace.entries = addrs;
	trace.skip = 3;
	save_stack_trace(&trace);

	if (trace.nr_entries != 0 &&
			trace.entries[trace.nr_entries - 1] == ULONG_MAX)
		trace.nr_entries--;

	for (i = trace.nr_entries; i < MEM_TRACK_ADDRS_COUNT; i++)
		addrs[i] = 0;
}
#endif

static inline void add_alloc_track(int type, ulong addr, size_t size)
{
	struct is_lib_support *lib = &gPtr_lib_support;
	struct lib_mem_tracks *tracks;
	struct lib_mem_track *track;
	unsigned long flag;

	spin_lock_irqsave(&lib->slock_mem_track, flag);
	if ((!lib->cur_tracks)
	    || (lib->cur_tracks && (lib->cur_tracks->num_of_track == MEM_TRACK_COUNT))) {
		err_lib("exceed memory track");
		spin_unlock_irqrestore(&lib->slock_mem_track, flag);
		return;
	}

	/* add a new alloc track */
	tracks = lib->cur_tracks;
	track = &tracks->track[tracks->num_of_track];
	tracks->num_of_track++;

	spin_unlock_irqrestore(&lib->slock_mem_track, flag);

	track->type = type;
	track->addr = addr;
	track->size = size;
	track->status = MT_STATUS_ALLOC;
	track->alloc.lr = (ulong)__builtin_return_address(0);
	track->alloc.cpu = raw_smp_processor_id();
	track->alloc.pid = current->pid;
	track->alloc.when = local_clock();

#ifdef LIB_MEM_TRACK_CALLSTACK
	save_callstack(track->alloc.addrs);
#endif
}

static inline void add_free_track(int type, ulong addr)
{
	struct is_lib_support *lib = &gPtr_lib_support;
	struct lib_mem_tracks *tracks, *temp;
	struct lib_mem_track *track;
	int found = 0;
	int i;
	unsigned long flag;

	spin_lock_irqsave(&lib->slock_mem_track, flag);
	list_for_each_entry_safe(tracks, temp, &lib->list_of_tracks, list) {
		for (i = 0; i < tracks->num_of_track; i++) {
			track = &tracks->track[i];

			if ((track->addr == addr)
				&& (track->status == MT_STATUS_ALLOC)
				&& (track->type == type)) {
				track->status |= MT_STATUS_FREE;
				track->free.lr = (ulong)__builtin_return_address(0);
				track->free.cpu = raw_smp_processor_id();
				track->free.pid = current->pid;
				track->free.when = local_clock();

#ifdef LIB_MEM_TRACK_CALLSTACK
				save_callstack(track->free.addrs);
#endif
				found = 1;
				break;
			}
		}

		if (found)
			break;
	}
	spin_unlock_irqrestore(&lib->slock_mem_track, flag);

	if (!found)
		err_lib("could not find buffer [0x%x, 0x%08lx]", type, addr);
}

static void print_track(const char *lvl, const char *str, struct lib_mem_track *track)
{
	unsigned long long when;
	ulong usec;
#ifdef LIB_MEM_TRACK_CALLSTACK
	int i;
#endif

	printk("%s %s type: 0x%x, addr: 0x%lx, size: %zd, status: %d\n",
			lvl, str,
			track->type, track->addr, track->size, track->status);

	when = track->alloc.when;
	usec = do_div(when, NSEC_PER_SEC) / NSEC_PER_USEC;
	printk("%s\tallocated - when: %5lu.%06lu, from: %pS, cpu: %d, pid: %d\n",
			lvl, (ulong)when, usec, (void *)track->alloc.lr,
			track->alloc.cpu, track->alloc.pid);

#ifdef LIB_MEM_TRACK_CALLSTACK
	for (i = 0; i < MEM_TRACK_ADDRS_COUNT; i++) {
		if (track->alloc.addrs[i])
			printk("%s\t\t[<%p>] %pS\n", lvl,
					(void *)track->alloc.addrs[i],
					(void *)track->alloc.addrs[i]);
		else
			break;
	}
#endif

	when = track->free.when;
	usec = do_div(when, NSEC_PER_SEC) / NSEC_PER_USEC;
	printk("%s\tfree - when: %5lu.%06lu, from: %pS, cpu: %d, pid: %d\n",
			lvl, (ulong)when, usec, (void *)track->free.lr,
			track->free.cpu, track->free.pid);

#ifdef LIB_MEM_TRACK_CALLSTACK
	for (i = 0; i < MEM_TRACK_ADDRS_COUNT; i++) {
		if (track->free.addrs[i])
			printk("%s\t\t[<%p>] %pS\n", lvl,
					(void *)track->free.addrs[i],
					(void *)track->free.addrs[i]);
		else
			break;
	}
#endif
}

static void print_tracks_status(const char *lvl, const char *str, int status)
{
	struct is_lib_support *lib = &gPtr_lib_support;
	struct lib_mem_tracks *tracks, *temp;
	struct lib_mem_track *track;
	int i;

	list_for_each_entry_safe(tracks, temp, &lib->list_of_tracks, list) {
		info_lib("number of memory track: %d\n", tracks->num_of_track);
		for (i = 0; i < tracks->num_of_track; i++) {
			track = &tracks->track[i];

			if (track->status == status)
				print_track(lvl, str, track);
		}
	}
}

static void free_tracks(void)
{
	struct is_lib_support *lib = &gPtr_lib_support;
	struct lib_mem_tracks *tracks;
	unsigned long flag;

	spin_lock_irqsave(&lib->slock_mem_track, flag);
	while (!list_empty(&lib->list_of_tracks)) {
		tracks = list_entry((&lib->list_of_tracks)->next,
						struct lib_mem_tracks, list);

		list_del(&tracks->list);
		is_vfree_atomic(tracks);
	}
	spin_unlock_irqrestore(&lib->slock_mem_track, flag);
}
#endif

/*
 * Memory alloc, free
 */
static void mblk_init(struct lib_mem_block *mblk, struct is_priv_buf *pb,
	u32 type, const char *name)
{
	if (!pb) {
		info_lib("memory block %s - not used\n", name);
		return;
	}

	spin_lock_init(&mblk->lock);
	INIT_LIST_HEAD(&mblk->list);
	mblk->align = pb->align;

	if (type == MT_TYPE_MB_DMA_TNR_S)
		mblk->kva_base = 0;
	else
		mblk->kva_base = CALL_BUFOP(pb, kvaddr, pb);

	mblk->dva_base = CALL_BUFOP(pb, dvaddr, pb);
	mblk->pb = pb;
	mblk->end = 0;
	strlcpy(mblk->name, name, sizeof(mblk->name));
	mblk->type = type;

	info_lib("memory block %s - kva: 0x%lx, dva: %pad, size: %zu\n",
			mblk->name, mblk->kva_base,
			&mblk->dva_base, mblk->pb->size);

	if (mblk->used) {
		warn_lib("memory block (%s) was not wiped out. used: %d",
				mblk->name, mblk->used);
		mblk->used = 0;
	}
}

static void *alloc_from_mblk(struct lib_mem_block *mblk, u32 size)
{
	struct lib_buf *buf;
	unsigned long flag;
	int fpsimd_flag = 0;

	if (!size) {
		err_lib("invalid request size: %d from (%s)", size, mblk->name);
		return NULL;
	}

	if (preempt_count() != 0) {
		is_fpsimd_put_func();
		fpsimd_flag = 1;
	}

	buf = kzalloc(sizeof(struct lib_buf), GFP_KERNEL);

	if (fpsimd_flag)
		is_fpsimd_get_func();

	if (!buf) {
		err_lib("failed to allocate a library buffer");
		return NULL;
	}

	spin_lock_irqsave(&mblk->lock, flag);
	if ((mblk->end + size) > mblk->pb->size) {
		spin_unlock_irqrestore(&mblk->lock, flag);
		kfree(buf);

		err_lib("out of (%s) memory block, available: %zu, request: %d",
			mblk->name, mblk->pb->size - mblk->end, size);
		return NULL;
	} else {
		buf->kva = mblk->kva_base + mblk->end;
		buf->dva = mblk->dva_base + mblk->end;
		buf->size = mblk->align ? ALIGN(size, mblk->align) : size;
		buf->priv = mblk;

		mblk->used += buf->size;
		mblk->end += buf->size;

		spin_unlock_irqrestore(&mblk->lock, flag);
	}

	spin_lock_irqsave(&mblk->lock, flag);
	list_add(&buf->list, &mblk->list);
	spin_unlock_irqrestore(&mblk->lock, flag);

	dbg_lib(3, "allocated memory info (%s)\n", mblk->name);
	dbg_lib(3, "\tkva: 0x%lx, dva: %pad, size: %d\n",
			buf->kva, &buf->dva, buf->size);

	is_debug_event_print(IS_EVENT_CRITICAL,
		NULL,
		NULL,
		0,
		"%s, %s, %d, 0x%16lx, 0x%8lx",
		__func__,
		current->comm,
		buf->size,
		buf->kva,
		&buf->dva);

#ifdef LIB_MEM_TRACK
	add_alloc_track(mblk->type, buf->kva, buf->size);
#endif

	return (void *)buf->kva;
}

static void free_to_mblk(struct lib_mem_block *mblk, void *kva)
{
	struct lib_buf *buf, *temp;
	unsigned long flag;

	if (!kva)
		return;

	spin_lock_irqsave(&mblk->lock, flag);
	list_for_each_entry_safe(buf, temp, &mblk->list, list) {
		if ((void *)buf->kva == kva) {
			mblk->used -= buf->size;
			mblk->end -= buf->size;
#ifdef LIB_MEM_TRACK
			add_free_track(mblk->type, buf->kva);
#endif
			is_debug_event_print(IS_EVENT_NORMAL,
				NULL,
				NULL,
				0,
				"%s, %s, %d, 0x%16lx, %pad",
				__func__,
				current->comm,
				buf->size,
				buf->kva,
				&buf->dva);

			dbg_lib(3, "free memory info (%s)\n", mblk->name);
			dbg_lib(3, "\tkva: 0x%lx, dva: %pad, size: %d\n",
					buf->kva, &buf->dva, buf->size);

			list_del(&buf->list);
			kfree(buf);

			break;
		}
	}
	spin_unlock_irqrestore(&mblk->lock, flag);
}

void *is_alloc_dma(int type, u32 size)
{
	struct lib_mem_block *mblk = NULL;

	mblk = (struct lib_mem_block *)is_get_dma_blk(type);
	return alloc_from_mblk(mblk, size);
}

void is_free_dma(int type, void *kva)
{
	struct lib_mem_block *mblk = NULL;

	mblk = (struct lib_mem_block *)is_get_dma_blk(type);

	return free_to_mblk(mblk, kva);
}

void *is_alloc_dma_taaisp(u32 size)
{
	return is_alloc_dma(ID_DMA_3AAISP, size);
}

void is_free_dma_taaisp(void *kva)
{
	return is_free_dma(ID_DMA_3AAISP, kva);
}

void *is_alloc_dma_medrc(u32 size)
{
	struct is_lib_support *lib = &gPtr_lib_support;

	return alloc_from_mblk(&lib->mb_dma_medrc, size);
}

void is_free_dma_medrc(void *kva)
{
	struct is_lib_support *lib = &gPtr_lib_support;

	return free_to_mblk(&lib->mb_dma_medrc, kva);
}

void *is_alloc_dma_tnr(u32 size)
{
	struct is_lib_support *lib = &gPtr_lib_support;

	return alloc_from_mblk(&lib->mb_dma_tnr, size);
}

void *is_alloc_dma_tnr_s(u32 size)
{
	struct is_lib_support *lib = &gPtr_lib_support;

	return alloc_from_mblk(&lib->mb_dma_tnr_s, size);
}

void is_free_dma_tnr(void *kva)
{
	struct is_lib_support *lib = &gPtr_lib_support;

	return free_to_mblk(&lib->mb_dma_tnr, kva);
}

void is_free_dma_tnr_s(void *kva)
{
	struct is_lib_support *lib = &gPtr_lib_support;

	return free_to_mblk(&lib->mb_dma_tnr_s, kva);
}

void *is_alloc_dma_orbmch(u32 size)
{
	struct is_lib_support *lib = &gPtr_lib_support;

	return alloc_from_mblk(&lib->mb_dma_orbmch, size);
}

void is_free_dma_orbmch(void *kva)
{
	struct is_lib_support *lib = &gPtr_lib_support;

	return free_to_mblk(&lib->mb_dma_orbmch, kva);
}

void *is_alloc_dma_clahe(u32 size)
{
	struct is_lib_support *lib = &gPtr_lib_support;

	return alloc_from_mblk(&lib->mb_dma_clahe, size);
}

void is_free_dma_clahe(void *kva)
{
	struct is_lib_support *lib = &gPtr_lib_support;

	return free_to_mblk(&lib->mb_dma_clahe, kva);
}

void *is_alloc_vra(u32 size)
{
	struct is_lib_support *lib = &gPtr_lib_support;

	return alloc_from_mblk(&lib->mb_vra, size);
}

void is_free_vra(void *kva)
{
	struct is_lib_support *lib = &gPtr_lib_support;

	return free_to_mblk(&lib->mb_vra, kva);
}

void *is_alloc_vra_net_array(u32 size)
{
	struct is_lib_support *lib = &gPtr_lib_support;

	return alloc_from_mblk(&lib->mb_vra_net_array, size);
}

void is_free_vra_net_array(void *kva)
{
	struct is_lib_support *lib = &gPtr_lib_support;

	return free_to_mblk(&lib->mb_vra_net_array, kva);
}

static void __maybe_unused *is_alloc_dma_pb(u32 size)
{
	struct is_lib_support *lib = &gPtr_lib_support;
	struct is_core *core;
	struct is_resourcemgr *resourcemgr;
	struct is_mem *mem;
	struct is_priv_buf *pb;
	struct lib_buf *buf;
	unsigned long flag;

	core = (struct is_core *)platform_get_drvdata(lib->pdev);
	resourcemgr = &core->resourcemgr;
	mem = &resourcemgr->mem;

	if (!size) {
		err_lib("invalid request size: %d from (%s)", size, "DMA");
		return NULL;
	}

	buf = kzalloc(sizeof(struct lib_buf), GFP_KERNEL);
	if (!buf) {
		err_lib("failed to allocate a library buffer");
		return NULL;
	}

	pb = CALL_PTR_MEMOP(mem, alloc, mem->default_ctx, size, NULL, 0);
	if (IS_ERR_OR_NULL(pb)) {
		err_lib("failed to allocate a private buffer");
		kfree(buf);
		return NULL;
	}

	buf->kva = CALL_BUFOP(pb, kvaddr, pb);
	buf->dva = CALL_BUFOP(pb, dvaddr, pb);
	buf->size = size;
	buf->priv = pb;

	spin_lock_irqsave(&lib->slock_nmb, flag);
	list_add(&buf->list, &lib->list_of_nmb);
	spin_unlock_irqrestore(&lib->slock_nmb, flag);

	dbg_lib(3, "allocated memory info (%s)\n", "DMA");
	dbg_lib(3, "\tkva: 0x%lx, dva: %pad, size: %d\n",
		buf->kva, &buf->dva, buf->size);

#ifdef ENABLE_DBG_EVENT
	snprintf(lib->debugmsg, sizeof(lib->debugmsg),
		"%s, %s, %d, 0x%16lx, %pad",
		__func__, current->comm,
		buf->size, buf->kva, &buf->dva);
	is_debug_event_add(IS_EVENT_CRITICAL, debugmsg,
				NULL, NULL, 0);
#endif

#ifdef LIB_MEM_TRACK
	add_alloc_track(MT_TYPE_DMA, buf->kva, buf->size);
#endif

	return (void *)buf->kva;
}

static void __maybe_unused is_free_dma_pb(void *kva)
{
	struct is_lib_support *lib = &gPtr_lib_support;
	struct is_core *core;
	struct is_resourcemgr *resourcemgr;
	struct is_mem *mem;
	struct is_priv_buf *pb;
	struct lib_buf *buf, *temp;
	unsigned long flag;

	core = (struct is_core *)platform_get_drvdata(lib->pdev);
	resourcemgr = &core->resourcemgr;
	mem = &resourcemgr->mem;

	if (!kva)
		return;

	spin_lock_irqsave(&lib->slock_nmb, flag);
	list_for_each_entry_safe(buf, temp, &lib->list_of_nmb, list) {
		if ((void *)buf->kva == kva) {
#ifdef LIB_MEM_TRACK
			add_free_track(MT_TYPE_DMA, buf->kva);
#endif

#ifdef ENABLE_DBG_EVENT
			snprintf(lib->debugmsg, sizeof(lib->debugmsg),
				"%s, %s, %d, 0x%16lx, %pad",
				__func__, current->comm,
				buf->size, buf->kva, &buf->dva);
			is_debug_event_add(IS_EVENT_NORMAL, debugmsg,
						NULL, NULL, 0);
#endif

			dbg_lib(3, "free memory info (%s)\n", "DMA");
			dbg_lib(3, "\tkva: 0x%lx, dva: %pad, size: %d\n",
					buf->kva, &buf->dva, buf->size);

			/* allocation specific */
			pb = (struct is_priv_buf *)buf->priv;
			CALL_VOID_BUFOP(pb, free, pb);

			list_del(&buf->list);
			kfree(buf);

			break;
		}
	}
	spin_unlock_irqrestore(&lib->slock_nmb, flag);
}

/*
 * Address translation
 */
static int mblk_dva(struct lib_mem_block *mblk, ulong kva, u32 *dva)
{
	if ((kva < mblk->kva_base) || (kva >= (mblk->kva_base + mblk->pb->size))) {
		err_lib("invalid DVA request - kva: 0x%lx", kva);
		*dva = 0;
		WARN_ON(1);

		return -EINVAL;
	}

	*dva = mblk->dva_base + (kva - mblk->kva_base);
	dbg_lib(3, "[%s]DVA request - kva: 0x%lx, dva: 0x%x\n", mblk->name, kva, *dva);

	return 0;
}

static int mblk_kva(struct lib_mem_block *mblk, u32 dva, ulong *kva)
{
	if ((dva < mblk->dva_base) || (dva >= (mblk->dva_base + mblk->pb->size))) {
		err_lib("invalid KVA request - dva: 0x%x", dva);
		*kva = 0;
		WARN_ON(1);

		return -EINVAL;
	}

	*kva = mblk->kva_base + (dva - mblk->dva_base);
	dbg_lib(3, "[%s]KVA request - dva: 0x%x, kva: 0x%lx\n", mblk->name, dva, *kva);

	return 0;
}

int is_dva_dma(int type, ulong kva, u32 *dva)
{
	struct lib_mem_block *mblk = NULL;

	mblk = (struct lib_mem_block *)is_get_dma_blk(type);

	return mblk_dva(mblk, kva, dva);
}

int is_kva_dma(int type, u32 dva, ulong *kva)
{
	struct lib_mem_block *mblk = NULL;

	mblk = (struct lib_mem_block *)is_get_dma_blk(type);

	return mblk_kva(mblk, dva, kva);
}

int is_dva_dma_taaisp(ulong kva, u32 *dva)
{
	return is_dva_dma(ID_DMA_3AAISP, kva, dva);
}

int is_dva_dma_medrc(ulong kva, u32 *dva)
{
	struct is_lib_support *lib = &gPtr_lib_support;

	return mblk_dva(&lib->mb_dma_medrc, kva, dva);
}

int is_dva_dma_tnr(ulong kva, u32 *dva)
{
	struct is_lib_support *lib = &gPtr_lib_support;

	return mblk_dva(&lib->mb_dma_tnr, kva, dva);
}

int is_dva_dma_tnr_s(ulong kva, u32 *dva)
{
	struct is_lib_support *lib = &gPtr_lib_support;

	return mblk_dva(&lib->mb_dma_tnr_s, kva, dva);
}

int is_dva_dma_orbmch(ulong kva, u32 *dva)
{
	struct is_lib_support *lib = &gPtr_lib_support;

	return mblk_dva(&lib->mb_dma_orbmch, kva, dva);
}

int is_dva_dma_clahe(ulong kva, u32 *dva)
{
	struct is_lib_support *lib = &gPtr_lib_support;

	return mblk_dva(&lib->mb_dma_clahe, kva, dva);
}

int is_dva_vra(ulong kva, u32 *dva)
{
	struct is_lib_support *lib = &gPtr_lib_support;

	return mblk_dva(&lib->mb_vra, kva, dva);
}

int is_dva_vra_net_array(ulong kva, u32 *dva)
{
	struct is_lib_support *lib = &gPtr_lib_support;

	return mblk_dva(&lib->mb_vra_net_array, kva, dva);
}

int is_kva_dma_taaisp(u32 dva, ulong *kva)
{
	return is_kva_dma(ID_DMA_3AAISP, dva, kva);
}

int is_kva_dma_medrc(u32 dva, ulong *kva)
{
	struct is_lib_support *lib = &gPtr_lib_support;

	return mblk_kva(&lib->mb_dma_medrc, dva, kva);
}

int is_kva_dma_tnr(u32 dva, ulong *kva)
{
	struct is_lib_support *lib = &gPtr_lib_support;

	return mblk_kva(&lib->mb_dma_tnr, dva, kva);
}

int is_kva_dma_tnr_s(u32 dva, ulong *kva)
{
	struct is_lib_support *lib = &gPtr_lib_support;

	return mblk_kva(&lib->mb_dma_tnr_s, dva, kva);
}

int is_kva_dma_orbmch(u32 dva, ulong *kva)
{
	struct is_lib_support *lib = &gPtr_lib_support;

	return mblk_kva(&lib->mb_dma_orbmch, dva, kva);
}

int is_kva_dma_clahe(u32 dva, ulong *kva)
{
	struct is_lib_support *lib = &gPtr_lib_support;

	return mblk_kva(&lib->mb_dma_clahe, dva, kva);
}

int is_kva_vra(u32 dva, ulong *kva)
{
	struct is_lib_support *lib = &gPtr_lib_support;

	return mblk_kva(&lib->mb_vra, dva, kva);
}

/*
 * cache operations
 */
static void mblk_inv(struct lib_mem_block *mblk, ulong kva, u32 size)
{
	CALL_BUFOP(mblk->pb, sync_for_cpu, mblk->pb,
		(kva - mblk->kva_base),	size,
		DMA_FROM_DEVICE);
}

static void mblk_clean(struct lib_mem_block *mblk, ulong kva, u32 size)
{
	CALL_BUFOP(mblk->pb, sync_for_device, mblk->pb,
		(kva - mblk->kva_base),	size,
		DMA_TO_DEVICE);
}

void is_inv_dma(int type, ulong kva, u32 size)
{
	struct lib_mem_block *mblk = NULL;

	mblk = (struct lib_mem_block *)is_get_dma_blk(type);

	return mblk_inv(mblk, kva, size);
}

void is_clean_dma(int type, ulong kva, u32 size)
{
	struct lib_mem_block *mblk = NULL;

	mblk = (struct lib_mem_block *)is_get_dma_blk(type);

	return mblk_clean(mblk, kva, size);
}

void is_inv_dma_taaisp(ulong kva, u32 size)
{
	return is_inv_dma(ID_DMA_3AAISP, kva, size);
}

void is_inv_dma_medrc(ulong kva, u32 size)
{
	struct is_lib_support *lib = &gPtr_lib_support;

	return mblk_inv(&lib->mb_dma_medrc, kva, size);
}

void is_clean_dma_medrc(ulong kva, u32 size)
{
	struct is_lib_support *lib = &gPtr_lib_support;

	return mblk_clean(&lib->mb_dma_medrc, kva, size);
}

void is_inv_dma_tnr(ulong kva, u32 size)
{
	struct is_lib_support *lib = &gPtr_lib_support;

	return mblk_inv(&lib->mb_dma_tnr, kva, size);
}

void is_inv_dma_tnr_s(ulong kva, u32 size)
{
	struct is_lib_support *lib = &gPtr_lib_support;

	return mblk_inv(&lib->mb_dma_tnr_s, kva, size);
}

void is_inv_dma_orbmch(ulong kva, u32 size)
{
	struct is_lib_support *lib = &gPtr_lib_support;

	return mblk_inv(&lib->mb_dma_orbmch, kva, size);
}

void is_clean_dma_orbmch(ulong kva, u32 size)
{
	struct is_lib_support *lib = &gPtr_lib_support;

	return mblk_clean(&lib->mb_dma_orbmch, kva, size);
}

void is_inv_dma_clahe(ulong kva, u32 size)
{
	struct is_lib_support *lib = &gPtr_lib_support;

	return mblk_inv(&lib->mb_dma_clahe, kva, size);
}

void is_clean_dma_tnr(ulong kva, u32 size)
{
	struct is_lib_support *lib = &gPtr_lib_support;

	return mblk_clean(&lib->mb_dma_tnr, kva, size);
}

void is_inv_vra(ulong kva, u32 size)
{
	struct is_lib_support *lib = &gPtr_lib_support;

	return mblk_inv(&lib->mb_vra, kva, size);
}

void is_clean_vra(ulong kva, u32 size)
{
	struct is_lib_support *lib = &gPtr_lib_support;

	return mblk_clean(&lib->mb_vra, kva, size);
}

void is_inv_vra_net_array(ulong kva, u32 size)
{
	struct is_lib_support *lib = &gPtr_lib_support;

	return mblk_inv(&lib->mb_vra_net_array, kva, size);
}
/*
 * Assert
 */
int is_lib_logdump(void)
{
	struct is_lib_support *lib = &gPtr_lib_support;
	size_t write_vptr, read_vptr;
	size_t read_cnt, read_cnt1, read_cnt2;
	void *read_ptr;
	ulong debug_kva = lib->minfo->kvaddr_debug;
	ulong ctrl_kva  = lib->minfo->kvaddr_debug_cnt;

	if (!test_bit(IS_DEBUG_OPEN, &is_debug.state))
		return 0;

	write_vptr = *((int *)(ctrl_kva));
	read_vptr = is_debug.read_vptr;

	if (write_vptr > DEBUG_REGION_SIZE)
		 write_vptr = (read_vptr + DEBUG_REGION_SIZE) % (DEBUG_REGION_SIZE + 1);

	if (write_vptr >= read_vptr) {
		read_cnt1 = write_vptr - read_vptr;
		read_cnt2 = 0;
	} else {
		read_cnt1 = DEBUG_REGION_SIZE - read_vptr;
		read_cnt2 = write_vptr;
	}

	read_cnt = read_cnt1 + read_cnt2;
	info_lib("library log start(%zd)\n", read_cnt);

	if (read_cnt1) {
		read_ptr = (void *)(debug_kva + is_debug.read_vptr);

		is_print_buffer(read_ptr, read_cnt1);
		is_debug.read_vptr += read_cnt1;
	}

	if (is_debug.read_vptr >= DEBUG_REGION_SIZE) {
		if (is_debug.read_vptr > DEBUG_REGION_SIZE)
			err_lib("[DBG] read_vptr(%zd) is invalid", is_debug.read_vptr);
		is_debug.read_vptr = 0;
	}

	if (read_cnt2) {
		read_ptr = (void *)(debug_kva + is_debug.read_vptr);

		is_print_buffer(read_ptr, read_cnt2);
		is_debug.read_vptr += read_cnt2;
	}

	info_lib("library log end\n");

	return read_cnt;
}

void is_assert(bool en_s2d)
{
	is_debug_s2d(en_s2d, "DDK/RTA ASSERT");
}

/*
 * Semaphore
 */
int is_sema_init(void **sema, int sema_count)
{
	int fpsimd_flag = 0;

	if (preempt_count() != 0) {
		is_fpsimd_put_func();
		fpsimd_flag = 1;
	}

	if (*sema == NULL)
		*sema = kzalloc(sizeof(struct semaphore), GFP_KERNEL);

	sema_init((struct semaphore *)*sema, sema_count);

	if (fpsimd_flag)
		is_fpsimd_get_func();

#ifdef LIB_MEM_TRACK
	add_alloc_track(MT_TYPE_SEMA, (ulong)*sema, sizeof(struct semaphore));
#endif
	return 0;
}

int is_sema_finish(void *sema)
{
	if (sema == NULL) {
		err_lib("invalid sema");
		return -1;
	}

#ifdef LIB_MEM_TRACK
	add_free_track(MT_TYPE_SEMA, (ulong)sema);
#endif
	kfree(sema);
	return 0;
}

int is_sema_up(void *sema)
{
	if (sema == NULL) {
		err_lib("invalid sema");
		return -1;
	}

	up((struct semaphore *)sema);

	return 0;
}

int is_sema_down(void *sema)
{
	if (sema == NULL) {
		err_lib("invalid sema");
		return -1;
	}

	down((struct semaphore *)sema);

	return 0;
}

/*
 * Mutex
 */
int is_mutex_init(void **mutex)
{
	int fpsimd_flag = 0;

	if (preempt_count() != 0) {
		is_fpsimd_put_func();
		fpsimd_flag = 1;
	}

	if (*mutex == NULL)
		*mutex = kzalloc(sizeof(struct mutex), GFP_KERNEL);

	mutex_init((struct mutex *)*mutex);

	if (fpsimd_flag)
		is_fpsimd_get_func();

#ifdef LIB_MEM_TRACK
	add_alloc_track(MT_TYPE_MUTEX, (ulong)*mutex, sizeof(struct mutex));
#endif
	return 0;
}

int is_mutex_finish(void *mutex_lib)
{
	struct mutex *_mutex = NULL;

	if (mutex_lib == NULL) {
		err_lib("invalid mutex");
		return -1;
	}

	_mutex = (struct mutex *)mutex_lib;

	if (mutex_is_locked(_mutex) == 1)
		mutex_unlock(_mutex);

#ifdef LIB_MEM_TRACK
	add_free_track(MT_TYPE_MUTEX, (ulong)mutex_lib);
#endif
	kfree(mutex_lib);
	return 0;
}

int is_mutex_lock(void *mutex_lib)
{
	struct mutex *_mutex = NULL;
	int fpsimd_flag = 0;

	if (mutex_lib == NULL) {
		err_lib("invalid mutex");
		return -1;
	}

	if (preempt_count() != 0) {
		is_fpsimd_put_func();
		fpsimd_flag = 1;
	}

	_mutex = (struct mutex *)mutex_lib;
	mutex_lock(_mutex);

	if (fpsimd_flag)
		is_fpsimd_get_func();

	return 0;
}

bool is_mutex_tryLock(void *mutex_lib)
{
	struct mutex *_mutex = NULL;
	int locked = 0;
	int fpsimd_flag = 0;

	if (mutex_lib == NULL) {
		err_lib("invalid mutex");
		return -1;
	}

	if (preempt_count() != 0) {
		is_fpsimd_put_func();
		fpsimd_flag = 1;
	}

	_mutex = (struct mutex *)mutex_lib;
	locked = mutex_trylock(_mutex);

	if (fpsimd_flag)
		is_fpsimd_get_func();

	return locked == 1 ? true : false;
}

int is_mutex_unlock(void *mutex_lib)
{
	struct mutex *_mutex = NULL;

	if (mutex_lib == NULL) {
		err_lib("invalid mutex");
		return -1;
	}

	_mutex = (struct mutex *)mutex_lib;
	mutex_unlock(_mutex);

	return 0;
}

/*
 * file read/write
 */
int is_fwrite(const char *pfname, void *pdata, int size)
{
	int ret = 0;
	u32 flags = 0;
	int total_size = 0;
	struct is_binary bin;
	char *filename = NULL;

	filename = __getname();

	if (unlikely(!filename))
		return -ENOMEM;

	if (unlikely(!pfname)) {
		ret = -ENOMEM;
		goto p_err;
	}

	snprintf(filename, PATH_MAX, "%s/%s", DBG_DMA_DUMP_PATH, pfname);

	bin.data = pdata;
	bin.size = size;

	/* first plane for image */
	flags = O_TRUNC | O_CREAT | O_RDWR | O_APPEND;
	total_size += bin.size;

	ret = put_filesystem_binary(filename, &bin, flags);
	if (ret) {
		err("failed to dump %s (%d)", filename, ret);
		ret = -EINVAL;
		goto p_err;
	}

	info_lib("%s: filename = %s, total size = %d\n",
			__func__, filename, total_size);

p_err:
	__putname(filename);

	return ret;
}

int is_file_write(const char *pfname, void *pdata, int size)
{
	int ret = 0;
	u32 flags = 0;
	int total_size = 0;
	struct is_binary bin;

	if (unlikely(!pfname)) {
		ret = -ENOMEM;
		goto p_err;
	}

	bin.data = pdata;
	bin.size = size;

	/* first plane for image */
	flags = O_TRUNC | O_CREAT | O_RDWR | O_APPEND;
	total_size += bin.size;

	ret = put_filesystem_binary(pfname, &bin, flags);
	if (ret) {
		err("failed to dump %s (%d)", pfname, ret);
		ret = -EINVAL;
		goto p_err;
	}

	info_lib("%s: filename = %s, total size = %d\n",
			__func__, pfname, total_size);

p_err:
	return ret;
}

int is_file_read(const char *ppath, const char *pfname, void **pdata, u32 *size)
{
	int ret = 0;

	if (unlikely(!pfname || !pdata)) {
		ret = -ENOMEM;
		goto p_err;
	}

	ret = request_binary(&gPtr_lib_support.lib_file,
		ppath, pfname, &gPtr_lib_support.pdev->dev);
	if (ret) {
		err("failed to read %s (%d)", pfname, ret);
		return -EINVAL;
	}

	*pdata = gPtr_lib_support.lib_file.data;
	*size = (u32)gPtr_lib_support.lib_file.size;

	info_lib("%s: %s%s, total size = %d\n",
			__func__, ppath, pfname, *size);

p_err:
	return ret;
}

/*
 * Timer
 */
static enum hrtimer_restart __nocfi ddk_timer(struct hrtimer *timer)
{
	struct lib_timer *t = container_of(timer, struct lib_timer, dtimer);
	int ret;

	is_fpsimd_get_func();
	ret = t->cb(t->data);
	is_fpsimd_put_func();

	return HRTIMER_NORESTART;
}

int __nocfi is_timer_create(int timer_id, void *func, void *data)
{
	struct is_lib_support *lib = &gPtr_lib_support;
	struct lib_timer *t = NULL;

	if (test_bit(timer_id, &lib->timer_state))
		return -EBUSY;

	t = &lib->timer[timer_id];
	hrtimer_init(&t->dtimer, CLOCK_MONOTONIC, HRTIMER_MODE_REL);

	t->cb = func;
	t->data = data;
	t->dtimer.function = ddk_timer;

	set_bit(timer_id, &lib->timer_state);

	return 0;
}

int __nocfi is_timer_delete(int timer_id)
{
	struct is_lib_support *lib = &gPtr_lib_support;

	if (!test_bit(timer_id, &lib->timer_state)) {
		err_lib("%d timer is not used!!", timer_id);
		return 0;
	}

	clear_bit(timer_id, &lib->timer_state);

	return 0;
}

int __nocfi is_timer_reset(void *timer, ulong expires)
{
	return 0;
}

int __nocfi is_timer_query(void *timer, ulong timerValue)
{
	/* TODO: will be IMPLEMENTED */
	return 0;
}

int __nocfi is_timer_enable(int timer_id, u32 timeout_ms)
{
	struct is_lib_support *lib = &gPtr_lib_support;
	struct lib_timer *t = NULL;
	u32 to_ms = 0;

	if (!test_bit(timer_id, &lib->timer_state)) {
		err_lib("%d timer is not created!!", timer_id);
		return -EINVAL;
	}

	if (!timeout_ms)
		to_ms = 4;
	else
		to_ms = timeout_ms;

	t = &lib->timer[timer_id];
	hrtimer_start(&t->dtimer, ktime_set(0, to_ms * NSEC_PER_MSEC), HRTIMER_MODE_REL);

	return 0;
}

int __nocfi is_timer_disable(int timer_id)
{
	struct is_lib_support *lib = &gPtr_lib_support;
	struct lib_timer *t = NULL;

	if (!test_bit(timer_id, &lib->timer_state)) {
		err_lib("%d timer is not used!!", timer_id);
		return 0;
	}

	t = &lib->timer[timer_id];
	hrtimer_cancel(&t->dtimer);

	return 0;
}

/*
 * Interrupts
 */
int is_register_interrupt(struct hwip_intr_handler info)
{
	struct is_lib_support *lib = &gPtr_lib_support;
	struct hwip_intr_handler *pHandler = NULL;
	int intr_index = -1;
	int ret = 0;

	if (unlikely(info.chain_id >= ID_3AAISP_MAX)) {
		err_lib("invalid chaind_id(%d)", info.chain_id);
		return -EINVAL;
	}
	intr_index = GET_IRQ_ID(info.chain_id, info.id);

	if (!valid_3aaisp_intr_index(intr_index)) {
		err_lib("invalid interrupt id chain ID(%d),(%d)(%d)",
			info.chain_id, info.id, intr_index);
		return -EINVAL;
	}
	pHandler = lib->intr_handler_taaisp[info.chain_id][intr_index];

	if (!pHandler) {
		err_lib("Register interrupt error!!:chain ID(%d)(%d)",
			info.chain_id, info.id);
		return 0;
	}

	pHandler->id		= info.id;
	pHandler->priority	= info.priority;
	pHandler->ctx		= info.ctx;
	pHandler->handler	= info.handler;
	pHandler->chain_id	= info.chain_id;

	smp_mb();
	pHandler->valid		= true;

	is_log_write("[@][DRV]Register interrupt:ID(%d), handler(%p)\n",
		info.id, info.handler);

	return ret;
}

int is_unregister_interrupt(u32 intr_id, u32 chain_id)
{
	struct is_lib_support *lib = &gPtr_lib_support;
	struct hwip_intr_handler *pHandler;
	int intr_index = -1;
	int ret = 0;

	if (unlikely(chain_id >= ID_3AAISP_MAX)) {
		err_lib("invalid chaind_id(%d)", chain_id);
		return -EINVAL;
	}
	intr_index = GET_IRQ_ID(chain_id, intr_id);

	if (!valid_3aaisp_intr_index(intr_index)) {
		err_lib("invalid interrupt id chain ID(%d),(%d)(%d)",
			chain_id, intr_id, intr_index);
		return -EINVAL;
	}

	pHandler = lib->intr_handler_taaisp[chain_id][intr_index];
	if (!pHandler) {
		err_lib("Unregister interrupt error!!:chain ID(%d)(%d)",
			chain_id, intr_id);
		return 0;
	}

	pHandler->valid		= false;
	smp_mb();

	pHandler->id		= 0;
	pHandler->priority	= 0;
	pHandler->ctx		= NULL;
	pHandler->handler	= NULL;
	pHandler->chain_id	= 0;

	is_log_write("[@][DRV]Unregister interrupt:ID(%d)\n", intr_id);

	return ret;
}

static irqreturn_t  is_general_interrupt_threaded_isr(int irq, void *data)
{
	struct general_intr_handler *intr_handler = (struct general_intr_handler *)data;

	is_fpsimd_get_func();
	intr_handler->intr_func();
	is_fpsimd_put_func();

	return IRQ_HANDLED;
}

static int is_register_general_interrupt(struct general_intr_handler info)
{
	struct is_lib_support *lib = &gPtr_lib_support;
	int ret = 0;
	int pdp_ch = 0;
	char name[20] = {0, };

	switch (info.id) {
	case ID_GENERAL_INTR_PREPROC_PDAF:
		sprintf(name, "preproc-pdaf-irq");
		break;
	case ID_GENERAL_INTR_PDP0_STAT:
		pdp_ch = 0;
		sprintf(name, "pdp%d-irq", pdp_ch);
		break;
	case ID_GENERAL_INTR_PDP1_STAT:
		pdp_ch = 1;
		sprintf(name, "pdp%d-irq", pdp_ch);
		break;
	default:
		err_lib("invalid general_interrupt id(%d)", info.id);
		ret = -EINVAL;
		break;
	}

	if (!ret) {
		lib->intr_handler[info.id].intr_func = info.intr_func;

		/* Request IRQ */
		ret = request_threaded_irq(lib->intr_handler[info.id].irq, NULL,
			is_general_interrupt_threaded_isr,
			IRQF_TRIGGER_RISING | IRQF_ONESHOT, name, &lib->intr_handler[info.id]);
		if (ret) {
			err("Failed to register %s(%d).", name, lib->intr_handler[info.id].irq);
			ret = -ENODEV;
		}
	}

	return ret;
}

static int is_unregister_general_interrupt(struct general_intr_handler info)
{
	struct is_lib_support *lib = &gPtr_lib_support;

	if (info.id < 0 || info.id >= ID_GENERAL_INTR_MAX) {
		err_lib("invalid general_interrupt id(%d)", info.id);
	} else {
		if (lib->intr_handler[info.id].irq)
			free_irq(lib->intr_handler[info.id].irq, &lib->intr_handler[info.id]);
	}

	return 0;
}


int is_enable_interrupt(u32 interrupt_id)
{
	/* TODO */
	return 0;
}

int is_disable_interrupt(u32 interrupt_id)
{
	/* TODO */
	return 0;
}

int is_clear_interrupt(u32 interrupt_id)
{
	/* TODO */
	return 0;
}

/*
 * Spinlock
 */
static DEFINE_SPINLOCK(svc_slock);
ulong is_svc_spin_lock_save(void)
{
	ulong flags = 0;

	spin_lock_irqsave(&svc_slock, flags);

	return flags;
}

void is_svc_spin_unlock_restore(ulong flags)
{
	spin_unlock_irqrestore(&svc_slock, flags);
}

static DEFINE_SPINLOCK(svc_slock_rta);
ulong is_svc_spin_lock_save_rta(void)
{
	ulong flags = 0;

	spin_lock_irqsave(&svc_slock_rta, flags);

	return flags;
}

void is_svc_spin_unlock_restore_rta(ulong flags)
{
	spin_unlock_irqrestore(&svc_slock_rta, flags);
}

int is_spin_lock_init(void **slock)
{
	int fpsimd_flag = 0;

	if (preempt_count() != 0) {
		is_fpsimd_put_func();
		fpsimd_flag = 1;
	}

	if (*slock == NULL)
		*slock = kzalloc(sizeof(spinlock_t), GFP_KERNEL);

	spin_lock_init((spinlock_t *)*slock);

	if (fpsimd_flag)
		is_fpsimd_get_func();

#ifdef LIB_MEM_TRACK
	add_alloc_track(MT_TYPE_SPINLOCK, (ulong)*slock, sizeof(spinlock_t));
#endif

	return 0;
}

int is_spin_lock_finish(void *slock_lib)
{
	spinlock_t *slock = NULL;

	if (slock_lib == NULL) {
		err_lib("invalid spinlock");
		return -EINVAL;
	}

	slock = (spinlock_t *)slock_lib;

#ifdef LIB_MEM_TRACK
	add_free_track(MT_TYPE_SPINLOCK, (ulong)slock_lib);
#endif
	kfree(slock_lib);

	return 0;
}

int is_spin_lock(void *slock_lib)
{
	spinlock_t *slock = NULL;

	FIMC_BUG(!slock_lib);

	slock = (spinlock_t *)slock_lib;
	spin_lock(slock);

	return 0;
}

int is_spin_unlock(void *slock_lib)
{
	spinlock_t *slock = NULL;

	FIMC_BUG(!slock_lib);

	slock = (spinlock_t *)slock_lib;
	spin_unlock(slock);

	return 0;
}

int is_spin_lock_irq(void *slock_lib)
{
	spinlock_t *slock = NULL;

	FIMC_BUG(!slock_lib);

	slock = (spinlock_t *)slock_lib;
	spin_lock_irq(slock);

	return 0;
}

int is_spin_unlock_irq(void *slock_lib)
{
	spinlock_t *slock = NULL;

	FIMC_BUG(!slock_lib);

	slock = (spinlock_t *)slock_lib;
	spin_unlock_irq(slock);

	return 0;
}

ulong is_spin_lock_irqsave(void *slock_lib)
{
	spinlock_t *slock = NULL;
	ulong flags = 0;

	FIMC_BUG(!slock_lib);

	slock = (spinlock_t *)slock_lib;
	spin_lock_irqsave(slock, flags);

	return flags;
}

int is_spin_unlock_irqrestore(void *slock_lib, ulong flag)
{
	spinlock_t *slock = NULL;

	FIMC_BUG(!slock_lib);

	slock = (spinlock_t *)slock_lib;
	spin_unlock_irqrestore(slock, flag);

	return 0;
}

int is_get_nsec(uint64_t *time)
{
	*time = (uint64_t)local_clock();

	return 0;
}

int is_get_usec(uint64_t *time)
{
	unsigned long long t;

	t = local_clock();
	*time = (uint64_t)(t / NSEC_PER_USEC);

	return 0;
}

/*
 * Sleep
 */
void is_sleep(u32 msec)
{
#if defined(LOCAL_FPSIMD_API) || defined(VH_FPSIMD_API)
	int fpsimd_flag = 0;

	if (preempt_count() != 0) {
		is_fpsimd_put_func();
		fpsimd_flag = 1;
	}
#endif

	msleep(msec);

#if defined(LOCAL_FPSIMD_API) || defined(VH_FPSIMD_API)
	if (fpsimd_flag)
		is_fpsimd_get_func();
#endif
}

void is_usleep(ulong usec)
{
#if defined(LOCAL_FPSIMD_API) || defined(VH_FPSIMD_API)
	int fpsimd_flag = 0;

	if (preempt_count() != 0) {
		is_fpsimd_put_func();
		fpsimd_flag = 1;
	}
#endif

	usleep_range(usec, usec);

#if defined(LOCAL_FPSIMD_API) || defined(VH_FPSIMD_API)
	if (fpsimd_flag)
		is_fpsimd_get_func();
#endif
}

void is_udelay(ulong usec)
{
	udelay(usec);
}

ulong get_reg_addr(u32 id)
{
	struct is_lib_support *lib = &gPtr_lib_support;
	ulong reg_addr = 0;
	int hw_id, hw_slot;

	switch(id) {
	case ID_3AA_0:
		hw_id = DEV_HW_3AA0;
		break;
	case ID_3AA_1:
		hw_id = DEV_HW_3AA1;
		break;
	case ID_3AA_2:
		hw_id = DEV_HW_3AA2;
		break;
	case ID_LME_0:
		hw_id = DEV_HW_LME0;
		break;
	case ID_LME_1:
		hw_id = DEV_HW_LME1;
		break;
	case ID_ISP_0:
		hw_id = DEV_HW_ISP0;
		break;
	case ID_ISP_1:
		hw_id = DEV_HW_ISP1;
		break;
	case ID_CLH_0:
		hw_id = DEV_HW_CLH0;
		break;
	default:
		warn_lib("get_reg_addr: invalid id(%d)\n", id);
		return 0;
	}

	hw_slot = is_hw_slot_id(hw_id);
	if (!valid_hw_slot_id(hw_slot)) {
		err_lib("invalid hw_slot (%d) ", hw_slot);
		return 0;
	}

	reg_addr = (ulong)lib->itfc->itf_ip[hw_slot].hw_ip->regs[REG_SETA];

	info_lib("get_reg_addr: [ID:%d](0x%lx)\n", hw_id, reg_addr);

	return reg_addr;
}

static void __nocfi lib_task_work(struct kthread_work *work)
{
	struct is_task_work *cur_work;

	FIMC_BUG_VOID(!work);

	cur_work = container_of(work, struct is_task_work, work);

	dbg_lib(3, "do task_work: func(%p), params(%p)\n",
		cur_work->func, cur_work->params);

	is_fpsimd_get_task();
	cur_work->func(cur_work->params);
	is_fpsimd_put_task();
}

bool lib_task_trigger(struct is_lib_support *this,
	int priority, void *func, void *params)
{
	struct is_lib_task *lib_task = NULL;
	u32 index = 0;

	FIMC_BUG_FALSE(!this);
	FIMC_BUG_FALSE(!func);
	FIMC_BUG_FALSE(!params);

	lib_task = &this->task_taaisp[(priority - TASK_PRIORITY_BASE - 1)];

	spin_lock(&lib_task->work_lock);
#ifdef VH_FPSIMD_API
	is_fpsimd_set_task_using(lib_task->task,
		&lib_task->fp_state[lib_task->work_index % LIB_MAX_TASK]);
#endif
	lib_task->work[lib_task->work_index % LIB_MAX_TASK].func = (task_func)func;
	lib_task->work[lib_task->work_index % LIB_MAX_TASK].params = params;
	lib_task->work_index++;
	index = (lib_task->work_index - 1) % LIB_MAX_TASK;
	spin_unlock(&lib_task->work_lock);

	return kthread_queue_work(&lib_task->worker, &lib_task->work[index].work);
}

int is_lib_check_priority(u32 type, int priority)
{
	switch (type) {
	case TASK_TYPE_DDK:
		if (priority < TASK_PRIORITY_1ST || priority > TASK_PRIORITY_11TH) {
			panic("%s: [DDK] task priority is invalid", __func__);
			return -EINVAL;
		}
		break;
	case TASK_TYPE_RTA:
		if (priority != TASK_PRIORITY_6TH) {
			panic("%s: [RTA] task priority is invalid", __func__);
			return -EINVAL;
		}
		break;
	}

	return 0;
}

bool is_add_task(int priority, void *func, void *param)
{
	dbg_lib(3, "add_task: priority(%d), func(%p), params(%p)\n",
		priority, func, param);

	is_lib_check_priority(TASK_TYPE_DDK, priority);

	return lib_task_trigger(&gPtr_lib_support, priority, func, param);
}

bool is_add_task_rta(int priority, void *func, void *param)
{
	dbg_lib(3, "add_task_rta: priority(%d), func(%p), params(%p)\n",
		priority, func, param);

	is_lib_check_priority(TASK_TYPE_RTA, priority);

	return lib_task_trigger(&gPtr_lib_support, priority, func, param);
}

int lib_get_task_priority(int task_index)
{
	int priority = IS_MAX_PRIO - 1;

	switch (task_index) {
	case TASK_OTF:
		priority = TASK_LIB_OTF_PRIO;
		break;
	case TASK_AF:
		priority = TASK_LIB_AF_PRIO;
		break;
	case TASK_ISP_DMA:
		priority = TASK_LIB_ISP_DMA_PRIO;
		break;
	case TASK_3AA_DMA:
		priority = TASK_LIB_3AA_DMA_PRIO;
		break;
	case TASK_AA:
		priority = TASK_LIB_AA_PRIO;
		break;
	case TASK_RTA:
		priority = TASK_LIB_RTA_PRIO;
		break;
	case TASK_EXT1:
		priority = TASK_LIB_AF_PRIO;
		break;
	case TASK_EXT2:
		priority = TASK_LIB_AF_PRIO;
		break;
	case TASK_EXT3:
		priority = TASK_LIB_ISP_DMA_PRIO;
		break;
	case TASK_EXT4:
		priority = TASK_LIB_RTA_PRIO;
		break;
	case TASK_EXT5:
		priority = TASK_LIB_RTA_PRIO;
		break;
	default:
		err_lib("Invalid task ID (%d)", task_index);
		break;
	}
	return priority;
}

u32 lib_get_task_affinity(int task_index)
{
	u32 cpu = 0;

	switch (task_index) {
	case TASK_OTF:
		cpu = TASK_OTF_AFFINITY;
		break;
	case TASK_AF:
		cpu = TASK_AF_AFFINITY;
		break;
	case TASK_ISP_DMA:
		cpu = TASK_ISP_DMA_AFFINITY;
		break;
	case TASK_3AA_DMA:
		cpu = TASK_3AA_DMA_AFFINITY;
		break;
	case TASK_AA:
		cpu = TASK_AA_AFFINITY;
		break;
	case TASK_EXT1:
		cpu = TASK_AA_AFFINITY;
		break;
	case TASK_EXT2:
		cpu = TASK_AA_AFFINITY;
		break;
	case TASK_EXT3:
		cpu = TASK_3AA_DMA_AFFINITY;
		break;
	case TASK_EXT4:
		cpu = TASK_ISP_DMA_AFFINITY;
		break;
	case TASK_EXT5:
		cpu = TASK_AF_AFFINITY;
		break;
	default:
		err_lib("Invalid task ID (%d)", task_index);
		break;
	}
	return cpu;
}

int lib_support_init(void)
{
	int ret = 0;
#ifdef LIB_MEM_TRACK
	struct is_lib_support *lib = &gPtr_lib_support;

	spin_lock_init(&lib->slock_mem_track);
	INIT_LIST_HEAD(&lib->list_of_tracks);
	lib->cur_tracks = vzalloc(sizeof(struct lib_mem_tracks));
	if (lib->cur_tracks)
		list_add(&lib->cur_tracks->list,
			&lib->list_of_tracks);
	else
		err_lib("failed to allocate memory track");
#endif

	return ret;
}

int is_init_ddk_thread(void)
{
	struct sched_param param = { .sched_priority = IS_MAX_PRIO - 1 };
	char name[30];
	int i, j, ret = 0;
	struct is_lib_support *lib = &gPtr_lib_support;

	for (i = 0 ; i < TASK_INDEX_MAX; i++) {
		spin_lock_init(&lib->task_taaisp[i].work_lock);
		kthread_init_worker(&lib->task_taaisp[i].worker);
		snprintf(name, sizeof(name), "lib_%d_worker", i);
		lib->task_taaisp[i].task = kthread_run(kthread_worker_fn,
							&lib->task_taaisp[i].worker,
							name);
		if (IS_ERR(lib->task_taaisp[i].task)) {
			err_lib("failed to create library task_handler(%d), err(%ld)",
				i, PTR_ERR(lib->task_taaisp[i].task));
			return PTR_ERR(lib->task_taaisp[i].task);
		}
#ifndef VH_FPSIMD_API
		is_fpsimd_set_task_using(lib->task_taaisp[i].task, NULL);
#endif
		/* TODO: consider task priority group worker */
		param.sched_priority = lib_get_task_priority(i);
		ret = sched_setscheduler_nocheck(lib->task_taaisp[i].task, SCHED_FIFO, &param);
		if (ret) {
			err_lib("sched_setscheduler_nocheck(%d) is fail(%d)", i, ret);
			return 0;
		}

		lib->task_taaisp[i].work_index = 0;
		for (j = 0; j < LIB_MAX_TASK; j++) {
			lib->task_taaisp[i].work[j].func = NULL;
			lib->task_taaisp[i].work[j].params = NULL;
			kthread_init_work(&lib->task_taaisp[i].work[j].work,
					lib_task_work);
		}

	}

	return ret;
}

void is_set_ddk_thread_affinity(void)
{
	struct is_lib_support *lib = &gPtr_lib_support;
	struct is_core *core;
	struct task_struct *task;
	int i;
	const char *cpus;

	core = (struct is_core *)platform_get_drvdata(lib->pdev);
	cpus = core->resourcemgr.dvfs_ctrl.cur_cpus;

	for (i = 0 ; i < TASK_INDEX_MAX; i++) {
		if (i == TASK_RTA)
			continue;

		task = lib->task_taaisp[i].task;

		if (!cpus) {
			/* Legacy mode */
			u32 cpu = lib_get_task_affinity(i);

			set_cpus_allowed_ptr(task, cpumask_of(cpu));
			dbg_lib(3, "lib_%d_worker: affinity %d\n", i, cpu);
		} else {
			/* DT mode */
			struct cpumask cpumask;

			cpulist_parse(cpus, &cpumask);
			set_cpus_allowed_ptr(task, &cpumask);
			dbg_lib(3, "lib_%d_worker: affinity %s\n", i, cpus);
		}
	}
}

void is_flush_ddk_thread(void)
{
	int ret = 0;
	int i = 0;
	struct is_lib_support *lib = &gPtr_lib_support;

	info_lib("%s\n", __func__);

	for (i = 0; i < TASK_INDEX_MAX; i++) {
		if (lib->task_taaisp[i].task) {
			dbg_lib(3, "flush_ddk_thread: flush_kthread_worker (%d)\n", i);
			kthread_flush_worker(&lib->task_taaisp[i].worker);

			ret = kthread_stop(lib->task_taaisp[i].task);
			if (ret) {
				err_lib("kthread_stop fail (%d): state(%ld), exit_code(%d), exit_state(%d)",
					ret,
					lib->task_taaisp[i].task->state,
					lib->task_taaisp[i].task->exit_code,
					lib->task_taaisp[i].task->exit_state);
			}

			lib->task_taaisp[i].task = NULL;

			info_lib("flush_ddk_thread: kthread_stop [%d]\n", i);
		}
	}
}

void is_lib_flush_task_handler(int priority)
{
#if defined(LOCAL_FPSIMD_API) || defined(VH_FPSIMD_API)
	int fpsimd_flag = 0;
#endif
	struct is_lib_support *lib = &gPtr_lib_support;
	int task_index = priority - TASK_PRIORITY_BASE - 1;

	dbg_lib(3, "%s: task_index(%d), priority(%d)\n", __func__, task_index, priority);

#if defined(LOCAL_FPSIMD_API) || defined(VH_FPSIMD_API)
	if (preempt_count() != 0) {
		is_fpsimd_put_func();
		fpsimd_flag = 1;
	}
#endif

	kthread_flush_worker(&lib->task_taaisp[task_index].worker);

#if defined(LOCAL_FPSIMD_API) || defined(VH_FPSIMD_API)
	if (fpsimd_flag)
		is_fpsimd_get_func();
#endif
}

void is_load_clear(void)
{
	struct is_lib_support *lib = &gPtr_lib_support;

	dbg_lib(3, "binary clear start");

	lib->binary_load_flg = false;

	dbg_lib(3, "binary clear done");

	return;
}

void check_lib_memory_leak(void)
{
#ifdef LIB_MEM_TRACK
	print_tracks_status(KERN_ERR, "Leaked memory -", MT_STATUS_ALLOC);
	free_tracks();
#endif
}

bool is_lib_in_irq(void)
{
	if (in_irq())
		return true;
	else
		return false;
}

static void _get_fd_data(u32 instance,
	struct nfd_info *fd_data)
{
	struct is_lib_support *lib = &gPtr_lib_support;
	struct is_core *core = NULL;
	struct is_device_ischain *ischain = NULL;
	struct is_region *is_region = NULL;
	struct nfd_info *fd_info = NULL;
	unsigned long flags = 0;

	if (unlikely(!lib)) {
		err_lib("lib is NULL");
		return;
	}

	core = (struct is_core *)platform_get_drvdata(lib->pdev);
	if (unlikely(!core)) {
		err_lib("core is NULL");
		return;
	}
	ischain = &core->ischain[instance];
	is_region = ischain->is_region;
	fd_info = (struct nfd_info *)&is_region->fd_info;

	spin_lock_irqsave(&is_region->fd_info_slock, flags);
	memcpy((void *)fd_data, (void *)fd_info, sizeof(struct nfd_info));
	spin_unlock_irqrestore(&is_region->fd_info_slock, flags);

	dbg_lib(3, "_get_fd_data: (fc: %d, fn: %d)\n",
		fd_data->frame_count, fd_data->face_num);
}

#ifdef SOC_VRA
static void is_get_fd_data(u32 instance,
	struct fd_info *face_data,
	struct fd_rectangle *fd_in_size)
{
	int i;
	unsigned long flags = 0;
	struct is_lib_vra *lib_vra = g_lib_vra;

	if (unlikely(!lib_vra)) {
		err_lib("lib_vra is NULL");
		face_data->face_num = 0;
		return;
	}

	spin_lock_irqsave(&lib_vra->ae_fd_slock[instance], flags);

	if (lib_vra->all_face_num[instance] > MAX_FACE_COUNT) {
		err_lib("Exceed MAX_FACE_COUNT at VRA lib (instance: %d face_num: %d)",
			instance, lib_vra->all_face_num[instance]);
		face_data->face_num = 0;
		spin_unlock_irqrestore(&lib_vra->ae_fd_slock[instance], flags);
		return;
	}

	for (i = 0; i < lib_vra->all_face_num[instance]; i++) {
		face_data->face[i].face_area.top_left.x =
			lib_vra->out_faces[instance][i].rect.topleft.x;
		face_data->face[i].face_area.top_left.y =
			lib_vra->out_faces[instance][i].rect.topleft.y;
		face_data->face[i].face_area.span.width =
			lib_vra->out_faces[instance][i].rect.width;
		face_data->face[i].face_area.span.height =
			lib_vra->out_faces[instance][i].rect.height;

		face_data->face[i].faceId = lib_vra->out_faces[instance][i].unique_id;
		face_data->face[i].score = lib_vra->out_faces[instance][i].score;
		face_data->face[i].rotation = 0;

		dbg_lib(3, "is_get_fd_data: [%d](%d,%d,%d,%d,%d,%d,%d)\n", i,
			face_data->face[i].face_area.top_left.x,
			face_data->face[i].face_area.top_left.y,
			face_data->face[i].face_area.span.width,
			face_data->face[i].face_area.span.height,
			face_data->face[i].faceId,
			face_data->face[i].score,
			face_data->face[i].rotation);
	}

	face_data->face_num = lib_vra->all_face_num[instance];
	face_data->frame_count = lib_vra->out_list_info[instance].fr_ind;

	fd_in_size->width = lib_vra->out_list_info[instance].in_sizes.width;
	fd_in_size->height = lib_vra->out_list_info[instance].in_sizes.height;

	dbg_lib(3, "is_get_fd_data: (%d,%d,%d,%d)\n",
		face_data->face_num,
		face_data->frame_count,
		fd_in_size->width,
		fd_in_size->height);

	spin_unlock_irqrestore(&lib_vra->ae_fd_slock[instance], flags);

}
#else
static void is_get_hybrid_fd_data(u32 instance,
	struct fd_info *face_data,
	struct fd_rectangle *fd_in_size)
{
	int i;
	unsigned long flags = 0;
	struct is_lib_vra *lib_vra = g_lib_vra;

	if (unlikely(!lib_vra)) {
		err_lib("lib_vra is NULL");
		return;
	}

	if (IS_ENABLED(ENABLE_HYBRID_FD) && lib_vra->post_detection_enable[instance]) {
		struct is_lib_support *lib = &gPtr_lib_support;
		u32 offset_region;
		struct is_region *is_region;
		struct is_fdae_info *fdae_info;

		offset_region = instance * PARAM_REGION_SIZE;
		is_region = (struct is_region *)(lib->minfo->kvaddr_region + offset_region);
		fdae_info = &is_region->fdae_info;

		spin_lock_irqsave(&fdae_info->slock, flags);

		if (fdae_info->face_num > MAX_FACE_COUNT) {
			err_lib("Exceed MAX_FACE_COUNT at HFD (instance: %d face_num: %d)",
				instance, fdae_info->face_num);
			face_data->face_num = 0;
			spin_unlock_irqrestore(&fdae_info->slock, flags);
			return;
		}

		for (i = 0; i < fdae_info->face_num; i++) {
			face_data->face[i].face_area.top_left.x =
				fdae_info->rect[i].offset_x;
			face_data->face[i].face_area.top_left.y =
				fdae_info->rect[i].offset_y;
			face_data->face[i].face_area.span.width =
				fdae_info->rect[i].width - fdae_info->rect[i].offset_x;
			face_data->face[i].face_area.span.height =
				fdae_info->rect[i].height - fdae_info->rect[i].offset_y;

			face_data->face[i].faceId = fdae_info->id[i];
			face_data->face[i].score = fdae_info->score[i];
			face_data->face[i].rotation =
				(fdae_info->rot[i] * 2 + fdae_info->is_rot[i]) * 45;

			dbg_lib(3, "is_get_fd_data: [%d](%d,%d,%d,%d,%d,%d,%d)\n", i,
				face_data->face[i].face_area.top_left.x,
				face_data->face[i].face_area.top_left.y,
				face_data->face[i].face_area.span.width,
				face_data->face[i].face_area.span.height,
				face_data->face[i].faceId,
				face_data->face[i].score,
				face_data->face[i].rotation);
		}

		face_data->face_num = (0x1 << 16) | (fdae_info->face_num & 0xFFFF);
		face_data->frame_count = fdae_info->frame_count;

		fd_in_size->width = lib_vra->pdt_out_list_info[instance].in_sizes.width;
		fd_in_size->height = lib_vra->pdt_out_list_info[instance].in_sizes.height;

		dbg_lib(3, "is_get_fd_data: (%d,%d,%d,%d)\n",
			face_data->face_num,
			face_data->frame_count,
			fd_in_size->width,
			fd_in_size->height);

		spin_unlock_irqrestore(&fdae_info->slock, flags);
	} else	{
		face_data->face_num = 0;
	}
}
#endif

void is_get_binary_version(char **buf, unsigned int type, unsigned int hint)
{
	char *p;

	*buf = get_binary_version(type, hint);

	if (type == IS_BIN_LIBRARY) {
		p = strrchr(*buf, ']');
		if (p)
			*buf = p + 1;
	}
}

static int is_votfif_create_ring(void)
{
	int ret = 0;

	dbg_lib(4, "%s\n", __func__);
	ret = votfitf_create_ring();

	return ret;
}

static int is_votfif_destroy_ring(void)
{
	int ret = 0;

	dbg_lib(4, "%s\n", __func__);
	ret = votfitf_destroy_ring();

	return ret;
}

static int is_votfitf_set_service_cfg(struct votf_info *vinfo, struct votf_service_cfg *cfg)
{
	int ret = 0;

	FIMC_BUG(!vinfo);
	FIMC_BUG(!cfg);

	dbg_lib(4, "%s\n", __func__);
	ret = votfitf_set_service_cfg(vinfo, cfg);

	return ret;
}

static int is_votfitf_reset(struct votf_info *vinfo, int type)
{
	int ret = 0;

	FIMC_BUG(!vinfo);

	dbg_lib(4, "%s\n", __func__);
	ret = votfitf_reset(vinfo, type);

	return ret;
}

static int is_votfitf_set_flush(struct votf_info *vinfo)
{
	int ret = 0;

	FIMC_BUG(!vinfo);

	dbg_lib(4, "%s\n", __func__);
	ret = votfitf_set_flush(vinfo);

	return ret;
}

static int is_votfitf_set_frame_size(struct votf_info *vinfo, u32 size)
{
	int ret = 0;

	FIMC_BUG(!vinfo);

	dbg_lib(4, "%s\n", __func__);
	ret = votfitf_set_frame_size(vinfo, size);

	return ret;
}

static int is_votfitf_set_trs_lost_cfg(struct votf_info *vinfo, struct votf_lost_cfg *cfg)
{
	int ret = 0;

	FIMC_BUG(!vinfo);

	dbg_lib(4, "%s\n", __func__);
	ret = votfitf_set_trs_lost_cfg(vinfo, cfg);

	return ret;
}

static int is_votfitf_set_token_size(struct votf_info *vinfo, u32 size)
{
	int ret = 0;

	FIMC_BUG(!vinfo);

	dbg_lib(4, "%s\n", __func__);
	ret = votfitf_set_token_size(vinfo, size);

	return ret;

}

static int is_votfitf_set_first_token_size(struct votf_info *vinfo, u32 size)
{
	int ret = 0;

	FIMC_BUG(!vinfo);

	dbg_lib(4, "%s\n", __func__);
	ret = votfitf_set_first_token_size(vinfo, size);

	return ret;

}

static void is_votfitf_sfr_dump(void)
{
	dbg_lib(4, "%s\n", __func__);
	votf_sfr_dump();
}

int is_set_config(int target_hw_id, int instance, u32 fcount, struct is_yuvpp_config *config)
{
	struct is_lib_support *lib = &gPtr_lib_support;
	struct is_hw_ip *hw_ip = NULL;
	int hw_slot = -1;
	int ret = 0;

	hw_slot = is_hw_slot_id(target_hw_id);
	if (hw_slot < 0) {
		err_lib("invalid target_hw_id (%d)", target_hw_id);
		return ret;
	}

	hw_ip = lib->itfc->itf_ip[hw_slot].hw_ip;
	ret = CALL_HW_OPS(hw_ip, set_config, instance, fcount, config);
	if (!ret) {
		err_lib("[%d] set_config error (%d)", hw_ip->id, ret);
		return ret;
	}
	return ret;
}

int is_set_regs_array(int target_hw_id, int instance, u32 fcount, struct cr_set *regs, u32 regs_size)
{
	struct is_lib_support *lib = &gPtr_lib_support;
	struct is_hw_ip *hw_ip = NULL;
	int hw_slot = -1;
	int ret = 0;

	hw_slot = is_hw_slot_id(target_hw_id);
	if (hw_slot < 0) {
		err_lib("invalid target_hw_id (%d)", target_hw_id);
		return -EINVAL;
	}

	hw_ip = lib->itfc->itf_ip[hw_slot].hw_ip;
	ret = CALL_HW_OPS(hw_ip, set_regs, instance, fcount, regs, regs_size);

	return ret;
}

int is_dump_regs_array(int target_hw_id, int instance, u32 fcount, struct cr_set *regs, u32 regs_size)
{
	struct is_lib_support *lib = &gPtr_lib_support;
	struct is_hw_ip *hw_ip = NULL;
	int hw_slot = -1;
	int ret = 0;

	hw_slot = is_hw_slot_id(target_hw_id);
	if (hw_slot < 0) {
		err_lib("invalid target_hw_id (%d)", target_hw_id);
		return -EINVAL;
	}

	hw_ip = lib->itfc->itf_ip[hw_slot].hw_ip;
	ret = CALL_HW_OPS(hw_ip, dump_regs, instance, fcount, regs, regs_size);

	return ret;
}


int is_debug_attr(char *cmd, int *param1, int *param2, int *param3)
{
	int ret = 0;

	/* Command should have proper string */
	if (cmd && attr_ddk[0]) {
		snprintf(cmd, PATH_MAX, "%s", attr_ddk[0]);
	} else {
		ret = -EINVAL;
		goto err;
	}

	if (param1 && attr_ddk[1]) {
		if (kstrtoint(attr_ddk[1], 0, param1))
			*param1 = 0;
	}

	if (param2 && attr_ddk[2]) {
		if (kstrtoint(attr_ddk[2], 0, param2))
			*param2 = 0;
	}

	if (param3 && attr_ddk[3]) {
		if (kstrtoint(attr_ddk[3], 0, param3))
			*param3 = 0;
	}
err:
	return ret;
}

static int is_debug_level(void)
{
	return debug_ddk;
}

static int is_debug_level_rta(void)
{
	return debug_rta;
}

bool is_get_secure_scenario(void)
{
	struct is_lib_support *lib = &gPtr_lib_support;
	struct is_core *core = NULL;
	bool ret = false;

	core = (struct is_core *)platform_get_drvdata(lib->pdev);
	if (unlikely(!core)) {
		err_lib("core is NULL");
		return false;
	}

	if (core->scenario == IS_SCENARIO_SECURE)
		ret = true;

	return ret;
}

void set_os_system_funcs(os_system_func_t *funcs)
{
	funcs[0] = (os_system_func_t)is_log_write_console;
#if defined(CONFIG_SAMSUNG_PRODUCT_SHIP)
	funcs[1] = NULL;
	funcs[2] = NULL;
#else
	funcs[1] = (os_system_func_t)is_file_read;
	funcs[2] = (os_system_func_t)is_file_write;
#endif
	funcs[3] = (os_system_func_t)is_assert;

	funcs[4] = (os_system_func_t)is_sema_init;
	funcs[5] = (os_system_func_t)is_sema_finish;
	funcs[6] = (os_system_func_t)is_sema_up;
	funcs[7] = (os_system_func_t)is_sema_down;

	funcs[8] = (os_system_func_t)is_mutex_init;
	funcs[9] = (os_system_func_t)is_mutex_finish;
	funcs[10] = (os_system_func_t)is_mutex_lock;
	funcs[11] = (os_system_func_t)is_mutex_tryLock;
	funcs[12] = (os_system_func_t)is_mutex_unlock;

	funcs[13] = (os_system_func_t)is_timer_create;
	funcs[14] = (os_system_func_t)is_timer_delete;
	funcs[15] = (os_system_func_t)is_timer_reset;
	funcs[16] = (os_system_func_t)is_timer_query;
	funcs[17] = (os_system_func_t)is_timer_enable;
	funcs[18] = (os_system_func_t)is_timer_disable;

	funcs[19] = (os_system_func_t)is_register_interrupt;
	funcs[20] = (os_system_func_t)is_unregister_interrupt;
	funcs[21] = (os_system_func_t)is_enable_interrupt;
	funcs[22] = (os_system_func_t)is_disable_interrupt;
	funcs[23] = (os_system_func_t)is_clear_interrupt;

	funcs[24] = (os_system_func_t)is_svc_spin_lock_save;
	funcs[25] = (os_system_func_t)is_svc_spin_unlock_restore;
	funcs[26] = (os_system_func_t)get_random_int;
	funcs[27] = (os_system_func_t)is_add_task;

	funcs[28] = (os_system_func_t)is_get_usec;
	funcs[29] = (os_system_func_t)is_log_write;

	funcs[30] = (os_system_func_t)is_dva_dma_taaisp;
	funcs[31] = (os_system_func_t)is_kva_dma_taaisp;
	funcs[32] = (os_system_func_t)is_sleep;
	funcs[33] = (os_system_func_t)is_inv_dma_taaisp;
	funcs[34] = (os_system_func_t)is_alloc_dma_taaisp;
	funcs[35] = (os_system_func_t)is_free_dma_taaisp;

	funcs[36] = (os_system_func_t)is_spin_lock_init;
	funcs[37] = (os_system_func_t)is_spin_lock_finish;
	funcs[38] = (os_system_func_t)is_spin_lock;
	funcs[39] = (os_system_func_t)is_spin_unlock;
	funcs[40] = (os_system_func_t)is_spin_lock_irq;
	funcs[41] = (os_system_func_t)is_spin_unlock_irq;
	funcs[42] = (os_system_func_t)is_spin_lock_irqsave;
	funcs[43] = (os_system_func_t)is_spin_unlock_irqrestore;
	funcs[44] = (os_system_func_t)is_usleep;
	funcs[45] = NULL;
	funcs[46] = (os_system_func_t)get_reg_addr;

	funcs[47] = (os_system_func_t)is_lib_in_irq;
	funcs[48] = (os_system_func_t)is_lib_flush_task_handler;

	funcs[49] = (os_system_func_t)_get_fd_data; /* for FDAE/FDAF */
#ifdef SOC_VRA
	funcs[50] = (os_system_func_t)is_get_fd_data;
#else
	funcs[50] = (os_system_func_t)is_get_hybrid_fd_data; /* for FDAE/FDAF */
#endif
	funcs[51] = (os_system_func_t)is_alloc_dma;
	funcs[52] = (os_system_func_t)is_free_dma;
	funcs[53] = (os_system_func_t)is_dva_dma;
	funcs[54] = (os_system_func_t)is_kva_dma;

	funcs[55] = (os_system_func_t)is_dva_dma_orbmch;
	funcs[56] = (os_system_func_t)is_kva_dma_orbmch;
	funcs[57] = (os_system_func_t)is_inv_dma_orbmch;
	funcs[58] = (os_system_func_t)is_alloc_dma_orbmch;
	funcs[59] = (os_system_func_t)is_free_dma_orbmch;
	funcs[60] = (os_system_func_t)is_dva_dma_tnr;
	funcs[61] = (os_system_func_t)is_kva_dma_tnr;
	funcs[62] = (os_system_func_t)is_inv_dma_tnr;
	funcs[63] = (os_system_func_t)is_alloc_dma_tnr;
	funcs[64] = (os_system_func_t)is_free_dma_tnr;
	funcs[65] = (os_system_func_t)is_dva_dma_medrc;
	funcs[66] = (os_system_func_t)is_kva_dma_medrc;
	funcs[67] = (os_system_func_t)is_inv_dma_medrc;
	funcs[68] = (os_system_func_t)is_alloc_dma_medrc;
	funcs[69] = (os_system_func_t)is_free_dma_medrc;
	funcs[70] = (os_system_func_t)is_dva_dma_clahe;
	funcs[71] = (os_system_func_t)is_kva_dma_clahe;
	funcs[72] = (os_system_func_t)is_inv_dma_clahe;
	funcs[73] = (os_system_func_t)is_alloc_dma_clahe;
	funcs[74] = (os_system_func_t)is_free_dma_clahe;
	funcs[75] = (os_system_func_t)is_clean_dma_medrc;
	funcs[76] = (os_system_func_t)is_clean_dma_tnr;
	funcs[77] = (os_system_func_t)is_clean_dma_orbmch;
	funcs[78] = (os_system_func_t)is_inv_dma;
	funcs[79] = (os_system_func_t)is_clean_dma;
	/* VOTF interface */
	funcs[80] = (os_system_func_t)is_votfif_create_ring;
	funcs[81] = (os_system_func_t)is_votfif_destroy_ring;
	funcs[82] = (os_system_func_t)is_votfitf_set_service_cfg;
	funcs[83] = (os_system_func_t)is_votfitf_reset;
	funcs[84] = (os_system_func_t)is_votfitf_set_flush;
	funcs[85] = (os_system_func_t)is_votfitf_set_frame_size;
	funcs[86] = (os_system_func_t)is_votfitf_set_trs_lost_cfg;
	funcs[87] = (os_system_func_t)is_votfitf_set_token_size;
	funcs[88] = (os_system_func_t)is_votfitf_set_first_token_size;

	funcs[89] = (os_system_func_t)is_votfitf_sfr_dump;

	funcs[91] = (os_system_func_t)is_get_binary_version;

	funcs[92] = (os_system_func_t)is_set_config;
	funcs[93] = (os_system_func_t)is_set_regs_array;
	funcs[94] = (os_system_func_t)is_dump_regs_array;
	/* TODO: re-odering function table */
	funcs[97] = (os_system_func_t)is_debug_attr;
	funcs[98] = (os_system_func_t)is_debug_level;
	funcs[99] = (os_system_func_t)is_event_write;

	funcs[100] = (os_system_func_t)is_dva_dma_tnr_s;
	funcs[101] = (os_system_func_t)is_kva_dma_tnr_s;
	funcs[102] = (os_system_func_t)is_inv_dma_tnr_s;
	funcs[103] = (os_system_func_t)is_alloc_dma_tnr_s;
	funcs[104] = (os_system_func_t)is_free_dma_tnr_s;
	funcs[105] = (os_system_func_t)is_get_secure_scenario;
}

#ifdef USE_RTA_BINARY
void set_os_system_funcs_for_rta(os_system_func_t *funcs)
{
	/* Index 0 => log, assert */
	funcs[0] = (os_system_func_t)is_log_write;
	funcs[1] = (os_system_func_t)is_log_write_console;
	funcs[2] = (os_system_func_t)is_assert;

#if defined(CONFIG_SAMSUNG_PRODUCT_SHIP)
	funcs[9] = NULL;
#else
	funcs[9] = (os_system_func_t)is_fwrite;
#endif

	/* Index 10 => memory : alloc/free */
	funcs[10] = NULL;
	funcs[11] = NULL;

	/* Index 20 => memory : misc */

	/* Index 30 => interrupt */
	funcs[30] = (os_system_func_t)is_register_general_interrupt;
	funcs[31] = (os_system_func_t)is_unregister_general_interrupt;

	/* Index 40 => task */
	funcs[40] = (os_system_func_t)is_add_task_rta;

	/* Index 50 => timer, delay, sleep */
	funcs[50] = (os_system_func_t)is_timer_create;
	funcs[51] = (os_system_func_t)is_timer_delete;
	funcs[52] = (os_system_func_t)is_timer_reset;
	funcs[53] = (os_system_func_t)is_timer_query;
	funcs[54] = (os_system_func_t)is_timer_enable;
	funcs[55] = (os_system_func_t)is_timer_disable;
	funcs[56] = (os_system_func_t)is_udelay;
	funcs[57] = (os_system_func_t)is_usleep;
	funcs[58] = (os_system_func_t)is_sleep;

	/* Index 60 => locking : semaphore */
	funcs[60] = (os_system_func_t)is_sema_init;
	funcs[61] = (os_system_func_t)is_sema_finish;
	funcs[62] = (os_system_func_t)is_sema_up;
	funcs[63] = (os_system_func_t)is_sema_down;

	/* Index 70 => locking : mutex */

	/* Index 80 => locking : spinlock */
	funcs[80] = (os_system_func_t)is_svc_spin_lock_save_rta;
	funcs[81] = (os_system_func_t)is_svc_spin_unlock_restore_rta;

	/* Index 90 => misc */
	funcs[90] = (os_system_func_t)is_get_usec;
	funcs[91] = (os_system_func_t)is_get_binary_version;
	funcs[92] = (os_system_func_t)is_debug_level_rta;
}
#endif

#ifndef CONFIG_DISABLE_CAMERA_MEM_ATTR
#if (LINUX_VERSION_CODE < KERNEL_VERSION(5, 3, 0))
static int is_memory_page_range(pte_t *ptep, pgtable_t token, unsigned long addr,
			void *data)
#else
static int is_memory_page_range(pte_t *ptep, unsigned long addr,
			void *data)
#endif
{
	struct is_memory_change_data *cdata = data;
	pte_t pte = *ptep;

	pte = clear_pte_bit(pte, cdata->clear_mask);
	pte = set_pte_bit(pte, cdata->set_mask);

	set_pte(ptep, pte);
	return 0;
}

static int is_memory_attribute_change(ulong addr, int numpages,
		pgprot_t set_mask, pgprot_t clear_mask)
{
	ulong start = addr;
	ulong size = PAGE_SIZE * numpages;
	ulong end = start + size;
	int ret;
	struct is_memory_change_data data;

	if (!IS_ALIGNED(addr, PAGE_SIZE)) {
		start &= PAGE_MASK;
		end = start + size;
		WARN_ON_ONCE(1);
	}

	data.set_mask = set_mask;
	data.clear_mask = clear_mask;

	ret = apply_to_page_range(&init_mm, start, size, is_memory_page_range,
					&data);

	flush_tlb_kernel_range(start, end);

	return ret;
}

int is_memory_attribute_nxrw(struct is_memory_attribute *attribute)
{
	int ret;

	if (pgprot_val(attribute->state) != PTE_RDONLY) {
		err_lib("memory attribute state is wrong!!");
		return -EINVAL;
	}

	ret = is_memory_attribute_change(attribute->vaddr,
			attribute->numpages,
			__pgprot(PTE_PXN),
			__pgprot(0));
	if (ret) {
		err_lib("memory attribute change fail [PTE_PX -> PTE_PXN]\n");
		return ret;
	}

	ret = is_memory_attribute_change(attribute->vaddr,
			attribute->numpages,
			__pgprot(PTE_WRITE),
			__pgprot(PTE_RDONLY));
	if (ret) {
		err_lib("memory attribute change fail [PTE_RDONLY -> PTE_WRITE]\n");
		goto memory_excutable;
	}

	pgprot_val(attribute->state) = PTE_WRITE;

	return 0;

memory_excutable:
	ret = is_memory_attribute_change(attribute->vaddr,
			attribute->numpages,
			__pgprot(0),
			__pgprot(PTE_PXN));
	if (ret)
		err_lib("memory attribute change fail [PTE_PXN -> PTE_PX]\n");

	return -ENOMEM;
}

int is_memory_attribute_rox(struct is_memory_attribute *attribute)
{
	int ret;

	if (pgprot_val(attribute->state) != PTE_WRITE) {
		err_lib("memory attribute state is wrong!!");
		return -EINVAL;
	}

	ret = is_memory_attribute_change(attribute->vaddr,
			attribute->numpages,
			__pgprot(PTE_RDONLY),
			__pgprot(PTE_WRITE));
	if (ret) {
		err_lib("memory attribute change fail [PTE_WRITE -> PTE_RDONLY]\n");
		return ret;
	}

	ret = is_memory_attribute_change(attribute->vaddr,
			attribute->numpages,
			__pgprot(0),
			__pgprot(PTE_PXN));
	if (ret) {
		err_lib("memory attribute change fail [PTE_PXN -> PTE_PX]\n");
		return ret;
	}

	pgprot_val(attribute->state) = PTE_RDONLY;

	return 0;
}
#endif

#define INDEX_VRA_BIN	0
#define INDEX_ISP_BIN	1
int __nocfi is_load_ddk_bin(int loadType)
{
	int ret = 0;
	char bin_type[4] = {0};
	struct is_binary bin;
	os_system_func_t os_system_funcs[106];
	struct device *device = &gPtr_lib_support.pdev->dev;
	/* fixup the memory attribute for every region */
	ulong lib_addr;
#ifdef CONFIG_RKP
	struct rkp_dynamic_load rkp_dyn;
	static struct rkp_dynamic_load rkp_dyn_before = {0};
#endif
	ulong lib_isp = DDK_LIB_ADDR;
#ifdef USE_ONE_BINARY
	size_t bin_size = VRA_LIB_SIZE + DDK_LIB_SIZE;
#else
	size_t bin_size = DDK_LIB_SIZE;
#endif
#ifdef CONFIG_RKP
	ulong lib_vra = VRA_LIB_ADDR;
	struct is_memory_attribute memory_attribute[] = {
		{__pgprot(PTE_RDONLY), PFN_UP(LIB_VRA_CODE_SIZE), lib_vra},
		{__pgprot(PTE_RDONLY), PFN_UP(LIB_ISP_CODE_SIZE), lib_isp}
	};
#else
#ifndef CONFIG_DISABLE_CAMERA_MEM_ATTR
	ulong lib_vra = VRA_LIB_ADDR;
	struct is_memory_attribute memory_attribute[] = {
		{__pgprot(PTE_RDONLY), PFN_UP(LIB_VRA_CODE_SIZE), lib_vra},
		{__pgprot(PTE_RDONLY), PFN_UP(LIB_ISP_CODE_SIZE), lib_isp}
	};
#endif
#endif
#if defined(ENABLE_DYNAMIC_HEAP_FOR_DDK_RTA)
	struct is_core *core = (struct is_core *)dev_get_drvdata(device);
	struct is_minfo *minfo = &core->resourcemgr.minfo;
#endif

#ifdef USE_ONE_BINARY
	lib_addr = LIB_START;
	snprintf(bin_type, sizeof(bin_type), "DDK");
#else
	lib_addr = lib_isp;
	snprintf(bin_type, sizeof(bin_type), "ISP");
#endif

	/* load DDK library */
	setup_binary_loader(&bin, 3, -EAGAIN, NULL, NULL);
#ifdef CAMERA_FW_LOADING_FROM
	ret = is_vender_request_binary(&bin, IS_ISP_LIB_SDCARD_PATH, IS_FW_DUMP_PATH,
						gPtr_lib_support.fw_name, device);
#else
	ret = request_binary(&bin, IS_ISP_LIB_SDCARD_PATH,
						IS_ISP_LIB, device);
#endif
	if (ret) {
		err_lib("failed to load ISP library (%d)", ret);
		return ret;
	}

#ifndef USE_TZ_CONTROLLED_MEM_ATTRIBUTE
#ifdef USE_CDH_BINARY
	lib_addr = lib_vra;		/* DDK start Addr */
	bin.data = bin.data + CDH_SIZE;
	bin.size = bin.size - CDH_SIZE;
#endif
#endif

	if (loadType == BINARY_LOAD_ALL) {
#ifdef CONFIG_RKP
		memset(&rkp_dyn, 0, sizeof(rkp_dyn));
		rkp_dyn.binary_base = lib_addr;
		rkp_dyn.binary_size = bin.size;
		rkp_dyn.code_base1 = memory_attribute[INDEX_ISP_BIN].vaddr;
		rkp_dyn.code_size1 = memory_attribute[INDEX_ISP_BIN].numpages * PAGE_SIZE;
#ifdef USE_ONE_BINARY
		rkp_dyn.type = RKP_DYN_FIMC_COMBINED;
		rkp_dyn.code_base2 = memory_attribute[INDEX_VRA_BIN].vaddr;
		rkp_dyn.code_size2 = memory_attribute[INDEX_VRA_BIN].numpages * PAGE_SIZE;
#else
		rkp_dyn.type = RKP_DYN_FIMC;
#endif
		if (rkp_dyn_before.type)
			uh_call2(UH_APP_RKP, RKP_DYNAMIC_LOAD, RKP_DYN_COMMAND_RM, (u64)&rkp_dyn_before, 0, 0);
		memcpy(&rkp_dyn_before, &rkp_dyn, sizeof(rkp_dyn));
#endif
#ifndef CONFIG_DISABLE_CAMERA_MEM_ATTR
		ret = is_memory_attribute_nxrw(&memory_attribute[INDEX_ISP_BIN]);
		if (ret) {
			err_lib("failed to change into NX memory attribute (%d)", ret);
			return ret;
		}

#ifdef USE_ONE_BINARY
		ret = is_memory_attribute_nxrw(&memory_attribute[INDEX_VRA_BIN]);
		if (ret) {
			err_lib("failed to change into NX memory attribute (%d)", ret);
			return ret;
		}
#endif
#endif
		info_lib("binary info[%s] - type: C/D, from: %s\n",
			bin_type,
			was_loaded_by(&bin) ? "built-in" : "user-provided");
		if (bin.size <= bin_size) {
			memcpy((void *)lib_addr, bin.data, bin.size);
#if !defined(MODULE)
			__flush_dcache_area((void *)lib_addr, bin.size);
#endif
		} else {
			err_lib("DDK bin size is bigger than memory area. %zd[%zd]",
				bin.size, bin_size);
			ret = -EBADF;
			goto fail;
		}

#ifdef CONFIG_RKP
		uh_call2(UH_APP_RKP, RKP_DYNAMIC_LOAD, RKP_DYN_COMMAND_INS, (u64)&rkp_dyn, (u64)&ret, 0);
		if (ret) {
			err_lib("fail to load verify FIMC in EL2");
		}
#else
#ifndef CONFIG_DISABLE_CAMERA_MEM_ATTR
		ret = is_memory_attribute_rox(&memory_attribute[INDEX_ISP_BIN]);
		if (ret) {
			err_lib("failed to change into EX memory attribute (%d)", ret);
			return ret;
		}

#ifdef USE_ONE_BINARY
		ret = is_memory_attribute_rox(&memory_attribute[INDEX_VRA_BIN]);
		if (ret) {
			err_lib("failed to change into EX memory attribute (%d)", ret);
			return ret;
		}
#endif
#endif
#endif
	} else { /* loadType == BINARY_LOAD_DATA */
		if ((bin.size > CAMERA_BINARY_DDK_DATA_OFFSET) && (bin.size <= bin_size)) {
			info_lib("binary info[%s] - type: D, from: %s\n",
				bin_type,
				was_loaded_by(&bin) ? "built-in" : "user-provided");
			memcpy((void *)lib_addr + CAMERA_BINARY_VRA_DATA_OFFSET + CDH_SIZE,
				bin.data + CAMERA_BINARY_VRA_DATA_OFFSET + CDH_SIZE,
				CAMERA_BINARY_VRA_DATA_SIZE);
#if !defined(MODULE)
			__flush_dcache_area((void *)lib_addr + CAMERA_BINARY_VRA_DATA_OFFSET + CDH_SIZE,
								CAMERA_BINARY_VRA_DATA_SIZE);
#endif
			info_lib("binary info[%s] - type: D, from: %s\n",
				bin_type,
				was_loaded_by(&bin) ? "built-in" : "user-provided");
			memcpy((void *)lib_addr + CAMERA_BINARY_DDK_DATA_OFFSET + CDH_SIZE,
				bin.data + CAMERA_BINARY_DDK_DATA_OFFSET + CDH_SIZE,
				(bin.size - CAMERA_BINARY_DDK_DATA_OFFSET - CDH_SIZE));
#if !defined(MODULE)
			__flush_dcache_area((void *)lib_addr + CAMERA_BINARY_DDK_DATA_OFFSET + CDH_SIZE,
								bin.size - CAMERA_BINARY_DDK_DATA_OFFSET - CDH_SIZE);
#endif
		} else {
			err_lib("DDK bin size is bigger than memory area. %zd[%zd]",
				bin.size, bin_size);
			ret = -EBADF;
			goto fail;
		}
	}

#if defined(ENABLE_DYNAMIC_HEAP_FOR_DDK_RTA)
	{
		u32 ddk_heap_size = 0;
		u32 env_args[4];

		((ddk_get_env_func_t)DDK_GET_ENV_ADDR)((void *)env_args);

		ddk_heap_size = env_args[0];

		ret = is_heap_mem_alloc_dynamic(&core->resourcemgr, IS_BIN_LIB_HINT_DDK, ddk_heap_size);
		if (ret) {
			err_lib("failed to alloc memory for DDK heap(%d)", ret);
			goto fail;
		}
	}
#endif

	carve_binary_version(IS_BIN_LIBRARY, IS_BIN_LIB_HINT_DDK, &bin);
	release_binary(&bin);

	gPtr_lib_support.log_ptr = 0;
	set_os_system_funcs(os_system_funcs);
	/* call start_up function for DDK binary */
	is_fpsimd_get_func();
#if defined(ENABLE_DYNAMIC_HEAP_FOR_DDK_RTA)
	((start_up_func_t)lib_isp)((void **)os_system_funcs, (void *)minfo->kvaddr_heap_ddk);
#else
	((start_up_func_t)lib_isp)((void **)os_system_funcs);
#endif
	is_fpsimd_put_func();

	return 0;

fail:
	carve_binary_version(IS_BIN_LIBRARY, IS_BIN_LIB_HINT_DDK, &bin);
	release_binary(&bin);
	return ret;
}

int __nocfi is_load_vra_bin(int loadType)
{
#ifdef USE_ONE_BINARY
	/* VRA binary loading was included in ddk binary */
#else
	int ret = 0;
	struct is_binary bin;
	struct device *device = &gPtr_lib_support.pdev->dev;
	/* fixup the memory attribute for every region */
	ulong lib_isp = DDK_LIB_ADDR;
	ulong lib_vra = VRA_LIB_ADDR;
	struct is_memory_attribute memory_attribute[] = {
		{__pgprot(PTE_RDONLY), PFN_UP(LIB_VRA_CODE_SIZE), lib_vra},
		{__pgprot(PTE_RDONLY), PFN_UP(LIB_ISP_CODE_SIZE), lib_isp}
	};
#ifndef CONFIG_DISABLE_CAMERA_MEM_ATTR
	/* load VRA library */
	ret = is_memory_attribute_nxrw(&memory_attribute[INDEX_VRA_BIN]);
	if (ret) {
		err_lib("failed to change into NX memory attribute (%d)", ret);
		return ret;
	}
#endif
	setup_binary_loader(&bin, 3, -EAGAIN, NULL, NULL);
	ret = request_binary(&bin, IS_ISP_LIB_SDCARD_PATH,
						IS_VRA_LIB, device);
	if (ret) {
		err_lib("failed to load VRA library (%d)", ret);
		return ret;
	}
	info_lib("binary info[VRA] - type: C/D, addr: %#lx, size: 0x%lx from: %s\n",
			lib_vra, bin.size,
			was_loaded_by(&bin) ? "built-in" : "user-provided");
	if (bin.size <= VRA_LIB_SIZE) {
		memcpy((void *)lib_vra, bin.data, bin.size);
	} else {
		err_lib("VRA bin size is bigger than memory area. %d[%d]",
			(unsigned int)bin.size, (unsigned int)VRA_LIB_SIZE);
		goto fail;
	}
#if !defined(MODULE)
	__flush_dcache_area((void *)lib_vra, bin.size);
#endif
	flush_icache_range(lib_vra, bin.size);

	release_binary(&bin);
#ifndef CONFIG_DISABLE_CAMERA_MEM_ATTR
	ret = is_memory_attribute_rox(&memory_attribute[INDEX_VRA_BIN]);
	if (ret) {
		err_lib("failed to change into EX memory attribute (%d)", ret);
		return ret;
	}
#endif
#endif

	if (IS_ENABLED(ENABLE_VRA))
		is_lib_vra_os_funcs();

	return 0;

#if !defined(USE_ONE_BINARY)
fail:
	release_binary(&bin);
	return -EBADF;
#endif
}

int __nocfi is_load_rta_bin(int loadType)
{
	int ret = 0;
#ifdef USE_RTA_BINARY
	struct is_binary bin;
	struct device *device = &gPtr_lib_support.pdev->dev;
	os_system_func_t os_system_funcs[100];
	ulong lib_rta = RTA_LIB_ADDR;
#ifdef CONFIG_RKP
	struct rkp_dynamic_load rkp_dyn;
	static struct rkp_dynamic_load rkp_dyn_before = {0};
	struct is_memory_attribute rta_memory_attribute = {
		__pgprot(PTE_RDONLY), PFN_UP(LIB_RTA_CODE_SIZE), lib_rta};
#else
#ifndef CONFIG_DISABLE_CAMERA_MEM_ATTR
	struct is_memory_attribute rta_memory_attribute = {
		__pgprot(PTE_RDONLY), PFN_UP(LIB_RTA_CODE_SIZE), lib_rta};
#endif
#endif
#if defined(ENABLE_DYNAMIC_HEAP_FOR_DDK_RTA)
	struct is_core *core = (struct is_core *)dev_get_drvdata(device);
	struct is_minfo *minfo = &core->resourcemgr.minfo;
#endif

	/* load RTA library */

	setup_binary_loader(&bin, 3, -EAGAIN, NULL, NULL);
#ifdef CAMERA_FW_LOADING_FROM
	ret = is_vender_request_binary(&bin, IS_ISP_LIB_SDCARD_PATH, IS_FW_DUMP_PATH,
						gPtr_lib_support.rta_fw_name, device);
#else
	ret = request_binary(&bin, IS_ISP_LIB_SDCARD_PATH,
						IS_RTA_LIB, device);
#endif
	if (ret) {
		err_lib("failed to load RTA library (%d)", ret);
		return ret;
	}

	if (loadType == BINARY_LOAD_ALL) {
#ifdef CONFIG_RKP
		memset(&rkp_dyn, 0, sizeof(rkp_dyn));
		rkp_dyn.binary_base = lib_rta;
		rkp_dyn.binary_size = bin.size;
		rkp_dyn.code_base1 = rta_memory_attribute.vaddr;
		rkp_dyn.code_size1 = rta_memory_attribute.numpages * PAGE_SIZE;
		rkp_dyn.type = RKP_DYN_FIMC;
		if (rkp_dyn_before.type)
			uh_call2(UH_APP_RKP, RKP_DYNAMIC_LOAD, RKP_DYN_COMMAND_RM, (u64)&rkp_dyn_before, 0, 0);
		memcpy(&rkp_dyn_before, &rkp_dyn, sizeof(rkp_dyn));
#endif
#ifndef CONFIG_DISABLE_CAMERA_MEM_ATTR
		ret = is_memory_attribute_nxrw(&rta_memory_attribute);
		if (ret) {
			err_lib("failed to change into NX memory attribute (%d)", ret);
			return ret;
		}
#endif
		info_lib("binary info[RTA] - type: C/D, from: %s\n",
			was_loaded_by(&bin) ? "built-in" : "user-provided");
		if (bin.size <= RTA_LIB_SIZE) {
			memcpy((void *)lib_rta, bin.data, bin.size);
#if !defined(MODULE)
			__flush_dcache_area((void *)lib_rta, bin.size);
#endif
		} else {
			err_lib("RTA bin size is bigger than memory area. %d[%d]",
				(unsigned int)bin.size, (unsigned int)RTA_LIB_SIZE);
			ret = -EBADF;
			goto fail;
		}
#ifdef CONFIG_RKP
		uh_call2(UH_APP_RKP, RKP_DYNAMIC_LOAD, RKP_DYN_COMMAND_INS, (u64)&rkp_dyn, (u64)&ret, 0);
		if (ret) {
			err_lib("fail to load verify FIMC in EL2");
		}
#else
#ifndef CONFIG_DISABLE_CAMERA_MEM_ATTR
		ret = is_memory_attribute_rox(&rta_memory_attribute);
		if (ret) {
			err_lib("failed to change into EX memory attribute (%d)", ret);
			return ret;
		}
#endif
#endif

	} else { /* loadType == BINARY_LOAD_DATA */
		if ((bin.size > CAMERA_BINARY_RTA_DATA_OFFSET) && (bin.size <= RTA_LIB_SIZE)) {
			info_lib("binary info[RTA] - type: D, from: %s\n",
				was_loaded_by(&bin) ? "built-in" : "user-provided");
			memcpy((void *)lib_rta + CAMERA_BINARY_RTA_DATA_OFFSET,
				bin.data + CAMERA_BINARY_RTA_DATA_OFFSET,
				bin.size - CAMERA_BINARY_RTA_DATA_OFFSET);
#if !defined(MODULE)
			__flush_dcache_area((void *)lib_rta + CAMERA_BINARY_RTA_DATA_OFFSET,
								bin.size - CAMERA_BINARY_RTA_DATA_OFFSET);
#endif
		} else {
			err_lib("RTA bin size is bigger than memory area. %d[%d]",
				(unsigned int)bin.size, (unsigned int)RTA_LIB_SIZE);
			ret = -EBADF;
			goto fail;
		}
	}

	carve_binary_version(IS_BIN_LIBRARY, IS_BIN_LIB_HINT_RTA, &bin);
	release_binary(&bin);

#if defined(ENABLE_DYNAMIC_HEAP_FOR_DDK_RTA)
	{
		u32 rta_heap_size = 0;
		u32 env_args[4];

		((rta_get_env_func_t)RTA_GET_ENV_ADDR)((void *)&env_args);
		rta_heap_size = env_args[0];

		ret = is_heap_mem_alloc_dynamic(&core->resourcemgr, IS_BIN_LIB_HINT_RTA, rta_heap_size);
		if (ret) {
			err_lib("failed to alloc memory for RTA heap(%d)", ret);
			goto fail;
		}
	}
#endif

	set_os_system_funcs_for_rta(os_system_funcs);
	/* call start_up function for RTA binary */
	is_fpsimd_get_func();
#if defined(ENABLE_DYNAMIC_HEAP_FOR_DDK_RTA)
	((rta_start_up_func_t)lib_rta)((void *)minfo->kvaddr_heap_rta, (void **)os_system_funcs);
#else
	((rta_start_up_func_t)lib_rta)(NULL, (void **)os_system_funcs);
#endif
	is_fpsimd_put_func();
#endif

	return ret;

fail:
	carve_binary_version(IS_BIN_LIBRARY, IS_BIN_LIB_HINT_RTA, &bin);
	release_binary(&bin);
	return ret;
}

int is_load_bin(void)
{
	int ret = 0;
	struct is_lib_support *lib = &gPtr_lib_support;
	struct is_core *core;

	core = (struct is_core *)platform_get_drvdata(lib->pdev);

	info_lib("binary load start\n");

	if (lib->binary_load_flg) {
		info_lib("%s: binary is already loaded\n", __func__);
		return ret;
	}

	if (lib->binary_load_fatal) {
		err_lib("%s: binary loading is fatal error 0x%x\n", __func__, lib->binary_load_fatal);
		ret = -EBADF;
		return ret;
	}

	ret = lib_support_init();
	if (ret < 0) {
		err_lib("lib_support_init failed!! (%d)", ret);
		return ret;
	}
	dbg_lib(3, "lib_support_init success!!\n");

#ifdef ENABLE_TNR
	mblk_init(&lib->mb_dma_tnr, lib->minfo->pb_tnr, MT_TYPE_MB_DMA_TNR, "DMA_TNR");
#if defined(SECURE_CAMERA_MEM_SHARE)
	if (core && core->scenario == IS_SCENARIO_SECURE) {
#if defined(SECURE_CAMERA_TNR)
		mblk_init(&lib->mb_dma_tnr_s, lib->minfo->pb_tnr_s,
				MT_TYPE_MB_DMA_TNR_S, "DMA_TNR_S");
#endif
	}
#endif
#endif
#if (ORBMCH_DMA_SIZE > 0)
	mblk_init(&lib->mb_dma_orbmch, lib->minfo->pb_orbmch, MT_TYPE_MB_DMA_ORBMCH, "DMA_ORB");
#endif
#if (CLAHE_DMA_SIZE > 0)
	mblk_init(&lib->mb_dma_clahe, lib->minfo->pb_clahe, MT_TYPE_MB_DMA_CLAHE, "DMA_CLA");
#endif

	if (IS_ENABLED(ENABLE_VRA))
		mblk_init(&lib->mb_vra, lib->minfo->pb_vra, MT_TYPE_MB_VRA, "VRA");
#ifdef ENABLE_VRA_NETARRAY
	mblk_init(&lib->mb_vra_net_array, lib->minfo->pb_vra_netarray, MT_TYPE_MB_VRA_NETARR, "VRA_NET");
#endif

	is_load_ctrl_lock();
#ifdef USE_TZ_CONTROLLED_MEM_ATTRIBUTE
	if (gPtr_lib_support.binary_code_load_flg & BINARY_LOAD_DDK_DONE) {
		ret = is_load_ddk_bin(BINARY_LOAD_DATA);
	} else {
		info_lib("DDK C/D binary reload start\n");
		ret = is_load_ddk_bin(BINARY_LOAD_ALL);
		if (ret == 0)
			gPtr_lib_support.binary_code_load_flg |= BINARY_LOAD_DDK_DONE;
	}
#else
	ret = is_load_ddk_bin(BINARY_LOAD_ALL);
	if (ret == 0)
		gPtr_lib_support.binary_code_load_flg |= BINARY_LOAD_DDK_DONE;
#endif
	if (ret) {
		err_lib("load_ddk_bin() failed (%d)\n", ret);
		is_load_ctrl_unlock();
		return ret;
	}

#ifdef USE_TZ_CONTROLLED_MEM_ATTRIBUTE
	ret = is_load_vra_bin(BINARY_LOAD_DATA);
#else
	ret = is_load_vra_bin(BINARY_LOAD_ALL);
#endif
	if (ret) {
		err_lib("load_vra_bin() failed (%d)\n", ret);
		is_load_ctrl_unlock();
		return ret;
	}

#ifdef USE_TZ_CONTROLLED_MEM_ATTRIBUTE
	if (gPtr_lib_support.binary_code_load_flg & BINARY_LOAD_RTA_DONE) {
		ret = is_load_rta_bin(BINARY_LOAD_DATA);
	} else {
		info_lib("RTA C/D binary reload start\n");
		ret = is_load_rta_bin(BINARY_LOAD_ALL);
		if (ret == 0)
			gPtr_lib_support.binary_code_load_flg |= BINARY_LOAD_RTA_DONE;
	}
#else
	ret = is_load_rta_bin(BINARY_LOAD_ALL);
	if (ret == 0)
		gPtr_lib_support.binary_code_load_flg |= BINARY_LOAD_RTA_DONE;
#endif
	if (ret) {
		err_lib("load_rta_bin() failed (%d)\n", ret);
		is_load_ctrl_unlock();
		return ret;
	}
	is_load_ctrl_unlock();

	lib->binary_load_flg = true;

#if defined(SECURE_CAMERA_MEM_SHARE)
	if (core && core->scenario == IS_SCENARIO_SECURE) {
#if defined(SECURE_CAMERA_TAAISP)
		mblk_init(&lib->mb_dma_taaisp, lib->minfo->pb_taaisp_s,
				MT_TYPE_MB_DMA_TAAISP, "DMA_TAAISP_S");
#else
		mblk_init(&lib->mb_dma_taaisp, lib->minfo->pb_taaisp,
				MT_TYPE_MB_DMA_TAAISP, "DMA_TAAISP");
#endif
#if (MEDRC_DMA_SIZE > 0)
#if defined(SECURE_CAMERA_MEDRC)
		mblk_init(&lib->mb_dma_medrc, lib->minfo->pb_medrc_s,
				MT_TYPE_MB_DMA_MEDRC, "DMA_MEDRC_S");
#else
		mblk_init(&lib->mb_dma_medrc, lib->minfo->pb_medrc,
				MT_TYPE_MB_DMA_MEDRC, "DMA_MEDRC");
#endif
#endif
	} else
#endif
	{
		mblk_init(&lib->mb_dma_taaisp, lib->minfo->pb_taaisp,
				MT_TYPE_MB_DMA_TAAISP, "DMA_TAAISP");
#if (MEDRC_DMA_SIZE > 0)
		mblk_init(&lib->mb_dma_medrc, lib->minfo->pb_medrc,
				MT_TYPE_MB_DMA_MEDRC, "DMA_MEDRC");
#endif
	}

	spin_lock_init(&lib->slock_nmb);
	INIT_LIST_HEAD(&lib->list_of_nmb);

	info_lib("binary load done\n");

	return 0;
}

void is_load_ctrl_init(void)
{
	mutex_init(&gPtr_bin_load_ctrl);
	spin_lock_init(&gPtr_lib_support.slock_debug);
	gPtr_lib_support.binary_code_load_flg = 0;
	gPtr_lib_support.binary_load_fatal = 0;
}

void is_load_ctrl_lock(void)
{
	mutex_lock(&gPtr_bin_load_ctrl);
}

void is_load_ctrl_unlock(void)
{
	mutex_unlock(&gPtr_bin_load_ctrl);
}

int is_load_bin_on_boot(void)
{
	int ret = 0;
#ifdef CONFIG_CAMERA_VENDER_MCD
	struct is_lib_support *lib = &gPtr_lib_support;
	struct is_core *core;

	core = (struct is_core *)platform_get_drvdata(lib->pdev);

#ifdef ENABLE_TNR
	mblk_init(&lib->mb_dma_tnr, lib->minfo->pb_tnr, MT_TYPE_MB_DMA_TNR, "DMA_TNR");
#endif
#if (ORBMCH_DMA_SIZE > 0)
	mblk_init(&lib->mb_dma_orbmch, lib->minfo->pb_orbmch, MT_TYPE_MB_DMA_ORBMCH, "DMA_ORB");
#endif
#if (CLAHE_DMA_SIZE > 0)
	mblk_init(&lib->mb_dma_clahe, lib->minfo->pb_clahe, MT_TYPE_MB_DMA_CLAHE, "DMA_CLA");
#endif

	if (IS_ENABLED(ENABLE_VRA))
		mblk_init(&lib->mb_vra, lib->minfo->pb_vra, MT_TYPE_MB_VRA, "VRA");
#ifdef ENABLE_VRA_NETARRAY
	mblk_init(&lib->mb_vra_net_array, lib->minfo->pb_vra_netarray, MT_TYPE_MB_VRA_NETARR, "VRA_NET");
#endif
#endif
	if (!(gPtr_lib_support.binary_code_load_flg & BINARY_LOAD_DDK_DONE)) {
		ret = is_load_ddk_bin(BINARY_LOAD_ALL);
		if (ret) {
			err_lib("is_load_ddk_bin is fail(%d)", ret);
			if (ret == -EBADF)
				is_vender_remove_dump_fw_file();
		} else {
			gPtr_lib_support.binary_code_load_flg |= BINARY_LOAD_DDK_DONE;
		}
	}

	if (!(gPtr_lib_support.binary_code_load_flg & BINARY_LOAD_RTA_DONE)) {
		ret = is_load_rta_bin(BINARY_LOAD_ALL);
		if (ret) {
			err_lib("is_load_rta_bin is fail(%d)", ret);
			if (ret == -EBADF)
				is_vender_remove_dump_fw_file();
		} else {
			gPtr_lib_support.binary_code_load_flg |= BINARY_LOAD_RTA_DONE;
		}
	}

	return ret;
}

int is_set_fw_names(char *fw_name, char *rta_fw_name)
{
	if (fw_name == NULL || rta_fw_name == NULL) {
		err_lib("%s %p %p fail. ", __func__, fw_name, rta_fw_name);
		return -1;
	}

	memcpy(gPtr_lib_support.fw_name, fw_name, sizeof(gPtr_lib_support.fw_name));
	memcpy(gPtr_lib_support.rta_fw_name, rta_fw_name, sizeof(gPtr_lib_support.rta_fw_name));

	return 0;
}
