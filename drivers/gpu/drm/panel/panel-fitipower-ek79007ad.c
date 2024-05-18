// SPDX-License-Identifier: GPL-2.0

#define DEBUG

#include <linux/delay.h>
#include <linux/device.h>
#include <linux/err.h>
#include <linux/errno.h>
#include <linux/fb.h>
#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/of_device.h>

#include <linux/gpio/consumer.h>
#include <linux/regulator/consumer.h>

#include <drm/drm_mipi_dsi.h>
#include <drm/drm_modes.h>
#include <drm/drm_panel.h>

#include <video/mipi_display.h>

struct ek79007ad_instr {
	u8 cmd;
	u8 data;
};

struct ek79007ad_desc {
	const struct ek79007ad_instr *init;
	const size_t init_length;
	const struct drm_display_mode *mode;
};

struct ek79007ad {
	struct drm_panel panel;
	struct mipi_dsi_device *dsi;
	const struct ek79007ad_desc *desc;

	struct regulator *power;
	struct gpio_desc *enable_gpio;
	struct gpio_desc *reset;
};

#define EK79007AD_COMMAND_INSTR(_cmd, _data)    \
	{                                       \
		.cmd = (_cmd), .data = (_data), \
	}

/* support new panel vklcd07 (kwh070kq40-c08) */
static const struct ek79007ad_instr ek79007ad_init_vklcd07[] = {
	EK79007AD_COMMAND_INSTR(0xB0, 0x80),
	EK79007AD_COMMAND_INSTR(0xB1, 0x00),
	EK79007AD_COMMAND_INSTR(0xB2, 0x00),
	EK79007AD_COMMAND_INSTR(0xB3, 0x00),
};

static inline struct ek79007ad *panel_to_ek79007ad(struct drm_panel *panel)
{
	return container_of(panel, struct ek79007ad, panel);
}

static int ek79007ad_send_cmd_data(struct ek79007ad *ctx, u8 cmd, u8 data)
{
	u8 buf[2] = { cmd, data };
	int ret;

	ret = mipi_dsi_dcs_write_buffer(ctx->dsi, buf, sizeof(buf));
	if (ret < 0) {
		dev_err(&ctx->dsi->dev, "Write fault %d\n", ret);
		return ret;
	}
	return 0;
}

static int ek79007ad_read_cmd_data(struct ek79007ad *tftcp, u8 cmd)
{
	u8 buf = 0;
	int ret;

	ret = mipi_dsi_dcs_read(tftcp->dsi, cmd, &buf, sizeof(buf));
	if (ret < 0) {
		dev_err(&tftcp->dsi->dev, "mipi_dsi_dcs_read  fault(%d)\n",
			ret);
	}

	return buf;
}
#ifdef DEBUG
static void ek79007ad_dump_reg(struct ek79007ad *ctx)
{
	unsigned int i;
	const u8 reg_dump_addr[] = { 0x0A, 0x0D, 0x0E, 0x0F, 0x36,
				     0x80, 0x81, 0x82, 0x83, 0x84,
				     0x85, 0xB0, 0xB1, 0xB2, 0xB3 };

	for (i = 0; i < ARRAY_SIZE(reg_dump_addr); i++) {
		u8 addr = reg_dump_addr[i];
		dev_dbg(&ctx->dsi->dev, "Read reg[%02X] = %02X\n", addr,
			ek79007ad_read_cmd_data(ctx, addr));
	}
}
#endif
static int ek79007ad_prepare(struct drm_panel *panel)
{
	struct ek79007ad *ctx = panel_to_ek79007ad(panel);
	unsigned int i;
	u8 reg_b2;
	int ret;

	/* Power the panel */
	ret = regulator_enable(ctx->power);
	if (ret)
		return ret;
	msleep(40);

	/* And reset it */
	gpiod_set_value(ctx->reset, 1);
	msleep(30);

	gpiod_set_value(ctx->reset, 0);
	msleep(60);

#ifdef DEBUG
	ek79007ad_dump_reg(ctx);
#endif
	for (i = 0; i < ctx->desc->init_length; i++) {
		const struct ek79007ad_instr *instr = &ctx->desc->init[i];
		ret = ek79007ad_send_cmd_data(ctx, instr->cmd, instr->data);
		if (ret)
			return ret;
	}

	reg_b2 = 0;
	switch (ctx->dsi->lanes) {
	case 2:
		break;
	case 3:
		reg_b2 |= 0x10;
		break;
	case 4:
		reg_b2 |= 0x30;
		break;
	default:
		return -EINVAL;
	}
	ret = ek79007ad_send_cmd_data(ctx, 0xB2, reg_b2);
	if (ret)
		return ret;

#ifdef DEBUG
	ek79007ad_dump_reg(ctx);
#endif
	ret = mipi_dsi_dcs_set_tear_on(ctx->dsi, MIPI_DSI_DCS_TEAR_MODE_VBLANK);
	if (ret)
		return ret;

	ret = mipi_dsi_dcs_exit_sleep_mode(ctx->dsi);
	if (ret)
		return ret;

	return 0;
}

static int ek79007ad_enable(struct drm_panel *panel)
{
	struct ek79007ad *ctx = panel_to_ek79007ad(panel);

	msleep(30);

	return mipi_dsi_dcs_exit_sleep_mode(ctx->dsi);
}

static int ek79007ad_disable(struct drm_panel *panel)
{
	struct ek79007ad *ctx = panel_to_ek79007ad(panel);

	return mipi_dsi_dcs_enter_sleep_mode(ctx->dsi);
}

static int ek79007ad_unprepare(struct drm_panel *panel)
{
	struct ek79007ad *ctx = panel_to_ek79007ad(panel);
	int ret;

	ret = ek79007ad_send_cmd_data(ctx, 0xB0, 0);
	if (ret < 0) {
		dev_err(&ctx->dsi->dev, "Write fault %d\n", ret);
	}

	mipi_dsi_dcs_enter_sleep_mode(ctx->dsi);
	regulator_disable(ctx->power);
	gpiod_set_value(ctx->reset, 1);

	return 0;
}

static const struct drm_display_mode vklcd07_default_mode = {
	.clock = 51200,

	.hdisplay = 1024,
	.hsync_start = 1024 + 160,
	.hsync_end = 1024 + 160 + 1,
	.htotal = 1024 + 160 + 1 + 160,

	.vdisplay = 600,
	.vsync_start = 600 + 23,
	.vsync_end = 600 + 23 + 1,
	.vtotal = 600 + 23 + 1 + 12,

	.width_mm = 154,
	.height_mm = 86,
};

static int ek79007ad_get_modes(struct drm_panel *panel,
			       struct drm_connector *connector)
{
	struct ek79007ad *ctx = panel_to_ek79007ad(panel);
	struct drm_display_mode *mode;

	mode = drm_mode_duplicate(connector->dev, ctx->desc->mode);
	if (!mode) {
		dev_err(&ctx->dsi->dev, "failed to add mode %ux%ux@%u\n",
			ctx->desc->mode->hdisplay, ctx->desc->mode->vdisplay,
			drm_mode_vrefresh(ctx->desc->mode));
		return -ENOMEM;
	}

	drm_mode_set_name(mode);

	mode->type = DRM_MODE_TYPE_DRIVER | DRM_MODE_TYPE_PREFERRED;
	drm_mode_probed_add(connector, mode);

	connector->display_info.width_mm = mode->width_mm;
	connector->display_info.height_mm = mode->height_mm;

	return 1;
}

static const struct drm_panel_funcs ek79007ad_funcs = {
	.prepare = ek79007ad_prepare,
	.unprepare = ek79007ad_unprepare,
	.enable = ek79007ad_enable,
	.disable = ek79007ad_disable,
	.get_modes = ek79007ad_get_modes,
};

static int ek79007ad_dsi_probe(struct mipi_dsi_device *dsi)
{
	struct ek79007ad *ctx;
	int ret;

	ctx = devm_kzalloc(&dsi->dev, sizeof(*ctx), GFP_KERNEL);
	if (!ctx)
		return -ENOMEM;
	mipi_dsi_set_drvdata(dsi, ctx);
	ctx->dsi = dsi;
	ctx->desc = of_device_get_match_data(&dsi->dev);

	if (ek79007ad_init_vklcd07 == ctx->desc->init) {
		dev_notice(&dsi->dev, "Initialize Vekatech VKLCD07 display\n");
	}
	drm_panel_init(&ctx->panel, &dsi->dev, &ek79007ad_funcs,
		       DRM_MODE_CONNECTOR_DSI);

	ctx->power = devm_regulator_get(&dsi->dev, "power");
	if (IS_ERR(ctx->power)) {
		dev_err(&dsi->dev, "Couldn't get our power regulator\n");
		return PTR_ERR(ctx->power);
	}

	/* The enable GPIO is optional, this pin is MIPI DSI/HDMI switch select input. */
	ctx->enable_gpio =
		devm_gpiod_get_optional(&dsi->dev, "switch", GPIOD_OUT_HIGH);
	if (IS_ERR_OR_NULL(ctx->enable_gpio)) {
		dev_dbg(&dsi->dev, "Couldn't get our switch GPIO\n");
		ctx->enable_gpio = NULL;
	}
	gpiod_set_value(ctx->enable_gpio, 1);

	ctx->reset = devm_gpiod_get(&dsi->dev, "reset", GPIOD_OUT_LOW);
	if (IS_ERR(ctx->reset)) {
		dev_err(&dsi->dev, "Couldn't get our reset GPIO\n");
		return PTR_ERR(ctx->reset);
	}
	ret = of_property_read_u32(dsi->dev.of_node, "dsi-lanes", &dsi->lanes);
	if (ret < 0) {
		dev_dbg(&dsi->dev,
			"Failed to get dsi-lanes property, use default setting - 4 lanes\n");
		dsi->lanes = 4;
	} else {
		dev_dbg(&dsi->dev, "dsi-lanes = %d\n", dsi->lanes);
	}

	ret = drm_panel_of_backlight(&ctx->panel);
	if (ret) {
		dev_err(&dsi->dev, "Couldn't get our backlight(%d)\n", ret);
		return ret;
	}

	drm_panel_add(&ctx->panel);

	/* non-burst mode with sync pulse */
	dsi->mode_flags = MIPI_DSI_MODE_VIDEO_SYNC_PULSE | MIPI_DSI_MODE_LPM;

	dsi->format = MIPI_DSI_FMT_RGB888;

	ret = mipi_dsi_attach(dsi);
	return ret;
}

static void ek79007ad_dsi_remove(struct mipi_dsi_device *dsi)
{
	struct ek79007ad *ctx = mipi_dsi_get_drvdata(dsi);

	mipi_dsi_detach(dsi);
	drm_panel_remove(&ctx->panel);
}

static const struct ek79007ad_desc vklcd07_desc = {
	.init = ek79007ad_init_vklcd07,
	.init_length = ARRAY_SIZE(ek79007ad_init_vklcd07),
	.mode = &vklcd07_default_mode,
};

static const struct of_device_id ek79007ad_of_match[] = {
	{ .compatible = "vekatech,vklcd07", .data = &vklcd07_desc },
	{}
};
MODULE_DEVICE_TABLE(of, ek79007ad_of_match);

static struct mipi_dsi_driver ek79007ad_dsi_driver = {
	.probe = ek79007ad_dsi_probe,
	.remove = ek79007ad_dsi_remove,
	.driver = {
		.name = "ek79007ad-dsi",
		.of_match_table = ek79007ad_of_match,
	},
};
module_mipi_dsi_driver(ek79007ad_dsi_driver);

MODULE_AUTHOR("Stanimir Bonev <bonev.stanimir@gmail.com>");
MODULE_DESCRIPTION("Fitipower ek79007ad Controller Driver");
MODULE_LICENSE("GPL v2");
