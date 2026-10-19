// SPDX-License-Identifier: GPL-2.0-only
/* Chipone ICNL9916 TCS SPI protocol, derived from Chipone's GPL driver. */
#include <drm/drm_panel.h>
#include <linux/delay.h>
#include <linux/firmware.h>
#include <linux/gpio/consumer.h>
#include <linux/input.h>
#include <linux/input/mt.h>
#include <linux/input/touchscreen.h>
#include <linux/interrupt.h>
#include <linux/module.h>
#include <linux/of.h>
#include <linux/regulator/consumer.h>
#include <linux/spi/spi.h>
#include <linux/unaligned.h>

#define ICNL_CONTACTS 10
#define ICNL_REPORT_SIZE 117

struct icnl_ts {
	struct spi_device *spi;
	struct drm_panel_follower follower;
	bool running;
	struct input_dev *input;
	struct touchscreen_properties prop;
	struct gpio_desc *reset;
	struct regulator *vdd;
	u8 tx[ICNL_REPORT_SIZE];
	u8 rx[ICNL_REPORT_SIZE];
};

/* TCS uses non-reflected CRC-16, polynomial 0x8005, initial value zero. */
static u16 icnl_crc(const u8 *data, size_t len)
{
	u16 crc = 0;
	unsigned int bit;

	while (len--) {
		crc ^= (u16)*data++ << 8;
		for (bit = 0; bit < 8; bit++)
			crc = (crc << 1) ^ ((crc & 0x8000) ? 0x8005 : 0);
	}
	return crc;
}

static int icnl_read(struct icnl_ts *ts, u16 cmd, size_t len, bool report)
{
	size_t total = report ? len : len + 5;
	struct spi_transfer xfer = {
		.tx_buf = ts->tx,
		.rx_buf = ts->rx,
		.len = total,
	};
	int ret;

	if (total > sizeof(ts->rx))
		return -EINVAL;
	memset(ts->tx, 0, sizeof(ts->tx));
	memset(ts->rx, 0, sizeof(ts->rx));
	ts->tx[0] = 0xf1;
	put_unaligned_le16(cmd, ts->tx + 1);
	put_unaligned_le16(len, ts->tx + 3);
	put_unaligned_le16(icnl_crc(ts->tx, 5), ts->tx + 5);
	if (report) {
		ret = spi_sync_transfer(ts->spi, &xfer, 1);
	} else {
		ret = spi_write(ts->spi, ts->tx, 7);
		if (ret)
			return ret;
		usleep_range(100, 200);
		ret = spi_read(ts->spi, ts->rx, total);
		usleep_range(100, 200);
	}
	if (ret)
		return ret;
	if (icnl_crc(ts->rx, total - 2) != get_unaligned_le16(ts->rx + total - 2))
		return -EBADMSG;
	if (ts->rx[total - 5] || get_unaligned_le16(ts->rx + total - 4) != cmd)
		return -EPROTO;
	return 0;
}

/* Program-mode accesses address volatile SRAM and controller registers. */
static int icnl_prog_read(struct icnl_ts *ts, u32 addr, size_t len)
{
	struct spi_transfer xfer = {
		.tx_buf = ts->tx, .rx_buf = ts->rx, .len = len + 5,
	};

	if (len > sizeof(ts->rx) - 5)
		return -EINVAL;
	memset(ts->tx, 0, sizeof(ts->tx));
	ts->tx[0] = 0x61;
	put_unaligned_be24(addr, ts->tx + 1);
	return spi_sync_transfer(ts->spi, &xfer, 1);
}

static int icnl_prog_write(struct icnl_ts *ts, u32 addr,
			   const u8 *data, size_t len)
{
	if (len > sizeof(ts->tx) - 4)
		return -EINVAL;
	ts->tx[0] = 0x60;
	put_unaligned_be24(addr, ts->tx + 1);
	memcpy(ts->tx + 4, data, len);
	return spi_write(ts->spi, ts->tx, len + 4);
}

static u32 icnl_fw_crc(const u8 *data, size_t len)
{
	u32 crc = 0;
	unsigned int bit;

	while (len--) {
		crc ^= (u32)*data++ << 24;
		for (bit = 0; bit < 8; bit++)
			crc = (crc << 1) ^ ((crc & BIT(31)) ? 0x04c11db7 : 0);
	}
	return crc;
}

static int icnl_load_firmware(struct icnl_ts *ts)
{
	const struct firmware *fw;
	u8 meta[16] = { 0 }, mode = 3;
	u32 hwid;
	size_t pos, len, i;
	u8 diff;
	int ret;

	ts->tx[0] = 0xcc;
	ts->tx[1] = 0x33;
	ts->tx[2] = 0x55;
	ts->tx[3] = 0x5a;
	ret = spi_write(ts->spi, ts->tx, 4);
	if (ret)
		return ret;
	usleep_range(5000, 6000);
	ret = icnl_prog_read(ts, 0x30000, 4);
	if (ret)
		return ret;
	hwid = get_unaligned_le32(ts->rx + 5) & 0xfffffff0;
	if (hwid != 0x990160)
		return dev_err_probe(&ts->spi->dev, -ENODEV,
				     "unsupported hardware ID %08x\n", hwid);
	ret = icnl_prog_read(ts, 0x30011, 1);
	if (ret)
		return ret;
	if ((ts->rx[5] & 7) != 2)
		return -EPROTO;
	ret = request_firmware(&fw, "chipone/ICNL9916.bin", &ts->spi->dev);
	if (ret)
		return ret;
	/* Only the stock single-section SRAM image is supported. */
	if (fw->size < 0x102 || fw->size > 0x1fff0) {
		ret = -EINVAL;
		goto out;
	}
	for (pos = 0; pos < fw->size; pos += len) {
		len = min_t(size_t, fw->size - pos, sizeof(ts->rx) - 5);
		ret = icnl_prog_write(ts, pos, fw->data + pos, len);
		if (ret)
			goto out;
		ret = icnl_prog_read(ts, pos, len);
		if (ret)
			goto out;
		diff = 0;
		for (i = 0; i < len; i++)
			diff |= ts->rx[5 + i] ^ fw->data[pos + i];
		if (diff) {
			dev_err(&ts->spi->dev, "SRAM verification failed at %zx\n", pos);
			ret = -EIO;
			goto out;
		}
	}
	put_unaligned_le32(0xcc33555a, meta);
	put_unaligned_le32(icnl_fw_crc(fw->data, fw->size), meta + 8);
	put_unaligned_le32(fw->size, meta + 12);
	ret = icnl_prog_write(ts, 0x1fff0, meta, sizeof(meta));
	if (ret)
		goto out;
	ret = icnl_prog_write(ts, 0x30010, &mode, 1);
	if (ret)
		goto out;
	msleep(30);
	dev_info(&ts->spi->dev, "verified %zu firmware bytes in SRAM\n", fw->size);
 out:
	release_firmware(fw);
	return ret;
}

static irqreturn_t icnl_irq(int irq, void *data)
{
	struct icnl_ts *ts = data;
	unsigned int i, n;
	int ret;

	ret = icnl_read(ts, 0x4103, ICNL_REPORT_SIZE, true);
	if (ret) {
		dev_warn_ratelimited(&ts->spi->dev, "touch packet error: %d\n", ret);
		return IRQ_HANDLED;
	}
	n = ts->rx[1];
	if (n > ICNL_CONTACTS)
		return IRQ_HANDLED;
	/* Validate every slot before changing the input state. */
	for (i = 0; i < n; i++)
		if (ts->rx[2 + i * 7] >= ICNL_CONTACTS)
			return IRQ_HANDLED;
	for (i = 0; i < n; i++) {
		const u8 *msg = ts->rx + 2 + i * 7;
		bool active = msg[6] >= 1 && msg[6] <= 3;

		input_mt_slot(ts->input, msg[0]);
		input_mt_report_slot_state(ts->input, MT_TOOL_FINGER, active);
		if (!active)
			continue;
		touchscreen_report_pos(ts->input, &ts->prop,
				       get_unaligned_le16(msg + 1),
				       get_unaligned_le16(msg + 3), true);
		input_report_abs(ts->input, ABS_MT_PRESSURE, msg[5]);
	}
	input_mt_sync_frame(ts->input);
	input_sync(ts->input);
	return IRQ_HANDLED;
}

static int icnl_panel_unpreparing(struct drm_panel_follower *follower)
{
	struct icnl_ts *ts = container_of(follower, struct icnl_ts, follower);

	if (!ts->running)
		return 0;
	disable_irq(ts->spi->irq);
	input_mt_sync_frame(ts->input);
	input_sync(ts->input);
	gpiod_set_value_cansleep(ts->reset, 1);
	regulator_disable(ts->vdd);
	ts->running = false;
	return 0;
}

static int icnl_panel_prepared(struct drm_panel_follower *follower)
{
	struct icnl_ts *ts = container_of(follower, struct icnl_ts, follower);
	struct device *dev = &ts->spi->dev;
	u16 id;
	int ret;

	ret = regulator_enable(ts->vdd);
	if (ret)
		return ret;
	/* Stock sequence: physical high 1 ms, low 10 ms, high 40 ms. */
	gpiod_set_value_cansleep(ts->reset, 0);
	usleep_range(1000, 2000);
	gpiod_set_value_cansleep(ts->reset, 1);
	usleep_range(10000, 11000);
	gpiod_set_value_cansleep(ts->reset, 0);
	msleep(40);
	ret = icnl_read(ts, 0x4003, 4, false);
	if (ret) {
		ret = icnl_load_firmware(ts);
		if (ret)
			goto fail;
		ret = icnl_read(ts, 0x4003, 4, false);
		if (ret)
			goto fail;
	}
	id = get_unaligned_le16(ts->rx);
	if (id != 0x9916 && id != 0x9959) {
		ret = -ENODEV;
		goto fail;
	}
	dev_info(dev, "TCS firmware ID %04x; touch ready after panel prepare\n", id);
	ts->running = true;
	enable_irq(ts->spi->irq);
	return 0;
fail:
	dev_err(dev, "touch startup after panel prepare failed: %d\n", ret);
	gpiod_set_value_cansleep(ts->reset, 1);
	regulator_disable(ts->vdd);
	return ret;
}

static const struct drm_panel_follower_funcs icnl_panel_funcs = {
	.panel_prepared = icnl_panel_prepared,
	.panel_unpreparing = icnl_panel_unpreparing,
};

static int icnl_probe(struct spi_device *spi)
{
	struct device *dev = &spi->dev;
	struct icnl_ts *ts;
	int ret;

	ts = devm_kzalloc(dev, sizeof(*ts), GFP_KERNEL);
	if (!ts)
		return -ENOMEM;
	ts->spi = spi;
	spi->mode = SPI_MODE_0;
	spi->bits_per_word = 8;
	ret = spi_setup(spi);
	if (ret)
		return ret;
	ts->vdd = devm_regulator_get(dev, "vdd");
	if (IS_ERR(ts->vdd))
		return dev_err_probe(dev, PTR_ERR(ts->vdd), "touch supply\n");
	ts->reset = devm_gpiod_get(dev, "reset", GPIOD_OUT_HIGH);
	if (IS_ERR(ts->reset))
		return dev_err_probe(dev, PTR_ERR(ts->reset), "reset GPIO\n");
	ts->input = devm_input_allocate_device(dev);
	if (!ts->input)
		return -ENOMEM;
	ts->input->name = "Chipone ICNL9916 Touchscreen";
	ts->input->id.bustype = BUS_SPI;
	input_set_abs_params(ts->input, ABS_MT_POSITION_X, 0, 0, 0, 0);
	input_set_abs_params(ts->input, ABS_MT_POSITION_Y, 0, 0, 0, 0);
	input_set_abs_params(ts->input, ABS_MT_PRESSURE, 0, 255, 0, 0);
	touchscreen_parse_properties(ts->input, true, &ts->prop);
	if (!ts->prop.max_x || !ts->prop.max_y)
		return dev_err_probe(dev, -EINVAL,
				     "touchscreen-size-x/y must describe the panel\n");
	ret = input_mt_init_slots(ts->input, ICNL_CONTACTS,
				  INPUT_MT_DIRECT | INPUT_MT_DROP_UNUSED);
	if (ret)
		return ret;
	ret = input_register_device(ts->input);
	if (ret)
		return ret;
	spi_set_drvdata(spi, ts);
	ret = devm_request_threaded_irq(dev, spi->irq, NULL, icnl_irq,
					IRQF_ONESHOT | IRQF_NO_AUTOEN, dev_name(dev), ts);
	if (ret)
		return ret;
	ts->follower.funcs = &icnl_panel_funcs;
	return devm_drm_panel_add_follower(dev, &ts->follower);
}

static const struct of_device_id icnl_of_match[] = {
	{ .compatible = "chipone,icnl9916" },
	{ }
};
MODULE_DEVICE_TABLE(of, icnl_of_match);

static const struct spi_device_id icnl_ids[] = {
	{ "icnl9916", 0 },
	{ }
};
MODULE_DEVICE_TABLE(spi, icnl_ids);

static struct spi_driver icnl_driver = {
	.driver = {
		.name = "chipone-icnl9916",
		.of_match_table = icnl_of_match,
	},
	.probe = icnl_probe,
	.id_table = icnl_ids,
};
module_spi_driver(icnl_driver);
MODULE_DESCRIPTION("Chipone ICNL9916 TCS touchscreen");
MODULE_LICENSE("GPL");
MODULE_FIRMWARE("chipone/ICNL9916.bin");
