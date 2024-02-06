// SPDX-License-Identifier: GPL-2.0+
/*
 * Copyright (C) 2018-2019, Bridge Systems BV
 * Copyright (C) 2023, Utsav Balar <utsavbalar1231@gmail.com>
 *
 * This file based on panel-ronbo-rb070d30.c
 */

#include <linux/backlight.h>
#include <linux/delay.h>
#include <linux/device.h>
#include <linux/err.h>
#include <linux/errno.h>
#include <linux/fb.h>
#include <linux/kernel.h>
#include <linux/media-bus-format.h>
#include <linux/module.h>

#include <linux/gpio/consumer.h>
#include <linux/regulator/consumer.h>

#include <drm/drm_crtc.h>
#include <drm/drm_mipi_dsi.h>
#include <drm/drm_modes.h>
#include <drm/drm_panel.h>
#include <drm/drmP.h>

struct gd3385a_panel {
	struct drm_panel panel;
	struct mipi_dsi_device *dsi;
	struct backlight_device *backlight;
	struct regulator *supply;

	struct {
		struct gpio_desc *power;
		struct gpio_desc *reset;
		struct gpio_desc *updn;
		struct gpio_desc *shlr;
		struct gpio_desc *standby;
	} gpios;
};

static inline struct gd3385a_panel *panel_to_gd3385a_panel(struct drm_panel *panel)
{
	return container_of(panel, struct gd3385a_panel, panel);
}

static int gd3385a_panel_prepare(struct drm_panel *panel)
{
	struct gd3385a_panel *ctx = panel_to_gd3385a_panel(panel);
	int ret;

	ret = regulator_enable(ctx->supply);
	if (ret < 0) {
		DRM_DEV_ERROR(&ctx->dsi->dev, "Failed to enable supply regulator: %d\n", ret);
		return ret;
	}

	msleep(20);
	gpiod_set_value(ctx->gpios.power, 1);
	msleep(20);
	gpiod_set_value(ctx->gpios.reset, 1);
	msleep(20);
	return 0;
}

static int gd3385a_panel_unprepare(struct drm_panel *panel)
{
	struct gd3385a_panel *ctx = panel_to_gd3385a_panel(panel);

	gpiod_set_value(ctx->gpios.reset, 0);
	gpiod_set_value(ctx->gpios.power, 0);
	regulator_disable(ctx->supply);

	return 0;
}

static int gd3385a_panel_enable(struct drm_panel *panel)
{
	struct gd3385a_panel *ctx = panel_to_gd3385a_panel(panel);
	int ret;

	ret = mipi_dsi_dcs_exit_sleep_mode(ctx->dsi);
	if (ret) {
		DRM_DEV_ERROR(&ctx->dsi->dev, "Failed to exit sleep mode: %d\n", ret);
		return ret;
	}

	// Standby mode off
	gpiod_set_value(ctx->gpios.standby, 0);
	// Wait a little for the panel to be ready
	usleep_range(10000, 11000);

	ret = backlight_enable(ctx->backlight);
	if (ret) {
		DRM_DEV_ERROR(&ctx->dsi->dev, "Failed to enable backlight: %d\n", ret);
		goto out;
	}

	return 0;

out:
	mipi_dsi_dcs_enter_sleep_mode(ctx->dsi);
	return ret;
}

static int gd3385a_panel_disable(struct drm_panel *panel)
{
	struct gd3385a_panel *ctx = panel_to_gd3385a_panel(panel);

	backlight_disable(ctx->backlight);

	// Standby mode on
	gpiod_set_value(ctx->gpios.standby, 1);

	return mipi_dsi_dcs_enter_sleep_mode(ctx->dsi);
}

/* Default timings */
static const struct drm_display_mode default_mode = {
	.clock		= 51206,
	.hdisplay	= 1024,
	.hsync_start	= 1024 + 160,
	.hsync_end	= 1024 + 160 + 80,
	.htotal		= 1024 + 160 + 80 + 80,
	.vdisplay	= 600,
	.vsync_start	= 600 + 12,
	.vsync_end	= 600 + 12 + 10,
	.vtotal		= 600 + 12 + 10 + 13,
	.vrefresh	= 60,

	.width_mm	= 154,
	.height_mm	= 85,
};

static int gd3385a_panel_get_modes(struct drm_panel *panel)
{
	struct drm_connector *connector = panel->connector;
	struct gd3385a_panel *ctx = panel_to_gd3385a_panel(panel);
	struct drm_display_mode *mode;
	static const u32 bus_format = MEDIA_BUS_FMT_RGB888_1X24;

	mode = drm_mode_duplicate(panel->drm, &default_mode);
	if (!mode) {
		DRM_DEV_ERROR(&ctx->dsi->dev,
			      "Failed to add mode:\n");
		drm_mode_debug_printmodeline(&default_mode);
		return -EINVAL;
	}

	drm_mode_set_name(mode);

	mode->type = DRM_MODE_TYPE_DRIVER | DRM_MODE_TYPE_PREFERRED;
	drm_mode_probed_add(connector, mode);

	panel->connector->display_info.bpc = 8;
	panel->connector->display_info.width_mm = mode->width_mm;
	panel->connector->display_info.height_mm = mode->height_mm;
	drm_display_info_set_bus_formats(&connector->display_info,
					 &bus_format, 1);

	return 1;
}

static const struct drm_panel_funcs gd3385a_panel_funcs = {
	.get_modes	= gd3385a_panel_get_modes,
	.prepare	= gd3385a_panel_prepare,
	.enable		= gd3385a_panel_enable,
	.disable	= gd3385a_panel_disable,
	.unprepare	= gd3385a_panel_unprepare,
};

static int gd3385a_panel_dsi_probe(struct mipi_dsi_device *dsi)
{
	struct gd3385a_panel *ctx;
	struct device_node *bl_node;
	int ret;

	ctx = devm_kzalloc(&dsi->dev, sizeof(*ctx), GFP_KERNEL);
	if (!ctx)
		return -ENOMEM;

	ctx->supply = devm_regulator_get(&dsi->dev, "vcc-lcd");
	if (IS_ERR(ctx->supply)) {
		DRM_DEV_ERROR(&dsi->dev, "Couldn't get LCD power supply\n");
		return PTR_ERR(ctx->supply);
	}

	mipi_dsi_set_drvdata(dsi, ctx);
	ctx->dsi = dsi;

	drm_panel_init(&ctx->panel);
	ctx->panel.dev = &dsi->dev;
	ctx->panel.funcs = &gd3385a_panel_funcs;

	ctx->gpios.reset = devm_gpiod_get(&dsi->dev, "reset", GPIOD_OUT_LOW);
	if (IS_ERR(ctx->gpios.reset)) {
		DRM_DEV_ERROR(&dsi->dev, "Couldn't get our reset GPIO\n");
		return PTR_ERR(ctx->gpios.reset);
	}

	ctx->gpios.power = devm_gpiod_get(&dsi->dev, "power", GPIOD_OUT_LOW);
	if (IS_ERR(ctx->gpios.power)) {
		DRM_DEV_ERROR(&dsi->dev, "Couldn't get power GPIO\n");
		return PTR_ERR(ctx->gpios.power);
	}

	/*
	 * We don't change the state of that GPIO later on but we need
	 * to force it into a low state.
	 */
	ctx->gpios.updn = devm_gpiod_get(&dsi->dev, "updn", GPIOD_OUT_LOW);
	if (IS_ERR(ctx->gpios.updn)) {
		DRM_DEV_ERROR(&dsi->dev, "Couldn't get updn GPIO\n");
		return PTR_ERR(ctx->gpios.updn);
	}

	/*
	 * We don't change the state of that GPIO later on but we need
	 * to force it into a low state.
	 */
	ctx->gpios.shlr = devm_gpiod_get(&dsi->dev, "shlr", GPIOD_OUT_LOW);
	if (IS_ERR(ctx->gpios.shlr)) {
		DRM_DEV_ERROR(&dsi->dev, "Couldn't get shlr GPIO\n");
		return PTR_ERR(ctx->gpios.shlr);
	}

	ctx->gpios.standby = devm_gpiod_get(&dsi->dev, "standby", GPIOD_OUT_LOW);
	if (IS_ERR(ctx->gpios.standby)) {
		DRM_DEV_ERROR(&dsi->dev, "Couldn't get standby GPIO\n");
		return PTR_ERR(ctx->gpios.standby);
	}

	bl_node = of_parse_phandle(dsi->dev.of_node, "backlight", 0);
	if (!bl_node) {
		DRM_DEV_ERROR(&dsi->dev, "Couldn't find backlight node in DT\n");
		return -EINVAL;
	}
	ctx->backlight = of_find_backlight_by_node(bl_node);
	of_node_put(bl_node);
	if (!ctx->backlight) {
		DRM_DEV_ERROR(&dsi->dev, "Couldn't get backlight\n");
		return -EPROBE_DEFER;
	}

	ret = drm_panel_add(&ctx->panel);
	if (ret < 0) {
		DRM_DEV_ERROR(&dsi->dev, "Failed to add panel: %d\n", ret);
		return ret;
	}

	dsi->mode_flags = MIPI_DSI_MODE_VIDEO | MIPI_DSI_MODE_VIDEO_BURST | MIPI_DSI_MODE_LPM;
	dsi->format = MIPI_DSI_FMT_RGB888;
	dsi->lanes = 4;

	ret = mipi_dsi_attach(dsi);
	if (ret < 0) {
		DRM_DEV_ERROR(&dsi->dev, "Failed to attach to DSI: %d\n", ret);
		drm_panel_remove(&ctx->panel);
		return ret;
	}

	DRM_DEV_INFO(&dsi->dev, "Panel probed successfully\n");
	return 0;
}

static int gd3385a_panel_dsi_remove(struct mipi_dsi_device *dsi)
{
	struct gd3385a_panel *ctx = mipi_dsi_get_drvdata(dsi);

	mipi_dsi_detach(dsi);
	drm_panel_remove(&ctx->panel);

	return 0;
}

static const struct of_device_id gd3385a_panel_of_match[] = {
	{ .compatible = "gelivable,gd3385a" },
	{ /* sentinel */ },
};
MODULE_DEVICE_TABLE(of, gd3385a_panel_of_match);

static struct mipi_dsi_driver gd3385a_panel_driver = {
	.probe = gd3385a_panel_dsi_probe,
	.remove = gd3385a_panel_dsi_remove,
	.driver = {
		.name = "panel-gelivable-gd3385a",
		.of_match_table	= gd3385a_panel_of_match,
	},
};
module_mipi_dsi_driver(gd3385a_panel_driver);

MODULE_AUTHOR("Utsav Balar <utsavbalar1231@gmail.com>");
MODULE_DESCRIPTION("Shenzhen Gelivable Optoelectronics GD3385A Panel Driver");
MODULE_LICENSE("GPL");
