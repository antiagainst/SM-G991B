/*
 * Samsung Exynos SoC series Sensor driver
 *
 *
 * Copyright (c) 2020 Samsung Electronics Co., Ltd
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 as
 * published by the Free Software Foundation.
 */

#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/init.h>

#include <linux/fs.h>
#include <linux/miscdevice.h>
#include <linux/mutex.h>
#include <asm/uaccess.h>

#include "is-config.h"
#include "is-vender-caminfo.h"
#include "is-vender-specific.h"
#include "is-sec-define.h"
#include "is-device-sensor-peri.h"
#include "is-sysfs.h"

static int is_vender_caminfo_open(struct inode *inode, struct file *file)
{
	is_vender_caminfo *p_vender_caminfo;

	p_vender_caminfo = vzalloc(sizeof(is_vender_caminfo));
	if(p_vender_caminfo == NULL) {
		err("failed to allocate memory for is_vender_caminfo");
		return -ENOMEM;
	}

	mutex_init(&p_vender_caminfo->mlock);

	file->private_data = p_vender_caminfo;

	return 0;
}

static int is_vender_caminfo_release(struct inode *inode, struct file *file)
{
	is_vender_caminfo *p_vender_caminfo = (is_vender_caminfo *)file->private_data;

	if (p_vender_caminfo)
		vfree(p_vender_caminfo);

	return 0;
}

static int is_vender_caminfo_cmd_get_factory_supported_id(void __user *user_data)
{
	int i;
	caminfo_supported_list supported_list;
	struct is_common_cam_info *common_camera_infos;

	is_get_common_cam_info(&common_camera_infos);
	if (!common_camera_infos) {
		err("common_camera_infos is NULL");
		return -EFAULT;
	}

	supported_list.size = common_camera_infos->max_supported_camera;

	for (i = 0; i < supported_list.size; i++) {
		supported_list.data[i] = common_camera_infos->supported_camera_ids[i];
	}

	if (copy_to_user(user_data, (void *)&supported_list, sizeof(caminfo_supported_list))) {
		err("%s : failed to copy data to user", __func__);
		return -EINVAL;
	}

	return 0;
}

static int is_vender_caminfo_cmd_get_rom_data_by_position(void __user *user_data)
{
	int ret = 0;
	caminfo_romdata romdata;
	int rom_id;
	struct is_rom_info *finfo;
	char *cal_buf;

	if (copy_from_user((void *)&romdata, user_data, sizeof(caminfo_romdata))) {
		err("%s : failed to copy data from user", __func__);
		ret = -EINVAL;
		goto EXIT;
	}

	rom_id = is_vendor_get_rom_id_from_position(romdata.cam_position);

	if(rom_id == ROM_ID_NOTHING) {
		err("%s : invalid camera position (%d)", __func__, romdata.cam_position);
		ret = -EINVAL;
		goto EXIT;
	}

	is_sec_get_cal_buf(&cal_buf, rom_id);
	is_sec_get_sysfs_finfo(&finfo, rom_id);

	romdata.rom_size = finfo->rom_size;

	if (copy_to_user(user_data, &romdata, sizeof(caminfo_romdata))) {
		err("%s : failed to copy data to user", __func__);
		ret = -EINVAL;
		goto EXIT;
	}

	if (romdata.buf_size >= sizeof(uint8_t) * finfo->rom_size) {
		if (copy_to_user(romdata.buf, cal_buf, sizeof(uint8_t) * finfo->rom_size)) {
			err("%s : failed to copy data to user", __func__);
			ret = -EINVAL;
			goto EXIT;
		}
	} else {
		err("%s : wrong buf size : buf size must be bigger than cal buf size", __func__);
		ret = -EINVAL;
	}

EXIT:
	return ret;
}

static int is_vender_caminfo_set_efs_data(void __user *p_efsdata_user)
{
	int ret = 0;
	struct is_core *core = NULL;
	struct is_vender_specific *specific;
	caminfo_efs_data efs_data;

	core = (struct is_core *)dev_get_drvdata(is_dev);
	specific = core->vender.private_data;

	if (copy_from_user((void *)&efs_data, p_efsdata_user, sizeof(caminfo_efs_data))) {
		err("%s : failed to copy data from user", __func__);
		ret = -EINVAL;
		goto EXIT;
	}

	if (efs_data.tilt_cal_tele_efs_size <= IS_TILT_CAL_TELE_EFS_MAX_SIZE) {
		if (copy_from_user(specific->tilt_cal_tele_efs_data, efs_data.tilt_cal_tele_efs_buf, sizeof(uint8_t) * efs_data.tilt_cal_tele_efs_size)) {
			err("%s : failed to copy data from user", __func__);
			ret = -EINVAL;
			goto EXIT;
		}

		specific->tilt_cal_tele_efs_size = efs_data.tilt_cal_tele_efs_size;
	} else {
		err("wrong tilt cal tele data size : data size must be smaller than max size.(%d)", efs_data.tilt_cal_tele_efs_size);
		ret = -EFAULT;
	}

	if (efs_data.gyro_efs_size <= IS_GYRO_EFS_MAX_SIZE) {
		if (copy_from_user(specific->gyro_efs_data, efs_data.gyro_efs_buf, sizeof(uint8_t) * efs_data.gyro_efs_size)) {
			err("%s : failed to copy data from user", __func__);
			ret = -EINVAL;
			goto EXIT;
		}
		specific->gyro_efs_size = efs_data.gyro_efs_size;
	} else {
		err("wrong gyro data size : data size must be smaller than max size.(%d)", efs_data.gyro_efs_size);
		ret = -EFAULT;
	}

EXIT:
	return ret;
}

static int is_vender_caminfo_cmd_get_sensorid_by_cameraid(void __user *user_data)
{
	int ret = 0;
	struct is_core *core = NULL;
	struct is_vender_specific *specific;
	caminfo_sensor_id sensor;

	if (copy_from_user((void *)&sensor, user_data, sizeof(caminfo_sensor_id))) {
		err("%s : failed to copy data from user", __func__);
		ret = -EINVAL;
		goto EXIT;
	}

	core = (struct is_core *)dev_get_drvdata(is_dev);
	specific = core->vender.private_data;

	switch (sensor.cameraId) {
		case SENSOR_POSITION_REAR:
			sensor.sensorId = specific->rear_sensor_id;
		break;
		case SENSOR_POSITION_FRONT:
			sensor.sensorId = specific->front_sensor_id;
		break;
		case SENSOR_POSITION_REAR2:
			sensor.sensorId = specific->rear2_sensor_id;
		break;
		case SENSOR_POSITION_FRONT2:
			sensor.sensorId = specific->front2_sensor_id;
		break;
		case SENSOR_POSITION_REAR3:
			sensor.sensorId = specific->rear3_sensor_id;
		break;
		case SENSOR_POSITION_REAR4:
			sensor.sensorId = specific->rear4_sensor_id;
		break;
		case SENSOR_POSITION_REAR_TOF:
			sensor.sensorId = specific->rear_tof_sensor_id;
		break;
		case SENSOR_POSITION_FRONT_TOF:
			sensor.sensorId = specific->front_tof_sensor_id;
		break;
#ifdef CONFIG_SECURE_CAMERA_USE
		case SENSOR_POSITION_SECURE:
			sensor.sensorId = specific->secure_sensor_id;
		break;
#endif
		case SENSOR_POSITION_VIRTUAL:
			sensor.sensorId = SENSOR_NAME_VIRTUAL;
		break;   //Logical Camera
		default:
			sensor.sensorId = -1;
		break;
	}

	if (copy_to_user(user_data, &sensor, sizeof(caminfo_sensor_id))) {
		err("%s : failed to copy data to user", __func__);
		ret = -EINVAL;
	}

EXIT:
	return ret;
}

static int is_vender_caminfo_cmd_get_awb_data_addr(void __user *user_data)
{
	int ret = 0;
	struct is_rom_info *finfo;
	int position;
	int rom_type;
	int rom_id;
	int rom_cal_index;
	caminfo_awb_data_addr awb_data_addr;

	if (copy_from_user((void *)&awb_data_addr, user_data, sizeof(caminfo_awb_data_addr))) {
		err("%s : failed to copy data from user", __func__);
		ret = -EINVAL;
		goto EXIT_ERR_AWB;
	}

	position = awb_data_addr.cameraId;
	is_vendor_get_rom_info_from_position(position, &rom_type, &rom_id, &rom_cal_index);

	if (rom_type == ROM_TYPE_NONE) {
		err("%s: not support, no rom for camera[%d]", __func__, position);
		goto EXIT_ERR_AWB;
	} else if (rom_id == ROM_ID_NOTHING) {
		err("%s: invalid ROM ID [%d][%d]", __func__, position, rom_id);
		goto EXIT_ERR_AWB;
	}

	read_from_firmware_version(rom_id);
	is_sec_get_sysfs_finfo(&finfo, rom_id);

	if (rom_cal_index == 1) {
		awb_data_addr.awb_master_addr = finfo->rom_sensor2_awb_master_addr;
	} else {
		awb_data_addr.awb_master_addr = finfo->rom_awb_master_addr;
	}

	awb_data_addr.awb_master_data_size = IS_AWB_MASTER_DATA_SIZE;

	if (rom_cal_index == 1) {
		awb_data_addr.awb_module_addr = finfo->rom_sensor2_awb_module_addr;
	} else {
		awb_data_addr.awb_module_addr = finfo->rom_awb_module_addr;
	}

	awb_data_addr.awb_module_data_size = IS_AWB_MODULE_DATA_SIZE;

	goto EXIT;

EXIT_ERR_AWB:
	awb_data_addr.awb_master_addr = -1;
	awb_data_addr.awb_master_data_size = IS_AWB_MASTER_DATA_SIZE;
	awb_data_addr.awb_module_addr = -1;
	awb_data_addr.awb_module_data_size = IS_AWB_MODULE_DATA_SIZE;

EXIT:
	if (copy_to_user(user_data, &awb_data_addr, sizeof(caminfo_awb_data_addr))) {
		err("%s : failed to copy data to user", __func__);
		ret = -EINVAL;
	}

	return ret;
}

static long is_vender_caminfo_ioctl(struct file *file, unsigned int cmd, unsigned long arg)
{
	int ret = 0;
	is_vender_caminfo *p_vender_caminfo;
	caminfo_ioctl_cmd ioctl_cmd;

	BUG_ON(!file->private_data);
	if (!file->private_data) {
		return -EFAULT;
	}

	p_vender_caminfo = (is_vender_caminfo *)file->private_data;

	mutex_lock(&p_vender_caminfo->mlock);

	if (cmd != IS_CAMINFO_IOCTL_COMMAND) {
		err("%s : not support cmd:%x, arg:%x", __func__, cmd, arg);
		ret = -EINVAL;
		goto EXIT;
	}

	if (copy_from_user((void *)&ioctl_cmd, (const void *)arg, sizeof(caminfo_ioctl_cmd))) {
		err("%s : failed to copy data from user", __func__);
		ret = -EINVAL;
		goto EXIT;
	}

	switch (ioctl_cmd.cmd) {
	case CAMINFO_CMD_ID_GET_FACTORY_SUPPORTED_ID:
		ret = is_vender_caminfo_cmd_get_factory_supported_id(ioctl_cmd.data);
		break;
	case CAMINFO_CMD_ID_GET_ROM_DATA_BY_POSITION:
		ret = is_vender_caminfo_cmd_get_rom_data_by_position(ioctl_cmd.data);
		break;
	case CAMINFO_CMD_ID_SET_EFS_DATA:
		ret = is_vender_caminfo_set_efs_data(ioctl_cmd.data);
		break;
	case CAMINFO_CMD_ID_GET_SENSOR_ID:
		ret = is_vender_caminfo_cmd_get_sensorid_by_cameraid(ioctl_cmd.data);
		break;
	case CAMINFO_CMD_ID_GET_AWB_DATA_ADDR:
		ret = is_vender_caminfo_cmd_get_awb_data_addr(ioctl_cmd.data);
		break;
	default:
		err("%s : not support cmd number:%u, arg:%x", __func__, ioctl_cmd.cmd, arg);
		ret = -EINVAL;
		break;
	}

EXIT:
	mutex_unlock(&p_vender_caminfo->mlock);

	return ret;
}

static struct file_operations is_vender_caminfo_fops =
{
	.owner = THIS_MODULE,
	.open = is_vender_caminfo_open,
	.release = is_vender_caminfo_release,
	.unlocked_ioctl = is_vender_caminfo_ioctl,
};

struct miscdevice is_vender_caminfo_driver =
{
	.minor = MISC_DYNAMIC_MINOR,
	.name = "caminfo",
	.fops = &is_vender_caminfo_fops,
};

#ifndef MODULE
static int is_vender_caminfo_init(void)
{
	info("%s\n", __func__);

	return misc_register(&is_vender_caminfo_driver);
}

static void is_vender_caminfo_exit(void)
{
	info("%s\n", __func__);

	misc_deregister(&is_vender_caminfo_driver);

}

module_init(is_vender_caminfo_init);
module_exit(is_vender_caminfo_exit);
#endif

MODULE_DESCRIPTION("Exynos Caminfo driver");
MODULE_LICENSE("GPL v2");

