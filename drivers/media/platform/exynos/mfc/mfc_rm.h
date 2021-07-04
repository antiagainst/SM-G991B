/*
 * drivers/media/platform/exynos/mfc/mfc_rm.h
 *
 * Copyright (c) 2020 Samsung Electronics Co., Ltd.
 *		http://www.samsung.com/
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 */
#ifndef __MFC_RM_H
#define __MFC_RM_H __FILE__

#include "mfc_sync.h"

#include "mfc_common.h"

#define MFC_RM_LOAD_DELETE		0
#define MFC_RM_LOAD_ADD			1
#define MFC_RM_LOAD_DELETE_UPDATE	2

static inline struct mfc_core *mfc_get_master_core_lock(struct mfc_dev *dev,
			struct mfc_ctx *ctx)
{
	struct mfc_core *core;

	mfc_get_corelock_ctx(ctx);

	if (ctx->op_core_num[MFC_CORE_MASTER] == MFC_CORE_INVALID)
		core = NULL;
	else
		core = dev->core[ctx->op_core_num[MFC_CORE_MASTER]];

	mfc_release_corelock_ctx(ctx);

	return core;
}

static inline struct mfc_core *mfc_get_slave_core_lock(struct mfc_dev *dev,
			struct mfc_ctx *ctx)
{
	struct mfc_core *core;

	mfc_get_corelock_ctx(ctx);

	if (ctx->op_core_num[MFC_CORE_SLAVE] == MFC_CORE_INVALID)
		core = NULL;
	else
		core = dev->core[ctx->op_core_num[MFC_CORE_SLAVE]];

	mfc_release_corelock_ctx(ctx);

	return core;
}

static inline struct mfc_core *mfc_get_master_core(struct mfc_dev *dev,
			struct mfc_ctx *ctx)
{
	if (ctx->op_core_num[MFC_CORE_MASTER] == MFC_CORE_INVALID)
		return NULL;

	return dev->core[ctx->op_core_num[MFC_CORE_MASTER]];
}

static inline struct mfc_core *mfc_get_slave_core(struct mfc_dev *dev,
			struct mfc_ctx *ctx)
{
	if (ctx->op_core_num[MFC_CORE_SLAVE] == MFC_CORE_INVALID)
		return NULL;

	return dev->core[ctx->op_core_num[MFC_CORE_SLAVE]];
}

static inline void mfc_rm_set_core_num(struct mfc_ctx *ctx, int master_core_num)
{
	ctx->op_core_num[MFC_CORE_MASTER] = master_core_num;

	if (master_core_num == MFC_DEC_DEFAULT_CORE)
		ctx->op_core_num[MFC_CORE_SLAVE] = MFC_SURPLUS_CORE;
	else
		ctx->op_core_num[MFC_CORE_SLAVE] = MFC_DEC_DEFAULT_CORE;

	mfc_debug(2, "[RM] master core %d, slave core %d\n",
			ctx->op_core_num[MFC_CORE_MASTER],
			ctx->op_core_num[MFC_CORE_SLAVE]);
}

/* load balancing */
void mfc_rm_migration_worker(struct work_struct *work);
void mfc_rm_load_balancing(struct mfc_ctx *ctx, int load_add);

/* core ops */
int mfc_rm_instance_init(struct mfc_dev *dev, struct mfc_ctx *ctx);
int mfc_rm_instance_deinit(struct mfc_dev *dev, struct mfc_ctx *ctx);
int mfc_rm_instance_open(struct mfc_dev *dev, struct mfc_ctx *ctx);
void mfc_rm_instance_dec_stop(struct mfc_dev *dev, struct mfc_ctx *ctx,
			unsigned int type);
void mfc_rm_instance_enc_stop(struct mfc_dev *dev, struct mfc_ctx *ctx,
			unsigned int type);
int mfc_rm_instance_setup(struct mfc_dev *dev, struct mfc_ctx *ctx);
void mfc_rm_request_work(struct mfc_dev *dev, enum mfc_request_work work,
		struct mfc_ctx *ctx);

/* utils */
void mfc_rm_qos_control(struct mfc_ctx *ctx, enum mfc_qos_control qos_control);
int mfc_rm_query_state(struct mfc_ctx *ctx, enum mfc_inst_state_query query,
			enum mfc_inst_state state);

#endif /* __MFC_RM_H */
