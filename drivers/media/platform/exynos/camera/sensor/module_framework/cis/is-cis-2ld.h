/*
 * Samsung Exynos SoC series Sensor driver
 *
 *
 * Copyright (c) 2016 Samsung Electronics Co., Ltd
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 as
 * published by the Free Software Foundation.
 */

#ifndef IS_CIS_2LD_H
#define IS_CIS_2LD_H

#include "is-cis.h"

#define EXT_CLK_Mhz (26)

#define SENSOR_2LD_MAX_WIDTH		(4032 + 0)
#define SENSOR_2LD_MAX_HEIGHT		(3024 + 0)

#define SENSOR_2LD_FINE_INTEGRATION_TIME_MIN                0x100
#define SENSOR_2LD_FINE_INTEGRATION_TIME_MAX                0x100
#define SENSOR_2LD_COARSE_INTEGRATION_TIME_MIN              0x6
#define SENSOR_2LD_COARSE_INTEGRATION_TIME_MAX_MARGIN       0x24

#define AEB_2LD_LUT0	0x0E10
#define AEB_2LD_LUT1	0x0E1C
#define AEB_2LD_LUT2	0x0E28
#define AEB_2LD_LUT3	0x0E34

#define AEB_2LD_OFFSET_CIT		0x0
#define AEB_2LD_OFFSET_AGAIN	0x2
#define AEB_2LD_OFFSET_LCIT		0x4
#define AEB_2LD_OFFSET_DGAIN	0x6
#define AEB_2LD_OFFSET_LDGAIN	0x8
#define AEB_2LD_OFFSET_FLL		0xA

#define USE_GROUP_PARAM_HOLD	(0)

enum sensor_2ld_mode_enum {
	/* MODE 3 */
	SENSOR_2LD_4032X3024_30FPS = 0,
	SENSOR_2LD_4032X3024_60FPS = 1,
	SENSOR_2LD_4032X2268_60FPS = 2,
	SENSOR_2LD_4032X2268_30FPS = 3,
	/* MODE 2 */
	SENSOR_2LD_2016X1512_30FPS = 4,
	SENSOR_2LD_2016X1134_30FPS = 5,
	/* MODE 3 - 24fps LIVE FOCUS */
	SENSOR_2LD_4032X3024_24FPS = 6,
	SENSOR_2LD_4032X2268_24FPS = 7,
	/* MODE 3 - SM */
	SENSOR_2LD_2016X1134_480FPS = 8,
	SENSOR_2LD_2016X1134_240FPS = 9,
	/* MODE 2 */
	SENSOR_2LD_1008X756_120FPS_MODE2 = 10,
	/* MODE 2 SSM */
	SENSOR_2LD_2016X1134_60FPS_MODE2_SSM_960 = 11,
	SENSOR_2LD_2016X1134_60FPS_MODE2_SSM_480 = 12,
	SENSOR_2LD_1280X720_60FPS_MODE2_SSM_960 = 13,
	SENSOR_2LD_1280X720_60FPS_MODE2_SSM_960_SDC_OFF = 14,
	/* MODE 3 PRO VIDEO */
	SENSOR_2LD_4032X2268_120FPS = 15,
	SENSOR_2LD_3328X1872_120FPS = 16,
	SENSOR_2LD_MODE_MAX,
};

static bool sensor_2ld_support_wdr[] = {
	/* MODE 3 */
	true, //SENSOR_2LD_4032X3024_30FPS = 0,
	true, //SENSOR_2LD_4032X3024_60FPS = 1,
	true, //SENSOR_2LD_4032X2268_60FPS = 2,
	true, //SENSOR_2LD_4032X2268_30FPS = 3,
	false, //SENSOR_2LD_2016X1512_30FPS = 4,
	false, //SENSOR_2LD_2016X1134_30FPS = 5,
	/* MODE 3 - 24fps LIVE FOCUS */
	true, //SENSOR_2LD_4032X3024_24FPS = 6,
	true, //SENSOR_2LD_4032X2268_24FPS = 7,
	/* MODE 3 - SM */
	false, //SENSOR_2LD_2016X1134_480FPS = 8,
	false, //SENSOR_2LD_2016X1134_240FPS = 9,
	/* MODE 2 */
	false, //SENSOR_2LD_1008X756_120FPS_MODE2 = 10,
	/* MODE 2 SSM */
	false, //SENSOR_2LD_2016X1134_60FPS_MODE2_SSM_960 = 11,
	false, //SENSOR_2LD_2016X1134_60FPS_MODE2_SSM_480 = 12,
	false, //SENSOR_2LD_1280X720_60FPS_MODE2_SSM_960 = 13,
	false, //SENSOR_2LD_1280X720_60FPS_MODE2_SSM_960_SDC_OFF = 14,
	/* MODE 3 PRO VIDEO */
	true, //SENSOR_2LD_4032X2268_120FPS = 15,
	true, //SENSOR_2LD_3328X1872_120FPS = 16,
};

static bool sensor_2ld_support_aeb[] = {
	/* MODE 3 */
	true, //SENSOR_2LD_4032X3024_30FPS = 0,
	false, //SENSOR_2LD_4032X3024_60FPS = 1,
	false, //SENSOR_2LD_4032X2268_60FPS = 2,
	true, //SENSOR_2LD_4032X2268_30FPS = 3,
	false, //SENSOR_2LD_2016X1512_30FPS = 4,
	false, //SENSOR_2LD_2016X1134_30FPS = 5,
	/* MODE 3 - 24fps LIVE FOCUS */
	false, //SENSOR_2LD_4032X3024_24FPS = 6,
	false, //SENSOR_2LD_4032X2268_24FPS = 7,
	/* MODE 3 - SM */
	false, //SENSOR_2LD_2016X1134_480FPS = 8,
	false, //SENSOR_2LD_2016X1134_240FPS = 9,
	/* MODE 2 */
	false, //SENSOR_2LD_1008X756_120FPS_MODE2 = 10,
	/* MODE 2 SSM */
	false, //SENSOR_2LD_2016X1134_60FPS_MODE2_SSM_960 = 11,
	false, //SENSOR_2LD_2016X1134_60FPS_MODE2_SSM_480 = 12,
	false, //SENSOR_2LD_1280X720_60FPS_MODE2_SSM_960 = 13,
	false, //SENSOR_2LD_1280X720_60FPS_MODE2_SSM_960_SDC_OFF = 14,
	/* MODEE 3 PRO VIDEO */
	false, //SENSOR_2LD_4032X2268_120FPS = 15,
	false, //SENSOR_2LD_3328X1872_120FPS = 16,
};

enum sensor_2ld_load_sram_mode {
	SENSOR_2LD_4032x3024_30FPS_LOAD_SRAM = 0,
	SENSOR_2LD_4032x2268_30FPS_LOAD_SRAM,
	SENSOR_2LD_4032x3024_24FPS_LOAD_SRAM,
	SENSOR_2LD_4032x2268_24FPS_LOAD_SRAM,
	SENSOR_2LD_4032x2268_60FPS_LOAD_SRAM,
	SENSOR_2LD_1008x756_120FPS_LOAD_SRAM,
};

static const u32 sensor_2ld_cis_LTE_settings_1[] = {
	0x6000, 0x0005, 0x02,
	0xFCFC, 0x2000, 0x02,
	0x1AA0, 0x0001, 0x02,
	0x0E7C, 0x4000, 0x02,
	0xFCFC, 0x2001, 0x02,
	0xBCA0, 0x0001, 0x02,
};

static const u32 sensor_2ld_cis_LTE_settings_1_size =
	ARRAY_SIZE(sensor_2ld_cis_LTE_settings_1);

static const u32 sensor_2ld_cis_LTE_settings_2[] = {
	0xBCA8, 0x0000, 0x02,
	0xBCAA, 0x0100, 0x02,
	0xBCAC, 0x0001, 0x02,
	0xBCAE, 0x0000, 0x02,
	0xBCB8, 0x0146, 0x02,
	0xBCB0, 0x0000, 0x02,
	0xBCB2, 0x0004, 0x02,
	0xBCB4, 0x0000, 0x02,
	0xBCB6, 0x0004, 0x02,
	0xFCFC, 0x4000, 0x02,
	0x0E0A, 0x0002, 0x02,
	0x0E0C, 0x0100, 0x02,
	0x0E0E, 0x0427, 0x02,
	0x0E10, 0x0000, 0x02,
	0x0E12, 0x0020, 0x02,
	0x0E14, 0x0000, 0x02,
	0x0E16, 0x0100, 0x02,
	0x0E18, 0x17AA, 0x02,
	0x6000, 0x0085, 0x02,
};

static const u32 sensor_2ld_cis_LTE_settings_2_size =
	ARRAY_SIZE(sensor_2ld_cis_LTE_settings_2);

static const u32 sensor_2ld_cis_LTE_settings_3[] = {
	0x6000, 0x0005, 0x02,
	0xFCFC, 0x4000, 0x02,
	0x6214, 0x7971, 0x02,
	0x6218, 0x7150, 0x02,
	0xFCFC, 0x2000, 0x02,
	0x1AA0, 0x0101, 0x02,
	0x0E7C, 0x0000, 0x02,
	0xFCFC, 0x2001, 0x02,
	0xBC96, 0x0100, 0x02,
	0xBCA0, 0x0000, 0x02,
	0xBCA2, 0x0000, 0x02,
	0xBCA4, 0x009E, 0x02,
	0xBCB8, 0x0046, 0x02,
	0xFCFC, 0x4000, 0x02,
	0xF460, 0x03FF, 0x02,
	0x6242, 0x0320, 0x02,
	0x0E0E, 0x0000, 0x02,
	0x0E0A, 0x0000, 0x02,
	0x0E0C, 0x0000, 0x02,
	0x6000, 0x0085, 0x02,
};

static const u32 sensor_2ld_cis_LTE_settings_3_size =
	ARRAY_SIZE(sensor_2ld_cis_LTE_settings_3);

static const u32 sensor_2ld_cis_night_settings_enable[] = {
	0xFCFC, 0x4000, 0x02,
	0x6000, 0x0005, 0x02,
	0xFCFC, 0x2000, 0x02,
	0x1AA0, 0x0001, 0x02,
	0xFCFC, 0x2001, 0x02,
	0xBCA0, 0x0000, 0x02,
	0xBCA8, 0x0000, 0x02,
	0xBCAA, 0x0100, 0x02,
	0xBCAC, 0x0001, 0x02,
	0xBCAE, 0x0000, 0x02,
	0xBCB8, 0x0146, 0x02,
	0xBCB0, 0x0000, 0x02,
	0xBCB2, 0x0004, 0x02,
	0xBCB4, 0x0000, 0x02,
	0xBCB6, 0x0004, 0x02,
	0xFCFC, 0x4000, 0x02,

	0xF460, 0x03FC, 0x02,
	0x6000, 0x0085, 0x02,
};

static const u32 sensor_2ld_cis_night_settings_enable_size =
	ARRAY_SIZE(sensor_2ld_cis_night_settings_enable);

static const u32 sensor_2ld_cis_night_settings_disable[] = {
	0xFCFC, 0x4000, 0x02,
	0x6000, 0x0005, 0x02,
	0x6242, 0x0320, 0x02,

	0xFCFC, 0x2000, 0x02,
	0x1AA0, 0x0101, 0x02,
	0x0E7C, 0x0000, 0x02,
	0xFCFC, 0x2001, 0x02,
	0xBCB8, 0x0046, 0x02,
	0xBCA0, 0x0000, 0x02,
	0xBCA2, 0x0000, 0x02,
	0xBCA4, 0x009E, 0x02,
	0xFCFC, 0x4000, 0x02,
	0xF460, 0x03FF, 0x02,
	0xF03C, 0xFFFF, 0x02,
	0xF03E, 0xFFFF, 0x02,
	0x6242, 0x0320, 0x02,
	0xFCFC, 0x2000, 0x02,
	0xD083, 0x0000, 0x02,
	0xD98B, 0x0000, 0x02,
	0xFCFC, 0x4000, 0x02,
	0x6000, 0x0085, 0x02,
};

static int sensor_2ld_cis_group_param_hold_func(struct v4l2_subdev *subdev, unsigned int hold);
static const u32 sensor_2ld_cis_night_settings_disable_size =
	ARRAY_SIZE(sensor_2ld_cis_night_settings_disable);

int sensor_2ld_cis_stream_on(struct v4l2_subdev *subdev);
int sensor_2ld_cis_stream_off(struct v4l2_subdev *subdev);
#ifdef USE_CAMERA_SENSOR_RETENTION
int sensor_2ld_cis_retention_crc_check(struct v4l2_subdev *subdev);
int sensor_2ld_cis_retention_prepare(struct v4l2_subdev *subdev);
#endif
int sensor_2ld_cis_set_lownoise_mode_change(struct v4l2_subdev *subdev);
#endif
