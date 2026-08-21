// SPDX-License-Identifier: GPL-2.0-only
/*
 * Airoha EN7572 BOSA controller
 *
 * The hardware initialization sequence and register definitions are derived
 * from Airoha's LDDLA SDK driver. The Linux integration uses standard I2C,
 * firmware, nvmem, GPIO and sysfs interfaces.
 */

#include <linux/ctype.h>
#include <linux/delay.h>
#include <linux/firmware.h>
#include <linux/gpio/consumer.h>
#include <linux/i2c.h>
#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/mutex.h>
#include <linux/nvmem-consumer.h>
#include <linux/property.h>
#include <linux/slab.h>
#include <linux/spinlock.h>
#include <linux/sysfs.h>
#include <linux/unaligned.h>

#include "airoha-en7572.h"
#include "en7572-calibration.h"
#include "en7572-tx-eye.h"

#define EN7572_A0_ADDR			0x50
#define EN7572_A2_ADDR			0x51

#define EN7572_ID1_REG			0x0408
#define EN7572_ID2_REG			0x040a
#define EN7572_ID1			0x1388

#define EN7572_MD32_PM_CFG		0x3000
#define EN7572_MD32_PM_ADDR		0x3004
#define EN7572_MD32_PM_DATA		0x3008
#define EN7572_MD32_DM_CFG		0x300c
#define EN7572_MD32_DM_ADDR		0x3010
#define EN7572_MD32_DM_DATA		0x3014
#define EN7572_MD32_ENABLE		0x3018

#define EN7572_OCP_CTRL			0x0160
#define EN7572_APD_CTRL			0x015c
#define EN7572_RESET_CTRL		0x0200
#define EN7572_FW_VERSION		0x0082
#define EN7572_MCU_IDLE			0x0083
#define EN7572_LOS_STATUS		0x00fa
#define EN7572_ALARM_FLAGS		0x0070
#define EN7572_WARNING_FLAGS		0x0074

/* SFF-8472 compatible real-time diagnostics maintained by the MD32. */
#define EN7572_DIAGNOSTICS_REG		0x0060
#define EN7572_DIAGNOSTICS_SIZE		10

#define EN7572_PM_SIZE			SZ_16K
#define EN7572_DM_SIZE			SZ_4K
#define EN7572_BOB_DM_ADDR		0x600

#define EN7572_DEFAULT_PM_FW		"airoha/xg2010g/en7572-A60993.pm"
#define EN7572_DEFAULT_DM_FW		"airoha/xg2010g/en7572-A60993.dm"
#define EN7572_DEFAULT_GPON_BOB_FW	"airoha/xg2010g/en7572-bob-gpon.bin"
#define EN7572_DEFAULT_10G_BOB_FW	"airoha/xg2010g/en7572-bob-xgspon.bin"

struct airoha_en7572 {
	struct i2c_client *a0;
	struct i2c_client *a2;
	struct gpio_desc *tx_disable;
	struct mutex lock;
	spinlock_t tx_lock;
	bool initialized;
	bool tx_is_disabled;
	bool fault_locked;
	bool calibration_valid;
	bool calibration_from_nvmem;
	bool calibration_a2_as_a0;
	u16 id1;
	u16 id2;
	u8 fw_version;
	enum airoha_xpon_mode pon_mode;
	unsigned int calibration_bank;
	u8 calibration[EN7572_BOB_SIZE];
	char calibration_source[32];
	char vendor[17];
	char part_number[17];
};

static int en7572_read(struct i2c_client *client, u16 reg, void *data,
			 size_t len)
{
	u8 address[] = { reg >> 8, reg & 0xff };
	struct i2c_msg messages[] = {
		{
			.addr = client->addr,
			.flags = 0,
			.len = sizeof(address),
			.buf = address,
		},
		{
			.addr = client->addr,
			.flags = I2C_M_RD,
			.len = len,
			.buf = data,
		},
	};
	int ret;

	ret = i2c_transfer(client->adapter, messages, ARRAY_SIZE(messages));
	if (ret == ARRAY_SIZE(messages))
		return 0;

	return ret < 0 ? ret : -EIO;
}

static int en7572_write(struct i2c_client *client, u16 reg, const void *data,
			  size_t len)
{
	u8 *buffer;
	int ret;

	buffer = kmalloc(len + 2, GFP_KERNEL);
	if (!buffer)
		return -ENOMEM;

	buffer[0] = reg >> 8;
	buffer[1] = reg & 0xff;
	memcpy(buffer + 2, data, len);

	ret = i2c_master_send(client, buffer, len + 2);
	kfree(buffer);
	if (ret == len + 2)
		return 0;

	return ret < 0 ? ret : -EIO;
}

static int en7572_read_u8(struct i2c_client *client, u16 reg, u8 *value)
{
	return en7572_read(client, reg, value, sizeof(*value));
}

static int en7572_read_le16(struct i2c_client *client, u16 reg, u16 *value)
{
	__le16 raw;
	int ret;

	ret = en7572_read(client, reg, &raw, sizeof(raw));
	if (!ret)
		*value = le16_to_cpu(raw);

	return ret;
}

static int en7572_write_le16(struct i2c_client *client, u16 reg, u16 value)
{
	__le16 raw = cpu_to_le16(value);

	return en7572_write(client, reg, &raw, sizeof(raw));
}

static int en7572_write_le32(struct i2c_client *client, u16 reg, u32 value)
{
	__le32 raw = cpu_to_le32(value);

	return en7572_write(client, reg, &raw, sizeof(raw));
}

static int en7572_read_le32(struct i2c_client *client, u16 reg, u32 *value)
{
	__le32 raw;
	int ret;

	ret = en7572_read(client, reg, &raw, sizeof(raw));
	if (!ret)
		*value = le32_to_cpu(raw);

	return ret;
}

static int en7572_update_bits(struct airoha_en7572 *priv, u16 reg, u32 mask,
			       u32 value)
{
	u8 raw[4];
	u32 old_value;
	int ret;

	ret = en7572_read(priv->a2, reg, raw, sizeof(raw));
	if (ret)
		return ret;

	old_value = get_unaligned_le32(raw);
	value = (old_value & ~mask) | (value & mask);
	put_unaligned_le32(value, raw);

	return en7572_write(priv->a2, reg, raw, sizeof(raw));
}

static u32 en7572_field_value(u32 mask, u32 value)
{
	return (value << __ffs(mask)) & mask;
}

static int en7572_read_tx_eye(struct airoha_en7572 *priv,
			      struct en7572_tx_eye_fingerprint *eye)
{
	int ret;

	ret = en7572_read_le32(priv->a2, EN7572_BEN_CTRL, &eye->ben_ctrl);
	if (ret)
		return ret;
	ret = en7572_read_le32(priv->a2, EN7572_DCL_CTRL2, &eye->dcl_ctrl2);
	if (ret)
		return ret;
	ret = en7572_read_le32(priv->a2, EN7572_APC_CTRL, &eye->apc_ctrl);
	if (ret)
		return ret;
	ret = en7572_read_le32(priv->a2, EN7572_TIA_CTRL, &eye->tia_ctrl);
	if (ret)
		return ret;
	ret = en7572_read_le32(priv->a2, EN7572_ERC_CTRL, &eye->erc_ctrl);
	if (ret)
		return ret;
	ret = en7572_read_le32(priv->a2, EN7572_PGA_CTRL, &eye->pga_ctrl);
	if (ret)
		return ret;
	ret = en7572_read_le32(priv->a2, EN7572_BOB_TSSI_OFFSET, &eye->tssi);
	if (ret)
		return ret;

	return en7572_read_le32(priv->a2, EN7572_LOOP_CTRL, &eye->loop_ctrl);
}

static int en7572_verify_tx_eye(struct airoha_en7572 *priv, const u8 *bank)
{
	struct en7572_tx_eye_fingerprint expected, actual;
	unsigned int mismatch;
	int ret;

	en7572_tx_eye_from_bank(bank, &expected);
	ret = en7572_read_tx_eye(priv, &actual);
	if (ret)
		return ret;

	mismatch = en7572_tx_eye_mismatch(&expected, &actual);
	if (!mismatch)
		return 0;

	dev_err_ratelimited(&priv->a0->dev,
			    "active TX-eye readback mismatch: %#x\n", mismatch);
	return -EIO;
}

static int en7572_calibration_bank_for_mode(enum airoha_xpon_mode mode,
						     unsigned int *bank)
{
	switch (mode) {
	case AIROHA_XPON_MODE_XGPON:
	case AIROHA_XPON_MODE_XGSPON:
		/* SDK LDDLA mode 0 (XG/XGS) selects the A2 TX eye. */
		*bank = EN7572_BOB_BANK_A2;
		return 0;
	case AIROHA_XPON_MODE_EPON_10G_1G:
	case AIROHA_XPON_MODE_EPON_10G_10G:
		/* SDK LDDLA mode 1 (XE/XES) selects the A0 TX eye. */
		*bank = EN7572_BOB_BANK_A0;
		return 0;
	default:
		return -EOPNOTSUPP;
	}
}

static bool en7572_mode_uses_shared_10g_bob(enum airoha_xpon_mode mode)
{
	return mode == AIROHA_XPON_MODE_XGPON ||
	       mode == AIROHA_XPON_MODE_XGSPON ||
	       mode == AIROHA_XPON_MODE_EPON_10G_1G ||
	       mode == AIROHA_XPON_MODE_EPON_10G_10G;
}

static void en7572_set_calibration_source(struct airoha_en7572 *priv,
					  enum airoha_xpon_mode mode,
					  unsigned int bank)
{
	const struct airoha_xpon_mode_descriptor *descriptor;

	descriptor = airoha_xpon_mode_descriptor(mode);
	if (en7572_mode_uses_shared_10g_bob(mode) &&
	    bank == EN7572_BOB_BANK_A0 &&
	    !priv->calibration_from_nvmem && priv->calibration_a2_as_a0)
		strscpy(priv->calibration_source, "experimental-a2-copy:a0",
			sizeof(priv->calibration_source));
	else if (en7572_mode_uses_shared_10g_bob(mode))
		snprintf(priv->calibration_source,
			 sizeof(priv->calibration_source), "%s:10g:%s",
			 priv->calibration_from_nvmem ? "nvmem" : "firmware",
			 bank == EN7572_BOB_BANK_A2 ? "a2" : "a0");
	else
		snprintf(priv->calibration_source,
			 sizeof(priv->calibration_source), "%s:%s",
			 priv->calibration_from_nvmem ? "nvmem" : "firmware",
			 descriptor ? descriptor->name : "invalid");
}

static int en7572_load_firmware(struct airoha_en7572 *priv, const char *property,
				 const char *default_name, void *buffer,
				 size_t max_size)
{
	const struct firmware *firmware;
	const char *name = default_name;
	int ret;

	device_property_read_string(&priv->a0->dev, property, &name);
	ret = request_firmware(&firmware, name, &priv->a0->dev);
	if (ret) {
		dev_err(&priv->a0->dev, "failed to load %s: %pe\n", name,
			ERR_PTR(ret));
		return ret;
	}

	if (!firmware->size || firmware->size > max_size) {
		dev_err(&priv->a0->dev, "%s has invalid size %zu (maximum %zu)\n",
			name, firmware->size, max_size);
		ret = -EINVAL;
		goto out;
	}

	memcpy(buffer, firmware->data, firmware->size);
	dev_info(&priv->a0->dev, "loaded %s (%zu bytes)\n", name,
		 firmware->size);

out:
	release_firmware(firmware);
	return ret;
}

static int en7572_load_calibration(struct airoha_en7572 *priv, u8 *bob)
{
	struct device *dev = &priv->a0->dev;
	const struct airoha_xpon_mode_descriptor *descriptor;
	struct nvmem_cell *cell;
	const struct firmware *firmware;
	const char *property;
	const char *cell_name;
	const char *name;
	unsigned int required_bank = 0;
	bool require_bank = false;
	void *data;
	size_t len;
	int ret;

	descriptor = airoha_xpon_mode_descriptor(priv->pon_mode);
	if (!descriptor)
		return -EINVAL;

	switch (priv->pon_mode) {
	case AIROHA_XPON_MODE_GPON:
		name = EN7572_DEFAULT_GPON_BOB_FW;
		property = "airoha,calibration-gpon-firmware";
		cell_name = "calibration-gpon";
		break;
	case AIROHA_XPON_MODE_XGPON:
	case AIROHA_XPON_MODE_XGSPON:
	case AIROHA_XPON_MODE_EPON_10G_1G:
	case AIROHA_XPON_MODE_EPON_10G_10G:
		name = EN7572_DEFAULT_10G_BOB_FW;
		property = "airoha,calibration-10g-firmware";
		cell_name = "calibration-10g";
		if (en7572_calibration_bank_for_mode(priv->pon_mode,
						       &required_bank))
			return -EINVAL;
		require_bank = true;
		break;
	default:
		return -EINVAL;
	}

	cell = NULL;
	/* The explicit lab override must not be shadowed by an A2-only ART cell. */
	if (!(priv->calibration_a2_as_a0 && require_bank) &&
	    device_property_present(dev, "nvmem-cells"))
		cell = nvmem_cell_get(dev, cell_name);
	if (cell && !IS_ERR(cell)) {
		data = nvmem_cell_read(cell, &len);
		if (IS_ERR(data)) {
			ret = PTR_ERR(data);
			if (ret == -EPROBE_DEFER) {
				nvmem_cell_put(cell);
				return ret;
			}
			dev_warn(dev, "failed to read nvmem calibration: %pe\n",
				 ERR_PTR(ret));
		} else {
				if ((!require_bank &&
				     en7572_calibration_factory_record_valid(data, len)) ||
				    (require_bank &&
				     en7572_calibration_factory_record_bank_valid(
					data, len, required_bank))) {
					memcpy(bob, data, EN7572_BOB_SIZE);
					priv->calibration_bank = required_bank;
					priv->calibration_from_nvmem = true;
					en7572_set_calibration_source(priv,
						priv->pon_mode, required_bank);
				kfree(data);
				nvmem_cell_put(cell);
				return 0;
			}
			dev_warn(dev, "%s has invalid format or size %zu\n",
				 cell_name, len);
			kfree(data);
		}
		nvmem_cell_put(cell);
	} else if (IS_ERR(cell)) {
		ret = PTR_ERR(cell);
		if (ret == -EPROBE_DEFER)
			return ret;
		dev_warn(dev, "%s nvmem cell is unavailable: %pe\n",
			 cell_name, ERR_CAST(cell));
	}

	device_property_read_string(dev, property, &name);
	ret = request_firmware(&firmware, name, dev);
	if (ret) {
		dev_err(dev, "failed to load fallback calibration %s: %pe\n",
			name, ERR_PTR(ret));
		return ret;
	}

	if ((!require_bank &&
	     !en7572_calibration_valid(firmware->data, firmware->size)) ||
	    (require_bank &&
	     !en7572_calibration_bank_index_valid(firmware->data,
						    firmware->size,
						    required_bank))) {
		if (require_bank)
			dev_err(dev, "fallback calibration %s lacks valid %s bank\n",
				name, required_bank == EN7572_BOB_BANK_A2 ?
				"A2" : "A0");
		else
			dev_err(dev, "fallback calibration %s is invalid\n", name);
		ret = require_bank ? -ENODATA : -EINVAL;
		goto out;
	}

	memcpy(bob, firmware->data, EN7572_BOB_SIZE);
	priv->calibration_bank = required_bank;
	priv->calibration_from_nvmem = false;
	en7572_set_calibration_source(priv, priv->pon_mode, required_bank);

out:
	release_firmware(firmware);
	return ret;
}

static int en7572_select_tx_eye(struct airoha_en7572 *priv, const u8 *bob,
				enum airoha_xpon_mode mode)
{
	struct en7572_tx_eye_fingerprint eye;
	const u8 *bank;
	unsigned int bank_index;
	int ret;

	ret = en7572_calibration_bank_for_mode(mode, &bank_index);
	if (ret == -EOPNOTSUPP)
		return 0;
	if (ret)
		return ret;
	if (!en7572_calibration_bank_index_valid(bob, EN7572_BOB_SIZE,
						    bank_index))
		return -ENODATA;

	bank = bob + bank_index * EN7572_BOB_BANK_SIZE;
	en7572_tx_eye_from_bank(bank, &eye);
	ret = en7572_update_bits(priv, EN7572_BEN_CTRL, EN7572_BEN_MODE,
		en7572_field_value(EN7572_BEN_MODE, EN7572_BEN_OFF));
	if (ret)
		return ret;

	ret = en7572_update_bits(priv, EN7572_DCL_CTRL2,
		EN7572_DCL_IMOD | EN7572_DCL_IAV, eye.dcl_ctrl2);
	if (ret)
		goto disable_ben;

	ret = en7572_update_bits(priv, EN7572_APC_CTRL, EN7572_APC_DAC,
		eye.apc_ctrl);
	if (ret)
		goto disable_ben;

	ret = en7572_update_bits(priv, EN7572_TIA_CTRL,
		EN7572_TIA_CURRENT | EN7572_TIA_GAIN_BW, eye.tia_ctrl);
	if (ret)
		goto disable_ben;

	ret = en7572_update_bits(priv, EN7572_ERC_CTRL,
		EN7572_ERC_CDAC | EN7572_ERC_DAC, eye.erc_ctrl);
	if (ret)
		goto disable_ben;

	ret = en7572_update_bits(priv, EN7572_PGA_CTRL,
		EN7572_PGA_GAIN | EN7572_PGA_CAP, eye.pga_ctrl);
	if (ret)
		goto disable_ben;

	ret = en7572_write_le32(priv->a2, EN7572_BOB_TSSI_OFFSET, eye.tssi);
	if (ret)
		goto disable_ben;
	ret = en7572_update_bits(priv, EN7572_LOOP_CTRL,
				 EN7572_LOOP_ENABLE, 0);
	if (ret)
		goto disable_ben;
	ret = en7572_update_bits(priv, EN7572_LOOP_CTRL,
				 EN7572_LOOP_ENABLE, EN7572_LOOP_ENABLE);
	if (ret)
		goto disable_ben;

	ret = en7572_update_bits(priv, EN7572_BEN_CTRL, EN7572_BEN_MODE,
		en7572_field_value(EN7572_BEN_MODE, EN7572_BEN_NORMAL));
	if (ret)
		goto disable_ben;
	ret = en7572_verify_tx_eye(priv, bank);
	if (ret)
		goto disable_ben;
	priv->calibration_bank = bank_index;
	return ret;

disable_ben:
	en7572_update_bits(priv, EN7572_BEN_CTRL, EN7572_BEN_MODE,
		en7572_field_value(EN7572_BEN_MODE, EN7572_BEN_OFF));
	return ret;
}

static int en7572_upload_words(struct airoha_en7572 *priv, u16 data_register,
				const u8 *data, size_t len)
{
	size_t offset;
	int ret;

	for (offset = 0; offset < len; offset += sizeof(u32)) {
		ret = en7572_write(priv->a2, data_register, data + offset,
				    sizeof(u32));
		if (ret)
			return ret;
	}

	return 0;
}

static int en7572_upload_firmware(struct airoha_en7572 *priv, const u8 *pm,
				   const u8 *dm, const u8 *bob)
{
	int ret;

	ret = en7572_update_bits(priv, EN7572_MD32_PM_CFG, BIT(0), BIT(0));
	if (ret)
		return ret;
	ret = en7572_write_le32(priv->a2, EN7572_MD32_PM_ADDR, 0);
	if (ret)
		return ret;
	ret = en7572_upload_words(priv, EN7572_MD32_PM_DATA, pm,
				  EN7572_PM_SIZE);
	if (ret)
		return ret;

	ret = en7572_update_bits(priv, EN7572_MD32_DM_CFG, BIT(0), BIT(0));
	if (ret)
		return ret;
	ret = en7572_write_le32(priv->a2, EN7572_MD32_DM_ADDR, 0);
	if (ret)
		return ret;
	ret = en7572_upload_words(priv, EN7572_MD32_DM_DATA, dm,
				  EN7572_DM_SIZE);
	if (ret)
		return ret;

	ret = en7572_update_bits(priv, EN7572_MD32_DM_CFG, BIT(0), BIT(0));
	if (ret)
		return ret;
	ret = en7572_write_le32(priv->a2, EN7572_MD32_DM_ADDR,
				EN7572_BOB_DM_ADDR);
	if (ret)
		return ret;

	return en7572_upload_words(priv, EN7572_MD32_DM_DATA, bob,
				   EN7572_BOB_SIZE);
}

static void en7572_copy_ascii(char *destination, size_t destination_size,
			      const u8 *source, size_t source_size)
{
	size_t i;

	for (i = 0; i < source_size && i + 1 < destination_size; i++)
		destination[i] = isprint(source[i]) ? source[i] : '.';
	destination[i] = '\0';

	while (i && destination[i - 1] == ' ')
		destination[--i] = '\0';
}

static int en7572_initialize(struct airoha_en7572 *priv)
{
	static const u8 alarm_thresholds[] = {
		0x64, 0x00, 0xce, 0x00, 0x64, 0x00, 0xce, 0x00,
		0x90, 0x88, 0x71, 0x48, 0x8e, 0x94, 0x73, 0x3c,
		0xa6, 0x05, 0x01, 0xf4, 0x9c, 0x40, 0x02, 0xee,
		0xff, 0xff, 0x00, 0x00, 0xff, 0xff, 0x00, 0x00,
		0x31, 0x24, 0x00, 0x01, 0x27, 0x10, 0x00, 0x03,
	};
	u8 *pm, *dm, *bob;
	u8 threshold_probe[4];
	int ret;

	pm = kzalloc(EN7572_PM_SIZE, GFP_KERNEL);
	dm = kzalloc(EN7572_DM_SIZE, GFP_KERNEL);
	bob = kzalloc(EN7572_BOB_SIZE, GFP_KERNEL);
	if (!pm || !dm || !bob) {
		ret = -ENOMEM;
		goto out;
	}

	ret = en7572_load_firmware(priv, "airoha,pm-firmware",
				   EN7572_DEFAULT_PM_FW, pm, EN7572_PM_SIZE);
	if (ret)
		goto out;
	ret = en7572_load_firmware(priv, "airoha,dm-firmware",
				   EN7572_DEFAULT_DM_FW, dm, EN7572_DM_SIZE);
	if (ret)
		goto out;
	ret = en7572_load_calibration(priv, bob);
	if (ret)
		goto out;

	en7572_copy_ascii(priv->vendor, sizeof(priv->vendor), bob + 20, 16);
	en7572_copy_ascii(priv->part_number, sizeof(priv->part_number), bob + 40, 16);

	ret = en7572_update_bits(priv, EN7572_MD32_ENABLE, BIT(0), 0);
	if (ret)
		goto out;
	ret = en7572_update_bits(priv, EN7572_OCP_CTRL, BIT(30), 0);
	if (ret)
		goto out;
	ret = en7572_update_bits(priv, EN7572_APD_CTRL, BIT(8), 0);
	if (ret)
		goto out;
	msleep(100);

	ret = en7572_update_bits(priv, EN7572_RESET_CTRL, GENMASK(31, 30), 0);
	if (ret)
		goto out;
	ret = en7572_update_bits(priv, EN7572_RESET_CTRL, GENMASK(31, 30),
				 GENMASK(31, 30));
	if (ret)
		goto out;

	ret = en7572_upload_firmware(priv, pm, dm, bob);
	if (ret)
		goto out;

	ret = en7572_read(priv->a2, 0, threshold_probe,
			   sizeof(threshold_probe));
	if (ret)
		goto out;
	if (get_unaligned_le32(threshold_probe) == U32_MAX) {
		ret = en7572_write(priv->a2, 0, alarm_thresholds,
				    sizeof(alarm_thresholds));
		if (ret)
			goto out;
	}

	ret = en7572_write_le16(priv->a2, EN7572_ALARM_FLAGS, 0);
	if (ret)
		goto out;
	ret = en7572_write_le16(priv->a2, EN7572_WARNING_FLAGS, 0);
	if (ret)
		goto out;

	ret = en7572_update_bits(priv, EN7572_MD32_ENABLE, BIT(0), BIT(0));
	if (ret)
		goto out;

	msleep(20);
	ret = en7572_read_u8(priv->a2, EN7572_FW_VERSION,
			      &priv->fw_version);
	if (!ret) {
		ret = en7572_select_tx_eye(priv, bob, priv->pon_mode);
		if (!ret) {
			memcpy(priv->calibration, bob, EN7572_BOB_SIZE);
			priv->calibration_valid = true;
		}
	}
out:
	kfree(pm);
	kfree(dm);
	kfree(bob);
	return ret;
}

struct airoha_en7572 *airoha_en7572_get(struct device_node *np)
{
	struct i2c_client *client;
	struct airoha_en7572 *priv;

	if (!np)
		return ERR_PTR(-EINVAL);

	client = of_find_i2c_device_by_node(np);
	if (!client)
		return ERR_PTR(-EPROBE_DEFER);

	priv = i2c_get_clientdata(client);
	if (!priv) {
		put_device(&client->dev);
		return ERR_PTR(-EPROBE_DEFER);
	}

	return priv;
}
EXPORT_SYMBOL_GPL(airoha_en7572_get);

void airoha_en7572_put(struct airoha_en7572 *priv)
{
	if (priv)
		put_device(&priv->a0->dev);
}
EXPORT_SYMBOL_GPL(airoha_en7572_put);

bool airoha_en7572_is_ready(struct airoha_en7572 *priv)
{
	return priv && READ_ONCE(priv->initialized);
}
EXPORT_SYMBOL_GPL(airoha_en7572_is_ready);

bool airoha_en7572_is_xgspon(struct airoha_en7572 *priv)
{
	return priv && READ_ONCE(priv->pon_mode) == AIROHA_XPON_MODE_XGSPON;
}
EXPORT_SYMBOL_GPL(airoha_en7572_is_xgspon);

bool airoha_en7572_tx_is_disabled(struct airoha_en7572 *priv)
{
	return !priv || READ_ONCE(priv->tx_is_disabled);
}
EXPORT_SYMBOL_GPL(airoha_en7572_tx_is_disabled);

bool airoha_en7572_fault_locked(struct airoha_en7572 *priv)
{
	return !priv || READ_ONCE(priv->fault_locked);
}
EXPORT_SYMBOL_GPL(airoha_en7572_fault_locked);

enum airoha_xpon_mode airoha_en7572_get_mode(struct airoha_en7572 *priv)
{
	return priv ? READ_ONCE(priv->pon_mode) : AIROHA_XPON_MODE_INVALID;
}
EXPORT_SYMBOL_GPL(airoha_en7572_get_mode);

int airoha_en7572_validate_mode(struct airoha_en7572 *priv,
				enum airoha_xpon_mode mode)
{
	unsigned int bank_index;
	const u8 *bank;
	int ret;

	if (!priv)
		return -ENODEV;
	if (!airoha_xpon_mode_valid(mode))
		return -EINVAL;

	mutex_lock(&priv->lock);
	if (!priv->initialized || priv->fault_locked || priv->pon_mode != mode) {
		ret = -EIO;
		goto unlock;
	}
	ret = en7572_calibration_bank_for_mode(mode, &bank_index);
	if (ret == -EOPNOTSUPP) {
		ret = 0;
		goto unlock;
	}
	if (ret)
		goto unlock;
	if (!priv->calibration_valid || priv->calibration_bank != bank_index) {
		ret = -ENODATA;
		goto unlock;
	}

	bank = priv->calibration + bank_index * EN7572_BOB_BANK_SIZE;
	ret = en7572_verify_tx_eye(priv, bank);

unlock:
	mutex_unlock(&priv->lock);
	return ret;
}
EXPORT_SYMBOL_GPL(airoha_en7572_validate_mode);

static int en7572_switch_10g_mode(struct airoha_en7572 *priv,
					 enum airoha_xpon_mode previous_mode,
					 enum airoha_xpon_mode mode)
{
	unsigned int previous_bank, target_bank;
	int ret, rollback_ret;

	if (!priv->calibration_valid)
		return -ENODATA;
	ret = en7572_calibration_bank_for_mode(previous_mode, &previous_bank);
	if (ret)
		return ret;
	ret = en7572_calibration_bank_for_mode(mode, &target_bank);
	if (ret)
		return ret;

	/* LDDLA_SET_TX_MODE suppresses writes when the selected eye is unchanged. */
	if (target_bank != previous_bank) {
		ret = en7572_select_tx_eye(priv, priv->calibration, mode);
		if (ret) {
			rollback_ret = en7572_select_tx_eye(priv, priv->calibration,
							 previous_mode);
			if (rollback_ret) {
				airoha_en7572_emergency_disable(priv);
				WRITE_ONCE(priv->pon_mode,
					   AIROHA_XPON_MODE_INVALID);
				priv->calibration_valid = false;
				strscpy(priv->calibration_source, "invalid",
					sizeof(priv->calibration_source));
				dev_crit(&priv->a0->dev,
					 "10G TX-eye rollback failed: %pe; TX permanently locked\n",
					 ERR_PTR(rollback_ret));
			}
			return ret;
		}
	}

	priv->calibration_bank = target_bank;
	en7572_set_calibration_source(priv, mode, target_bank);
	WRITE_ONCE(priv->pon_mode, mode);
	return 0;
}

int airoha_en7572_set_mode(struct airoha_en7572 *priv,
				  enum airoha_xpon_mode mode)
{
	enum airoha_xpon_mode previous_mode;
	int ret, rollback_ret;

	if (!priv)
		return -ENODEV;
	if (!airoha_xpon_mode_valid(mode))
		return -EINVAL;

	mutex_lock(&priv->lock);
	previous_mode = priv->pon_mode;
	if (mode == previous_mode) {
		ret = 0;
		goto unlock;
	}

	/* A planned mode change disables TX without latching a safety fault. */
	ret = airoha_en7572_set_tx_enabled(priv, false);
	if (ret)
		goto unlock;
	WRITE_ONCE(priv->initialized, false);
	if (en7572_mode_uses_shared_10g_bob(previous_mode) &&
	    en7572_mode_uses_shared_10g_bob(mode)) {
		ret = en7572_switch_10g_mode(priv, previous_mode, mode);
		if (!ret || priv->pon_mode == previous_mode)
			WRITE_ONCE(priv->initialized, true);
		goto unlock;
	}

	WRITE_ONCE(priv->pon_mode, mode);
	ret = en7572_initialize(priv);
	if (!ret) {
		WRITE_ONCE(priv->initialized, true);
		goto unlock;
	}

	dev_err(&priv->a0->dev,
		"mode %s initialization failed: %pe; restoring previous mode\n",
		airoha_xpon_mode_descriptor(mode)->name, ERR_PTR(ret));
	WRITE_ONCE(priv->pon_mode, previous_mode);
	rollback_ret = en7572_initialize(priv);
	if (rollback_ret) {
		airoha_en7572_emergency_disable(priv);
		WRITE_ONCE(priv->pon_mode, AIROHA_XPON_MODE_INVALID);
		priv->calibration_valid = false;
		strscpy(priv->calibration_source, "invalid",
			sizeof(priv->calibration_source));
		dev_crit(&priv->a0->dev,
			 "optical mode rollback failed: %pe; TX permanently locked\n",
			 ERR_PTR(rollback_ret));
	} else {
		WRITE_ONCE(priv->initialized, true);
	}

unlock:
	mutex_unlock(&priv->lock);
	return ret;
}
EXPORT_SYMBOL_GPL(airoha_en7572_set_mode);

int airoha_en7572_set_tx_enabled(struct airoha_en7572 *priv, bool enabled)
{
	unsigned long flags;
	int ret = 0;

	if (!priv)
		return -ENODEV;

	spin_lock_irqsave(&priv->tx_lock, flags);
	if (enabled && (!READ_ONCE(priv->initialized) || priv->fault_locked)) {
		ret = priv->fault_locked ? -EIO : -EAGAIN;
	} else {
		gpiod_set_value(priv->tx_disable, !enabled);
		WRITE_ONCE(priv->tx_is_disabled, !enabled);
	}
	spin_unlock_irqrestore(&priv->tx_lock, flags);

	return ret;
}
EXPORT_SYMBOL_GPL(airoha_en7572_set_tx_enabled);

int airoha_en7572_clear_fault(struct airoha_en7572 *priv)
{
	unsigned long flags;

	if (!priv)
		return -ENODEV;

	spin_lock_irqsave(&priv->tx_lock, flags);
	if (!priv->tx_is_disabled) {
		spin_unlock_irqrestore(&priv->tx_lock, flags);
		return -EBUSY;
	}
	priv->fault_locked = false;
	spin_unlock_irqrestore(&priv->tx_lock, flags);

	return 0;
}
EXPORT_SYMBOL_GPL(airoha_en7572_clear_fault);

void airoha_en7572_emergency_disable(struct airoha_en7572 *priv)
{
	unsigned long flags;

	if (!priv)
		return;

	spin_lock_irqsave(&priv->tx_lock, flags);
	priv->fault_locked = true;
	gpiod_set_value(priv->tx_disable, 1);
	WRITE_ONCE(priv->tx_is_disabled, true);
	spin_unlock_irqrestore(&priv->tx_lock, flags);
}
EXPORT_SYMBOL_GPL(airoha_en7572_emergency_disable);

static ssize_t initialized_show(struct device *dev,
				struct device_attribute *attribute, char *buf)
{
	struct airoha_en7572 *priv = dev_get_drvdata(dev);

	return sysfs_emit(buf, "%u\n", priv->initialized);
}
static DEVICE_ATTR_RO(initialized);

static ssize_t calibration_source_show(struct device *dev,
				       struct device_attribute *attribute,
				       char *buf)
{
	struct airoha_en7572 *priv = dev_get_drvdata(dev);

	return sysfs_emit(buf, "%s\n", priv->calibration_source);
}
static DEVICE_ATTR_RO(calibration_source);

static ssize_t pon_mode_show(struct device *dev,
			     struct device_attribute *attribute, char *buf)
{
	struct airoha_en7572 *priv = dev_get_drvdata(dev);
	const struct airoha_xpon_mode_descriptor *descriptor;

	descriptor = airoha_xpon_mode_descriptor(READ_ONCE(priv->pon_mode));
	return sysfs_emit(buf, "%s\n", descriptor ? descriptor->name : "invalid");
}
static DEVICE_ATTR_RO(pon_mode);

static ssize_t vendor_show(struct device *dev,
			   struct device_attribute *attribute, char *buf)
{
	struct airoha_en7572 *priv = dev_get_drvdata(dev);

	return sysfs_emit(buf, "%s\n", priv->vendor);
}
static DEVICE_ATTR_RO(vendor);

static ssize_t part_number_show(struct device *dev,
				struct device_attribute *attribute, char *buf)
{
	struct airoha_en7572 *priv = dev_get_drvdata(dev);

	return sysfs_emit(buf, "%s\n", priv->part_number);
}
static DEVICE_ATTR_RO(part_number);

static ssize_t firmware_version_show(struct device *dev,
				     struct device_attribute *attribute,
				     char *buf)
{
	struct airoha_en7572 *priv = dev_get_drvdata(dev);

	return sysfs_emit(buf, "0x%02x\n", priv->fw_version);
}
static DEVICE_ATTR_RO(firmware_version);

static ssize_t mcu_idle_show(struct device *dev,
			     struct device_attribute *attribute, char *buf)
{
	struct airoha_en7572 *priv = dev_get_drvdata(dev);
	u8 value;
	int ret;

	mutex_lock(&priv->lock);
	ret = en7572_read_u8(priv->a2, EN7572_MCU_IDLE, &value);
	mutex_unlock(&priv->lock);
	if (ret)
		return ret;

	return sysfs_emit(buf, "0x%02x\n", value);
}
static DEVICE_ATTR_RO(mcu_idle);

static ssize_t los_show(struct device *dev, struct device_attribute *attribute,
			char *buf)
{
	struct airoha_en7572 *priv = dev_get_drvdata(dev);
	u8 value;
	int ret;

	mutex_lock(&priv->lock);
	ret = en7572_read_u8(priv->a2, EN7572_LOS_STATUS, &value);
	mutex_unlock(&priv->lock);
	if (ret)
		return ret;

	return sysfs_emit(buf, "%u\n", !!value);
}
static DEVICE_ATTR_RO(los);

static ssize_t tx_disable_show(struct device *dev,
			       struct device_attribute *attribute, char *buf)
{
	struct airoha_en7572 *priv = dev_get_drvdata(dev);

	return sysfs_emit(buf, "%u\n", READ_ONCE(priv->tx_is_disabled));
}
static DEVICE_ATTR_RO(tx_disable);

static ssize_t fault_locked_show(struct device *dev,
				 struct device_attribute *attribute, char *buf)
{
	struct airoha_en7572 *priv = dev_get_drvdata(dev);
	unsigned long flags;
	bool locked;

	spin_lock_irqsave(&priv->tx_lock, flags);
	locked = priv->fault_locked;
	spin_unlock_irqrestore(&priv->tx_lock, flags);

	return sysfs_emit(buf, "%u\n", locked);
}
static DEVICE_ATTR_RO(fault_locked);

static ssize_t optical_diagnostics_show(struct device *dev,
					struct device_attribute *attribute,
					char *buf)
{
	struct airoha_en7572 *priv = dev_get_drvdata(dev);
	u8 raw[EN7572_DIAGNOSTICS_SIZE];
	int ret;

	mutex_lock(&priv->lock);
	if (!priv->initialized) {
		ret = -EAGAIN;
		goto unlock;
	}
	ret = en7572_read(priv->a2, EN7572_DIAGNOSTICS_REG, raw,
			   sizeof(raw));
unlock:
	mutex_unlock(&priv->lock);
	if (ret)
		return ret;

	/* temperature, Vcc, bias, TX power and RX power, in that order */
	return sysfs_emit(buf, "%u %u %u %u %u\n",
			  get_unaligned_be16(raw),
			  get_unaligned_be16(raw + 2),
			  get_unaligned_be16(raw + 4),
			  get_unaligned_be16(raw + 6),
			  get_unaligned_be16(raw + 8));
}
static DEVICE_ATTR_RO(optical_diagnostics);

static struct attribute *en7572_attrs[] = {
	&dev_attr_initialized.attr,
	&dev_attr_pon_mode.attr,
	&dev_attr_calibration_source.attr,
	&dev_attr_vendor.attr,
	&dev_attr_part_number.attr,
	&dev_attr_firmware_version.attr,
	&dev_attr_mcu_idle.attr,
	&dev_attr_los.attr,
	&dev_attr_tx_disable.attr,
	&dev_attr_fault_locked.attr,
	&dev_attr_optical_diagnostics.attr,
	NULL,
};
ATTRIBUTE_GROUPS(en7572);

static int en7572_probe(struct i2c_client *client)
{
	struct device *dev = &client->dev;
	struct airoha_en7572 *priv;
	const char *pon_mode;
	int ret;

	if (client->addr != EN7572_A0_ADDR)
		return dev_err_probe(dev, -EINVAL, "A0 address must be 0x50\n");
	if (!i2c_check_functionality(client->adapter, I2C_FUNC_I2C))
		return dev_err_probe(dev, -EOPNOTSUPP,
				     "adapter lacks raw I2C transfers\n");

	priv = devm_kzalloc(dev, sizeof(*priv), GFP_KERNEL);
	if (!priv)
		return -ENOMEM;

	priv->a0 = client;
	priv->calibration_a2_as_a0 = device_property_read_bool(dev,
					"airoha,calibration-10g-a2-as-a0");
	if (priv->calibration_a2_as_a0)
		dev_warn(dev,
			 "using experimental A2-to-A0 10G calibration override\n");
	ret = device_property_read_string(dev, "airoha,pon-mode", &pon_mode);
	if (ret)
		return dev_err_probe(dev, ret,
				     "missing explicit airoha,pon-mode\n");
	if (!strcmp(pon_mode, "gpon"))
		priv->pon_mode = AIROHA_XPON_MODE_GPON;
	else if (!strcmp(pon_mode, "xgpon"))
		priv->pon_mode = AIROHA_XPON_MODE_XGPON;
	else if (!strcmp(pon_mode, "xgspon"))
		priv->pon_mode = AIROHA_XPON_MODE_XGSPON;
	else if (!strcmp(pon_mode, "epon-10g-1g"))
		priv->pon_mode = AIROHA_XPON_MODE_EPON_10G_1G;
	else if (!strcmp(pon_mode, "epon-10g-10g"))
		priv->pon_mode = AIROHA_XPON_MODE_EPON_10G_10G;
	else
		return dev_err_probe(dev, -EINVAL,
				     "unsupported PON mode %s\n", pon_mode);
	mutex_init(&priv->lock);
	spin_lock_init(&priv->tx_lock);
	i2c_set_clientdata(client, priv);

	priv->tx_disable = devm_gpiod_get(dev, "tx-disable", GPIOD_OUT_HIGH);
	if (IS_ERR(priv->tx_disable))
		return dev_err_probe(dev, PTR_ERR(priv->tx_disable),
				     "failed to claim TX-disable GPIO\n");
	priv->tx_is_disabled = true;
	if (gpiod_cansleep(priv->tx_disable))
		return dev_err_probe(dev, -EOPNOTSUPP,
				     "TX-disable GPIO is not hard-IRQ safe\n");

	priv->a2 = devm_i2c_new_dummy_device(dev, client->adapter,
					      EN7572_A2_ADDR);
	if (IS_ERR(priv->a2))
		return dev_err_probe(dev, PTR_ERR(priv->a2),
				     "failed to claim A2 address\n");

	ret = en7572_read_le16(priv->a2, EN7572_ID1_REG, &priv->id1);
	if (ret)
		return dev_err_probe(dev, ret, "failed to read EN7572 ID\n");
	ret = en7572_read_le16(priv->a2, EN7572_ID2_REG, &priv->id2);
	if (ret)
		return dev_err_probe(dev, ret, "failed to read EN7572 revision\n");
	if (priv->id1 != EN7572_ID1)
		return dev_err_probe(dev, -ENODEV,
				     "unexpected device ID 0x%04x:0x%04x\n",
				     priv->id1, priv->id2);

	mutex_lock(&priv->lock);
	ret = en7572_initialize(priv);
	if (!ret)
		priv->initialized = true;
	mutex_unlock(&priv->lock);
	if (ret)
		return dev_err_probe(dev, ret,
				     "initialization failed; optical TX remains disabled\n");

	dev_info(dev,
		 "EN7572 0x%04x:0x%04x initialized, mode=%s, calibration=%s, vendor=%s, part=%s; TX disabled\n",
		 priv->id1, priv->id2, pon_mode, priv->calibration_source,
		 priv->vendor, priv->part_number);

	return 0;
}

static void en7572_remove(struct i2c_client *client)
{
	struct airoha_en7572 *priv = i2c_get_clientdata(client);

	WRITE_ONCE(priv->initialized, false);
	airoha_en7572_emergency_disable(priv);
	en7572_update_bits(priv, EN7572_MD32_ENABLE, BIT(0), 0);
}

static void en7572_shutdown(struct i2c_client *client)
{
	struct airoha_en7572 *priv = i2c_get_clientdata(client);

	airoha_en7572_emergency_disable(priv);
}

static const struct of_device_id en7572_of_match[] = {
	{ .compatible = "airoha,en7572" },
	{ }
};
MODULE_DEVICE_TABLE(of, en7572_of_match);

static const struct i2c_device_id en7572_id[] = {
	{ "en7572" },
	{ }
};
MODULE_DEVICE_TABLE(i2c, en7572_id);

static struct i2c_driver en7572_driver = {
	.driver = {
		.name = "airoha-en7572",
		.of_match_table = en7572_of_match,
		.dev_groups = en7572_groups,
	},
	.probe = en7572_probe,
	.remove = en7572_remove,
	.shutdown = en7572_shutdown,
	.id_table = en7572_id,
};
module_i2c_driver(en7572_driver);

MODULE_AUTHOR("OpenWrt contributors");
MODULE_DESCRIPTION("Airoha EN7572 BOSA controller");
MODULE_LICENSE("GPL");
