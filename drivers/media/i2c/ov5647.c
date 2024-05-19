// SPDX-License-Identifier: GPL-2.0
/*
 * ov5647 driver
 *
 * Copyright (C) 2017 Fuzhou Rockchip Electronics Co., Ltd.
 *
 * V0.0X01.0X01 add poweron function.
 * V0.0X01.0X02 fix mclk issue when probe multiple camera.
 * V0.0X01.0X03 add enum_frame_interval function.
 * V0.0X01.0X04 add quick stream on/off
 * V0.0X01.0X05 add function g_mbus_config
 */

#include <linux/clk.h>
#include <linux/device.h>
#include <linux/delay.h>
#include <linux/gpio/consumer.h>
#include <linux/i2c.h>
#include <linux/module.h>
#include <linux/pm_runtime.h>
#include <linux/of.h>
#include <linux/of_graph.h>
#include <linux/regulator/consumer.h>
#include <linux/sysfs.h>
#include <linux/pinctrl/consumer.h>
#include <linux/version.h>
#include <media/v4l2-async.h>
#include <media/media-entity.h>
#include <media/v4l2-common.h>
#include <media/v4l2-ctrls.h>
#include <media/v4l2-device.h>
#include <media/v4l2-event.h>
#include <media/v4l2-fwnode.h>
#include <media/v4l2-image-sizes.h>
#include <media/v4l2-mediabus.h>
#include <media/v4l2-subdev.h>

#include <linux/rk-camera-module.h>

#define DRIVER_VERSION			KERNEL_VERSION(0, 0x01, 0x05)

#ifndef V4L2_CID_DIGITAL_GAIN
#define V4L2_CID_DIGITAL_GAIN		V4L2_CID_GAIN
#endif

/* pixel rate = link frequency * 2 * lanes / BITS_PER_SAMPLE */
#define MIPI_FREQ	210000000U
#define OV5647_PIXEL_RATE		(210000000LL * 2LL * 2LL / 10)
#define OV5647_XVCLK_FREQ		24000000

#define CHIP_ID				0x5647
#define OV5647_REG_CHIP_ID		0x300a

#define OV5647_REG_CTRL_MODE		0x0100
#define OV5647_MODE_SW_STANDBY		0x00
#define OV5647_MODE_STREAMING		0x01

#define OV5647_REG_EXPOSURE		0x3500
#define	OV5647_EXPOSURE_MIN		4
#define	OV5647_EXPOSURE_STEP		1
#define OV5647_VTS_MAX			0x7fff

#define OV5647_REG_ANALOG_GAIN		0x3509
#define	ANALOG_GAIN_MIN			0x10
#define	ANALOG_GAIN_MAX			0xf8
#define	ANALOG_GAIN_STEP		1
#define	ANALOG_GAIN_DEFAULT		0xf8

#define OV5647_REG_GAIN_H		0x350a
#define OV5647_REG_GAIN_L		0x350b
#define OV5647_GAIN_L_MASK		0xff
#define OV5647_GAIN_H_MASK		0x03
#define OV5647_DIGI_GAIN_H_SHIFT	8
#define OV5647_DIGI_GAIN_MIN		0
#define OV5647_DIGI_GAIN_MAX		(0x4000 - 1)
#define OV5647_DIGI_GAIN_STEP		1
#define OV5647_DIGI_GAIN_DEFAULT	1024

#define OV5647_REG_TEST_PATTERN		0x503d
#define	OV5647_TEST_PATTERN_ENABLE	0x80
#define	OV5647_TEST_PATTERN_DISABLE	0x0

#define OV5647_REG_VTS			0x380e

#define REG_NULL			0xFFFF

#define OV5647_REG_VALUE_08BIT		1
#define OV5647_REG_VALUE_16BIT		2
#define OV5647_REG_VALUE_24BIT		3

#define OV5647_LANES			2
#define OV5647_BITS_PER_SAMPLE		10

#define OF_CAMERA_PINCTRL_STATE_DEFAULT	"rockchip,camera_default"
#define OF_CAMERA_PINCTRL_STATE_SLEEP	"rockchip,camera_sleep"

#define OV5647_NAME			"ov5647"

static const char * const ov5647_supply_names[] = {
	"avdd",		/* Analog power */
	"dovdd",	/* Digital I/O power */
	"dvdd",		/* Digital core power */
};

#define OV5647_NUM_SUPPLIES ARRAY_SIZE(ov5647_supply_names)

#define PWDN_ACTIVE_DELAY_MS	500
#define OV5647_LANES  2

#define MIPI_CTRL00_CLOCK_LANE_GATE		BIT(5)
#define MIPI_CTRL00_LINE_SYNC_ENABLE		BIT(4)
#define MIPI_CTRL00_BUS_IDLE			BIT(2)
#define MIPI_CTRL00_CLOCK_LANE_DISABLE		BIT(0)

#define OV5647_SW_STANDBY		0x0100
#define OV5647_SW_RESET			0x0103
#define OV5647_REG_CHIPID_H		0x300a
#define OV5647_REG_CHIPID_L		0x300b
#define OV5640_REG_PAD_OUT		0x300d
#define OV5647_REG_EXP_HI		0x3500
#define OV5647_REG_EXP_MID		0x3501
#define OV5647_REG_EXP_LO		0x3502
#define OV5647_REG_AEC_AGC		0x3503
#define OV5647_REG_GAIN_HI		0x350a
#define OV5647_REG_GAIN_LO		0x350b
#define OV5647_REG_VTS_HI		0x380e
#define OV5647_REG_VTS_LO		0x380f
#define OV5647_REG_VFLIP		0x3820
#define OV5647_REG_HFLIP		0x3821
#define OV5647_REG_FRAME_OFF_NUMBER	0x4202
#define OV5647_REG_MIPI_CTRL00		0x4800

struct regval {
	u16 addr;
	u8 val;
};

struct ov5647_mode {
	u32 width;
	u32 height;
	struct v4l2_fract max_fps;
	u32 hts_def;
	u32 vts_def;
	u32 exp_def;
	const struct regval *reg_list;
};

struct ov5647 {
	struct i2c_client	*client;
	struct clk		*xvclk;
	struct gpio_desc	*power_gpio;
	struct gpio_desc	*reset_gpio;
	struct gpio_desc	*pwdn_gpio;
	struct regulator_bulk_data supplies[OV5647_NUM_SUPPLIES];

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
	struct v4l2_ctrl	*test_pattern;
	struct mutex		mutex;
	bool			streaming;
	bool			power_on;
	const struct ov5647_mode *cur_mode;
	unsigned int lane_num;
	unsigned int cfg_num;
	unsigned int pixel_rate;
	u32			module_index;
	const char		*module_facing;
	const char		*module_name;
	const char		*len_name;
};

#define to_ov5647(sd) container_of(sd, struct ov5647, subdev)

static const struct regval sensor_oe_disable_regs[] = {
	{0x3000, 0x00}, // SC_CMMN_PAD_OEN0
	{0x3001, 0x00}, // SC_CMMN_PAD_OEN1
	{0x3002, 0x00}, // SC_CMMN_PAD_OEN2
	{REG_NULL, 0x00},
};

static const struct regval sensor_oe_enable_regs[] = {
	{0x3000, 0x0f}, // SC_CMMN_PAD_OEN0
	{0x3001, 0xff}, // SC_CMMN_PAD_OEN1
	{0x3002, 0xe4}, // SC_CMMN_PAD_OEN2
	{REG_NULL, 0x00},
};

/*
 * Xclk 24Mhz
 * Pclk 84Mhz
 * linelength 2816(0xb00)
 * framelength 1984(0x7c0)
 * grabwindow_width 2592
 * grabwindow_height 1944
 * max_framerate 15fps
 * mipi_datarate per lane 420Mbps
 */
static const struct regval ov5647_global_regs[] = {
	{0x0100, 0x00},
	{0x3001, 0x00}, //SC_CMMN_PAD_OEN1
	{0x3002, 0x00}, //SC_CMMN_PAD_OEN2
	{0x3011, 0x02}, //SC_CMMN_PAD_PK
	{0x3017, 0x05},	//SC_CMMN_MIPI_PHY
	{0x3018, 0x4c}, //bit[7:5] 001: 1lane;010: 2lane  SC_CMMN_MIPI_SC_CTRL
	{0x301c, 0xd2},
	{0x3022, 0x00},
	{0x3034, 0x1a},
	{0x3035, 0x21},
	{0x3036, 0x69},
	{0x3037, 0x03},
	{0x3038, 0x00},
	{0x3039, 0x00},
	{0x303a, 0x00},
	{0x303b, 0x19},
	{0x303c, 0x11},
	{0x303d, 0x30},
	{0x3105, 0x11},
	{0x3106, 0x05},
	{0x3304, 0x28},
	{0x3305, 0x41},
	{0x3306, 0x30},
	{0x3308, 0x00},
	{0x3309, 0xc8},
	{0x330a, 0x01},
	{0x330b, 0x90},
	{0x330c, 0x02},
	{0x330d, 0x58},
	{0x330e, 0x03},
	{0x330f, 0x20},
	{0x3300, 0x00},
	{0x3500, 0x00},
	{0x3501, 0x3d},
	{0x3502, 0x00},
	{0x3503, 0x07},
	{0x350a, 0x00},
	{0x350b, 0x40},
	{0x3601, 0x33},
	{0x3602, 0x00},
	{0x3611, 0x0e},
	{0x3612, 0x2b},
	{0x3614, 0x50},

	{0x3620, 0x33},
	{0x3622, 0x00},
	{0x3630, 0xad},
	{0x3631, 0x00},
	{0x3632, 0x94},
	{0x3633, 0x17},
	{0x3634, 0x14},
	{0x3704, 0xc0},
	{0x3705, 0x2a},
	{0x3708, 0x66},
	{0x3709, 0x52},
	{0x370b, 0x23},
	{0x370c, 0xcf},
	{0x370d, 0x00},
	{0x370e, 0x00},
	{0x371c, 0x07},
	{0x3739, 0xd2},
	{0x373c, 0x00},
	{0x3800, 0x00},
	{0x3801, 0x00},
	{0x3802, 0x00},
	{0x3803, 0x00},
	{0x3804, 0x0a},
	{0x3805, 0x3f},
	{0x3806, 0x07},
	{0x3807, 0xa3},
	{0x3808, 0x05},
	{0x3809, 0x10},
	{0x380a, 0x03},
	{0x380b, 0xcc},
	{0x380c, 0x0b},
	{0x380d, 0x00},
	{0x380e, 0x03},
	{0x380f, 0xe0},
	{0x3810, 0x00},
	{0x3811, 0x08},
	{0x3812, 0x00},
	{0x3813, 0x04},
	{0x3814, 0x31},
	{0x3815, 0x31},
	{0x3817, 0x00},
	{0x3820, 0x08},
	{0x3821, 0x07},
	{0x3826, 0x03},
	{0x3829, 0x00},
	{0x382b, 0x0b},
	{0x3830, 0x00},
	{0x3836, 0x00},
	{0x3837, 0x00},
	{0x3838, 0x00},
	{0x3839, 0x04},
	{0x383a, 0x00},
	{0x383b, 0x01},
	{0x3b00, 0x00},
	{0x3b02, 0x08},
	{0x3b03, 0x00},
	{0x3b04, 0x04},

	{0x3b05, 0x00},
	{0x3b06, 0x04},
	{0x3b07, 0x08},
	{0x3b08, 0x00},
	{0x3b09, 0x02},
	{0x3b0a, 0x04},
	{0x3b0b, 0x00},
	{0x3b0c, 0x3d},
	{0x3f01, 0x0d},
	{0x3f0f, 0xf5},
	{0x4000, 0x89},
	{0x4001, 0x02},
	{0x4002, 0x45},
	{0x4004, 0x02},
	{0x4005, 0x18},
	{0x4006, 0x08},
	{0x4007, 0x10},
	{0x4008, 0x00},
	{0x4050, 0x6e},
	{0x4051, 0x8f},
	{0x4300, 0xf8},
	{0x4303, 0xff},
	{0x4304, 0x00},
	{0x4307, 0xff},
	{0x4520, 0x00},
	{0x4521, 0x00},
	{0x4511, 0x22},
	{0x4801, 0x0f},
	{0x4814, 0x2a},
	{0x481f, 0x3c},
	{0x4823, 0x3c},
	{0x4826, 0x00},
	{0x481b, 0x3c},
	{0x4827, 0x32},
	{0x4837, 0x18},
	{0x4b00, 0x06},
	{0x4b01, 0x0a},
	{0x4b04, 0x10},
	{0x5000, 0xff},
	{0x5001, 0x00},
	{0x5002, 0x41},
	{0x5003, 0x0a},
	{0x5004, 0x00},
	{0x5043, 0x00},
	{0x5013, 0x00},
	{0x501f, 0x03},
	{0x503d, 0x00},
	{0x5780, 0xfc},
	{0x5781, 0x1f},
	{0x5782, 0x03},
	{0x5786, 0x20},
	{0x5787, 0x40},
	{0x5788, 0x08},
	{0x5789, 0x08},
	{0x578a, 0x02},
	{0x578b, 0x01},
	{0x578c, 0x01},

	{0x578d, 0x0c},
	{0x578e, 0x02},
	{0x578f, 0x01},
	{0x5790, 0x01},
	{0x5a00, 0x08},
	{0x5b00, 0x01},
	{0x5b01, 0x40},
	{0x5b02, 0x00},
	{0x5b03, 0xf0},

	{REG_NULL, 0x00},
};

/*
 * Xclk 24Mhz
 * Pclk 84Mhz
 * linelength 2816(0xb00)
 * framelength 1984(0x7c0)
 * grabwindow_width 2592
 * grabwindow_height 1944
 * max_framerate 15fps
 * mipi_datarate per lane 420Mbps
 */
static const struct regval ov5647_2592x1944_regs[] = {
	{0x0100, 0x00},
	{0x0103, 0x01},
	{0x3034, 0x1a},
	{0x3035, 0x21},
	{0x3036, 0x69},
	{0x303c, 0x11},
	{0x3106, 0xf5},
	{0x3821, 0x00},
	{0x3820, 0x00},
	{0x3827, 0xec},
	{0x370c, 0x03},
	{0x3612, 0x5b},
	{0x3618, 0x04},
	{0x5000, 0x06},
	{0x5002, 0x41},
	{0x5003, 0x08},
	{0x5a00, 0x08},
	{0x3000, 0x00},
	{0x3001, 0x00},
	{0x3002, 0x00},
	{0x3016, 0x08},
	{0x3017, 0xe0},
	{0x3018, 0x44},
	{0x301c, 0xf8},
	{0x301d, 0xf0},
	{0x3a18, 0x00},
	{0x3a19, 0xf8},
	{0x3c01, 0x80},
	{0x3b07, 0x0c},
	{0x380c, 0x0b},
	{0x380d, 0x1c},
	{0x3814, 0x11},
	{0x3815, 0x11},
	{0x3708, 0x64},
	{0x3709, 0x12},
	{0x3808, 0x0a},
	{0x3809, 0x20},
	{0x380a, 0x07},
	{0x380b, 0x98},
	{0x3800, 0x00},
	{0x3801, 0x00},
	{0x3802, 0x00},
	{0x3803, 0x00},
	{0x3804, 0x0a},
	{0x3805, 0x3f},
	{0x3806, 0x07},
	{0x3807, 0xa3},
	{0x3811, 0x10},
	{0x3813, 0x06},
	{0x3630, 0x2e},
	{0x3632, 0xe2},
	{0x3633, 0x23},
	{0x3634, 0x44},
	{0x3636, 0x06},
	{0x3620, 0x64},
	{0x3621, 0xe0},
	{0x3600, 0x37},
	{0x3704, 0xa0},
	{0x3703, 0x5a},
	{0x3715, 0x78},
	{0x3717, 0x01},
	{0x3731, 0x02},
	{0x370b, 0x60},
	{0x3705, 0x1a},
	{0x3f05, 0x02},
	{0x3f06, 0x10},
	{0x3f01, 0x0a},
	{0x3a08, 0x01},
	{0x3a09, 0x28},
	{0x3a0a, 0x00},
	{0x3a0b, 0xf6},
	{0x3a0d, 0x08},
	{0x3a0e, 0x06},
	{0x3a0f, 0x58},
	{0x3a10, 0x50},
	{0x3a1b, 0x58},
	{0x3a1e, 0x50},
	{0x3a11, 0x60},
	{0x3a1f, 0x28},
	{0x4001, 0x02},
	{0x4004, 0x04},
	{0x4000, 0x09},
	{0x4837, 0x19},
	{0x4800, 0x24},
	{0x3503, 0x03},
	{0x0100, 0x01},

	{REG_NULL, 0x00},
};

/*
 * Xclk 24Mhz
 * Pclk 84Mhz
 * linelength 2816(0xb00)
 * framelength 992(0x3e0)
 * grabwindow_width 1296
 * grabwindow_height 972
 * max_framerate 30fps
 * mipi_datarate per lane 420Mbps
 */
static const struct regval ov5647_1296x972_regs[] = {
	{0x0100, 0x00},
	{0x0103, 0x01},
	{0x3034, 0x1a},
	{0x3035, 0x21},
	{0x3036, 0x62},
	{0x303c, 0x11},
	{0x3106, 0xf5},
	{0x3827, 0xec},
	{0x370c, 0x03},
	{0x3612, 0x59},
	{0x3618, 0x00},
	{0x5000, 0x06},
	{0x5002, 0x41},
	{0x5003, 0x08},
	{0x5a00, 0x08},
	{0x3000, 0x00},
	{0x3001, 0x00},
	{0x3002, 0x00},
	{0x3016, 0x08},
	{0x3017, 0xe0},
	{0x3018, 0x44},
	{0x301c, 0xf8},
	{0x301d, 0xf0},
	{0x3a18, 0x00},
	{0x3a19, 0xf8},
	{0x3c01, 0x80},
	{0x3b07, 0x0c},
	{0x3800, 0x00},
	{0x3801, 0x00},
	{0x3802, 0x00},
	{0x3803, 0x00},
	{0x3804, 0x0a},
	{0x3805, 0x3f},
	{0x3806, 0x07},
	{0x3807, 0xa3},
	{0x3808, 0x05},
	{0x3809, 0x10},
	{0x380a, 0x03},
	{0x380b, 0xcc},
	{0x380c, 0x07},
	{0x380d, 0x68},
	{0x3811, 0x0c},
	{0x3813, 0x06},
	{0x3814, 0x31},
	{0x3815, 0x31},
	{0x3630, 0x2e},
	{0x3632, 0xe2},
	{0x3633, 0x23},
	{0x3634, 0x44},
	{0x3636, 0x06},
	{0x3620, 0x64},
	{0x3621, 0xe0},
	{0x3600, 0x37},
	{0x3704, 0xa0},
	{0x3703, 0x5a},
	{0x3715, 0x78},
	{0x3717, 0x01},
	{0x3731, 0x02},
	{0x370b, 0x60},
	{0x3705, 0x1a},
	{0x3f05, 0x02},
	{0x3f06, 0x10},
	{0x3f01, 0x0a},
	{0x3a08, 0x01},
	{0x3a09, 0x28},
	{0x3a0a, 0x00},
	{0x3a0b, 0xf6},
	{0x3a0d, 0x08},
	{0x3a0e, 0x06},
	{0x3a0f, 0x58},
	{0x3a10, 0x50},
	{0x3a1b, 0x58},
	{0x3a1e, 0x50},
	{0x3a11, 0x60},
	{0x3a1f, 0x28},
	{0x4001, 0x02},
	{0x4004, 0x04},
	{0x4000, 0x09},
	{0x4837, 0x16},
	{0x4800, 0x24},
	{0x3503, 0x03},
	{0x3820, 0x41},
	{0x3821, 0x01},
	{0x350a, 0x00},
	{0x350b, 0x10},
	{0x3500, 0x00},
	{0x3501, 0x1a},
	{0x3502, 0xf0},
	{0x3212, 0xa0},
	{0x0100, 0x01},

	{REG_NULL, 0x00}
};

static const struct regval ov5647_1920x1080_regs[] = {
	{0x0100, 0x00},
	{0x0103, 0x01},
	{0x3034, 0x1a},
	{0x3035, 0x21},
	{0x3036, 0x62},
	{0x303c, 0x11},
	{0x3106, 0xf5},
	{0x3821, 0x00},
	{0x3820, 0x00},
	{0x3827, 0xec},
	{0x370c, 0x03},
	{0x3612, 0x5b},
	{0x3618, 0x04},
	{0x5000, 0x06},
	{0x5002, 0x41},
	{0x5003, 0x08},
	{0x5a00, 0x08},
	{0x3000, 0x00},
	{0x3001, 0x00},
	{0x3002, 0x00},
	{0x3016, 0x08},
	{0x3017, 0xe0},
	{0x3018, 0x44},
	{0x301c, 0xf8},
	{0x301d, 0xf0},
	{0x3a18, 0x00},
	{0x3a19, 0xf8},
	{0x3c01, 0x80},
	{0x3b07, 0x0c},
	{0x380c, 0x09},
	{0x380d, 0x70},
	{0x3814, 0x11},
	{0x3815, 0x11},
	{0x3708, 0x64},
	{0x3709, 0x12},
	{0x3808, 0x07},
	{0x3809, 0x80},
	{0x380a, 0x04},
	{0x380b, 0x38},
	{0x3800, 0x01},
	{0x3801, 0x5c},
	{0x3802, 0x01},
	{0x3803, 0xb2},
	{0x3804, 0x08},
	{0x3805, 0xe3},
	{0x3806, 0x05},
	{0x3807, 0xf1},
	{0x3811, 0x04},
	{0x3813, 0x02},
	{0x3630, 0x2e},
	{0x3632, 0xe2},
	{0x3633, 0x23},
	{0x3634, 0x44},
	{0x3636, 0x06},
	{0x3620, 0x64},
	{0x3621, 0xe0},
	{0x3600, 0x37},
	{0x3704, 0xa0},
	{0x3703, 0x5a},
	{0x3715, 0x78},
	{0x3717, 0x01},
	{0x3731, 0x02},
	{0x370b, 0x60},
	{0x3705, 0x1a},
	{0x3f05, 0x02},
	{0x3f06, 0x10},
	{0x3f01, 0x0a},
	{0x3a08, 0x01},
	{0x3a09, 0x4b},
	{0x3a0a, 0x01},
	{0x3a0b, 0x13},
	{0x3a0d, 0x04},
	{0x3a0e, 0x03},
	{0x3a0f, 0x58},
	{0x3a10, 0x50},
	{0x3a1b, 0x58},
	{0x3a1e, 0x50},
	{0x3a11, 0x60},
	{0x3a1f, 0x28},
	{0x4001, 0x02},
	{0x4004, 0x04},
	{0x4000, 0x09},
	{0x4837, 0x19},
	{0x4800, 0x34},
	{0x3503, 0x03},
	{0x0100, 0x01},

	{REG_NULL, 0x00}
};

static const struct ov5647_mode supported_modes_2lane[] = {
	{
		.width = 2592,
		.height = 1944,
		.max_fps = {
			.numerator = 10000,
			.denominator = 150000,
		},
		.exp_def = 0x0450,
		.hts_def = 2844,
		.vts_def = 0x7b0,
		.reg_list = ov5647_2592x1944_regs,
	},
	{
		.width = 1920,
		.height = 1080,
		.max_fps = {
			.numerator = 10000,
			.denominator = 300000,
		},
		.exp_def = 0x0450,
		.hts_def = 2416,
		.vts_def = 0x450,
		.reg_list = ov5647_1920x1080_regs,
	},
	{
		.width = 1296,
		.height = 972,
		.max_fps = {
			.numerator = 10000,
			.denominator = 300000,
		},
		.exp_def = 0x03d0,
		.hts_def = 1896,
		.vts_def = 0x59b,
		.reg_list = ov5647_1296x972_regs,
	},
};

static const struct ov5647_mode *supported_modes;

static const s64 link_freq_menu_items[] = {
	MIPI_FREQ
};

static const char * const ov5647_test_pattern_menu[] = {
	"Disabled",
	"Vertical Color Bar Type 1",
	"Vertical Color Bar Type 2",
	"Vertical Color Bar Type 3",
	"Vertical Color Bar Type 4"
};

/* Write registers up to 4 at a time */
static int ov5647_write_reg(struct i2c_client *client, u16 reg,
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

	if (i2c_master_send(client, buf, len + 2) != len + 2) {
		dev_err(&client->dev,
			   "write reg(0x%x val:0x%x)failed !\n", reg, val);
		return -EIO;
	}
	return 0;
}

static int ov5647_write_array(struct i2c_client *client,
			      const struct regval *regs)
{
	u32 i;
	int ret = 0;

	for (i = 0; ret == 0 && regs[i].addr != REG_NULL; i++)
		ret = ov5647_write_reg(client, regs[i].addr,
				       OV5647_REG_VALUE_08BIT, regs[i].val);

	return ret;
}

/* Read registers up to 4 at a time */
static int ov5647_read_reg(struct i2c_client *client, u16 reg,
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

static int ov5647_get_reso_dist(const struct ov5647_mode *mode,
				struct v4l2_mbus_framefmt *framefmt)
{
	return abs(mode->width - framefmt->width) +
	       abs(mode->height - framefmt->height);
}

static const struct ov5647_mode *
ov5647_find_best_fit(struct ov5647 *ov5647,
			struct v4l2_subdev_format *fmt)
{
	struct v4l2_mbus_framefmt *framefmt = &fmt->format;
	int dist;
	int cur_best_fit = 0;
	int cur_best_fit_dist = -1;
	int i;

	for (i = 0; i < ov5647->cfg_num; i++) {
		dist = ov5647_get_reso_dist(&supported_modes[i], framefmt);
		if (cur_best_fit_dist == -1 || dist < cur_best_fit_dist) {
			cur_best_fit_dist = dist;
			cur_best_fit = i;
		}
	}

	return &supported_modes[cur_best_fit];
}

static int ov5647_set_fmt(struct v4l2_subdev *sd,
			  struct v4l2_subdev_pad_config *cfg,
			  struct v4l2_subdev_format *fmt)
{
	struct ov5647 *ov5647 = to_ov5647(sd);
	const struct ov5647_mode *mode;
	s64 h_blank, vblank_def;

	mutex_lock(&ov5647->mutex);

	mode = ov5647_find_best_fit(ov5647, fmt);
	fmt->format.code = MEDIA_BUS_FMT_SBGGR10_1X10;
	fmt->format.width = mode->width;
	fmt->format.height = mode->height;
	fmt->format.field = V4L2_FIELD_NONE;
	if (fmt->which == V4L2_SUBDEV_FORMAT_TRY) {
#ifdef CONFIG_VIDEO_V4L2_SUBDEV_API
		*v4l2_subdev_get_try_format(sd, cfg, fmt->pad) = fmt->format;
#else
		mutex_unlock(&ov5647->mutex);
		return -ENOTTY;
#endif
	} else {
		ov5647->cur_mode = mode;
		h_blank = mode->hts_def - mode->width;
		__v4l2_ctrl_modify_range(ov5647->hblank, h_blank,
					 h_blank, 1, h_blank);
		vblank_def = mode->vts_def - mode->height;
		__v4l2_ctrl_modify_range(ov5647->vblank, vblank_def,
					 OV5647_VTS_MAX - mode->height,
					 1, vblank_def);
	}

	mutex_unlock(&ov5647->mutex);

	return 0;
}

static int ov5647_get_fmt(struct v4l2_subdev *sd,
			  struct v4l2_subdev_pad_config *cfg,
			  struct v4l2_subdev_format *fmt)
{
	struct ov5647 *ov5647 = to_ov5647(sd);
	const struct ov5647_mode *mode = ov5647->cur_mode;

	mutex_lock(&ov5647->mutex);
	if (fmt->which == V4L2_SUBDEV_FORMAT_TRY) {
#ifdef CONFIG_VIDEO_V4L2_SUBDEV_API
		fmt->format = *v4l2_subdev_get_try_format(sd, cfg, fmt->pad);
#else
		mutex_unlock(&ov5647->mutex);
		return -ENOTTY;
#endif
	} else {
		fmt->format.width = mode->width;
		fmt->format.height = mode->height;
		fmt->format.code = MEDIA_BUS_FMT_SBGGR10_1X10;
		fmt->format.field = V4L2_FIELD_NONE;
	}
	mutex_unlock(&ov5647->mutex);

	return 0;
}

static int ov5647_enum_mbus_code(struct v4l2_subdev *sd,
				 struct v4l2_subdev_pad_config *cfg,
				 struct v4l2_subdev_mbus_code_enum *code)
{
	if (code->index != 0)
		return -EINVAL;
	code->code = MEDIA_BUS_FMT_SBGGR10_1X10;

	return 0;
}

static int ov5647_enum_frame_sizes(struct v4l2_subdev *sd,
				   struct v4l2_subdev_pad_config *cfg,
				   struct v4l2_subdev_frame_size_enum *fse)
{
	struct ov5647 *ov5647 = to_ov5647(sd);

	if (fse->index >= ov5647->cfg_num)
		return -EINVAL;

	if (fse->code != MEDIA_BUS_FMT_SBGGR10_1X10)
		return -EINVAL;

	fse->min_width  = supported_modes[fse->index].width;
	fse->max_width  = supported_modes[fse->index].width;
	fse->max_height = supported_modes[fse->index].height;
	fse->min_height = supported_modes[fse->index].height;

	return 0;
}

static int ov5647_enable_test_pattern(struct ov5647 *ov5647, u32 pattern)
{
	u32 val;

	if (pattern)
		val = (pattern - 1) | OV5647_TEST_PATTERN_ENABLE;
	else
		val = OV5647_TEST_PATTERN_DISABLE;

	return ov5647_write_reg(ov5647->client, OV5647_REG_TEST_PATTERN,
				OV5647_REG_VALUE_08BIT, val);
}

static int ov5647_g_frame_interval(struct v4l2_subdev *sd,
				   struct v4l2_subdev_frame_interval *fi)
{
	struct ov5647 *ov5647 = to_ov5647(sd);
	const struct ov5647_mode *mode = ov5647->cur_mode;

	fi->interval = mode->max_fps;

	return 0;
}

static void ov5647_get_module_inf(struct ov5647 *ov5647,
				  struct rkmodule_inf *inf)
{
	memset(inf, 0, sizeof(*inf));
	strlcpy(inf->base.sensor, OV5647_NAME, sizeof(inf->base.sensor));
	strlcpy(inf->base.module, ov5647->module_name,
		sizeof(inf->base.module));
	strlcpy(inf->base.lens, ov5647->len_name, sizeof(inf->base.lens));
}

static long ov5647_ioctl(struct v4l2_subdev *sd, unsigned int cmd, void *arg)
{
	struct ov5647 *ov5647 = to_ov5647(sd);
	long ret = 0;
	u32 stream = 0;

	switch (cmd) {
	case RKMODULE_GET_MODULE_INFO:
		ov5647_get_module_inf(ov5647, (struct rkmodule_inf *)arg);
		break;
	case RKMODULE_SET_QUICK_STREAM:
		stream = *((u32 *)arg);

		if (stream)
			ret = ov5647_write_reg(ov5647->client, OV5647_REG_CTRL_MODE,
				OV5647_REG_VALUE_08BIT, OV5647_MODE_STREAMING);
		else
			ret = ov5647_write_reg(ov5647->client, OV5647_REG_CTRL_MODE,
				OV5647_REG_VALUE_08BIT, OV5647_MODE_SW_STANDBY);
		break;
	default:
		ret = -ENOIOCTLCMD;
		break;
	}

	return ret;
}

#ifdef CONFIG_COMPAT
static long ov5647_compat_ioctl32(struct v4l2_subdev *sd,
				  unsigned int cmd, unsigned long arg)
{
	void __user *up = compat_ptr(arg);
	struct rkmodule_inf *inf;
	struct rkmodule_awb_cfg *cfg;
	long ret;
	u32 stream = 0;

	switch (cmd) {
	case RKMODULE_GET_MODULE_INFO:
		inf = kzalloc(sizeof(*inf), GFP_KERNEL);
		if (!inf) {
			ret = -ENOMEM;
			return ret;
		}

		ret = ov5647_ioctl(sd, cmd, inf);
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
			ret = ov5647_ioctl(sd, cmd, cfg);
		kfree(cfg);
		break;
	case RKMODULE_SET_QUICK_STREAM:
		ret = copy_from_user(&stream, up, sizeof(u32));
		if (!ret)
			ret = ov5647_ioctl(sd, cmd, &stream);
		break;
	default:
		ret = -ENOIOCTLCMD;
		break;
	}

	return ret;
}
#endif

static int __ov5647_start_stream(struct ov5647 *ov5647)
{
	int ret;

	ret = ov5647_write_array(ov5647->client, ov5647->cur_mode->reg_list);
	if (ret)
		return ret;

	/* In case these controls are set before streaming */
	mutex_unlock(&ov5647->mutex);
	ret = v4l2_ctrl_handler_setup(&ov5647->ctrl_handler);
	mutex_lock(&ov5647->mutex);
	if (ret)
		return ret;

	return ov5647_write_reg(ov5647->client, OV5647_REG_CTRL_MODE,
				OV5647_REG_VALUE_08BIT, OV5647_MODE_STREAMING);
}

static int __ov5647_stop_stream(struct ov5647 *ov5647)
{
	return ov5647_write_reg(ov5647->client, OV5647_REG_CTRL_MODE,
				OV5647_REG_VALUE_08BIT, OV5647_MODE_SW_STANDBY);
}

static int ov5647_s_stream(struct v4l2_subdev *sd, int on)
{
	struct ov5647 *ov5647 = to_ov5647(sd);
	struct i2c_client *client = ov5647->client;
	int ret = 0;

	dev_info(&client->dev, "%s(%d) enter!\n", __func__, __LINE__);
	mutex_lock(&ov5647->mutex);
	on = !!on;
	if (on == ov5647->streaming)
		goto unlock_and_return;

	if (on) {
		dev_info(&client->dev, "stream on!!!\n");
		ret = pm_runtime_get_sync(&client->dev);
		if (ret < 0) {
			pm_runtime_put_noidle(&client->dev);
			goto unlock_and_return;
		}

		ret = __ov5647_start_stream(ov5647);
		if (ret) {
			v4l2_err(sd, "start stream failed while write regs\n");
			pm_runtime_put(&client->dev);
			goto unlock_and_return;
		}
	} else {
		dev_info(&client->dev, "stream off!!!\n");
		__ov5647_stop_stream(ov5647);
		pm_runtime_put(&client->dev);
	}

	ov5647->streaming = on;

unlock_and_return:
	mutex_unlock(&ov5647->mutex);

	return ret;
}

static int ov5647_s_power(struct v4l2_subdev *sd, int on)
{
	struct ov5647 *ov5647 = to_ov5647(sd);
	struct i2c_client *client = ov5647->client;
	int ret = 0;

	mutex_lock(&ov5647->mutex);

	/* If the power state is not modified - no work to do. */
	if (ov5647->power_on == !!on)
		goto unlock_and_return;

	if (on) {
		ret = pm_runtime_get_sync(&client->dev);
		if (ret < 0) {
			pm_runtime_put_noidle(&client->dev);
			goto unlock_and_return;
		}
/*
		ret = ov5647_write_array(ov5647->client, ov5647_global_regs);
		if (ret) {
			v4l2_err(sd, "could not set init registers\n");
			pm_runtime_put_noidle(&client->dev);
			goto unlock_and_return;
		}*/

		ov5647->power_on = true;
	} else {
		pm_runtime_put(&client->dev);
		ov5647->power_on = false;
	}

unlock_and_return:
	mutex_unlock(&ov5647->mutex);

	return ret;
}

/* Calculate the delay in us by clock rate and clock cycles */
static inline u32 ov5647_cal_delay(u32 cycles)
{
	return DIV_ROUND_UP(cycles, OV5647_XVCLK_FREQ / 1000 / 1000);
}

static int ov5647_stream_off(struct v4l2_subdev *sd)
{
	int ret;

	struct ov5647 *ov5647 = to_ov5647(sd);
	ret = ov5647_write_reg(ov5647->client, OV5647_REG_MIPI_CTRL00,OV5647_REG_VALUE_08BIT,
			   MIPI_CTRL00_CLOCK_LANE_GATE | MIPI_CTRL00_BUS_IDLE |
			   MIPI_CTRL00_CLOCK_LANE_DISABLE);

	if (ret < 0)
		return ret;

	ret = ov5647_write_reg(ov5647->client, OV5647_REG_FRAME_OFF_NUMBER, OV5647_REG_VALUE_08BIT, 0x0f);
	if (ret < 0)
		return ret;

	return ov5647_write_reg(ov5647->client, OV5640_REG_PAD_OUT, OV5647_REG_VALUE_08BIT, 0x01);
}
static int __ov5647_power_on(struct ov5647 *ov5647)
{
	struct ov5647 *sensor = ov5647;
	int ret;
	struct device *dev = &ov5647->client->dev;

	dev_info(dev, "OV5647 power on\n");

	if (!IS_ERR(sensor->pwdn_gpio)) {
		gpiod_set_value_cansleep(sensor->pwdn_gpio, 0);
		msleep(PWDN_ACTIVE_DELAY_MS);
	}

	ret = regulator_bulk_enable(OV5647_NUM_SUPPLIES, ov5647->supplies);
	if (ret < 0) {
		dev_err(dev, "Failed to enable regulators\n");
	}

	ret = ov5647_write_array(sensor->client, sensor_oe_enable_regs);
	if (ret < 0) {
		dev_err(dev, "write sensor_oe_enable_regs error\n");
	}

	/* Stream off to coax lanes into LP-11 state. */
	ret = ov5647_stream_off(&sensor->subdev);
	if (ret < 0) {
		dev_err(dev, "camera not available, check power\n");
	}

	return 0;

	if (!IS_ERR(sensor->power_gpio))
		gpiod_set_value_cansleep(sensor->pwdn_gpio, 1);

	return ret;
}

static void __ov5647_power_off(struct ov5647 *ov5647)
{
	int ret;
	struct device *dev = &ov5647->client->dev;

	if (!IS_ERR(ov5647->pwdn_gpio))
		gpiod_set_value_cansleep(ov5647->pwdn_gpio, 0);
	clk_disable_unprepare(ov5647->xvclk);
	if (!IS_ERR(ov5647->reset_gpio))
		gpiod_set_value_cansleep(ov5647->reset_gpio, 1);
	if (!IS_ERR_OR_NULL(ov5647->pins_sleep)) {
		ret = pinctrl_select_state(ov5647->pinctrl,
					   ov5647->pins_sleep);
		if (ret < 0)
			dev_dbg(dev, "could not set pins\n");
	}
	if (!IS_ERR(ov5647->power_gpio))
		gpiod_set_value_cansleep(ov5647->power_gpio, 0);

	regulator_bulk_disable(OV5647_NUM_SUPPLIES, ov5647->supplies);
}

static int ov5647_runtime_resume(struct device *dev)
{
	struct i2c_client *client = to_i2c_client(dev);
	struct v4l2_subdev *sd = i2c_get_clientdata(client);
	struct ov5647 *ov5647 = to_ov5647(sd);

	return __ov5647_power_on(ov5647);
}

static int ov5647_runtime_suspend(struct device *dev)
{
	struct i2c_client *client = to_i2c_client(dev);
	struct v4l2_subdev *sd = i2c_get_clientdata(client);
	struct ov5647 *ov5647 = to_ov5647(sd);

	__ov5647_power_off(ov5647);

	return 0;
}

#ifdef CONFIG_VIDEO_V4L2_SUBDEV_API
static int ov5647_open(struct v4l2_subdev *sd, struct v4l2_subdev_fh *fh)
{
	struct ov5647 *ov5647 = to_ov5647(sd);
	struct v4l2_mbus_framefmt *try_fmt =
				v4l2_subdev_get_try_format(sd, fh->pad, 0);
	const struct ov5647_mode *def_mode = &supported_modes[0];

	mutex_lock(&ov5647->mutex);
	/* Initialize try_fmt */
	try_fmt->width = def_mode->width;
	try_fmt->height = def_mode->height;
	try_fmt->code = MEDIA_BUS_FMT_SBGGR10_1X10;
	try_fmt->field = V4L2_FIELD_NONE;

	mutex_unlock(&ov5647->mutex);
	/* No crop or compose */

	return 0;
}
#endif

static int ov5647_enum_frame_interval(struct v4l2_subdev *sd,
				       struct v4l2_subdev_pad_config *cfg,
				       struct v4l2_subdev_frame_interval_enum *fie)
{
	struct ov5647 *ov5647 = to_ov5647(sd);

	if (fie->index >= ov5647->cfg_num)
		return -EINVAL;

	fie->code = MEDIA_BUS_FMT_SBGGR10_1X10;
	fie->width = supported_modes[fie->index].width;
	fie->height = supported_modes[fie->index].height;
	fie->interval = supported_modes[fie->index].max_fps;
	return 0;
}

static int ov5647_g_mbus_config(struct v4l2_subdev *sd, unsigned int pad_id,
				struct v4l2_mbus_config *config)
{
	u32 val = 0;

	val = 1 << (OV5647_LANES - 1) |
	      V4L2_MBUS_CSI2_CHANNEL_0 |
	      V4L2_MBUS_CSI2_CONTINUOUS_CLOCK;
	config->type = V4L2_MBUS_CSI2_DPHY;
	config->flags = val;

	return 0;
}

static const struct dev_pm_ops ov5647_pm_ops = {
	SET_RUNTIME_PM_OPS(ov5647_runtime_suspend,
			   ov5647_runtime_resume, NULL)
};

#ifdef CONFIG_VIDEO_V4L2_SUBDEV_API
static const struct v4l2_subdev_internal_ops ov5647_internal_ops = {
	.open = ov5647_open,
};
#endif

static const struct v4l2_subdev_core_ops ov5647_core_ops = {
	.s_power = ov5647_s_power,
	.ioctl = ov5647_ioctl,
#ifdef CONFIG_COMPAT
	.compat_ioctl32 = ov5647_compat_ioctl32,
#endif
	.subscribe_event = v4l2_ctrl_subdev_subscribe_event,
	.unsubscribe_event = v4l2_event_subdev_unsubscribe,
};

static const struct v4l2_subdev_video_ops ov5647_video_ops = {
	.s_stream = ov5647_s_stream,
	.g_frame_interval = ov5647_g_frame_interval,
};

static const struct v4l2_subdev_pad_ops ov5647_pad_ops = {
	.enum_mbus_code = ov5647_enum_mbus_code,
	.enum_frame_size = ov5647_enum_frame_sizes,
	.enum_frame_interval = ov5647_enum_frame_interval,
	.get_fmt = ov5647_get_fmt,
	.set_fmt = ov5647_set_fmt,
	.get_mbus_config = ov5647_g_mbus_config,
};

static const struct v4l2_subdev_ops ov5647_subdev_ops = {
	.core	= &ov5647_core_ops,
	.video	= &ov5647_video_ops,
	.pad	= &ov5647_pad_ops,
};

static int ov5647_set_ctrl(struct v4l2_ctrl *ctrl)
{
	struct ov5647 *ov5647 = container_of(ctrl->handler,
					     struct ov5647, ctrl_handler);
	struct i2c_client *client = ov5647->client;
	s64 max;
	int ret = 0;

	/* Propagate change of current control to all related controls */
	switch (ctrl->id) {
	case V4L2_CID_VBLANK:
		/* Update max exposure while meeting expected vblanking */
		max = ov5647->cur_mode->height + ctrl->val - 4;
		__v4l2_ctrl_modify_range(ov5647->exposure,
					 ov5647->exposure->minimum, max,
					 ov5647->exposure->step,
					 ov5647->exposure->default_value);
		break;
	}

	if (!pm_runtime_get_if_in_use(&client->dev))
		return 0;

	switch (ctrl->id) {
	case V4L2_CID_EXPOSURE:
		/* 4 least significant bits of expsoure are fractional part */

		ret = ov5647_write_reg(ov5647->client, OV5647_REG_EXPOSURE,
				       OV5647_REG_VALUE_24BIT, ctrl->val << 4);

		break;
	case V4L2_CID_ANALOGUE_GAIN:
		ret = ov5647_write_reg(ov5647->client, OV5647_REG_GAIN_L,
				       OV5647_REG_VALUE_08BIT,
				       ctrl->val & OV5647_GAIN_L_MASK);
		ret |= ov5647_write_reg(ov5647->client, OV5647_REG_GAIN_H,
				       OV5647_REG_VALUE_08BIT,
				       (ctrl->val >> OV5647_DIGI_GAIN_H_SHIFT) &
				       OV5647_GAIN_H_MASK);
		break;
	case V4L2_CID_VBLANK:

		ret = ov5647_write_reg(ov5647->client, OV5647_REG_VTS,
				       OV5647_REG_VALUE_16BIT,
				       ctrl->val + ov5647->cur_mode->height);
		break;
	case V4L2_CID_TEST_PATTERN:
		ret = ov5647_enable_test_pattern(ov5647, ctrl->val);
		break;
	default:
		dev_warn(&client->dev, "%s Unhandled id:0x%x, val:0x%x\n",
			 __func__, ctrl->id, ctrl->val);
		break;
	}

	pm_runtime_put(&client->dev);

	return ret;
}

static const struct v4l2_ctrl_ops ov5647_ctrl_ops = {
	.s_ctrl = ov5647_set_ctrl,
};

static int ov5647_initialize_controls(struct ov5647 *ov5647)
{
	const struct ov5647_mode *mode;
	struct v4l2_ctrl_handler *handler;
	struct v4l2_ctrl *ctrl;
	s64 exposure_max, vblank_def;
	u32 h_blank;
	int ret;

	handler = &ov5647->ctrl_handler;
	mode = ov5647->cur_mode;
	ret = v4l2_ctrl_handler_init(handler, 8);
	if (ret)
		return ret;
	handler->lock = &ov5647->mutex;

	ctrl = v4l2_ctrl_new_int_menu(handler, NULL, V4L2_CID_LINK_FREQ,
				      0, 0, link_freq_menu_items);
	if (ctrl)
		ctrl->flags |= V4L2_CTRL_FLAG_READ_ONLY;

	v4l2_ctrl_new_std(handler, NULL, V4L2_CID_PIXEL_RATE,
			  0, ov5647->pixel_rate, 1, ov5647->pixel_rate);

	h_blank = mode->hts_def - mode->width;
	ov5647->hblank = v4l2_ctrl_new_std(handler, NULL, V4L2_CID_HBLANK,
				h_blank, h_blank, 1, h_blank);
	if (ov5647->hblank)
		ov5647->hblank->flags |= V4L2_CTRL_FLAG_READ_ONLY;

	vblank_def = mode->vts_def - mode->height;
	ov5647->vblank = v4l2_ctrl_new_std(handler, &ov5647_ctrl_ops,
				V4L2_CID_VBLANK, vblank_def,
				OV5647_VTS_MAX - mode->height,
				1, vblank_def);

	exposure_max = mode->vts_def - 4;
	ov5647->exposure = v4l2_ctrl_new_std(handler, &ov5647_ctrl_ops,
				V4L2_CID_EXPOSURE, OV5647_EXPOSURE_MIN,
				exposure_max, OV5647_EXPOSURE_STEP,
				mode->exp_def);

	ov5647->anal_gain = v4l2_ctrl_new_std(handler, &ov5647_ctrl_ops,
				V4L2_CID_ANALOGUE_GAIN, ANALOG_GAIN_MIN,
				ANALOG_GAIN_MAX, ANALOG_GAIN_STEP,
				ANALOG_GAIN_DEFAULT);

	/* Digital gain */
	ov5647->digi_gain = v4l2_ctrl_new_std(handler, &ov5647_ctrl_ops,
				V4L2_CID_DIGITAL_GAIN, OV5647_DIGI_GAIN_MIN,
				OV5647_DIGI_GAIN_MAX, OV5647_DIGI_GAIN_STEP,
				OV5647_DIGI_GAIN_DEFAULT);

	ov5647->test_pattern = v4l2_ctrl_new_std_menu_items(handler,
				&ov5647_ctrl_ops, V4L2_CID_TEST_PATTERN,
				ARRAY_SIZE(ov5647_test_pattern_menu) - 1,
				0, 0, ov5647_test_pattern_menu);

	if (handler->error) {
		ret = handler->error;
		dev_err(&ov5647->client->dev,
			"Failed to init controls(%d)\n", ret);
		goto err_free_handler;
	}

	ov5647->subdev.ctrl_handler = handler;

	return 0;

err_free_handler:
	v4l2_ctrl_handler_free(handler);

	return ret;
}

static int ov5647_check_sensor_id(struct ov5647 *ov5647,
				  struct i2c_client *client)
{
	struct device *dev = &ov5647->client->dev;
	u32 id = 0;
	int ret;

	ret = ov5647_read_reg(client, OV5647_REG_CHIP_ID,
			      OV5647_REG_VALUE_16BIT, &id);
	if (id != CHIP_ID) {
		dev_err(dev, "Unexpected sensor id(%06x), ret(%d)\n", id, ret);
		return -ENODEV;
	}

	dev_info(dev, "Detected OV%06x sensor\n", CHIP_ID);

	return 0;
}

static int ov5647_configure_regulators(struct ov5647 *ov5647)
{
	int i;

	for (i = 0; i < OV5647_NUM_SUPPLIES; i++)
		ov5647->supplies[i].supply = ov5647_supply_names[i];

	return devm_regulator_bulk_get(&ov5647->client->dev,
				       OV5647_NUM_SUPPLIES,
				       ov5647->supplies);
}

static int ov5647_parse_of(struct ov5647 *ov5647)
{
	struct device *dev = &ov5647->client->dev;
	struct device_node *endpoint;
	struct fwnode_handle *fwnode;
	int rval;

	endpoint = of_graph_get_next_endpoint(dev->of_node, NULL);
	if (!endpoint) {
		dev_err(dev, "Failed to get endpoint\n");
		return -EINVAL;
	}
	fwnode = of_fwnode_handle(endpoint);
	rval = fwnode_property_read_u32_array(fwnode, "data-lanes", NULL, 0);
	if (rval <= 0) {
		dev_warn(dev, " Get mipi lane num failed!\n");
		return -EINVAL;
	}

	ov5647->lane_num = rval;
	if (2 == ov5647->lane_num) {
		ov5647->cur_mode = &supported_modes_2lane[0];
		supported_modes = supported_modes_2lane;
		ov5647->cfg_num = ARRAY_SIZE(supported_modes_2lane);

		/* pixel rate = link frequency * 2 * lanes / BITS_PER_SAMPLE */
		ov5647->pixel_rate = MIPI_FREQ * 2U * ov5647->lane_num / 10U;
		dev_info(dev, "lane_num(%d)  pixel_rate(%u)\n",
				 ov5647->lane_num, ov5647->pixel_rate);
	} else {
		dev_err(dev, "unsupported lane_num(%d)\n", ov5647->lane_num);
		return -EINVAL;
	}
	return 0;
}



static int ov5647_probe(struct i2c_client *client,
			const struct i2c_device_id *id)
{
	struct device *dev = &client->dev;
	struct device_node *node = dev->of_node;
	struct ov5647 *ov5647;
	struct v4l2_subdev *sd;
	char facing[2] = "b";
	int ret;

	dev_info(dev, "driver version: %02x.%02x.%02x",
		DRIVER_VERSION >> 16,
		(DRIVER_VERSION & 0xff00) >> 8,
		DRIVER_VERSION & 0x00ff);

	ov5647 = devm_kzalloc(dev, sizeof(*ov5647), GFP_KERNEL);
	if (!ov5647)
		return -ENOMEM;

	ret = of_property_read_u32(node, RKMODULE_CAMERA_MODULE_INDEX,
				   &ov5647->module_index);
	if (ret) {
		dev_warn(dev, "could not get module index!\n");
		ov5647->module_index = 0;
	}
	ret |= of_property_read_string(node, RKMODULE_CAMERA_MODULE_FACING,
				       &ov5647->module_facing);
	ret |= of_property_read_string(node, RKMODULE_CAMERA_MODULE_NAME,
				       &ov5647->module_name);
	ret |= of_property_read_string(node, RKMODULE_CAMERA_LENS_NAME,
				       &ov5647->len_name);
	if (ret) {
		dev_err(dev, "could not get module information!\n");
		return -EINVAL;
	}

	ov5647->client = client;

	ov5647->power_gpio = devm_gpiod_get(dev, "power", GPIOD_OUT_LOW);
	if (IS_ERR(ov5647->power_gpio))
		dev_warn(dev, "Failed to get power-gpios, maybe no use\n");

	ov5647->reset_gpio = devm_gpiod_get(dev, "reset", GPIOD_OUT_LOW);
	if (IS_ERR(ov5647->reset_gpio))
		dev_warn(dev, "Failed to get reset-gpios, maybe no use\n");

	ov5647->pwdn_gpio = devm_gpiod_get(dev, "pwdn", GPIOD_OUT_LOW);
	if (IS_ERR(ov5647->pwdn_gpio))
		dev_warn(dev, "Failed to get pwdn-gpios\n");

	ret = ov5647_configure_regulators(ov5647);
	if (ret) {
		dev_err(dev, "Failed to get power regulators\n");
		return ret;
	}
	ret = ov5647_parse_of(ov5647);
	if (ret != 0)
		return -EINVAL;

	ov5647->pinctrl = devm_pinctrl_get(dev);
	if (!IS_ERR(ov5647->pinctrl)) {
		ov5647->pins_default =
			pinctrl_lookup_state(ov5647->pinctrl,
					     OF_CAMERA_PINCTRL_STATE_DEFAULT);
		if (IS_ERR(ov5647->pins_default))
			dev_err(dev, "could not get default pinstate\n");

		ov5647->pins_sleep =
			pinctrl_lookup_state(ov5647->pinctrl,
					     OF_CAMERA_PINCTRL_STATE_SLEEP);
		if (IS_ERR(ov5647->pins_sleep))
			dev_err(dev, "could not get sleep pinstate\n");
	}

	mutex_init(&ov5647->mutex);

	sd = &ov5647->subdev;
	v4l2_i2c_subdev_init(sd, client, &ov5647_subdev_ops);
	ret = ov5647_initialize_controls(ov5647);
	if (ret)
		goto err_destroy_mutex;

	ret = __ov5647_power_on(ov5647);
	if (ret)
		goto err_free_handler;

	ret = ov5647_check_sensor_id(ov5647, client);
	if (ret < 0) {
		dev_err(&client->dev, "%s(%d) Check id  failed\n"
				  "check following information:\n"
				  "Power/PowerDown/Reset/Mclk/I2cBus !!\n",
				  __func__, __LINE__);
		goto err_power_off;
	}

#ifdef CONFIG_VIDEO_V4L2_SUBDEV_API
	sd->internal_ops = &ov5647_internal_ops;
	sd->flags |= V4L2_SUBDEV_FL_HAS_DEVNODE |
		     V4L2_SUBDEV_FL_HAS_EVENTS;
#endif
#if defined(CONFIG_MEDIA_CONTROLLER)
	ov5647->pad.flags = MEDIA_PAD_FL_SOURCE;
	sd->entity.function = MEDIA_ENT_F_CAM_SENSOR;
	ret = media_entity_pads_init(&sd->entity, 1, &ov5647->pad);
	if (ret < 0)
		goto err_power_off;
#endif

	memset(facing, 0, sizeof(facing));
	if (strcmp(ov5647->module_facing, "back") == 0)
		facing[0] = 'b';
	else
		facing[0] = 'f';

	snprintf(sd->name, sizeof(sd->name), "m%02d_%s_%s %s",
		 ov5647->module_index, facing,
		 OV5647_NAME, dev_name(sd->dev));

	ret = v4l2_async_register_subdev_sensor_common(sd);
	if (ret) {
		dev_err(dev, "v4l2 async register subdev failed\n");
		goto err_clean_entity;
	}

	pm_runtime_set_active(dev);
	pm_runtime_enable(dev);
	pm_runtime_idle(dev);

	return 0;

err_clean_entity:
#if defined(CONFIG_MEDIA_CONTROLLER)
	media_entity_cleanup(&sd->entity);
#endif
err_power_off:
	__ov5647_power_off(ov5647);
err_free_handler:
	v4l2_ctrl_handler_free(&ov5647->ctrl_handler);
err_destroy_mutex:
	mutex_destroy(&ov5647->mutex);

	return ret;
}

static int ov5647_remove(struct i2c_client *client)
{
	struct v4l2_subdev *sd = i2c_get_clientdata(client);
	struct ov5647 *ov5647 = to_ov5647(sd);

	v4l2_async_unregister_subdev(sd);
#if defined(CONFIG_MEDIA_CONTROLLER)
	media_entity_cleanup(&sd->entity);
#endif
	v4l2_ctrl_handler_free(&ov5647->ctrl_handler);
	mutex_destroy(&ov5647->mutex);

	pm_runtime_disable(&client->dev);
	if (!pm_runtime_status_suspended(&client->dev))
		__ov5647_power_off(ov5647);
	pm_runtime_set_suspended(&client->dev);

	return 0;
}

#if IS_ENABLED(CONFIG_OF)
static const struct of_device_id ov5647_of_match[] = {
	{ .compatible = "ovti,ov5647" },
	{},
};
MODULE_DEVICE_TABLE(of, ov5647_of_match);
#endif

static const struct i2c_device_id ov5647_match_id[] = {
	{ "ovti,ov5647", 0 },
	{ },
};

static struct i2c_driver ov5647_i2c_driver = {
	.driver = {
		.name = OV5647_NAME,
		.pm = &ov5647_pm_ops,
		.of_match_table = of_match_ptr(ov5647_of_match),
	},
	.probe		= &ov5647_probe,
	.remove		= &ov5647_remove,
	.id_table	= ov5647_match_id,
};

static int __init sensor_mod_init(void)
{
	return i2c_add_driver(&ov5647_i2c_driver);
}

static void __exit sensor_mod_exit(void)
{
	i2c_del_driver(&ov5647_i2c_driver);
}

device_initcall_sync(sensor_mod_init);
module_exit(sensor_mod_exit);

MODULE_DESCRIPTION("OmniVision ov5647 sensor driver");
MODULE_LICENSE("GPL v2");
