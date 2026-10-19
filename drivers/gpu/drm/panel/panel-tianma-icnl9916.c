// SPDX-License-Identifier: GPL-2.0-only
/*
 * Tianma ICNL9916 panel on Motorola penangf.
 * Initialization sequence derived from Motorola's MediaTek panel driver:
 * Copyright (c) 2019 MediaTek Inc.
 */
#include <linux/backlight.h>
#include <linux/delay.h>
#include <linux/gpio/consumer.h>
#include <linux/module.h>
#include <linux/mutex.h>
#include <linux/regulator/consumer.h>
#include <drm/drm_connector.h>
#include <drm/drm_mipi_dsi.h>
#include <drm/drm_modes.h>
#include <drm/drm_panel.h>

struct icnl_panel {
	struct drm_panel panel;
	struct mipi_dsi_device *dsi;
	struct regulator *vddio, *avdd, *avee;
	struct gpio_desc *reset;
	struct mutex lock;
	bool prepared;
};

static inline struct icnl_panel *to_icnl(struct drm_panel *panel)
{
	return container_of(panel, struct icnl_panel, panel);
}

/* Preserve the vendor's generic-versus-DCS packet selection. */
static int icnl_write(struct icnl_panel *ctx, const u8 *data, size_t len)
{
	ssize_t ret;

	if (data[0] < 0xb0)
		ret = mipi_dsi_dcs_write_buffer(ctx->dsi, data, len);
	else
		ret = mipi_dsi_generic_write(ctx->dsi, data, len);
	return ret < 0 ? ret : 0;
}

#define ICNL_WRITE(...) do { \
	const u8 data[] = { __VA_ARGS__ }; \
	if (!ret) \
		ret = icnl_write(ctx, data, sizeof(data)); \
} while (0)

static int icnl_init_sequence(struct icnl_panel *ctx)
{
	int ret = 0;

	ICNL_WRITE(0xf0, 0x5a, 0x59);
	ICNL_WRITE(0xf1, 0xa5, 0xa6);
	ICNL_WRITE(0xcc, 0x02, 0x4b);
	ICNL_WRITE(0xc1, 0x10, 0x20, 0x20, 0x2c, 0x04, 0x28, 0x1e, 0x04, 0x40, 0x06, 0x22, 0x70, 0x30, 0x30, 0x07, 0x11, 0x84, 0x4c, 0x00, 0x93, 0x13, 0x63, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00);
	ICNL_WRITE(0xbb, 0x01, 0x02, 0x03, 0x0a, 0x04, 0x13, 0x14, 0x12);
	ICNL_WRITE(0xc8, 0x46, 0x00, 0x49);
	ICNL_WRITE(0xe0, 0x0c, 0x00, 0xb0, 0x0c, 0x00, 0xc4, 0x09, 0x00, 0x0b, 0x04, 0x01, 0x00, 0x1f, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x1f, 0x0f, 0x09);
	ICNL_WRITE(0xe2, 0x36, 0x24, 0x36, 0xd5, 0x37, 0x86, 0x38, 0x37, 0x38, 0xe8, 0x39, 0x99, 0x3a, 0x1c, 0x3a, 0x9f, 0x3b, 0x22, 0x3b, 0xa5, 0x3c, 0x28, 0x3c, 0xed, 0x3d, 0xb1, 0x3e, 0x76, 0x3f, 0x3a, 0x3f, 0xff);
	ICNL_WRITE(0xe1, 0xd4, 0x0f, 0x1f, 0x2f, 0x3f, 0x4f, 0x5f, 0x6f, 0x7f, 0x90, 0xa0, 0xb0, 0xc0, 0xd0, 0xe0, 0xe8, 0xec, 0xed, 0xee, 0xef, 0xf0, 0xf1, 0xf3, 0xf5, 0xf7, 0xf9, 0xfb, 0xfb, 0xfc, 0xfd, 0xfe, 0xff);
	ICNL_WRITE(0xf1, 0x5a, 0x59);
	ICNL_WRITE(0xf0, 0xa5, 0xa6);
	ICNL_WRITE(0x53, 0x2c);
	ICNL_WRITE(0x55, 0x01);
	ICNL_WRITE(0x35, 0x00, 0x00);
	ICNL_WRITE(0x11, 0x00, 0x00);
	msleep(110);
	ICNL_WRITE(0x29, 0x00, 0x00);
	usleep_range(10000, 11000);
	ICNL_WRITE(0x6d, 0x02, 0x00);
	return ret;
}

static void icnl_poweroff(struct icnl_panel *ctx)
{
	gpiod_set_value_cansleep(ctx->reset, 1);
	regulator_disable(ctx->avee);
	usleep_range(5000, 6000);
	regulator_disable(ctx->avdd);
	regulator_disable(ctx->vddio);
}

static int icnl_prepare(struct drm_panel *panel)
{
	struct icnl_panel *ctx = to_icnl(panel);
	int ret;

	mutex_lock(&ctx->lock);
	ret = regulator_enable(ctx->vddio);
	if (ret)
		goto unlock;
	ret = regulator_enable(ctx->avdd);
	if (ret)
		goto disable_vddio;
	usleep_range(8000, 9000);
	gpiod_set_value_cansleep(ctx->reset, 0);
	usleep_range(10000, 11000);
	gpiod_set_value_cansleep(ctx->reset, 1);
	usleep_range(1000, 2000);
	gpiod_set_value_cansleep(ctx->reset, 0);
	usleep_range(10000, 11000);
	ret = regulator_enable(ctx->avee);
	if (ret)
		goto disable_avdd;
	usleep_range(10000, 11000);
	gpiod_set_value_cansleep(ctx->reset, 1);
	usleep_range(2000, 3000);
	gpiod_set_value_cansleep(ctx->reset, 0);
	usleep_range(10000, 11000);
	ret = icnl_init_sequence(ctx);
	if (ret) {
		dev_err(&ctx->dsi->dev, "panel initialization failed: %d\n", ret);
		icnl_poweroff(ctx);
		goto unlock;
	}
	/* Stock requires a delay before the first brightness command. */
	msleep(30);
	ctx->prepared = true;
	dev_info(&ctx->dsi->dev, "full panel initialization complete\n");
	goto unlock;
disable_avdd:
	gpiod_set_value_cansleep(ctx->reset, 1);
	regulator_disable(ctx->avdd);
disable_vddio:
	regulator_disable(ctx->vddio);
unlock:
	mutex_unlock(&ctx->lock);
	return ret;
}

static int icnl_sleep_sequence(struct icnl_panel *ctx)
{
	int ret = 0;

	ICNL_WRITE(0xf0, 0x5a, 0x59);
	ICNL_WRITE(0xf1, 0xa5, 0xa6);
	ICNL_WRITE(0xbb, 0x01, 0x02, 0x03, 0x0a, 0x04, 0x13, 0x14, 0x12);
	ICNL_WRITE(0xf1, 0x5a, 0x59);
	ICNL_WRITE(0xf0, 0xa5, 0xa6);
	ICNL_WRITE(0x6d, 0x25, 0x00);
	ICNL_WRITE(0x28, 0x00, 0x00);
	usleep_range(10000, 11000);
	ICNL_WRITE(0x10, 0x00, 0x00);
	msleep(120);
	return ret;
}

static int icnl_unprepare(struct drm_panel *panel)
{
	struct icnl_panel *ctx = to_icnl(panel);
	int ret;

	mutex_lock(&ctx->lock);
	ret = icnl_sleep_sequence(ctx);
	if (ret)
		dev_warn(&ctx->dsi->dev, "sleep sequence failed: %d\n", ret);
	/* Even on a transfer failure, reset and release the power rails. */
	ctx->prepared = false;
	icnl_poweroff(ctx);
	mutex_unlock(&ctx->lock);
	return 0;
}

static int icnl_update_brightness(struct backlight_device *bl)
{
	struct icnl_panel *ctx = bl_get_data(bl);
	int ret = 0;

	mutex_lock(&ctx->lock);
	if (ctx->prepared)
		ret = mipi_dsi_dcs_set_display_brightness_large(ctx->dsi,
						backlight_get_brightness(bl));
	mutex_unlock(&ctx->lock);
	return ret;
}

static const struct backlight_ops icnl_backlight_ops = {
	.update_status = icnl_update_brightness,
};

/*
 * 912 Mbps and the MT6768 packet timings give a measured line rate of
 * approximately 177294 Hz. These vertical totals produce 60/90 Hz while
 * preserving the vendor active area, sync widths and horizontal porches.
 */
static const struct drm_display_mode icnl_modes[] = {
	{
		.clock = 140776,
		.hdisplay = 720,
		.hsync_start = 750,
		.hsync_end = 754,
		.htotal = 794,
		.vdisplay = 1600,
		.vsync_start = 2919,
		.vsync_end = 2923,
		.vtotal = 2955,
	}, {
		.clock = 140776,
		.hdisplay = 720,
		.hsync_start = 750,
		.hsync_end = 754,
		.htotal = 794,
		.vdisplay = 1600,
		.vsync_start = 1934,
		.vsync_end = 1938,
		.vtotal = 1970,
	},
};

static int icnl_get_modes(struct drm_panel *panel, struct drm_connector *connector)
{
	struct drm_display_mode *mode;
	unsigned int i;

	for (i = 0; i < ARRAY_SIZE(icnl_modes); i++) {
		mode = drm_mode_duplicate(connector->dev, &icnl_modes[i]);
		if (!mode)
			return -ENOMEM;
		mode->type = DRM_MODE_TYPE_DRIVER;
		if (!i)
			mode->type |= DRM_MODE_TYPE_PREFERRED;
		drm_mode_set_name(mode);
		drm_mode_probed_add(connector, mode);
	}
	connector->display_info.width_mm = 64;
	connector->display_info.height_mm = 129;
	connector->display_info.bpc = 8;
	return ARRAY_SIZE(icnl_modes);
}

static const struct drm_panel_funcs icnl_funcs = {
	.prepare = icnl_prepare,
	.unprepare = icnl_unprepare,
	.get_modes = icnl_get_modes,
};

static int icnl_probe(struct mipi_dsi_device *dsi)
{
	struct device *dev = &dsi->dev;
	struct backlight_properties props = {
		.type = BACKLIGHT_RAW,
		/* Stay within the stock non-HBM brightness range. */
		.max_brightness = 1432,
		.brightness = 511,
	};
	struct icnl_panel *ctx;
	int ret;

	ctx = devm_kzalloc(dev, sizeof(*ctx), GFP_KERNEL);
	if (!ctx)
		return -ENOMEM;
	ctx->dsi = dsi;
	mutex_init(&ctx->lock);
	ctx->vddio = devm_regulator_get(dev, "vddio");
	if (IS_ERR(ctx->vddio))
		return dev_err_probe(dev, PTR_ERR(ctx->vddio), "vddio supply\n");
	ctx->avdd = devm_regulator_get(dev, "avdd");
	if (IS_ERR(ctx->avdd))
		return dev_err_probe(dev, PTR_ERR(ctx->avdd), "positive bias supply\n");
	ctx->avee = devm_regulator_get(dev, "avee");
	if (IS_ERR(ctx->avee))
		return dev_err_probe(dev, PTR_ERR(ctx->avee), "negative bias supply\n");
	ctx->reset = devm_gpiod_get(dev, "reset", GPIOD_OUT_HIGH);
	if (IS_ERR(ctx->reset))
		return dev_err_probe(dev, PTR_ERR(ctx->reset), "reset GPIO\n");
	drm_panel_init(&ctx->panel, dev, &icnl_funcs, DRM_MODE_CONNECTOR_DSI);
	ctx->panel.prepare_prev_first = true;
	ctx->panel.backlight = devm_backlight_device_register(dev, dev_name(dev),
				dev, ctx, &icnl_backlight_ops, &props);
	if (IS_ERR(ctx->panel.backlight))
		return dev_err_probe(dev, PTR_ERR(ctx->panel.backlight), "backlight\n");
	/* Vendor static D-PHY lane rate for this panel. */
	dsi->hs_rate = 912000000;
	dsi->lanes = 4;
	dsi->format = MIPI_DSI_FMT_RGB888;
	dsi->mode_flags = MIPI_DSI_MODE_VIDEO | MIPI_DSI_MODE_VIDEO_BURST |
			  MIPI_DSI_MODE_LPM | MIPI_DSI_CLOCK_NON_CONTINUOUS;
	mipi_dsi_set_drvdata(dsi, ctx);
	drm_panel_add(&ctx->panel);
	ret = mipi_dsi_attach(dsi);
	if (ret)
		drm_panel_remove(&ctx->panel);
	return ret;
}

static void icnl_remove(struct mipi_dsi_device *dsi)
{
	struct icnl_panel *ctx = mipi_dsi_get_drvdata(dsi);

	if (ctx->panel.enabled)
		drm_panel_disable(&ctx->panel);
	if (ctx->panel.prepared)
		drm_panel_unprepare(&ctx->panel);
	mipi_dsi_detach(dsi);
	drm_panel_remove(&ctx->panel);
}

static const struct of_device_id icnl_of_match[] = {
	{ .compatible = "tianma,icnl9916-penangf" },
	{ }
};
MODULE_DEVICE_TABLE(of, icnl_of_match);

static struct mipi_dsi_driver icnl_driver = {
	.probe = icnl_probe,
	.remove = icnl_remove,
	.driver = {
		.name = "panel-tianma-icnl9916",
		.of_match_table = icnl_of_match,
	},
};
module_mipi_dsi_driver(icnl_driver);

MODULE_DESCRIPTION("Tianma ICNL9916 panel for Motorola penangf");
MODULE_LICENSE("GPL");
