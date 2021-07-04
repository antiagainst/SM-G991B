/*
 *
 * Copyright (c) 2016 Samsung Electronics
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 as
 * published by the Free Software Foundation.
 */

#ifndef __S6E3HAB_PROFILER_PANEL_H__
#define __S6E3HAB_PROFILER_PANEL_H__

#include "s6e3hab_profiler.h"
#include "../display_profiler/maskgen.h"

static u8 HAB_PROFILE_KEY2_ENABLE[] = { 0xF0, 0x5A, 0x5A };
static u8 HAB_PROFILE_KEY2_DISABLE[] = { 0xF0, 0xA5, 0xA5 };

#if defined(__PANEL_NOT_USED_VARIABLE__)
static u8 HAB_PROFILE_KEY3_ENABLE[] = { 0xFC, 0x5A, 0x5A };
static u8 HAB_PROFILE_KEY3_DISABLE[] = { 0xFC, 0xA5, 0xA5 };
#endif

static DEFINE_STATIC_PACKET(hab_profile_key2_enable, DSI_PKT_TYPE_WR, HAB_PROFILE_KEY2_ENABLE, 0);
static DEFINE_STATIC_PACKET(hab_profile_key2_disable, DSI_PKT_TYPE_WR, HAB_PROFILE_KEY2_DISABLE, 0);

#if defined(__PANEL_NOT_USED_VARIABLE__)
static DEFINE_STATIC_PACKET(hab_profile_key3_enable, DSI_PKT_TYPE_WR, HAB_PROFILE_KEY3_ENABLE, 0);
static DEFINE_STATIC_PACKET(hab_profile_key3_disable, DSI_PKT_TYPE_WR, HAB_PROFILE_KEY3_DISABLE, 0);
#endif

static DEFINE_PANEL_KEY(hab_profile_key2_enable, CMD_LEVEL_2,
	KEY_ENABLE, &PKTINFO(hab_profile_key2_enable));
static DEFINE_PANEL_KEY(hab_profile_key2_disable, CMD_LEVEL_2,
	KEY_DISABLE, &PKTINFO(hab_profile_key2_disable));

#if defined(__PANEL_NOT_USED_VARIABLE__)
static DEFINE_PANEL_KEY(hab_profile_key3_enable, CMD_LEVEL_3,
	KEY_ENABLE, &PKTINFO(hab_profile_key3_enable));
static DEFINE_PANEL_KEY(hab_profile_key3_disable, CMD_LEVEL_3,
	KEY_DISABLE, &PKTINFO(hab_profile_key3_disable));
#endif

static struct keyinfo KEYINFO(hab_profile_key2_enable);
static struct keyinfo KEYINFO(hab_profile_key2_disable);
#if defined(__PANEL_NOT_USED_VARIABLE__)
static struct keyinfo KEYINFO(hab_profile_key3_enable);
static struct keyinfo KEYINFO(hab_profile_key3_disable);
#endif

static struct maptbl hab_profiler_maptbl[] = {
	[PROFILE_WIN_UPDATE_MAP] = DEFINE_0D_MAPTBL(hab_set_self_grid,
		init_common_table, NULL, hab_profile_win_update),
	[DISPLAY_PROFILE_FPS_MAP] = DEFINE_0D_MAPTBL(hab_display_profile_fps,
		init_common_table, NULL, hab_profile_display_fps),
	[PROFILE_SET_COLOR_MAP] = DEFINE_0D_MAPTBL(hab_set_fps_color,
		init_common_table, NULL, hab_profile_set_color),
	[PROFILE_SET_CIRCLE] = DEFINE_0D_MAPTBL(hab_set_circle,
		init_common_table, NULL, hab_profile_circle),
	[PROFILE_FPS_MASK_POSITION_MAP] = DEFINE_0D_MAPTBL(hab_set_fps_mask_pos,
		init_common_table, NULL, hab_profile_fps_mask_pos),
	[PROFILE_FPS_MASK_COLOR_MAP] = DEFINE_0D_MAPTBL(hab_set_fps_mask_color,
		init_common_table, NULL, hab_profile_fps_mask_color),
};

static char HAB_SET_SELF_GRID[] = {
	0x7C,
	0x01, 0x01, 0x40, 0x1F,
	0x00, 0x00, 0x00, 0x00,
	0x01, 0x00, 0x01, 0x00,
	0x11,
};
static DEFINE_PKTUI(hab_set_self_grid, &hab_profiler_maptbl[PROFILE_WIN_UPDATE_MAP], 0);
static DEFINE_VARIABLE_PACKET(hab_set_self_grid, DSI_PKT_TYPE_WR, HAB_SET_SELF_GRID, 0);

static void *hab_profile_win_update_cmdtbl[] = {
	&KEYINFO(hab_profile_key2_enable),
	&PKTINFO(hab_set_self_grid),
	&KEYINFO(hab_profile_key2_disable),
};

/*
 * NEED TO CHECK by kernel.lee
 */
static char HAB_DISABLE_SELF_GRID[] = {
	0x7C,
	0x00, 0x00,
};
static DEFINE_STATIC_PACKET(hab_disable_self_grid, DSI_PKT_TYPE_WR, HAB_DISABLE_SELF_GRID, 0);

static void *hab_profile_win_disable_cmdtbl[] = {
	&KEYINFO(hab_profile_key2_enable),
	&PKTINFO(hab_disable_self_grid),
	&KEYINFO(hab_profile_key2_disable),
};

#if defined(__PANEL_NOT_USED_VARIABLE__)
static char HAB_SET_PROFILE_SD_DIGIT[] = {
	0x75,
	0x02,
};
static DEFINE_STATIC_PACKET(hab_set_profile_sd_digit, DSI_PKT_TYPE_WR, HAB_SET_PROFILE_SD_DIGIT, 0);

static char HAB_RESET_PROFILE_SD_DIGIT[] = {
	0x75,
	0x00,
};
static DEFINE_STATIC_PACKET(hab_reset_profile_sd_digit, DSI_PKT_TYPE_WR, HAB_RESET_PROFILE_SD_DIGIT, 0);
#endif

static char HAB_INIT_PROFILE_FPS[] = {
	0x80,
	0x00, 0x03, 0x02, 0x1F,
	0x03, 0xE8, 0x01, 0x90,
	0x04, 0x1A, 0x01, 0x90,
	/* invisiable M2 x : 2230*/
	0x04, 0x4C, 0x01, 0x90,
	0x04, 0x4C, 0x01, 0x90,
	0x00, 0x28, 0x00, 0x4B,
	0x80, 0x0F, 0xFF, 0x0F,
	0x00, 0x00,
};
static DEFINE_STATIC_PACKET(hab_init_profile_fps, DSI_PKT_TYPE_WR, HAB_INIT_PROFILE_FPS, 0);

static char HAB_INIT_PROFILE_TIMER[] = {
	0x81,
	0x00, 0x03, 0x09, 0x09,
	0x1E, 0x00, 0x1E, 0x03,
	0x00, 0x00
};
static DEFINE_STATIC_PACKET(hab_init_profile_timer, DSI_PKT_TYPE_WR, HAB_INIT_PROFILE_TIMER, 0);

static char HAB_UPDATE_PROFILE_FPS[] = {
	0x78,
	0x01,
};
static DEFINE_STATIC_PACKET(hab_update_profile_fps, DSI_PKT_TYPE_WR, HAB_UPDATE_PROFILE_FPS, 0);

void *hab_init_profile_fps_cmdtbl[] = {
	&KEYINFO(hab_profile_key2_enable),
	&PKTINFO(hab_init_profile_fps),
	&PKTINFO(hab_init_profile_timer),
	&PKTINFO(hab_update_profile_fps),
	&KEYINFO(hab_profile_key2_disable),
};

static char HAB_DISPLAY_PROFILE_FPS[] = {
	0x81,
	0x0A, 0x0A,
};
static DEFINE_PKTUI(hab_display_profile_fps, &hab_profiler_maptbl[DISPLAY_PROFILE_FPS_MAP], 0);
static DEFINE_VARIABLE_PACKET(hab_display_profile_fps, DSI_PKT_TYPE_WR, HAB_DISPLAY_PROFILE_FPS, 2);

void *hab_display_profile_fps_cmdtbl[] = {
	&KEYINFO(hab_profile_key2_enable),
	&PKTINFO(hab_display_profile_fps),
	&PKTINFO(hab_update_profile_fps),
	&KEYINFO(hab_profile_key2_disable),
};

static char HAB_SET_FPS_COLOR[] = {
	0x80,
	0x80, 0xFF, 0x0F, 0x0F,
};
static DEFINE_PKTUI(hab_set_fps_color, &hab_profiler_maptbl[PROFILE_SET_COLOR_MAP], 0);
static DEFINE_VARIABLE_PACKET(hab_set_fps_color, DSI_PKT_TYPE_WR, HAB_SET_FPS_COLOR, 24);

void *hab_profile_set_color_cmdtbl[] = {
	&KEYINFO(hab_profile_key2_enable),
	&PKTINFO(hab_set_fps_color),
	&KEYINFO(hab_profile_key2_disable),
};

static char HAB_SET_CIRCLE[] = {
	0x79,
	0x00, 0x88, 0x00, 0x00, 0x00,
	0x00, 0x57, 0x01, 0x30, 0x0A,
	0x00, 0xFF, 0x00, 0x00, 0xFF,
	0x00, 0x00, 0x64, 0x01, 0x2C,
	0x00, 0x00, 0x00, 0x00, 0x01,
	0x00,
};

static DEFINE_PKTUI(hab_set_circle, &hab_profiler_maptbl[PROFILE_SET_CIRCLE], 0);
static DEFINE_VARIABLE_PACKET(hab_set_circle, DSI_PKT_TYPE_WR, HAB_SET_CIRCLE, 0);


void *hab_profile_circle_cmdtbl[] = {
	&KEYINFO(hab_profile_key2_enable),
	&PKTINFO(hab_set_circle),
	&KEYINFO(hab_profile_key2_disable),
};

static char HAB_FPS_MASK_DISABLE[] = {
	0x7A, 0x00
};

static char HAB_FPS_MASK_MEM_SELECT[] = {
	0x75, 0x10
};

static char HAB_FPS_MASK_MEM_RESET[] = {
	0x75, 0x01
};

static char HAB_FPS_MASK_MEM_ENABLE[] = {
	0x7A,
	0x21, 0x00, 0x00, 0x01,
	0x8F, 0x0C, 0xE4, 0x0D,
	0xAB, 0x09, 0x0F, 0x00,
	0x00, 0x00, 0x00,
};
/*
static char HAB_SELF_MASK_BUFFER[9728] = {
	0x00,
};
*/
static DEFINE_STATIC_PACKET(hab_fps_mask_disable, DSI_PKT_TYPE_WR, HAB_FPS_MASK_DISABLE, 0);
static DEFINE_STATIC_PACKET(hab_fps_mask_mem_select, DSI_PKT_TYPE_WR, HAB_FPS_MASK_MEM_SELECT, 0);
static DEFINE_STATIC_PACKET(hab_fps_mask_mem_reset, DSI_PKT_TYPE_WR, HAB_FPS_MASK_MEM_RESET, 0);

static DECLARE_PKTUI(hab_fps_mask_enable) = {
	{ .offset = 2, .maptbl = &hab_profiler_maptbl[PROFILE_FPS_MASK_POSITION_MAP] },
	{ .offset = 12, .maptbl = &hab_profiler_maptbl[PROFILE_FPS_MASK_COLOR_MAP] },
};
static DEFINE_VARIABLE_PACKET(hab_fps_mask_enable, DSI_PKT_TYPE_WR, HAB_FPS_MASK_MEM_ENABLE, 0);

//static DEFINE_PKTUI(hab_self_mask_buffer, &hab_profiler_maptbl[PROFILE_FPS_MASK_GENERATE_MAP], 0);
//static DEFINE_VARIABLE_PACKET(hab_self_mask_buffer, DSI_PKT_TYPE_WR_SR, HAB_SELF_MASK_BUFFER, 0);

static DEFINE_PANEL_MDELAY(hab_fps_mask_delay, 17);
static DEFINE_PANEL_UDELAY_NO_SLEEP(hab_fps_mask_sel_memory_delay, 1);

void *hab_profile_fps_mask_disable_cmdtbl[] = {
	&KEYINFO(hab_profile_key2_enable),
	&PKTINFO(hab_fps_mask_disable),
	&KEYINFO(hab_profile_key2_disable),
};

void *hab_profile_fps_mask_wait_cmdtbl[] = {
	&DLYINFO(hab_fps_mask_delay),
};

void *hab_profile_fps_mask_mem_select_cmdtbl[] = {
	&KEYINFO(hab_profile_key2_enable),
	&PKTINFO(hab_fps_mask_mem_select),
	&DLYINFO(hab_fps_mask_sel_memory_delay),
};

void *hab_profile_fps_mask_enable_cmdtbl[] = {
	&PKTINFO(hab_fps_mask_mem_reset),
	&DLYINFO(hab_fps_mask_sel_memory_delay),
	&PKTINFO(hab_fps_mask_enable),
	&KEYINFO(hab_profile_key2_disable),
};

static struct seqinfo hab_profiler_seqtbl[MAX_PROFILER_SEQ] = {
	[PROFILE_WIN_UPDATE_SEQ] = SEQINFO_INIT("update_profile_win", hab_profile_win_update_cmdtbl),
	[PROFILE_DISABLE_WIN_SEQ] = SEQINFO_INIT("disable_profile_win", hab_profile_win_disable_cmdtbl),
	[INIT_PROFILE_FPS_SEQ] = SEQINFO_INIT("init_profile_fps", hab_init_profile_fps_cmdtbl),
	[DISPLAY_PROFILE_FPS_SEQ] = SEQINFO_INIT("display_profile_fps", hab_display_profile_fps_cmdtbl),
	[PROFILE_SET_COLOR_SEQ] = SEQINFO_INIT("profile_set_color", hab_profile_set_color_cmdtbl),
	[PROFILER_SET_CIRCLR_SEQ] = SEQINFO_INIT("profile_circle", hab_profile_circle_cmdtbl),
	[DISABLE_PROFILE_FPS_MASK_SEQ] = SEQINFO_INIT("disable_profile_fps_mask", hab_profile_fps_mask_disable_cmdtbl),
	[WAIT_PROFILE_FPS_MASK_SEQ] = SEQINFO_INIT("wait_profile_fps_mask", hab_profile_fps_mask_wait_cmdtbl),
	[MEM_SELECT_PROFILE_FPS_MASK_SEQ] = SEQINFO_INIT("mem_select_profile_fps_mask", hab_profile_fps_mask_mem_select_cmdtbl),
	[ENABLE_PROFILE_FPS_MASK_SEQ] = SEQINFO_INIT("enable_profile_fps_mask", hab_profile_fps_mask_enable_cmdtbl),
};

static struct profiler_config hab_profiler_config = {
	.profiler_en = 1,
	.profiler_debug = 0,
	.systrace = 1,
	.timediff_en = 0,
	.cycle_time = 50,
	.fps_en = 1,
	.fps_disp = 1,
	.te_en = 1,
	.te_disp = 1,
	.hiber_en = 1,
	.hiber_disp = 1,
	.log_en = 1,
	.log_debug = 0,
	.log_disp = 2,
	.log_level = 2,
	.log_filter_en = 0,
	.mprint_en = 0,
};

static struct mprint_config hab_mprint_config = {
	.debug = 0,
	.scale = 10,
	.color = 0x1FF7FDFF,
	.resol_x = 1440,
	.resol_y = 400,
	.padd_x = 100,
	.padd_y = 100,
	.spacing = 4,
	.skip_y = 3,
	.max_len = 9728,
};

static struct profiler_tune hab_profiler_tune = {
	.name = "s6e3hab_profiler",
	.nr_seqtbl = ARRAY_SIZE(hab_profiler_seqtbl),
	.seqtbl = hab_profiler_seqtbl,
	.nr_maptbl = ARRAY_SIZE(hab_profiler_maptbl),
	.maptbl = hab_profiler_maptbl,
	.mprint_config = &hab_mprint_config,
	.conf = &hab_profiler_config,
};

#endif //__S6E3HAB_PROFILER_PANEL_H__
