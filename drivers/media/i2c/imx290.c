// SPDX-License-Identifier: GPL-2.0
/*
 * imx290 driver
 *
 * Copyright (C) 2020 Rockchip Electronics Co., Ltd.
 * V0.0X01.0X03 add enum_frame_interval function.
 * V0.0X01.0X04 support lvds interface.
 * V0.0X01.0X05 add quick stream on/off
 * V0.0X01.0X06 fixed linear mode exp calc
 */

#include <linux/clk.h>
#include <linux/device.h>
#include <linux/delay.h>
#include <linux/gpio/consumer.h>
#include <linux/i2c.h>
#include <linux/module.h>
#include <linux/pm_runtime.h>
#include <linux/regulator/consumer.h>
#include <linux/sysfs.h>
#include <linux/slab.h>
#include <linux/version.h>
#include <linux/rk-camera-module.h>
#include <linux/of_graph.h>
#include <media/media-entity.h>
#include <media/v4l2-async.h>
#include <media/v4l2-ctrls.h>
#include <media/v4l2-subdev.h>
#include <media/v4l2-fwnode.h>
#include <media/v4l2-mediabus.h>
#include <linux/pinctrl/consumer.h>
#include <linux/rk-preisp.h>

#define DRIVER_VERSION			KERNEL_VERSION(0, 0x01, 0x06)
#ifndef V4L2_CID_DIGITAL_GAIN
#define V4L2_CID_DIGITAL_GAIN		V4L2_CID_GAIN
#endif

#define IMX290_LINK_FREQ		222750000

/* pixel rate = link_freq * 2 * nr_of_lanes / bits_per_sample */
#define IMX290_PIXEL_RATE 		(IMX290_LINK_FREQ * 2 * 2 / 12)

#define IMX290_XVCLK_FREQ		37125000

#define CHIP_ID					0xb2
#define IMX290_REG_CHIP_ID		0x301e

#define IMX290_REG_CTRL_MODE	0x3000
#define IMX290_MODE_SW_STANDBY	0x1
#define IMX290_MODE_STREAMING	0x0

#define IMX290_REG_SHS1_H		0x3022
#define IMX290_REG_SHS1_M		0x3021
#define IMX290_REG_SHS1_L		0x3020

#define IMX290_FETCH_HIGH_BYTE_EXP(VAL)	(((VAL) >> 16) & 0x0F)
#define IMX290_FETCH_MID_BYTE_EXP(VAL)	(((VAL) >> 8) & 0xFF)
#define IMX290_FETCH_LOW_BYTE_EXP(VAL)	((VAL) & 0xFF)

#define	IMX290_EXPOSURE_MIN		2
#define	IMX290_EXPOSURE_STEP	1
#define IMX290_VTS_MAX			0x7fff

#define IMX290_GAIN_SWITCH_REG	0x3009
#define IMX290_REG_LF_GAIN		0x3014
#define IMX290_GAIN_MIN			0x00
#define IMX290_GAIN_MAX			0x64
#define IMX290_GAIN_STEP		1
#define IMX290_GAIN_DEFAULT		0x00

#define IMX290_GROUP_HOLD_REG		0x3001
#define IMX290_GROUP_HOLD_START		0x01
#define IMX290_GROUP_HOLD_END		0x00

#define USED_TEST_PATTERN
#ifdef USED_TEST_PATTERN
#define IMX290_REG_TEST_PATTERN		0x308c
#define	IMX290_TEST_PATTERN_ENABLE	BIT(0)
#endif

#define IMX290_REG_VTS_H		0x301a
#define IMX290_REG_VTS_M		0x3019
#define IMX290_REG_VTS_L		0x3018
#define IMX290_FETCH_HIGH_BYTE_VTS(VAL)	(((VAL) >> 16) & 0x03)
#define IMX290_FETCH_MID_BYTE_VTS(VAL)	(((VAL) >> 8) & 0xFF)
#define IMX290_FETCH_LOW_BYTE_VTS(VAL)	((VAL) & 0xFF)

#define REG_NULL					0xFFFF

#define IMX290_REG_VALUE_08BIT		1
#define IMX290_REG_VALUE_16BIT		2
#define IMX290_REG_VALUE_24BIT		3

static bool g_isHCG;

#define IMX290_NAME			"imx290"

#define OF_CAMERA_PINCTRL_STATE_DEFAULT	"rockchip,camera_default"
#define OF_CAMERA_PINCTRL_STATE_SLEEP	"rockchip,camera_sleep"

#define IMX290_FLIP_REG			0x3007
#define MIRROR_BIT_MASK			BIT(1)
#define FLIP_BIT_MASK			BIT(0)

static const char * const imx290_supply_names[] = {
	"avdd",		/* Analog power */
	"dovdd",	/* Digital I/O power */
	"dvdd",		/* Digital core power */
};

#define IMX290_NUM_SUPPLIES ARRAY_SIZE(imx290_supply_names)

struct regval {
	u16 addr;
	u8 val;
};

struct imx290_mode {
	u32 bus_fmt;
	u32 width;
	u32 height;
	struct v4l2_fract max_fps;
	u32 hts_def;
	u32 vts_def;
	u32 exp_def;
	const struct regval *reg_list;
};

struct imx290 {
	struct i2c_client	*client;
	struct clk		*xvclk;
	struct gpio_desc	*reset_gpio;
	struct gpio_desc	*pwdn_gpio;
	struct regulator_bulk_data supplies[IMX290_NUM_SUPPLIES];

	struct pinctrl		*pinctrl;
	struct pinctrl_state	*pins_default;
	struct pinctrl_state	*pins_sleep;

	struct v4l2_subdev	subdev;
	struct media_pad	pad;
	struct v4l2_ctrl_handler ctrl_handler;
	struct v4l2_ctrl	*exposure;
	struct v4l2_ctrl	*anal_gain;
	struct v4l2_ctrl	*digi_gain;
	struct v4l2_ctrl	*hblank;
	struct v4l2_ctrl	*vblank;
	struct v4l2_ctrl	*pixel_rate;
	struct v4l2_ctrl	*link_freq;
	struct v4l2_ctrl	*h_flip;
	struct v4l2_ctrl	*v_flip;
#ifdef USED_TEST_PATTERN
	struct v4l2_ctrl	*test_pattern;
#endif
	struct mutex		mutex;
	bool			streaming;
	bool			power_on;
	const struct imx290_mode *support_modes;
	u32			support_modes_num;
	const struct imx290_mode *cur_mode;
	u32			module_index;
	const char		*module_facing;
	const char		*module_name;
	const char		*len_name;
	u32			cur_vts;
	bool			has_init_exp;
	struct v4l2_fwnode_endpoint bus_cfg;
	u8			flip;
};

#define to_imx290(sd) container_of(sd, struct imx290, subdev)

static const struct regval imx290_global_regs[] = {
	{REG_NULL, 0x00},
};

/*
 * Xclk 37.125Mhz
 * max_framerate 30fps
 * 12 bit, 30fps
 */
static const struct regval imx290_linear_1920x1080_mipi_regs[] = {
	{ 0x301a, 0x00 },
	{ 0x303a, 0x0c },
	{ 0x3040, 0x00 },
	{ 0x3041, 0x00 },
	{ 0x303c, 0x00 },
	{ 0x303d, 0x00 },
	{ 0x3042, 0x9c },
	{ 0x3043, 0x07 },
	{ 0x303e, 0x49 },
	{ 0x303f, 0x04 },
	{ 0x304b, 0x0a },
	{ 0x300f, 0x00 },
	{ 0x3010, 0x21 },
	{ 0x3012, 0x64 },
	{ 0x3016, 0x09 },
	{ 0x3070, 0x02 },
	{ 0x3071, 0x11 },
	{ 0x309b, 0x10 },
	{ 0x309c, 0x22 },
	{ 0x30a2, 0x02 },
	{ 0x30a6, 0x20 },
	{ 0x30a8, 0x20 },
	{ 0x30aa, 0x20 },
	{ 0x30ac, 0x20 },
	{ 0x30b0, 0x43 },
	{ 0x3119, 0x9e },
	{ 0x311c, 0x1e },
	{ 0x311e, 0x08 },
	{ 0x3128, 0x05 },
	{ 0x313d, 0x83 },
	{ 0x3150, 0x03 },
	{ 0x317e, 0x00 },
	{ 0x32b8, 0x50 },
	{ 0x32b9, 0x10 },
	{ 0x32ba, 0x00 },
	{ 0x32bb, 0x04 },
	{ 0x32c8, 0x50 },
	{ 0x32c9, 0x10 },
	{ 0x32ca, 0x00 },
	{ 0x32cb, 0x04 },
	{ 0x332c, 0xd3 },
	{ 0x332d, 0x10 },
	{ 0x332e, 0x0d },
	{ 0x3358, 0x06 },
	{ 0x3359, 0xe1 },
	{ 0x335a, 0x11 },
	{ 0x3360, 0x1e },
	{ 0x3361, 0x61 },
	{ 0x3362, 0x10 },
	{ 0x33b0, 0x50 },
	{ 0x33b2, 0x1a },
	{ 0x33b3, 0x04 },

	{ 0x301c, 0x30 },
	{ 0x301d, 0x11 },
	{ 0x305c, 0x18 },
	{ 0x305d, 0x03 },
	{ 0x305e, 0x20 },
	{ 0x305f, 0x01 },
	{ 0x315e, 0x1a },
	{ 0x3164, 0x1a },
	{ 0x3444, 0x20 },
	{ 0x3445, 0x25 },
	{ 0x3480, 0x49 },

	{ 0x3009, 0x02 },
	{ 0x303a, 0x0c },
	{ 0x3414, 0x0a },
	{ 0x3472, 0x9c },
	{ 0x3473, 0x07 },
	{ 0x3418, 0x49 },
	{ 0x3419, 0x04 },
	{ 0x3012, 0x64 },
	{ 0x3013, 0x00 },

	{ 0x3405, 0x10 },
	{ 0x3407, 0x01 },
	{ 0x3443, 0x01 },
	{ 0x3446, 0x57 },
	{ 0x3447, 0x00 },
	{ 0x3448, 0x37 },
	{ 0x3449, 0x00 },
	{ 0x344a, 0x1f },
	{ 0x344b, 0x00 },
	{ 0x344c, 0x1f },
	{ 0x344d, 0x00 },
	{ 0x344e, 0x1f },
	{ 0x344f, 0x00 },
	{ 0x3450, 0x77 },
	{ 0x3451, 0x00 },
	{ 0x3452, 0x1f },
	{ 0x3453, 0x00 },
	{ 0x3454, 0x17 },
	{ 0x3455, 0x00 },

	{ 0x3005, 0x01 },
	{ 0x3046, 0x01 },
	{ 0x3129, 0x00 },
	{ 0x317c, 0x00 },
	{ 0x31ec, 0x0e },
	{ 0x3441, 0x0c },
	{ 0x3442, 0x0c },
	{ 0x300a, 0xf0 },
	{ 0x300b, 0x00 },

	{REG_NULL, 0x00},
};

static const struct imx290_mode mipi_supported_modes[] = {
	{
		.bus_fmt = MEDIA_BUS_FMT_SRGGB12_1X12,
		.width = 1948,
		.height = 1097,
		.max_fps = {
			.numerator = 10000,
			.denominator = 300000,
		},
		.exp_def = 0x03fe,
		.hts_def = 0x1130,
		.vts_def = 0x0465,
		.reg_list = imx290_linear_1920x1080_mipi_regs,
	}
};

static const s64 link_freq_menu_items[] = {
	IMX290_LINK_FREQ
};

#ifdef USED_TEST_PATTERN
static const char * const imx290_test_pattern_menu[] = {
	"Disabled",
	"Bar Type 1",
	"Bar Type 2",
	"Bar Type 3",
	"Bar Type 4",
	"Bar Type 5",
	"Bar Type 6",
	"Bar Type 7",
	"Bar Type 8",
	"Bar Type 9",
	"Bar Type 10",
	"Bar Type 11",
	"Bar Type 12",
	"Bar Type 13",
	"Bar Type 14",
	"Bar Type 15"
};
#endif

/* Write registers up to 4 at a time */
static int imx290_write_reg(struct i2c_client *client, u16 reg,
			    u32 len, u32 val)
{
	u32 buf_i, val_i;
	u8 buf[6];
	u8 *val_p;
	__be32 val_be;

	if (len > 4)
		return -EINVAL;

	buf[0] = reg >> 8;
	buf[1] = reg & 0xff;

	val_be = cpu_to_be32(val);
	val_p = (u8 *)&val_be;
	buf_i = 2;
	val_i = 4 - len;

	while (val_i < 4)
		buf[buf_i++] = val_p[val_i++];

	if (i2c_master_send(client, buf, len + 2) != len + 2)
		return -EIO;

	return 0;
}

static int imx290_write_array(struct i2c_client *client,
			      const struct regval *regs)
{
	u32 i;
	int ret = 0;

	for (i = 0; ret == 0 && regs[i].addr != REG_NULL; i++)
		ret = imx290_write_reg(client, regs[i].addr,
			IMX290_REG_VALUE_08BIT,
			regs[i].val);

	return ret;
}

/* Read registers up to 4 at a time */
static int imx290_read_reg(struct i2c_client *client, u16 reg,
			   unsigned int len, u32 *val)
{
	struct i2c_msg msgs[2];
	u8 *data_be_p;
	__be32 data_be = 0;
	__be16 reg_addr_be = cpu_to_be16(reg);
	int ret;

	if (len > 4 || !len)
		return -EINVAL;

	data_be_p = (u8 *)&data_be;
	/* Write register address */
	msgs[0].addr = client->addr;
	msgs[0].flags = 0;
	msgs[0].len = 2;
	msgs[0].buf = (u8 *)&reg_addr_be;

	/* Read data from register */
	msgs[1].addr = client->addr;
	msgs[1].flags = I2C_M_RD;
	msgs[1].len = len;
	msgs[1].buf = &data_be_p[4 - len];

	ret = i2c_transfer(client->adapter, msgs, ARRAY_SIZE(msgs));
	if (ret != ARRAY_SIZE(msgs))
		return -EIO;

	*val = be32_to_cpu(data_be);
	return 0;
}

static int imx290_set_fmt(struct v4l2_subdev *sd,
			  struct v4l2_subdev_state *sd_state,
			  struct v4l2_subdev_format *fmt)
{
	struct imx290 *imx290 = to_imx290(sd);
	const struct imx290_mode *mode;
	s64 h_blank, vblank_def;
	s32 dst_link_freq = 0;
	s64 dst_pixel_rate = 0;

	mutex_lock(&imx290->mutex);

	mode = v4l2_find_nearest_size(imx290->support_modes,
				      imx290->support_modes_num,
				      width, height,
				      fmt->format.width, fmt->format.height);
	fmt->format.code = mode->bus_fmt;
	fmt->format.width = mode->width;
	fmt->format.height = mode->height;
	fmt->format.field = V4L2_FIELD_NONE;
	if (fmt->which == V4L2_SUBDEV_FORMAT_TRY) {
#ifdef CONFIG_VIDEO_V4L2_SUBDEV_API
		*v4l2_subdev_get_try_format(sd, sd_state, fmt->pad) = fmt->format;
#else
		mutex_unlock(&imx290->mutex);
		return -ENOTTY;
#endif
	} else {
		imx290->cur_mode = mode;
		h_blank = mode->hts_def - mode->width;
		__v4l2_ctrl_modify_range(imx290->hblank, h_blank,
					 h_blank, 1, h_blank);
		vblank_def = mode->vts_def - mode->height;
		__v4l2_ctrl_modify_range(imx290->vblank, vblank_def,
					 IMX290_VTS_MAX - mode->height,
					 1, vblank_def);
		dst_link_freq = 0;
		dst_pixel_rate = IMX290_PIXEL_RATE;
		__v4l2_ctrl_s_ctrl_int64(imx290->pixel_rate,
					 dst_pixel_rate);
		__v4l2_ctrl_s_ctrl(imx290->link_freq,
				   dst_link_freq);
		imx290->cur_vts = mode->vts_def;
	}

	mutex_unlock(&imx290->mutex);

	return 0;
}

static int imx290_get_fmt(struct v4l2_subdev *sd,
			  struct v4l2_subdev_state *sd_state,
			  struct v4l2_subdev_format *fmt)
{
	struct imx290 *imx290 = to_imx290(sd);
	const struct imx290_mode *mode = imx290->cur_mode;

	mutex_lock(&imx290->mutex);
	if (fmt->which == V4L2_SUBDEV_FORMAT_TRY) {
#ifdef CONFIG_VIDEO_V4L2_SUBDEV_API
		fmt->format = *v4l2_subdev_get_try_format(sd, sd_state, fmt->pad);
#else
		mutex_unlock(&imx290->mutex);
		return -ENOTTY;
#endif
	} else {
		fmt->format.width = mode->width;
		fmt->format.height = mode->height;
		fmt->format.code = mode->bus_fmt;
		fmt->format.field = V4L2_FIELD_NONE;
	}
	mutex_unlock(&imx290->mutex);
	return 0;
}

static int imx290_enum_mbus_code(struct v4l2_subdev *sd,
				 struct v4l2_subdev_state *sd_state,
				 struct v4l2_subdev_mbus_code_enum *code)
{
	struct imx290 *imx290 = to_imx290(sd);
	const struct imx290_mode *mode = imx290->cur_mode;

	if (code->index != 0)
		return -EINVAL;
	code->code = mode->bus_fmt;

	return 0;
}

static int imx290_enum_frame_sizes(struct v4l2_subdev *sd,
				   struct v4l2_subdev_state *sd_state,
				   struct v4l2_subdev_frame_size_enum *fse)
{
	struct imx290 *imx290 = to_imx290(sd);

	if (fse->index >= imx290->support_modes_num)
		return -EINVAL;

	if (fse->code != imx290->support_modes[fse->index].bus_fmt)
		return -EINVAL;

	fse->min_width  = imx290->support_modes[fse->index].width;
	fse->max_width  = imx290->support_modes[fse->index].width;
	fse->max_height = imx290->support_modes[fse->index].height;
	fse->min_height = imx290->support_modes[fse->index].height;

	return 0;
}

#ifdef USED_TEST_PATTERN
static int imx290_enable_test_pattern(struct imx290 *imx290, u32 pattern)
{
	u32 val = 0;

	imx290_read_reg(imx290->client,
			IMX290_REG_TEST_PATTERN,
			IMX290_REG_VALUE_08BIT,
			&val);
	if (pattern) {
		val = ((pattern - 1) << 4) | IMX290_TEST_PATTERN_ENABLE;
		imx290_write_reg(imx290->client,
				 0x300a,
				 IMX290_REG_VALUE_08BIT,
				 0x00);
		imx290_write_reg(imx290->client,
				 0x300e,
				 IMX290_REG_VALUE_08BIT,
				 0x00);
	} else {
		val &= ~IMX290_TEST_PATTERN_ENABLE;
		imx290_write_reg(imx290->client,
				 0x300a,
				 IMX290_REG_VALUE_08BIT,
				 0x3c);
		imx290_write_reg(imx290->client,
				 0x300e,
				 IMX290_REG_VALUE_08BIT,
				 0x01);
	}
	return imx290_write_reg(imx290->client,
				IMX290_REG_TEST_PATTERN,
				IMX290_REG_VALUE_08BIT,
				val);
}
#endif

static int imx290_g_frame_interval(struct v4l2_subdev *sd,
				   struct v4l2_subdev_frame_interval *fi)
{
	struct imx290 *imx290 = to_imx290(sd);
	const struct imx290_mode *mode = imx290->cur_mode;

	fi->interval = mode->max_fps;

	return 0;
}

static int imx290_g_mbus_config(struct v4l2_subdev *sd, unsigned int pad_id,
				struct v4l2_mbus_config *config)
{
	struct imx290 *imx290 = to_imx290(sd);

	config->type = imx290->bus_cfg.bus_type;
	if (imx290->bus_cfg.bus_type == V4L2_MBUS_CCP2)
		config->bus.mipi_csi1 = imx290->bus_cfg.bus.mipi_csi1;
	else if (imx290->bus_cfg.bus_type == V4L2_MBUS_CSI2_DPHY)
		config->bus.mipi_csi2 = imx290->bus_cfg.bus.mipi_csi2;
	return 0;
}

static void imx290_get_module_inf(struct imx290 *imx290,
				  struct rkmodule_inf *inf)
{
	memset(inf, 0, sizeof(*inf));
	strlcpy(inf->base.sensor, IMX290_NAME, sizeof(inf->base.sensor));
	strlcpy(inf->base.module, imx290->module_name,
		sizeof(inf->base.module));
	strlcpy(inf->base.lens, imx290->len_name, sizeof(inf->base.lens));
}

static long imx290_ioctl(struct v4l2_subdev *sd, unsigned int cmd, void *arg)
{
	struct imx290 *imx290 = to_imx290(sd);
	long ret = 0;

	switch (cmd) {
	case RKMODULE_GET_MODULE_INFO:
		imx290_get_module_inf(imx290, (struct rkmodule_inf *)arg);
		break;
	default:
		ret = -ENOIOCTLCMD;
		break;
	}
	return ret;
}

#ifdef CONFIG_COMPAT
static long imx290_compat_ioctl32(struct v4l2_subdev *sd,
				  unsigned int cmd, unsigned long arg)
{
	void __user *up = compat_ptr(arg);
	struct rkmodule_inf *inf;
	struct rkmodule_awb_cfg *cfg;
	long ret;

	switch (cmd) {
	case RKMODULE_GET_MODULE_INFO:
		inf = kzalloc(sizeof(*inf), GFP_KERNEL);
		if (!inf) {
			ret = -ENOMEM;
			return ret;
		}

		ret = imx290_ioctl(sd, cmd, inf);
		if (!ret)
			ret = copy_to_user(up, inf, sizeof(*inf));
		kfree(inf);
		break;
	case RKMODULE_AWB_CFG:
		cfg = kzalloc(sizeof(*cfg), GFP_KERNEL);
		if (!cfg) {
			ret = -ENOMEM;
			return ret;
		}
		ret = copy_from_user(cfg, up, sizeof(*cfg));
		if (!ret)
			ret = imx290_ioctl(sd, cmd, cfg);
		kfree(cfg);
		break;
	default:
		ret = -ENOIOCTLCMD;
		break;
	}

	return ret;
}
#endif

static int __imx290_start_stream(struct imx290 *imx290)
{
	int ret;

	ret = imx290_write_array(imx290->client, imx290->cur_mode->reg_list);
	if (ret)
		return ret;
	/* In case these controls are set before streaming */
	ret = __v4l2_ctrl_handler_setup(&imx290->ctrl_handler);

	ret = imx290_write_reg(imx290->client,
		IMX290_REG_CTRL_MODE,
		IMX290_REG_VALUE_08BIT,
		0);
	msleep(30);
	ret = imx290_write_reg(imx290->client,
		0x3002,
		IMX290_REG_VALUE_08BIT,
		0);
	return ret;
}

static int __imx290_stop_stream(struct imx290 *imx290)
{
	int ret;
	ret = imx290_write_reg(imx290->client,
		IMX290_REG_CTRL_MODE,
		IMX290_REG_VALUE_08BIT,
		1);
	ret = imx290_write_reg(imx290->client,
		0x3002,
		IMX290_REG_VALUE_08BIT,
		1);
	return ret;
}

static int imx290_s_stream(struct v4l2_subdev *sd, int on)
{
	struct imx290 *imx290 = to_imx290(sd);
	struct i2c_client *client = imx290->client;
	int ret = 0;

	mutex_lock(&imx290->mutex);
	on = !!on;
	if (on == imx290->streaming)
		goto unlock_and_return;

	if (on) {
		ret = pm_runtime_get_sync(&client->dev);
		if (ret < 0) {
			pm_runtime_put_noidle(&client->dev);
			goto unlock_and_return;
		}

		ret = __imx290_start_stream(imx290);
		if (ret) {
			v4l2_err(sd, "start stream failed while write regs\n");
			pm_runtime_put(&client->dev);
			goto unlock_and_return;
		}
	} else {
		__imx290_stop_stream(imx290);
		pm_runtime_put(&client->dev);
	}

	imx290->streaming = on;

unlock_and_return:
	mutex_unlock(&imx290->mutex);

	return ret;
}

static int imx290_s_power(struct v4l2_subdev *sd, int on)
{
	struct imx290 *imx290 = to_imx290(sd);
	struct i2c_client *client = imx290->client;
	int ret = 0;

	mutex_lock(&imx290->mutex);

	/* If the power state is not modified - no work to do. */
	if (imx290->power_on == !!on)
		goto unlock_and_return;

	if (on) {
		ret = pm_runtime_get_sync(&client->dev);
		if (ret < 0) {
			pm_runtime_put_noidle(&client->dev);
			goto unlock_and_return;
		}

		ret = imx290_write_array(imx290->client, imx290_global_regs);
		if (ret) {
			v4l2_err(sd, "could not set init registers\n");
			pm_runtime_put_noidle(&client->dev);
			goto unlock_and_return;
		}

		imx290->power_on = true;
	} else {
		pm_runtime_put(&client->dev);
		imx290->power_on = false;
	}

unlock_and_return:
	mutex_unlock(&imx290->mutex);

	return ret;
}


static int __imx290_power_on(struct imx290 *imx290)
{
	int ret;
	struct device *dev = &imx290->client->dev;

	if (!IS_ERR_OR_NULL(imx290->pins_default)) {
		ret = pinctrl_select_state(imx290->pinctrl,
					   imx290->pins_default);
		if (ret < 0)
			dev_err(dev, "could not set pins\n");
	}

	ret = clk_set_rate(imx290->xvclk, IMX290_XVCLK_FREQ);
	if (ret < 0)
		dev_warn(dev, "Failed to set xvclk rate (37.125M Hz)\n");

	if (clk_get_rate(imx290->xvclk) != IMX290_XVCLK_FREQ)
		dev_warn(dev, "xvclk mismatched,based on 24M Hz\n");

	ret = clk_prepare_enable(imx290->xvclk);
	if (ret < 0) {
		dev_err(dev, "Failed to enable xvclk\n");
		return ret;
	}


	ret = regulator_bulk_enable(IMX290_NUM_SUPPLIES, imx290->supplies);
	if (ret < 0) {
		dev_err(dev, "Failed to enable regulators\n");
		goto disable_clk;
	}

	if (!IS_ERR(imx290->reset_gpio))
		gpiod_set_value_cansleep(imx290->reset_gpio, 0);
	usleep_range(500, 1000);
	if (!IS_ERR(imx290->reset_gpio))
		gpiod_set_value_cansleep(imx290->reset_gpio, 1);

	if (!IS_ERR(imx290->pwdn_gpio))
		gpiod_set_value_cansleep(imx290->pwdn_gpio, 1);

	usleep_range(30000, 40000);
	return 0;

disable_clk:
	clk_disable_unprepare(imx290->xvclk);

	return ret;
}

static void __imx290_power_off(struct imx290 *imx290)
{
	int ret;
	struct device *dev = &imx290->client->dev;

	if (!IS_ERR(imx290->pwdn_gpio))
		gpiod_set_value_cansleep(imx290->pwdn_gpio, 0);
	clk_disable_unprepare(imx290->xvclk);
	if (!IS_ERR(imx290->reset_gpio))
		gpiod_set_value_cansleep(imx290->reset_gpio, 0);
	if (!IS_ERR_OR_NULL(imx290->pins_sleep)) {
		ret = pinctrl_select_state(imx290->pinctrl,
					   imx290->pins_sleep);
		if (ret < 0)
			dev_dbg(dev, "could not set pins\n");
	}
	regulator_bulk_disable(IMX290_NUM_SUPPLIES, imx290->supplies);
}

static int imx290_runtime_resume(struct device *dev)
{
	struct i2c_client *client = to_i2c_client(dev);
	struct v4l2_subdev *sd = i2c_get_clientdata(client);
	struct imx290 *imx290 = to_imx290(sd);

	return __imx290_power_on(imx290);
}

static int imx290_runtime_suspend(struct device *dev)
{
	struct i2c_client *client = to_i2c_client(dev);
	struct v4l2_subdev *sd = i2c_get_clientdata(client);
	struct imx290 *imx290 = to_imx290(sd);

	__imx290_power_off(imx290);

	return 0;
}

#ifdef CONFIG_VIDEO_V4L2_SUBDEV_API
static int imx290_open(struct v4l2_subdev *sd, struct v4l2_subdev_fh *fh)
{
	struct imx290 *imx290 = to_imx290(sd);
	struct v4l2_mbus_framefmt *try_fmt =
				v4l2_subdev_get_try_format(sd, fh->state, 0);
	const struct imx290_mode *def_mode = &imx290->support_modes[0];

	mutex_lock(&imx290->mutex);
	/* Initialize try_fmt */
	try_fmt->width = def_mode->width;
	try_fmt->height = def_mode->height;
	try_fmt->code = def_mode->bus_fmt;
	try_fmt->field = V4L2_FIELD_NONE;

	mutex_unlock(&imx290->mutex);
	/* No crop or compose */

	return 0;
}
#endif

static int imx290_enum_frame_interval(struct v4l2_subdev *sd,
				       struct v4l2_subdev_state *sd_state,
				       struct v4l2_subdev_frame_interval_enum *fie)
{
	struct imx290 *imx290 = to_imx290(sd);

	if (fie->index >= imx290->support_modes_num)
		return -EINVAL;

	fie->code = imx290->support_modes[fie->index].bus_fmt;
	fie->width = imx290->support_modes[fie->index].width;
	fie->height = imx290->support_modes[fie->index].height;
	fie->interval = imx290->support_modes[fie->index].max_fps;
	return 0;
}

#define CROP_START(SRC, DST) (((SRC) - (DST)) / 2 / 4 * 4)
#define DST_WIDTH 1920
#define DST_HEIGHT 1080

/*
 * The resolution of the driver configuration needs to be exactly
 * the same as the current output resolution of the sensor,
 * the input width of the isp needs to be 16 aligned,
 * the input height of the isp needs to be 8 aligned.
 * Can be cropped to standard resolution by this function,
 * otherwise it will crop out strange resolution according
 * to the alignment rules.
 */

static int imx290_get_selection(struct v4l2_subdev *sd,
				struct v4l2_subdev_state *sd_state,
				struct v4l2_subdev_selection *sel)
{
	struct imx290 *imx290 = to_imx290(sd);

	if (sel->target == V4L2_SEL_TGT_CROP_BOUNDS) {
		sel->r.left = CROP_START(imx290->cur_mode->width, DST_WIDTH);
		sel->r.width = DST_WIDTH;
		if (imx290->bus_cfg.bus_type == V4L2_MBUS_CCP2) {
			sel->r.top = 21;
		} else {
			sel->r.top = CROP_START(imx290->cur_mode->height, DST_HEIGHT);
		}
		sel->r.height = DST_HEIGHT;
		return 0;
	}
	return -EINVAL;
}

static const struct dev_pm_ops imx290_pm_ops = {
	SET_RUNTIME_PM_OPS(imx290_runtime_suspend,
			   imx290_runtime_resume, NULL)
};

#ifdef CONFIG_VIDEO_V4L2_SUBDEV_API
static const struct v4l2_subdev_internal_ops imx290_internal_ops = {
	.open = imx290_open,
};
#endif

static const struct v4l2_subdev_core_ops imx290_core_ops = {
	.s_power = imx290_s_power,
	.ioctl = imx290_ioctl,
#ifdef CONFIG_COMPAT
	.compat_ioctl32 = imx290_compat_ioctl32,
#endif
};

static const struct v4l2_subdev_video_ops imx290_video_ops = {
	.s_stream = imx290_s_stream,
	.g_frame_interval = imx290_g_frame_interval,
};

static const struct v4l2_subdev_pad_ops imx290_pad_ops = {
	.enum_mbus_code = imx290_enum_mbus_code,
	.enum_frame_size = imx290_enum_frame_sizes,
	.enum_frame_interval = imx290_enum_frame_interval,
	.get_fmt = imx290_get_fmt,
	.set_fmt = imx290_set_fmt,
	.get_selection = imx290_get_selection,
	.get_mbus_config = imx290_g_mbus_config,
};

static const struct v4l2_subdev_ops imx290_subdev_ops = {
	.core	= &imx290_core_ops,
	.video	= &imx290_video_ops,
	.pad	= &imx290_pad_ops,
};

static int imx290_set_ctrl(struct v4l2_ctrl *ctrl)
{
	struct imx290 *imx290 = container_of(ctrl->handler,
					     struct imx290, ctrl_handler);
	struct i2c_client *client = imx290->client;
	s64 max;
	int ret = 0;
	u32 shs1 = 0;
	u32 vts = 0;
	u32 val = 0;

	/* Propagate change of current control to all related controls */
	switch (ctrl->id) {
	case V4L2_CID_VBLANK:
		/* Update max exposure while meeting expected vblanking */
		max = imx290->cur_mode->height + ctrl->val - 2;
		__v4l2_ctrl_modify_range(imx290->exposure,
					 imx290->exposure->minimum, max,
					 imx290->exposure->step,
					 imx290->exposure->default_value);
		break;
	}

	if (!pm_runtime_get_if_in_use(&client->dev))
		return 0;

	switch (ctrl->id) {
	case V4L2_CID_EXPOSURE:
			shs1 = imx290->cur_vts - ctrl->val - 1;
			ret = imx290_write_reg(imx290->client,
				IMX290_REG_SHS1_H,
				IMX290_REG_VALUE_08BIT,
				IMX290_FETCH_HIGH_BYTE_EXP(shs1));
			ret |= imx290_write_reg(imx290->client,
				IMX290_REG_SHS1_M,
				IMX290_REG_VALUE_08BIT,
				IMX290_FETCH_MID_BYTE_EXP(shs1));
			ret |= imx290_write_reg(imx290->client,
				IMX290_REG_SHS1_L,
				IMX290_REG_VALUE_08BIT,
				IMX290_FETCH_LOW_BYTE_EXP(shs1));
			dev_dbg(&client->dev, "set exposure 0x%x, cur_vts 0x%x,shs1 0x%x\n",
				ctrl->val, imx290->cur_vts, shs1);
		break;
	case V4L2_CID_ANALOGUE_GAIN:
			ret = imx290_write_reg(imx290->client,
				IMX290_REG_LF_GAIN,
				IMX290_REG_VALUE_08BIT,
				ctrl->val);
			dev_dbg(&client->dev, "set analog gain 0x%x\n",
				ctrl->val);
		break;
	case V4L2_CID_VBLANK:
		vts = ctrl->val + imx290->cur_mode->height;
		imx290->cur_vts = vts;
		ret = imx290_write_reg(imx290->client,
			IMX290_REG_VTS_H,
			IMX290_REG_VALUE_08BIT,
			IMX290_FETCH_HIGH_BYTE_VTS(vts));
		ret |= imx290_write_reg(imx290->client,
			IMX290_REG_VTS_M,
			IMX290_REG_VALUE_08BIT,
			IMX290_FETCH_MID_BYTE_VTS(vts));
		ret |= imx290_write_reg(imx290->client,
			IMX290_REG_VTS_L,
			IMX290_REG_VALUE_08BIT,
			IMX290_FETCH_LOW_BYTE_VTS(vts));
		dev_dbg(&client->dev, "set vts 0x%x\n",
			vts);
		break;
	case V4L2_CID_TEST_PATTERN:
#ifdef USED_TEST_PATTERN
		ret = imx290_enable_test_pattern(imx290, ctrl->val);
#endif
		break;
	case V4L2_CID_HFLIP:
		ret = imx290_read_reg(client,
				      IMX290_FLIP_REG,
				      IMX290_REG_VALUE_08BIT,
				      &val);
		if (ctrl->val)
			val |= MIRROR_BIT_MASK;
		else
			val &= ~MIRROR_BIT_MASK;
		ret |= imx290_write_reg(client,
					IMX290_FLIP_REG,
					IMX290_REG_VALUE_08BIT,
					val);
		if (ret == 0)
			imx290->flip = val;
		break;
	case V4L2_CID_VFLIP:
		ret = imx290_read_reg(client,
				      IMX290_FLIP_REG,
				      IMX290_REG_VALUE_08BIT,
				      &val);
		if (ctrl->val)
			val |= FLIP_BIT_MASK;
		else
			val &= ~FLIP_BIT_MASK;
		ret |= imx290_write_reg(client,
					IMX290_FLIP_REG,
					IMX290_REG_VALUE_08BIT,
					val);
		if (ret == 0)
			imx290->flip = val;
		break;
	default:
		dev_warn(&client->dev, "%s Unhandled id:0x%x, val:0x%x\n",
			 __func__, ctrl->id, ctrl->val);
		break;
	}

	pm_runtime_put(&client->dev);

	return ret;
}

static const struct v4l2_ctrl_ops imx290_ctrl_ops = {
	.s_ctrl = imx290_set_ctrl,
};

static int imx290_initialize_controls(struct imx290 *imx290)
{
	const struct imx290_mode *mode;
	struct v4l2_ctrl_handler *handler;
	s64 exposure_max, vblank_def;
	u32 h_blank;
	int ret;

	handler = &imx290->ctrl_handler;
	mode = imx290->cur_mode;
	ret = v4l2_ctrl_handler_init(handler, 9);
	if (ret)
		return ret;
	handler->lock = &imx290->mutex;

	imx290->link_freq = v4l2_ctrl_new_int_menu(handler, NULL, V4L2_CID_LINK_FREQ,
				      1, 0, link_freq_menu_items);

	imx290->pixel_rate = v4l2_ctrl_new_std(handler, NULL, V4L2_CID_PIXEL_RATE,
			  0, IMX290_PIXEL_RATE, 1, IMX290_PIXEL_RATE);

	h_blank = mode->hts_def - mode->width;

	imx290->hblank = v4l2_ctrl_new_std(handler, NULL, V4L2_CID_HBLANK,
				h_blank, h_blank, 1, h_blank);
	if (imx290->hblank)
		imx290->hblank->flags |= V4L2_CTRL_FLAG_READ_ONLY;

	vblank_def = mode->vts_def - mode->height;
	imx290->cur_vts = mode->vts_def;
	imx290->vblank = v4l2_ctrl_new_std(handler, &imx290_ctrl_ops,
				V4L2_CID_VBLANK, vblank_def,
				IMX290_VTS_MAX - mode->height,
				1, vblank_def);

	exposure_max = mode->vts_def - 2;

	imx290->exposure = v4l2_ctrl_new_std(handler, &imx290_ctrl_ops,
				V4L2_CID_EXPOSURE, IMX290_EXPOSURE_MIN,
				exposure_max, IMX290_EXPOSURE_STEP,
				mode->exp_def);

	imx290->anal_gain = v4l2_ctrl_new_std(handler, &imx290_ctrl_ops,
				V4L2_CID_ANALOGUE_GAIN, IMX290_GAIN_MIN,
				IMX290_GAIN_MAX, IMX290_GAIN_STEP,
				IMX290_GAIN_DEFAULT);

#ifdef USED_TEST_PATTERN
	imx290->test_pattern = v4l2_ctrl_new_std_menu_items(handler,
				&imx290_ctrl_ops, V4L2_CID_TEST_PATTERN,
				ARRAY_SIZE(imx290_test_pattern_menu) - 1,
				0, 0, imx290_test_pattern_menu);
#endif
	imx290->h_flip = v4l2_ctrl_new_std(handler, &imx290_ctrl_ops,
				V4L2_CID_HFLIP, 0, 1, 1, 0);

	imx290->v_flip = v4l2_ctrl_new_std(handler, &imx290_ctrl_ops,
				V4L2_CID_VFLIP, 0, 1, 1, 0);
	imx290->flip = 0;
	if (handler->error) {
		ret = handler->error;
		dev_err(&imx290->client->dev,
			"Failed to init controls(%d)\n", ret);
		goto err_free_handler;
	}

	imx290->subdev.ctrl_handler = handler;
	imx290->has_init_exp = false;

	return 0;

err_free_handler:
	v4l2_ctrl_handler_free(handler);

	return ret;
}

static int imx290_check_sensor_id(struct imx290 *imx290,
				  struct i2c_client *client)
{
	struct device *dev = &imx290->client->dev;
	u32 id = 0;
	int ret;

	ret = imx290_read_reg(client, IMX290_REG_CHIP_ID,
			      IMX290_REG_VALUE_08BIT, &id);

	if (id != CHIP_ID) {
		dev_err(dev, "Unexpected sensor id(%06x), ret(%d)\n", id, ret);
		return -EINVAL;
	}
	return ret;
}

static int imx290_configure_regulators(struct imx290 *imx290)
{
	unsigned int i;

	for (i = 0; i < IMX290_NUM_SUPPLIES; i++)
		imx290->supplies[i].supply = imx290_supply_names[i];

	return devm_regulator_bulk_get(&imx290->client->dev,
				       IMX290_NUM_SUPPLIES,
				       imx290->supplies);
}

static int imx290_probe(struct i2c_client *client,
			 const struct i2c_device_id *id)
{
	struct device *dev = &client->dev;
	struct device_node *node = dev->of_node;
	struct imx290 *imx290;
	struct v4l2_subdev *sd;
	char facing[2];
	int ret;
	struct device_node *endpoint;

	dev_info(dev, "driver version: %02x.%02x.%02x",
		DRIVER_VERSION >> 16,
		(DRIVER_VERSION & 0xff00) >> 8,
		DRIVER_VERSION & 0x00ff);

	imx290 = devm_kzalloc(dev, sizeof(*imx290), GFP_KERNEL);
	if (!imx290)
		return -ENOMEM;

	ret = of_property_read_u32(node, RKMODULE_CAMERA_MODULE_INDEX,
				   &imx290->module_index);
	ret |= of_property_read_string(node, RKMODULE_CAMERA_MODULE_FACING,
				       &imx290->module_facing);
	ret |= of_property_read_string(node, RKMODULE_CAMERA_MODULE_NAME,
				       &imx290->module_name);
	ret |= of_property_read_string(node, RKMODULE_CAMERA_LENS_NAME,
				       &imx290->len_name);
	if (ret) {
		dev_err(dev, "could not get module information!\n");
		return -EINVAL;
	}
	endpoint = of_graph_get_next_endpoint(dev->of_node, NULL);
	if (!endpoint) {
		dev_err(dev, "Failed to get endpoint\n");
		return -EINVAL;
	}

	ret = v4l2_fwnode_endpoint_parse(of_fwnode_handle(endpoint),
		&imx290->bus_cfg);
	imx290->support_modes = mipi_supported_modes;
	imx290->support_modes_num = ARRAY_SIZE(mipi_supported_modes);

	imx290->client = client;
	imx290->cur_mode = &imx290->support_modes[0];

	imx290->xvclk = devm_clk_get(dev, "xvclk");
	if (IS_ERR(imx290->xvclk)) {
		dev_err(dev, "Failed to get xvclk\n");
		return -EINVAL;
	}

	imx290->reset_gpio = devm_gpiod_get(dev, "reset", GPIOD_OUT_LOW);
	if (IS_ERR(imx290->reset_gpio))
		dev_warn(dev, "Failed to get reset-gpios\n");

	imx290->pwdn_gpio = devm_gpiod_get(dev, "pwdn", GPIOD_OUT_LOW);
	if (IS_ERR(imx290->pwdn_gpio))
		dev_warn(dev, "Failed to get pwdn-gpios\n");

	ret = imx290_configure_regulators(imx290);
	if (ret) {
		dev_err(dev, "Failed to get power regulators\n");
		return ret;
	}

	imx290->pinctrl = devm_pinctrl_get(dev);
	if (!IS_ERR(imx290->pinctrl)) {
		imx290->pins_default =
			pinctrl_lookup_state(imx290->pinctrl,
					     OF_CAMERA_PINCTRL_STATE_DEFAULT);
		if (IS_ERR(imx290->pins_default))
			dev_err(dev, "could not get default pinstate\n");

		imx290->pins_sleep =
			pinctrl_lookup_state(imx290->pinctrl,
					     OF_CAMERA_PINCTRL_STATE_SLEEP);
		if (IS_ERR(imx290->pins_sleep))
			dev_err(dev, "could not get sleep pinstate\n");
	}

	mutex_init(&imx290->mutex);

	sd = &imx290->subdev;
	v4l2_i2c_subdev_init(sd, client, &imx290_subdev_ops);
	ret = imx290_initialize_controls(imx290);
	if (ret)
		goto err_destroy_mutex;

	ret = __imx290_power_on(imx290);
	if (ret)
		goto err_free_handler;

	ret = imx290_check_sensor_id(imx290, client);
	if (ret)
		goto err_power_off;

#ifdef CONFIG_VIDEO_V4L2_SUBDEV_API
	dev_err(dev, "set the video v4l2 subdev api\n");
	sd->internal_ops = &imx290_internal_ops;
	sd->flags |= V4L2_SUBDEV_FL_HAS_DEVNODE |
		     V4L2_SUBDEV_FL_HAS_EVENTS;
#endif
#if defined(CONFIG_MEDIA_CONTROLLER)
	dev_err(dev, "set the media controller\n");
	imx290->pad.flags = MEDIA_PAD_FL_SOURCE;
	sd->entity.function = MEDIA_ENT_F_CAM_SENSOR;
	ret = media_entity_pads_init(&sd->entity, 1, &imx290->pad);
	if (ret < 0)
		goto err_power_off;
#endif

	memset(facing, 0, sizeof(facing));
	if (strcmp(imx290->module_facing, "back") == 0)
		facing[0] = 'b';
	else
		facing[0] = 'f';

	snprintf(sd->name, sizeof(sd->name), "m%02d_%s_%s %s",
		 imx290->module_index, facing,
		 IMX290_NAME, dev_name(sd->dev));
	ret = v4l2_async_register_subdev_sensor(sd);
	if (ret) {
		dev_err(dev, "v4l2 async register subdev failed\n");
		goto err_clean_entity;
	}

	pm_runtime_set_active(dev);
	pm_runtime_enable(dev);
	pm_runtime_idle(dev);
	g_isHCG = false;
#ifdef USED_SYS_DEBUG
	add_sysfs_interfaces(dev);
#endif
	dev_err(dev, "v4l2 async register subdev success\n");
	return 0;

err_clean_entity:
#if defined(CONFIG_MEDIA_CONTROLLER)
	media_entity_cleanup(&sd->entity);
#endif
err_power_off:
	__imx290_power_off(imx290);
err_free_handler:
	v4l2_ctrl_handler_free(&imx290->ctrl_handler);
err_destroy_mutex:
	mutex_destroy(&imx290->mutex);

	return ret;
}

static void imx290_remove(struct i2c_client *client)
{
	struct v4l2_subdev *sd = i2c_get_clientdata(client);
	struct imx290 *imx290 = to_imx290(sd);

	v4l2_async_unregister_subdev(sd);
#if defined(CONFIG_MEDIA_CONTROLLER)
	media_entity_cleanup(&sd->entity);
#endif
	v4l2_ctrl_handler_free(&imx290->ctrl_handler);
	mutex_destroy(&imx290->mutex);

	pm_runtime_disable(&client->dev);
	if (!pm_runtime_status_suspended(&client->dev))
		__imx290_power_off(imx290);
	pm_runtime_set_suspended(&client->dev);
}

#if IS_ENABLED(CONFIG_OF)
static const struct of_device_id imx290_of_match[] = {
	{ .compatible = "sony,imx290" },
	{},
};
MODULE_DEVICE_TABLE(of, imx290_of_match);
#endif

static const struct i2c_device_id imx290_match_id[] = {
	{ "sony,imx290", 0 },
	{ },
};

static struct i2c_driver imx290_i2c_driver = {
	.driver = {
		.name = IMX290_NAME,
		.pm = &imx290_pm_ops,
		.of_match_table = of_match_ptr(imx290_of_match),
	},
	.probe		= &imx290_probe,
	.remove		= &imx290_remove,
	.id_table	= imx290_match_id,
};

static int __init sensor_mod_init(void)
{
	return i2c_add_driver(&imx290_i2c_driver);
}

static void __exit sensor_mod_exit(void)
{
	i2c_del_driver(&imx290_i2c_driver);
}

device_initcall_sync(sensor_mod_init);
module_exit(sensor_mod_exit);

MODULE_DESCRIPTION("Sony imx290 sensor driver");
MODULE_LICENSE("GPL v2");
