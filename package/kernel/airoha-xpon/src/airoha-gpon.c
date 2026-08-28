// SPDX-License-Identifier: GPL-2.0-only
/*
 * Airoha EN7581 GPON MAC and burst-mode PHY support
 */

#include <linux/bitfield.h>
#include <linux/bitmap.h>
#include <linux/ctype.h>
#include <linux/device.h>
#include <linux/interrupt.h>
#include <linux/io.h>
#include <linux/iopoll.h>
#include <linux/kernel.h>
#include <linux/mfd/syscon.h>
#include <linux/module.h>
#include <linux/mutex.h>
#include <linux/of.h>
#include <linux/of_address.h>
#include <linux/platform_device.h>
#include <linux/property.h>
#include <linux/random.h>
#include <linux/regmap.h>
#include <linux/slab.h>
#include <linux/sysfs.h>
#include <linux/unaligned.h>
#include <linux/workqueue.h>

#include "airoha-en7572.h"
#include "airoha-eth.h"
#include "airoha-gpon-port-transaction.h"
#include "airoha-xpon-core.h"

#define EN7581_GPON_ONU_ID		0x000
#define EN7581_GPON_GBL_CFG		0x004
#define EN7581_GPON_INT_STATUS		0x008
#define EN7581_GPON_INT_ENABLE		0x00c
#define EN7581_GPON_TCONT_BASE		0x020
#define EN7581_GPON_GEM_PORT_CFG		0x040
#define EN7581_GPON_GEM_PORT_STS		0x044
#define EN7581_GPON_OMCI_ID		0x048
#define EN7581_GPON_GEM_TBL_INIT		0x04c
#define EN7581_GPON_PLOAMU_FIFO_STS	0x050
#define EN7581_GPON_PLOAMU_WDATA		0x054
#define EN7581_GPON_PLOAMD_FIFO_STS	0x058
#define EN7581_GPON_PLOAMD_RDATA		0x05c
#define EN7581_GPON_AES_CFG		0x060
#define EN7581_GPON_AES_SHADOW_KEY0	0x074
#define EN7581_GPON_AES_SHADOW_KEY1	0x078
#define EN7581_GPON_AES_SHADOW_KEY2	0x07c
#define EN7581_GPON_AES_SHADOW_KEY3	0x080
#define EN7581_GPON_PLOU_GUARD_BIT	0x094
#define EN7581_GPON_PLOU_PREAMBLE	0x098
#define EN7581_GPON_PLOU_DELIMITER_BIT	0x0a0
#define EN7581_GPON_PRE_ASSIGNED_DLY	0x0a4
#define EN7581_GPON_EQD			0x0a8
#define EN7581_GPON_RSP_TIME		0x0ac
#define EN7581_GPON_VENDOR_ID		0x0b0
#define EN7581_GPON_VS_SN		0x0b4
#define EN7581_GPON_SN_MSG_CFG		0x0b8
#define EN7581_GPON_ACTIVATION_ST	0x0bc
#define EN7581_GPON_MIB_CTRL		0x120
#define EN7581_GPON_MIB_RDATA_L32	0x124
#define EN7581_GPON_MIB_RDATA_H32	0x128
#define EN7581_GPON_DBG_DLY		0x208
#define EN7581_GPON_DBG_IDLE_GEM_THLD	0x20c

#define EN7581_PHY_CSR_PHYSET5		0x110
#define EN7581_PHY_CSR_XPON_SETTING	0x138
#define EN7581_PHY_CSR_ERRCNT_EN		0x230
#define EN7581_PHY_CSR_ERRCNT_CTL	0x234
#define EN7581_PHY_CSR_FEC_CORRECTED_BYTES	0x238
#define EN7581_PHY_CSR_FEC_CORRECTED_CW		0x23c
#define EN7581_PHY_CSR_FEC_UNCORRECTABLE_CW	0x240
#define EN7581_PHY_CSR_FEC_TOTAL_CW		0x244
#define EN7581_PHY_CSR_FEC_SECONDS		0x248
#define EN7581_PHY_CSR_BIP_COUNT		0x24c
#define EN7581_PHY_CSR_RX_STATUS		0x21c
#define EN7581_PHY_CSR_GPON_PREAMBLE	0x400
#define EN7581_PHY_CSR_GPON_DELIMITER	0x404
#define EN7581_PHY_CSR_XPON_STATUS	0x5e0
#define EN7581_PHY_CSR_XPON_INT_ENABLE	0x5f0
#define EN7581_PHY_CSR_XPON_INT_CLEAR	0x5f4
#define EN7581_PHY_CSR_XPON_INT_STATUS	0x5f8

#define EN7581_PMA_XPON_SETTING_0	0x270
#define EN7581_PMA_XPON_SETTING_1	0x274
#define EN7581_PMA_XPON_INT_EN_0		0x280
#define EN7581_PMA_XPON_INT_STA_0	0x288

#define EN7581_SCU_FORCE_GPIO32_EN	0x22c
#define EN7581_SCU_FORCE_GPIO41_EN	BIT(9)

#define EN7581_GPON_ONU_ID_VALID	BIT(15)
#define EN7581_GPON_ONU_ID_MASK		GENMASK(7, 0)
#define EN7581_GPON_US_FEC_ENABLE	BIT(16)
#define EN7581_GPON_TCONT_ID_MASK	GENMASK(11, 0)
#define EN7581_GPON_TCONT_VALID		BIT(15)
#define EN7581_GPON_OMCI_ID_VALID	BIT(16)
#define EN7581_GPON_OMCI_ID_MASK	GENMASK(11, 0)
#define EN7581_GPON_GEM_CFG_CMD		BIT(31)
#define EN7581_GPON_GEM_CFG_ENCRYPT	BIT(17)
#define EN7581_GPON_GEM_CFG_VALID	BIT(16)
#define EN7581_GPON_GEM_CFG_ID		GENMASK(11, 0)
#define EN7581_GPON_GEM_STS_DONE		BIT(31)
#define EN7581_GPON_GEM_TBL_START	BIT(0)
#define EN7581_GPON_GEM_TBL_DONE	BIT(8)
#define EN7581_GPON_ACTIVATION_MASK	GENMASK(2, 0)
#define EN7581_GPON_DBA_BLOCK_MASK	GENMASK(7, 0)
#define EN7581_GPON_RSP_TIME_MASK	GENMASK(15, 0)
#define EN7581_GPON_FINE_DLY_MASK	GENMASK(15, 8)
#define EN7581_GPON_IDLE_GEM_MASK	GENMASK(15, 0)
#define EN7581_GPON_PLOAMD_USED_MASK	GENMASK(7, 0)
#define EN7581_GPON_PLOAMD_OVERRUN	BIT(31)
#define EN7581_GPON_PLOAMU_AVAIL_MASK	GENMASK(7, 0)
#define EN7581_GPON_AES_SPF_MASK	GENMASK(29, 0)
#define EN7581_GPON_PRE_DELAY_MASK	GENMASK(15, 0)
#define EN7581_GPON_PRE_DELAY_ENABLE	BIT(31)
#define EN7581_GPON_SN_TX_POWER_MASK	GENMASK(17, 16)
#define EN7581_GPON_SN_REQ_THRESHOLD	GENMASK(31, 24)
#define EN7581_GPON_MIB_DONE		BIT(30)
#define EN7581_GPON_MIB_READ_CLEAR	BIT(24)
#define EN7581_GPON_MIB_TYPE		GENMASK(18, 16)
#define EN7581_GPON_MIB_GEM_ID		GENMASK(11, 0)

#define EN7581_PHY_BIT_DELAY		GENMASK(22, 19)
#define EN7581_PHY_BIT_DELAY_SELECT	BIT(23)
#define EN7581_PMA_ROGUE_ONU		BIT(0)
#define EN7581_PHY_ERRCNT_FEC_ENABLE	BIT(0)
#define EN7581_PHY_ERRCNT_BIP_ENABLE	BIT(1)
#define EN7581_PHY_ERRCNT_FEC_LATCH	BIT(0)
#define EN7581_PHY_ERRCNT_BIP_LATCH	BIT(2)
#define EN7581_PHY_ERRCNT_BIP_CLEAR	BIT(3)
#define EN7581_PHY_XPON_STATUS_LOS	BIT(0)
#define EN7581_PHY_RX_SYNC_MASK		GENMASK(3, 0)
#define EN7581_PHY_RX_SYNC_OK		0x0a
#define EN7581_PHY_INT_LOS		BIT(0)
#define EN7581_PHY_INT_LOF		BIT(1)
#define EN7581_PHY_INT_READY		BIT(5)
#define EN7581_PHY_INT_NO_LOS		BIT(8)
#define EN7581_PHY_INT_MASK		(EN7581_PHY_INT_LOS | \
					 EN7581_PHY_INT_LOF | \
					 EN7581_PHY_INT_READY | \
					 EN7581_PHY_INT_NO_LOS)

#define EN7581_GPON_INT_PLOAMD		BIT(0)
#define EN7581_GPON_INT_ERRORS		GENMASK(31, 16)
#define EN7581_GPON_INT_MASK		(EN7581_GPON_INT_PLOAMD | \
					 GENMASK(8, 2) | \
					 EN7581_GPON_INT_ERRORS)

#define EN7581_GPON_DBA_BLOCK_48	0xd4
#define EN7581_GPON_RESPONSE_TIME	0x577
#define EN7581_GPON_FINE_DELAY		0x0d
#define EN7581_GPON_IDLE_GEM_THRESHOLD	0x1a
#define EN7581_GPON_GUARD_BITS		20
#define EN7581_GPON_DELIMITER_BITS	24
#define EN7581_GPON_SN_REQUEST_THRESHOLD 10
#define EN7581_GPON_PLOAM_SIZE		12
#define EN7581_GPON_PASSWORD_SIZE	10
#define EN7581_GPON_MAX_IRQ_PLOAMS	64
#define EN7581_GPON_MAX_TCONTS		16
#define EN7581_GPON_MAX_GEMS		4096
#define EN7581_GPON_MAX_ACTIVE_GEMS	256
#define EN7581_GPON_PLOAM_REPEATS	3
#define EN7581_GPON_TO1_MS		10000
#define EN7581_GPON_TO2_MS		100

#define EN7581_EN7572_PHY_SETTING	0x0000014f
#define EN7581_EN7572_PMA_SETTING_0	0x00010001
#define EN7581_EN7572_PMA_SETTING_1	0x01010100

#define EN7581_PLOAM_BROADCAST_ID	0xff

enum en7581_ploam_downstream_type {
	EN7581_PLOAM_UPSTREAM_OVERHEAD = 0x01,
	EN7581_PLOAM_ASSIGN_ONU_ID = 0x03,
	EN7581_PLOAM_RANGING_TIME = 0x04,
	EN7581_PLOAM_DEACTIVATE_ONU_ID = 0x05,
	EN7581_PLOAM_DISABLE_SERIAL_NUMBER = 0x06,
	EN7581_PLOAM_ENCRYPTED_PORT_ID = 0x08,
	EN7581_PLOAM_REQUEST_PASSWORD = 0x09,
	EN7581_PLOAM_ASSIGN_ALLOC_ID = 0x0a,
	EN7581_PLOAM_POPUP = 0x0c,
	EN7581_PLOAM_REQUEST_KEY = 0x0d,
	EN7581_PLOAM_CONFIGURE_PORT_ID = 0x0e,
	EN7581_PLOAM_BER_INTERVAL = 0x12,
	EN7581_PLOAM_KEY_SWITCHING_TIME = 0x13,
};

enum en7581_ploam_upstream_type {
	EN7581_PLOAMU_PASSWORD = 0x02,
	EN7581_PLOAMU_ENCRYPTION_KEY = 0x05,
	EN7581_PLOAMU_REI = 0x08,
	EN7581_PLOAMU_ACKNOWLEDGE = 0x09,
};

enum en7581_gpon_state {
	EN7581_GPON_STATE_O1 = 1,
	EN7581_GPON_STATE_O2,
	EN7581_GPON_STATE_O3,
	EN7581_GPON_STATE_O4,
	EN7581_GPON_STATE_O5,
	EN7581_GPON_STATE_O6,
	EN7581_GPON_STATE_O7,
};

enum en7581_gpon_init_stage {
	EN7581_GPON_INIT_NEVER,
	EN7581_GPON_INIT_PRECHECK,
	EN7581_GPON_INIT_MAC_IRQ_MASK,
	EN7581_GPON_INIT_SESSION_RESET,
	EN7581_GPON_INIT_PHY_SETTINGS,
	EN7581_GPON_INIT_ROGUE_IRQ,
	EN7581_GPON_INIT_SCU_GPIO,
	EN7581_GPON_INIT_READY,
};

enum en7581_gpon_gem_counter {
	EN7581_GPON_GEM_RX_FRAMES,
	EN7581_GPON_GEM_RX_PAYLOAD_BYTES,
	EN7581_GPON_GEM_TX_FRAMES,
	EN7581_GPON_GEM_TX_PAYLOAD_BYTES,
};

struct en7581_gpon {
	struct device *dev;
	void __iomem *base;
	void __iomem *phy_csr;
	void __iomem *pma;
	struct regmap *chip_scu;
	struct airoha_en7572 *bosa;
	struct device_node *ethernet_np;
	int mac_irq;
	int phy_irq;
	struct airoha_xpon_backend *xpon_backend;
	struct mutex lock;
	bool xpon_irqs_masked;
	bool switch_resume_enabled;
	bool hardware_selected;
	bool enabled;
	bool safety_ready;
	bool rogue_fault;
	bool olt_disabled;
	bool serial_set;
	bool password_set;
	bool onu_assigned;
	u8 onu_id;
	u16 omcc_id;
	u32 eqd;
	u8 bit_delay;
	u32 idle_gem_threshold;
	u16 alloc_ids[EN7581_GPON_MAX_TCONTS];
	bool alloc_valid[EN7581_GPON_MAX_TCONTS];
	DECLARE_BITMAP(gem_valid, EN7581_GPON_MAX_GEMS);
	DECLARE_BITMAP(gem_encrypted, EN7581_GPON_MAX_GEMS);
	DECLARE_BITMAP(data_gems, EN7581_GPON_MAX_GEMS);
	u8 data_channels[EN7581_GPON_MAX_GEMS];
	u8 data_directions[EN7581_GPON_MAX_GEMS];
	u16 data_gem_count;
	u16 data_gem;
	u16 counter_gem;
	u8 data_channel;
	bool data_gem_valid;
	u8 serial[8];
	u8 password[EN7581_GPON_PASSWORD_SIZE];
	u8 encryption_key[16];
	u8 encryption_key_index;
	u8 last_ploam[EN7581_GPON_PLOAM_SIZE];
	bool have_last_ploam;
	struct delayed_work activation_work;
	unsigned long activation_deadline;
	u32 activation_timer_state;
	struct delayed_work ber_work;
	u32 ber_interval_ms;
	u32 last_bip_count;
	u64 ber_sample_sequence;
	u8 rei_sequence;
	bool ber_sample_valid;
	bool los;
	bool lof;
	bool phy_ready;
	bool popup_reranging;
	enum en7581_gpon_init_stage init_stage;
	enum airoha_xpon_mode init_bosa_mode;
	bool init_bosa_ready;
	bool init_tx_disabled;
	int init_error;
	u32 init_phy_setting;
	u32 init_errcnt_enable;
	u32 init_pma_setting_0;
	u32 init_pma_setting_1;
	u32 init_pma_interrupt_enable;
	u32 init_scu_gpio_force;
	u64 init_attempts;
	u64 irq_count;
	u64 phy_irq_count;
	u64 ploam_rx_count;
	u64 error_irq_count;
	u64 fifo_overrun_count;
	u64 ploam_drop_count;
	u64 ploam_unknown_count;
	u64 ploam_tx_blocked_count;
	u64 ploam_tx_count;
	u64 to1_timeout_count;
	u64 to2_timeout_count;
	u64 popup_recovery_count;
};

static const char *
en7581_gpon_init_stage_name(enum en7581_gpon_init_stage stage)
{
	switch (stage) {
	case EN7581_GPON_INIT_NEVER:
		return "never";
	case EN7581_GPON_INIT_PRECHECK:
		return "precheck";
	case EN7581_GPON_INIT_MAC_IRQ_MASK:
		return "mac-irq-mask";
	case EN7581_GPON_INIT_SESSION_RESET:
		return "session-reset";
	case EN7581_GPON_INIT_PHY_SETTINGS:
		return "phy-settings";
	case EN7581_GPON_INIT_ROGUE_IRQ:
		return "rogue-irq";
	case EN7581_GPON_INIT_SCU_GPIO:
		return "scu-gpio";
	case EN7581_GPON_INIT_READY:
		return "ready";
	default:
		return "invalid";
	}
}

static u32 en7581_gpon_get_state(struct en7581_gpon *priv);
static int en7581_gpon_xpon_mask_irqs(void *context);
static int en7581_gpon_xpon_unmask_irqs(void *context);

static u32 en7581_gpon_read(struct en7581_gpon *priv, u32 reg)
{
	return readl(priv->base + reg);
}

static void en7581_gpon_write(struct en7581_gpon *priv, u32 reg, u32 value)
{
	writel(value, priv->base + reg);
}

static int en7581_gpon_set_mac_irq_enable(struct en7581_gpon *priv, u32 mask)
{
	en7581_gpon_write(priv, EN7581_GPON_INT_ENABLE, mask);
	if (en7581_gpon_read(priv, EN7581_GPON_INT_ENABLE) == mask)
		return 0;

	/* A stale enable value is unsafe even when the software owner is valid. */
	en7581_gpon_xpon_mask_irqs(priv);
	dev_err(priv->dev, "failed to verify GPON MAC interrupt enable %#x\n",
		mask);
	return -EIO;
}

static void en7581_gpon_update_bits(struct en7581_gpon *priv, u32 reg,
				    u32 mask, u32 value)
{
	u32 old_value = en7581_gpon_read(priv, reg);

	en7581_gpon_write(priv, reg, (old_value & ~mask) | (value & mask));
}

static int en7581_gpon_read_gem_counter(struct en7581_gpon *priv, u16 gem,
					 enum en7581_gpon_gem_counter type,
					 u64 *counter)
{
	u32 status, high, low, command;
	int ret;

	if (gem >= EN7581_GPON_MAX_GEMS || type > EN7581_GPON_GEM_TX_PAYLOAD_BYTES)
		return -ERANGE;

	command = FIELD_PREP(EN7581_GPON_MIB_TYPE, type) |
		  FIELD_PREP(EN7581_GPON_MIB_GEM_ID, gem);
	en7581_gpon_write(priv, EN7581_GPON_MIB_CTRL, command);
	ret = readl_poll_timeout(priv->base + EN7581_GPON_MIB_CTRL, status,
				 status & EN7581_GPON_MIB_DONE, 1, 3000);
	if (ret)
		return ret;

	low = en7581_gpon_read(priv, EN7581_GPON_MIB_RDATA_L32);
	high = en7581_gpon_read(priv, EN7581_GPON_MIB_RDATA_H32);
	*counter = (u64)high << 32 | low;
	return 0;
}

static int en7581_gpon_set_gem(struct en7581_gpon *priv, u16 gem,
			       bool valid, bool encrypted)
{
	u32 status, value;
	int ret;

	if (gem >= EN7581_GPON_MAX_GEMS)
		return -ERANGE;

	value = EN7581_GPON_GEM_CFG_CMD |
		FIELD_PREP(EN7581_GPON_GEM_CFG_ID, gem);
	if (valid)
		value |= EN7581_GPON_GEM_CFG_VALID;
	if (encrypted)
		value |= EN7581_GPON_GEM_CFG_ENCRYPT;

	en7581_gpon_write(priv, EN7581_GPON_GEM_PORT_CFG, value);
	ret = readl_poll_timeout(priv->base + EN7581_GPON_GEM_PORT_STS,
				 status, status & EN7581_GPON_GEM_STS_DONE,
				 1, 3000);
	if (ret)
		return ret;

	if (valid)
		__set_bit(gem, priv->gem_valid);
	else
		__clear_bit(gem, priv->gem_valid);
	if (valid && encrypted)
		__set_bit(gem, priv->gem_encrypted);
	else
		__clear_bit(gem, priv->gem_encrypted);

	return 0;
}

static int en7581_gpon_init_gem_table(struct en7581_gpon *priv)
{
	u32 value;
	int ret;

	en7581_gpon_write(priv, EN7581_GPON_GEM_TBL_INIT,
			   EN7581_GPON_GEM_TBL_START);
	ret = readl_poll_timeout(priv->base + EN7581_GPON_GEM_TBL_INIT,
				 value, value & EN7581_GPON_GEM_TBL_DONE,
				 1, 10000);
	if (ret)
		return ret;

	bitmap_zero(priv->gem_valid, EN7581_GPON_MAX_GEMS);
	bitmap_zero(priv->gem_encrypted, EN7581_GPON_MAX_GEMS);
	return 0;
}

static int en7581_gpon_disable_data_gems(struct en7581_gpon *priv)
{
	unsigned long gem;
	int err, ret = 0;

	if (priv->ethernet_np) {
		err = airoha_gpon_set_qos(priv->ethernet_np, NULL, 0);
		if (err && !ret)
			ret = err;
		err = airoha_gpon_set_gems(priv->ethernet_np, NULL, NULL, NULL, 0);
		if (err && !ret)
			ret = err;
	}
	for_each_set_bit(gem, priv->data_gems, EN7581_GPON_MAX_GEMS) {
		if (gem == priv->omcc_id)
			continue;
		err = en7581_gpon_set_gem(priv, gem, false, false);
		if (err && !ret)
			ret = err;
	}
	if (ret)
		return ret;

	bitmap_zero(priv->data_gems, EN7581_GPON_MAX_GEMS);
	memset(priv->data_channels, 0, sizeof(priv->data_channels));
	memset(priv->data_directions, 0, sizeof(priv->data_directions));
	priv->data_gem_count = 0;
	priv->data_gem_valid = false;
	return 0;
}

static int en7581_gpon_send_ploamu(struct en7581_gpon *priv,
				   const u8 *message, unsigned int repeats)
{
	u32 status;
	unsigned int i, word;
	int ret;

	if (!priv->enabled || priv->rogue_fault)
		return -ESHUTDOWN;

	for (i = 0; i < repeats; i++) {
		ret = readl_poll_timeout(priv->base + EN7581_GPON_PLOAMU_FIFO_STS,
					 status,
					 FIELD_GET(EN7581_GPON_PLOAMU_AVAIL_MASK,
						   status) >= 3,
					 1, 3000);
		if (ret) {
			priv->ploam_tx_blocked_count += repeats - i;
			return ret;
		}

		for (word = 0; word < 3; word++)
			en7581_gpon_write(priv, EN7581_GPON_PLOAMU_WDATA,
					   get_unaligned_be32(message + word * 4));
		priv->ploam_tx_count++;
	}

	return 0;
}

static u32 en7581_gpon_read_bip_sample(struct en7581_gpon *priv)
{
	u32 count;

	writel(EN7581_PHY_ERRCNT_BIP_LATCH,
	       priv->phy_csr + EN7581_PHY_CSR_ERRCNT_CTL);
	count = readl(priv->phy_csr + EN7581_PHY_CSR_BIP_COUNT);
	writel(EN7581_PHY_ERRCNT_BIP_CLEAR,
	       priv->phy_csr + EN7581_PHY_CSR_ERRCNT_CTL);
	return count;
}

static void en7581_gpon_stop_ber(struct en7581_gpon *priv)
{
	priv->ber_interval_ms = 0;
	priv->ber_sample_valid = false;
	cancel_delayed_work(&priv->ber_work);
	writel(EN7581_PHY_ERRCNT_BIP_CLEAR,
	       priv->phy_csr + EN7581_PHY_CSR_ERRCNT_CTL);
}

static void en7581_gpon_ber_work(struct work_struct *work)
{
	struct en7581_gpon *priv =
		container_of(to_delayed_work(work), struct en7581_gpon, ber_work);
	u8 message[EN7581_GPON_PLOAM_SIZE] = {
		[1] = EN7581_PLOAMU_REI,
	};
	u32 interval;

	mutex_lock(&priv->lock);
	interval = priv->ber_interval_ms;
	if (!priv->enabled || !interval ||
	    en7581_gpon_get_state(priv) != EN7581_GPON_STATE_O5)
		goto out;

	priv->last_bip_count = en7581_gpon_read_bip_sample(priv);
	priv->ber_sample_sequence++;
	priv->ber_sample_valid = true;
	message[0] = priv->onu_id;
	put_unaligned_be32(priv->last_bip_count, message + 2);
	message[6] = priv->rei_sequence & 0x0f;
	if (en7581_gpon_send_ploamu(priv, message, 1))
		priv->ploam_drop_count++;
	priv->rei_sequence = (priv->rei_sequence + 1) & 0x0f;
	mod_delayed_work(system_wq, &priv->ber_work,
			 msecs_to_jiffies(interval));

out:
	mutex_unlock(&priv->lock);
}

static int en7581_gpon_send_ack(struct en7581_gpon *priv,
				const u8 *downstream)
{
	u8 message[EN7581_GPON_PLOAM_SIZE] = {
		priv->onu_id, EN7581_PLOAMU_ACKNOWLEDGE, downstream[1],
	};

	memcpy(message + 3, downstream, 9);
	return en7581_gpon_send_ploamu(priv, message,
					EN7581_GPON_PLOAM_REPEATS);
}

static void en7581_gpon_write_aes_key(struct en7581_gpon *priv,
				      const u8 *key)
{
	en7581_gpon_write(priv, EN7581_GPON_AES_SHADOW_KEY0,
			   get_unaligned_be32(key + 12));
	en7581_gpon_write(priv, EN7581_GPON_AES_SHADOW_KEY1,
			   get_unaligned_be32(key + 8));
	en7581_gpon_write(priv, EN7581_GPON_AES_SHADOW_KEY2,
			   get_unaligned_be32(key + 4));
	en7581_gpon_write(priv, EN7581_GPON_AES_SHADOW_KEY3,
			   get_unaligned_be32(key));
}

static int en7581_gpon_parse_serial(const char *value, size_t len, u8 *serial)
{
	size_t i;

	while (len && isspace(value[len - 1]))
		len--;
	if (len != 12)
		return -EINVAL;

	for (i = 0; i < 4; i++) {
		if (!isalnum(value[i]))
			return -EINVAL;
		serial[i] = toupper(value[i]);
	}

	return hex2bin(serial + 4, value + 4, 4);
}

static int en7581_gpon_parse_password(const char *value, size_t len,
				      u8 *password, bool *password_set)
{
	while (len && (value[len - 1] == '\n' || value[len - 1] == '\r'))
		len--;

	memset(password, 0, EN7581_GPON_PASSWORD_SIZE);
	if (!len) {
		*password_set = false;
		return 0;
	}
	if (len >= 4 && !memcmp(value, "hex:", 4)) {
		if (len != 4 + EN7581_GPON_PASSWORD_SIZE * 2)
			return -EINVAL;
		if (hex2bin(password, value + 4, EN7581_GPON_PASSWORD_SIZE))
			return -EINVAL;
	} else {
		if (len > EN7581_GPON_PASSWORD_SIZE)
			return -EINVAL;
		memcpy(password, value, len);
	}

	*password_set = true;
	return 0;
}

static void en7581_gpon_write_serial(struct en7581_gpon *priv)
{
	en7581_gpon_write(priv, EN7581_GPON_VENDOR_ID,
			   get_unaligned_be32(priv->serial));
	en7581_gpon_write(priv, EN7581_GPON_VS_SN,
			   get_unaligned_be32(priv->serial + 4));
}

static u32 en7581_gpon_get_state(struct en7581_gpon *priv)
{
	return FIELD_GET(EN7581_GPON_ACTIVATION_MASK,
			 en7581_gpon_read(priv, EN7581_GPON_ACTIVATION_ST));
}

static void en7581_gpon_set_state(struct en7581_gpon *priv, u32 state)
{
	unsigned long delay = 0;

	en7581_gpon_update_bits(priv, EN7581_GPON_ACTIVATION_ST,
				EN7581_GPON_ACTIVATION_MASK,
				FIELD_PREP(EN7581_GPON_ACTIVATION_MASK, state));

	cancel_delayed_work(&priv->activation_work);
	if (state == EN7581_GPON_STATE_O3 || state == EN7581_GPON_STATE_O4)
		delay = msecs_to_jiffies(EN7581_GPON_TO1_MS);
	else if (state == EN7581_GPON_STATE_O6)
		delay = msecs_to_jiffies(EN7581_GPON_TO2_MS);

	priv->activation_timer_state = delay ? state : 0;
	priv->activation_deadline = delay ? jiffies + delay : 0;
	if (delay)
		mod_delayed_work(system_wq, &priv->activation_work, delay);
}

static void en7581_gpon_phy_update_bits(struct en7581_gpon *priv, u32 reg,
					u32 mask, u32 value)
{
	u32 old_value = readl(priv->phy_csr + reg);

	writel((old_value & ~mask) | (value & mask), priv->phy_csr + reg);
}

static void en7581_gpon_set_bit_delay(struct en7581_gpon *priv, u8 delay)
{
	priv->bit_delay = delay & 0x7;
	en7581_gpon_phy_update_bits(priv, EN7581_PHY_CSR_PHYSET5,
				     EN7581_PHY_BIT_DELAY |
				     EN7581_PHY_BIT_DELAY_SELECT,
				     FIELD_PREP(EN7581_PHY_BIT_DELAY,
						priv->bit_delay) |
				     EN7581_PHY_BIT_DELAY_SELECT);
}

static void en7581_gpon_set_tcont(struct en7581_gpon *priv, unsigned int index,
				  u16 alloc_id, bool valid)
{
	u32 shift = (index & 1) * 16;
	u32 mask = (EN7581_GPON_TCONT_ID_MASK |
		    EN7581_GPON_TCONT_VALID) << shift;
	u32 value = ((u32)alloc_id & EN7581_GPON_TCONT_ID_MASK) << shift;
	u32 reg = EN7581_GPON_TCONT_BASE + (index / 2) * sizeof(u32);

	if (valid)
		value |= EN7581_GPON_TCONT_VALID << shift;
	en7581_gpon_update_bits(priv, reg, mask, value);
	priv->alloc_ids[index] = alloc_id;
	priv->alloc_valid[index] = valid;
}

static int en7581_gpon_find_tcont(struct en7581_gpon *priv, u16 alloc_id)
{
	int i;

	for (i = 0; i < EN7581_GPON_MAX_TCONTS; i++)
		if (priv->alloc_valid[i] && priv->alloc_ids[i] == alloc_id)
			return i;

	return -ENOENT;
}

static int en7581_gpon_alloc_tcont(struct en7581_gpon *priv, u16 alloc_id)
{
	int i;

	if (en7581_gpon_find_tcont(priv, alloc_id) >= 0)
		return 0;

	for (i = 1; i < EN7581_GPON_MAX_TCONTS; i++) {
		if (!priv->alloc_valid[i]) {
			en7581_gpon_set_tcont(priv, i, alloc_id, true);
			return 0;
		}
	}

	return -ENOSPC;
}

static void en7581_gpon_clear_tconts(struct en7581_gpon *priv)
{
	int i;

	for (i = 0; i < EN7581_GPON_MAX_TCONTS / 2; i++)
		en7581_gpon_write(priv, EN7581_GPON_TCONT_BASE + i * sizeof(u32),
				   0);
	memset(priv->alloc_ids, 0, sizeof(priv->alloc_ids));
	memset(priv->alloc_valid, 0, sizeof(priv->alloc_valid));
}

static int en7581_gpon_session_disable_tx(void *context)
{
	struct en7581_gpon *priv = context;
	int ret;

	ret = airoha_en7572_set_tx_enabled(priv->bosa, false);
	if (ret)
		airoha_en7572_emergency_disable(priv->bosa);
	return ret;
}

static int en7581_gpon_session_clear_data(void *context)
{
	return en7581_gpon_disable_data_gems(context);
}

static int en7581_gpon_session_disable_omcc(void *context)
{
	struct en7581_gpon *priv = context;

	return airoha_gpon_set_omcc(priv->ethernet_np, 0, false);
}

static int en7581_gpon_session_clear_gem_table(void *context)
{
	return en7581_gpon_init_gem_table(context);
}

static void en7581_gpon_session_commit_reset(void *context,
					      unsigned int state)
{
	static const u8 zero_key[sizeof(((struct en7581_gpon *)0)->encryption_key)];
	struct en7581_gpon *priv = context;

	en7581_gpon_stop_ber(priv);
	en7581_gpon_write(priv, EN7581_GPON_ONU_ID,
			   FIELD_PREP(EN7581_GPON_ONU_ID_MASK,
				      EN7581_PLOAM_BROADCAST_ID));
	en7581_gpon_write(priv, EN7581_GPON_OMCI_ID, 0);
	en7581_gpon_write(priv, EN7581_GPON_AES_CFG, 0);
	en7581_gpon_write_aes_key(priv, zero_key);
	en7581_gpon_update_bits(priv, EN7581_GPON_GBL_CFG,
				EN7581_GPON_US_FEC_ENABLE, 0);
	en7581_gpon_write(priv, EN7581_GPON_EQD, 0);
	en7581_gpon_set_bit_delay(priv, 0);
	en7581_gpon_clear_tconts(priv);

	priv->onu_assigned = false;
	priv->onu_id = EN7581_PLOAM_BROADCAST_ID;
	priv->omcc_id = 0;
	priv->eqd = 0;
	priv->encryption_key_index = 0;
	memzero_explicit(priv->encryption_key, sizeof(priv->encryption_key));
	memzero_explicit(priv->last_ploam, sizeof(priv->last_ploam));
	priv->have_last_ploam = false;
	priv->popup_reranging = false;
	en7581_gpon_set_state(priv, state);
}

static const struct airoha_gpon_session_reset_ops
en7581_gpon_session_reset_ops = {
	.disable_tx = en7581_gpon_session_disable_tx,
	.clear_data = en7581_gpon_session_clear_data,
	.disable_omcc = en7581_gpon_session_disable_omcc,
	.clear_gem_table = en7581_gpon_session_clear_gem_table,
	.commit_reset = en7581_gpon_session_commit_reset,
};

static int en7581_gpon_reset_session(struct en7581_gpon *priv, u32 state)
{
	int ret;

	ret = airoha_gpon_session_reset_transaction(
		&en7581_gpon_session_reset_ops, priv, state,
		EN7581_GPON_STATE_O1);
	if (ret)
		dev_warn_ratelimited(priv->dev,
				     "GPON session cleanup failed: %d\n", ret);
	return ret;
}

static int en7581_gpon_enter_standby(struct en7581_gpon *priv)
{
	int ret;

	if (!priv->enabled || priv->olt_disabled || READ_ONCE(priv->rogue_fault))
		return -ESHUTDOWN;

	ret = en7581_gpon_reset_session(priv, EN7581_GPON_STATE_O2);
	if (ret)
		return ret;
	ret = airoha_en7572_set_tx_enabled(priv->bosa, true);
	if (ret)
		en7581_gpon_reset_session(priv, EN7581_GPON_STATE_O1);

	return ret;
}

static void en7581_gpon_resume_ber(struct en7581_gpon *priv)
{
	if (priv->ber_interval_ms)
		mod_delayed_work(system_wq, &priv->ber_work,
				 msecs_to_jiffies(priv->ber_interval_ms));
}

static void en7581_gpon_restore_operation(struct en7581_gpon *priv)
{
	en7581_gpon_set_state(priv, EN7581_GPON_STATE_O5);
	en7581_gpon_resume_ber(priv);
	priv->popup_recovery_count++;
}

static void en7581_gpon_activation_work(struct work_struct *work)
{
	struct en7581_gpon *priv = container_of(to_delayed_work(work),
						 struct en7581_gpon,
						 activation_work);
	unsigned long remaining;
	u32 state;

	mutex_lock(&priv->lock);
	if (!priv->enabled)
		goto out;

	state = en7581_gpon_get_state(priv);
	if (!priv->activation_timer_state ||
	    state != priv->activation_timer_state)
		goto out;
	if (time_before(jiffies, priv->activation_deadline)) {
		remaining = priv->activation_deadline - jiffies;
		mod_delayed_work(system_wq, &priv->activation_work, remaining);
		goto out;
	}
	priv->activation_timer_state = 0;
	priv->activation_deadline = 0;

	if (state == EN7581_GPON_STATE_O3 || state == EN7581_GPON_STATE_O4) {
		priv->to1_timeout_count++;
		if (priv->phy_ready && !priv->los && !priv->lof) {
			if (en7581_gpon_enter_standby(priv))
				dev_warn_ratelimited(priv->dev,
						     "failed to restart GPON discovery after TO1\n");
		} else {
			en7581_gpon_reset_session(priv, EN7581_GPON_STATE_O1);
		}
	} else if (state == EN7581_GPON_STATE_O6) {
		/* READY can race the 100 ms popup deadline. */
		if (priv->phy_ready && !priv->los && !priv->lof) {
			en7581_gpon_restore_operation(priv);
		} else {
			priv->to2_timeout_count++;
			en7581_gpon_reset_session(priv, EN7581_GPON_STATE_O1);
		}
	}

out:
	mutex_unlock(&priv->lock);
}

static bool en7581_gpon_ploam_for_onu(struct en7581_gpon *priv,
				      const u8 *message, bool broadcast)
{
	return (priv->onu_assigned && message[0] == priv->onu_id) ||
	       (broadcast && message[0] == EN7581_PLOAM_BROADCAST_ID);
}

static void en7581_gpon_ploam_upstream_overhead(struct en7581_gpon *priv,
						const u8 *message)
{
	u8 flags = message[9];
	u32 phy_preamble, delimiter;
	u16 pre_delay;

	if (message[0] != EN7581_PLOAM_BROADCAST_ID ||
	    en7581_gpon_get_state(priv) != EN7581_GPON_STATE_O2) {
		priv->ploam_drop_count++;
		return;
	}

	pre_delay = get_unaligned_be16(message + 10);
	en7581_gpon_update_bits(priv, EN7581_GPON_PLOU_GUARD_BIT,
				GENMASK(7, 0), EN7581_GPON_GUARD_BITS);
	en7581_gpon_update_bits(priv, EN7581_GPON_PLOU_PREAMBLE,
				GENMASK(15, 0), message[3] | message[4] << 8);
	en7581_gpon_update_bits(priv, EN7581_GPON_PLOU_DELIMITER_BIT,
				GENMASK(7, 0), EN7581_GPON_DELIMITER_BITS);
	en7581_gpon_update_bits(priv, EN7581_GPON_PRE_ASSIGNED_DLY,
				EN7581_GPON_PRE_DELAY_MASK |
				EN7581_GPON_PRE_DELAY_ENABLE,
				FIELD_PREP(EN7581_GPON_PRE_DELAY_MASK, pre_delay) |
				((flags & BIT(5)) ?
				 EN7581_GPON_PRE_DELAY_ENABLE : 0));
	en7581_gpon_update_bits(priv, EN7581_GPON_SN_MSG_CFG,
				EN7581_GPON_SN_TX_POWER_MASK |
				EN7581_GPON_SN_REQ_THRESHOLD,
				FIELD_PREP(EN7581_GPON_SN_TX_POWER_MASK,
					   flags & GENMASK(1, 0)) |
				FIELD_PREP(EN7581_GPON_SN_REQ_THRESHOLD,
					   EN7581_GPON_SN_REQUEST_THRESHOLD));

	phy_preamble = EN7581_GPON_GUARD_BITS | message[4] << 8 |
		       message[3] << 16 | message[5] << 24;
	delimiter = 0xaa << 24 | message[6] << 16 | message[7] << 8 |
		    message[8];
	writel(phy_preamble, priv->phy_csr + EN7581_PHY_CSR_GPON_PREAMBLE);
	writel(delimiter, priv->phy_csr + EN7581_PHY_CSR_GPON_DELIMITER);
	en7581_gpon_set_state(priv, EN7581_GPON_STATE_O3);
}

static void en7581_gpon_ploam_assign_onu_id(struct en7581_gpon *priv,
					     const u8 *message)
{
	if (message[0] != EN7581_PLOAM_BROADCAST_ID ||
	    en7581_gpon_get_state(priv) != EN7581_GPON_STATE_O3 ||
	    memcmp(priv->serial, message + 3, sizeof(priv->serial))) {
		priv->ploam_drop_count++;
		return;
	}

	priv->onu_id = message[2];
	priv->onu_assigned = true;
	en7581_gpon_write(priv, EN7581_GPON_ONU_ID,
			   EN7581_GPON_ONU_ID_VALID |
			   FIELD_PREP(EN7581_GPON_ONU_ID_MASK, priv->onu_id));
	en7581_gpon_set_state(priv, EN7581_GPON_STATE_O4);
}

static void en7581_gpon_ploam_ranging_time(struct en7581_gpon *priv,
					   const u8 *message)
{
	bool popup_reranging = priv->popup_reranging;
	u32 eqd;

	if (!en7581_gpon_ploam_for_onu(priv, message, false) ||
	    en7581_gpon_get_state(priv) != EN7581_GPON_STATE_O4 ||
	    (message[2] & BIT(0))) {
		priv->ploam_drop_count++;
		return;
	}

	eqd = get_unaligned_be32(message + 3);
	priv->eqd = eqd;
	en7581_gpon_write(priv, EN7581_GPON_EQD, eqd & ~GENMASK(2, 0));
	en7581_gpon_set_bit_delay(priv, eqd & GENMASK(2, 0));
	en7581_gpon_set_tcont(priv, 0, priv->onu_id, true);
	en7581_gpon_update_bits(priv, EN7581_GPON_GBL_CFG,
				EN7581_GPON_US_FEC_ENABLE,
				EN7581_GPON_US_FEC_ENABLE);
	en7581_gpon_set_state(priv, EN7581_GPON_STATE_O5);
	priv->popup_reranging = false;
	if (popup_reranging) {
		en7581_gpon_resume_ber(priv);
		priv->popup_recovery_count++;
	}
}

static void en7581_gpon_ploam_deactivate(struct en7581_gpon *priv,
					 const u8 *message)
{
	u32 state = en7581_gpon_get_state(priv);
	int ret;

	if (!en7581_gpon_ploam_for_onu(priv, message, true) ||
	    (state != EN7581_GPON_STATE_O4 &&
	     state != EN7581_GPON_STATE_O5 &&
	     state != EN7581_GPON_STATE_O6)) {
		priv->ploam_drop_count++;
		return;
	}

	ret = en7581_gpon_reset_session(priv, EN7581_GPON_STATE_O2);
	if (!ret && priv->enabled && !priv->rogue_fault) {
		ret = airoha_en7572_set_tx_enabled(priv->bosa, true);
		if (ret)
			en7581_gpon_reset_session(priv, EN7581_GPON_STATE_O1);
	}
	if (ret)
		priv->ploam_drop_count++;
}

static void en7581_gpon_ploam_request_password(struct en7581_gpon *priv,
						const u8 *message)
{
	u8 response[EN7581_GPON_PLOAM_SIZE] = {
		priv->onu_id, EN7581_PLOAMU_PASSWORD,
	};

	if (!en7581_gpon_ploam_for_onu(priv, message, false) ||
	    en7581_gpon_get_state(priv) != EN7581_GPON_STATE_O5 ||
	    !priv->password_set) {
		priv->ploam_drop_count++;
		return;
	}

	memcpy(response + 2, priv->password, sizeof(priv->password));
	if (en7581_gpon_send_ploamu(priv, response,
				     EN7581_GPON_PLOAM_REPEATS))
		priv->ploam_drop_count++;
}

static void en7581_gpon_ploam_assign_alloc_id(struct en7581_gpon *priv,
					       const u8 *message)
{
	u16 alloc_id = message[2] << 4 | (message[3] & GENMASK(3, 0));
	int index, ret = 0;

	if (!en7581_gpon_ploam_for_onu(priv, message, false) ||
	    en7581_gpon_get_state(priv) != EN7581_GPON_STATE_O5) {
		priv->ploam_drop_count++;
		return;
	}

	if (alloc_id != priv->onu_id) {
		if (message[4] == 0x01) {
			ret = en7581_gpon_alloc_tcont(priv, alloc_id);
		} else if (message[4] == 0xff) {
			index = en7581_gpon_find_tcont(priv, alloc_id);
			if (index >= 0)
				en7581_gpon_set_tcont(priv, index, 0, false);
		} else {
			ret = -EINVAL;
		}
	}

	if (ret) {
		priv->ploam_drop_count++;
		return;
	}
	if (en7581_gpon_send_ack(priv, message))
		priv->ploam_drop_count++;
}

struct en7581_gpon_port_transaction_context {
	struct en7581_gpon *priv;
	const u8 *message;
};

static int en7581_gpon_port_transaction_set_gem(void *context, u16 port,
						 bool valid, bool encrypted)
{
	struct en7581_gpon_port_transaction_context *transaction = context;

	return en7581_gpon_set_gem(transaction->priv, port, valid, encrypted);
}

static int en7581_gpon_port_transaction_set_omcc(void *context, u16 port,
						  bool valid)
{
	struct en7581_gpon_port_transaction_context *transaction = context;

	return airoha_gpon_set_omcc(transaction->priv->ethernet_np, port, valid);
}

static void en7581_gpon_port_transaction_commit(void *context, u16 port,
						 bool valid)
{
	struct en7581_gpon_port_transaction_context *transaction = context;
	struct en7581_gpon *priv = transaction->priv;

	priv->omcc_id = valid ? port : 0;
	if (valid) {
		en7581_gpon_write(priv, EN7581_GPON_OMCI_ID,
				   EN7581_GPON_OMCI_ID_VALID |
				   FIELD_PREP(EN7581_GPON_OMCI_ID_MASK, port));
		en7581_gpon_set_tcont(priv, 0, priv->onu_id, true);
	} else {
		en7581_gpon_write(priv, EN7581_GPON_OMCI_ID, 0);
	}
}

static int en7581_gpon_port_transaction_acknowledge(void *context)
{
	struct en7581_gpon_port_transaction_context *transaction = context;

	return en7581_gpon_send_ack(transaction->priv, transaction->message);
}

static void en7581_gpon_port_transaction_fail_closed(void *context)
{
	struct en7581_gpon_port_transaction_context *transaction = context;

	en7581_gpon_reset_session(transaction->priv, EN7581_GPON_STATE_O1);
}

static const struct airoha_gpon_port_transaction_ops
en7581_gpon_port_transaction_ops = {
	.set_gem = en7581_gpon_port_transaction_set_gem,
	.set_omcc = en7581_gpon_port_transaction_set_omcc,
	.commit_omcc = en7581_gpon_port_transaction_commit,
	.acknowledge = en7581_gpon_port_transaction_acknowledge,
	.fail_closed = en7581_gpon_port_transaction_fail_closed,
};

static void en7581_gpon_ploam_configure_port_id(struct en7581_gpon *priv,
						 const u8 *message)
{
	u16 port_id = message[3] << 4 | (message[4] & GENMASK(3, 0));
	u32 omci_config = en7581_gpon_read(priv, EN7581_GPON_OMCI_ID);
	struct airoha_gpon_port_snapshot snapshot = {
		.active_port = priv->omcc_id,
		.active_valid = omci_config & EN7581_GPON_OMCI_ID_VALID,
		.active_encrypted = test_bit(priv->omcc_id,
					     priv->gem_encrypted),
		.target_valid = test_bit(port_id, priv->gem_valid),
		.target_encrypted = test_bit(port_id, priv->gem_encrypted),
	};
	struct en7581_gpon_port_transaction_context context = {
		.priv = priv,
		.message = message,
	};
	int ret;

	if (!en7581_gpon_ploam_for_onu(priv, message, false) ||
	    en7581_gpon_get_state(priv) != EN7581_GPON_STATE_O5) {
		priv->ploam_drop_count++;
		return;
	}

	ret = airoha_gpon_configure_port_transaction(
		&en7581_gpon_port_transaction_ops, &context, port_id,
		message[2] & BIT(0), &snapshot);
	if (ret)
		priv->ploam_drop_count++;
}

static void en7581_gpon_ploam_disable_serial(struct en7581_gpon *priv,
						      const u8 *message)
{
	u8 mode = message[2];
	bool matches = !memcmp(priv->serial, message + 3, sizeof(priv->serial));
	u32 state = en7581_gpon_get_state(priv);
	int ret;

	if (message[0] != EN7581_PLOAM_BROADCAST_ID) {
		priv->ploam_drop_count++;
		return;
	}

	if (state == EN7581_GPON_STATE_O7 && priv->olt_disabled &&
	    (mode == 0x0f || (mode == 0x00 && matches))) {
		ret = en7581_gpon_reset_session(priv, EN7581_GPON_STATE_O2);
		if (ret) {
			en7581_gpon_set_state(priv, EN7581_GPON_STATE_O7);
			priv->ploam_drop_count++;
			return;
		}
		priv->olt_disabled = false;
		if (!priv->enabled || READ_ONCE(priv->rogue_fault))
			return;

		ret = airoha_en7572_clear_fault(priv->bosa);
		if (ret) {
			priv->olt_disabled = true;
			en7581_gpon_set_state(priv, EN7581_GPON_STATE_O7);
			priv->ploam_drop_count++;
			return;
		}
		ret = airoha_en7572_set_tx_enabled(priv->bosa, true);
		if (ret) {
			priv->olt_disabled = true;
			airoha_en7572_emergency_disable(priv->bosa);
			en7581_gpon_set_state(priv, EN7581_GPON_STATE_O7);
			priv->ploam_drop_count++;
		}
		return;
	}

	if (state != EN7581_GPON_STATE_O1 &&
	    (mode == 0xf0 || (mode == 0xff && matches))) {
		priv->olt_disabled = true;
		ret = en7581_gpon_reset_session(priv, EN7581_GPON_STATE_O7);
		if (ret) {
			en7581_gpon_set_state(priv, EN7581_GPON_STATE_O7);
			priv->ploam_drop_count++;
		}
	}
}

static void en7581_gpon_ploam_encrypted_port(struct en7581_gpon *priv,
					      const u8 *message)
{
	u16 port_id = message[3] << 4 | (message[4] & GENMASK(3, 0));
	bool encrypted = (message[2] & GENMASK(1, 0)) == GENMASK(1, 0);
	struct en7581_gpon_port_transaction_context context = {
		.priv = priv,
		.message = message,
	};
	int ret;

	if (!en7581_gpon_ploam_for_onu(priv, message, false) ||
	    en7581_gpon_get_state(priv) != EN7581_GPON_STATE_O5) {
		priv->ploam_drop_count++;
		return;
	}

	ret = airoha_gpon_encrypted_port_transaction(
		&en7581_gpon_port_transaction_ops, &context, port_id,
		test_bit(port_id, priv->gem_valid),
		test_bit(port_id, priv->gem_encrypted), encrypted);
	if (ret)
		priv->ploam_drop_count++;
}

static void en7581_gpon_ploam_request_key(struct en7581_gpon *priv,
					   const u8 *message)
{
	u8 response[EN7581_GPON_PLOAM_SIZE] = {
		priv->onu_id, EN7581_PLOAMU_ENCRYPTION_KEY,
	};
	int fragment;

	if (!en7581_gpon_ploam_for_onu(priv, message, false) ||
	    en7581_gpon_get_state(priv) != EN7581_GPON_STATE_O5) {
		priv->ploam_drop_count++;
		return;
	}

	get_random_bytes(priv->encryption_key, sizeof(priv->encryption_key));
	priv->encryption_key_index ^= 1;
	en7581_gpon_write_aes_key(priv, priv->encryption_key);

	for (fragment = 0; fragment < 2; fragment++) {
		response[2] = priv->encryption_key_index;
		response[3] = fragment;
		memcpy(response + 4, priv->encryption_key + fragment * 8, 8);
		if (en7581_gpon_send_ploamu(priv, response,
					     EN7581_GPON_PLOAM_REPEATS)) {
			priv->ploam_drop_count++;
			return;
		}
	}
}

static void en7581_gpon_ploam_popup(struct en7581_gpon *priv,
				     const u8 *message)
{
	if (!en7581_gpon_ploam_for_onu(priv, message, true) ||
	    en7581_gpon_get_state(priv) != EN7581_GPON_STATE_O6) {
		priv->ploam_drop_count++;
		return;
	}

	if (message[0] == EN7581_PLOAM_BROADCAST_ID) {
		priv->popup_reranging = true;
		en7581_gpon_set_state(priv, EN7581_GPON_STATE_O4);
	} else {
		en7581_gpon_restore_operation(priv);
	}
}

static void en7581_gpon_ploam_key_switch(struct en7581_gpon *priv,
					  const u8 *message)
{
	u32 counter;

	if (!en7581_gpon_ploam_for_onu(priv, message, false) ||
	    en7581_gpon_get_state(priv) != EN7581_GPON_STATE_O5) {
		priv->ploam_drop_count++;
		return;
	}

	counter = get_unaligned_be32(message + 2);
	en7581_gpon_update_bits(priv, EN7581_GPON_AES_CFG,
				EN7581_GPON_AES_SPF_MASK,
				counter & EN7581_GPON_AES_SPF_MASK);
	if (en7581_gpon_send_ack(priv, message))
		priv->ploam_drop_count++;
}

static void en7581_gpon_ploam_ber_interval(struct en7581_gpon *priv,
					    const u8 *message)
{
	u32 frames, interval;

	if (!en7581_gpon_ploam_for_onu(priv, message, true) ||
	    en7581_gpon_get_state(priv) != EN7581_GPON_STATE_O5) {
		priv->ploam_drop_count++;
		return;
	}

	frames = get_unaligned_be32(message + 2);
	interval = frames >> 3;
	en7581_gpon_stop_ber(priv);
	if (interval) {
		priv->ber_interval_ms = interval;
		mod_delayed_work(system_wq, &priv->ber_work,
				 msecs_to_jiffies(interval));
	}
	if (en7581_gpon_send_ack(priv, message))
		priv->ploam_drop_count++;
}

static void en7581_gpon_handle_ploam(struct en7581_gpon *priv,
				     const u8 *message)
{
	switch (message[1]) {
	case EN7581_PLOAM_UPSTREAM_OVERHEAD:
		en7581_gpon_ploam_upstream_overhead(priv, message);
		break;
	case EN7581_PLOAM_ASSIGN_ONU_ID:
		en7581_gpon_ploam_assign_onu_id(priv, message);
		break;
	case EN7581_PLOAM_RANGING_TIME:
		en7581_gpon_ploam_ranging_time(priv, message);
		break;
	case EN7581_PLOAM_DEACTIVATE_ONU_ID:
		en7581_gpon_ploam_deactivate(priv, message);
		break;
	case EN7581_PLOAM_DISABLE_SERIAL_NUMBER:
		en7581_gpon_ploam_disable_serial(priv, message);
		break;
	case EN7581_PLOAM_ENCRYPTED_PORT_ID:
		en7581_gpon_ploam_encrypted_port(priv, message);
		break;
	case EN7581_PLOAM_REQUEST_PASSWORD:
		en7581_gpon_ploam_request_password(priv, message);
		break;
	case EN7581_PLOAM_ASSIGN_ALLOC_ID:
		en7581_gpon_ploam_assign_alloc_id(priv, message);
		break;
	case EN7581_PLOAM_POPUP:
		en7581_gpon_ploam_popup(priv, message);
		break;
	case EN7581_PLOAM_REQUEST_KEY:
		en7581_gpon_ploam_request_key(priv, message);
		break;
	case EN7581_PLOAM_CONFIGURE_PORT_ID:
		en7581_gpon_ploam_configure_port_id(priv, message);
		break;
	case EN7581_PLOAM_BER_INTERVAL:
		en7581_gpon_ploam_ber_interval(priv, message);
		break;
	case EN7581_PLOAM_KEY_SWITCHING_TIME:
		en7581_gpon_ploam_key_switch(priv, message);
		break;
	default:
		priv->ploam_unknown_count++;
		break;
	}
}

static int en7581_gpon_phy_init(struct en7581_gpon *priv)
{
	u32 value;
	int ret;

	priv->init_stage = EN7581_GPON_INIT_PHY_SETTINGS;
	writel(EN7581_EN7572_PHY_SETTING,
	       priv->phy_csr + EN7581_PHY_CSR_XPON_SETTING);
	value = readl(priv->phy_csr + EN7581_PHY_CSR_ERRCNT_EN);
	writel(value | EN7581_PHY_ERRCNT_FEC_ENABLE |
	       EN7581_PHY_ERRCNT_BIP_ENABLE,
	       priv->phy_csr + EN7581_PHY_CSR_ERRCNT_EN);
	writel(EN7581_PHY_ERRCNT_BIP_CLEAR,
	       priv->phy_csr + EN7581_PHY_CSR_ERRCNT_CTL);
	writel(EN7581_EN7572_PMA_SETTING_0,
	       priv->pma + EN7581_PMA_XPON_SETTING_0);
	writel(EN7581_EN7572_PMA_SETTING_1,
	       priv->pma + EN7581_PMA_XPON_SETTING_1);

	priv->init_phy_setting =
		readl(priv->phy_csr + EN7581_PHY_CSR_XPON_SETTING);
	priv->init_errcnt_enable =
		readl(priv->phy_csr + EN7581_PHY_CSR_ERRCNT_EN);
	priv->init_pma_setting_0 =
		readl(priv->pma + EN7581_PMA_XPON_SETTING_0);
	priv->init_pma_setting_1 =
		readl(priv->pma + EN7581_PMA_XPON_SETTING_1);
	if (priv->init_phy_setting != EN7581_EN7572_PHY_SETTING ||
	    !(priv->init_errcnt_enable & EN7581_PHY_ERRCNT_BIP_ENABLE) ||
	    priv->init_pma_setting_0 != EN7581_EN7572_PMA_SETTING_0 ||
	    priv->init_pma_setting_1 != EN7581_EN7572_PMA_SETTING_1) {
		dev_err(priv->dev,
			"GPON PHY setting readback failed: phy=0x%08x expected=0x%08x errcnt=0x%08x pma0=0x%08x expected0=0x%08x pma1=0x%08x expected1=0x%08x\n",
			priv->init_phy_setting, EN7581_EN7572_PHY_SETTING,
			priv->init_errcnt_enable, priv->init_pma_setting_0,
			EN7581_EN7572_PMA_SETTING_0,
			priv->init_pma_setting_1, EN7581_EN7572_PMA_SETTING_1);
		return -EIO;
	}

	priv->init_stage = EN7581_GPON_INIT_ROGUE_IRQ;
	value = readl(priv->pma + EN7581_PMA_XPON_INT_STA_0);
	writel(value | EN7581_PMA_ROGUE_ONU,
	       priv->pma + EN7581_PMA_XPON_INT_STA_0);
	value = readl(priv->pma + EN7581_PMA_XPON_INT_EN_0);
	writel(value | EN7581_PMA_ROGUE_ONU,
	       priv->pma + EN7581_PMA_XPON_INT_EN_0);
	priv->init_pma_interrupt_enable =
		readl(priv->pma + EN7581_PMA_XPON_INT_EN_0);
	if (!(priv->init_pma_interrupt_enable & EN7581_PMA_ROGUE_ONU)) {
		dev_err(priv->dev,
			"GPON rogue-ONU interrupt readback failed: enable=0x%08x required=0x%08x\n",
			priv->init_pma_interrupt_enable,
			(u32)EN7581_PMA_ROGUE_ONU);
		return -EIO;
	}

	priv->init_stage = EN7581_GPON_INIT_SCU_GPIO;
	ret = regmap_update_bits(priv->chip_scu, EN7581_SCU_FORCE_GPIO32_EN,
				 EN7581_SCU_FORCE_GPIO41_EN, 0);
	if (ret) {
		dev_err(priv->dev,
			"failed to release GPON GPIO41 force-enable: %d\n", ret);
		return ret;
	}
	ret = regmap_read(priv->chip_scu, EN7581_SCU_FORCE_GPIO32_EN, &value);
	if (ret) {
		dev_err(priv->dev,
			"failed to read GPON GPIO41 force-enable: %d\n", ret);
		return ret;
	}
	priv->init_scu_gpio_force = value;
	if (value & EN7581_SCU_FORCE_GPIO41_EN) {
		dev_err(priv->dev,
			"GPON GPIO41 force-enable remained set: scu=0x%08x mask=0x%08x\n",
			value, (u32)EN7581_SCU_FORCE_GPIO41_EN);
		return -EIO;
	}

	writel(EN7581_PHY_INT_MASK,
	       priv->phy_csr + EN7581_PHY_CSR_XPON_INT_CLEAR);
	writel(EN7581_PHY_INT_MASK,
	       priv->phy_csr + EN7581_PHY_CSR_XPON_INT_ENABLE);
	priv->los = !!(readl(priv->phy_csr + EN7581_PHY_CSR_XPON_STATUS) &
		       EN7581_PHY_XPON_STATUS_LOS);
	priv->phy_ready = !priv->los &&
		(readl(priv->phy_csr + EN7581_PHY_CSR_RX_STATUS) &
		 EN7581_PHY_RX_SYNC_MASK) == EN7581_PHY_RX_SYNC_OK;
	priv->lof = false;

	priv->safety_ready = true;
	return 0;
}

static irqreturn_t en7581_gpon_phy_irq(int irq, void *data)
{
	struct en7581_gpon *priv = data;
	u32 pma_status, phy_status;

	if (READ_ONCE(priv->xpon_irqs_masked) ||
	    !airoha_xpon_backend_is_active(priv->xpon_backend))
		return IRQ_NONE;
	pma_status = readl(priv->pma + EN7581_PMA_XPON_INT_STA_0);
	phy_status = readl(priv->phy_csr + EN7581_PHY_CSR_XPON_INT_STATUS);

	if (!(pma_status & EN7581_PMA_ROGUE_ONU) &&
	    !(phy_status & EN7581_PHY_INT_MASK))
		return IRQ_NONE;

	if (pma_status & EN7581_PMA_ROGUE_ONU) {
		WRITE_ONCE(priv->rogue_fault, true);
		airoha_en7572_emergency_disable(priv->bosa);
	}
	return IRQ_WAKE_THREAD;
}

static irqreturn_t en7581_gpon_phy_irq_thread(int irq, void *data)
{
	struct en7581_gpon *priv = data;
	u32 pma_status, phy_status, value;
	u32 state;

	if (READ_ONCE(priv->xpon_irqs_masked) ||
	    !airoha_xpon_backend_is_active(priv->xpon_backend))
		return IRQ_HANDLED;
	mutex_lock(&priv->lock);
	phy_status = readl(priv->phy_csr + EN7581_PHY_CSR_XPON_INT_STATUS) &
		     EN7581_PHY_INT_MASK;
	if (phy_status)
		writel(phy_status,
		       priv->phy_csr + EN7581_PHY_CSR_XPON_INT_CLEAR);
	if (phy_status & EN7581_PHY_INT_LOS) {
		priv->los = true;
		priv->phy_ready = false;
	}
	if (phy_status & EN7581_PHY_INT_NO_LOS)
		priv->los = false;
	if (phy_status & EN7581_PHY_INT_LOF) {
		priv->lof = true;
		priv->phy_ready = false;
	}
	if (phy_status & EN7581_PHY_INT_READY) {
		priv->phy_ready = true;
		priv->los = false;
		priv->lof = false;
	}

	state = en7581_gpon_get_state(priv);
	if (priv->enabled && !priv->olt_disabled && priv->phy_ready) {
		if (state == EN7581_GPON_STATE_O6) {
			en7581_gpon_restore_operation(priv);
		} else if (state == EN7581_GPON_STATE_O1 &&
			   en7581_gpon_enter_standby(priv)) {
			dev_warn_ratelimited(priv->dev,
					     "failed to enter GPON standby after PHY ready\n");
		}
	} else if (priv->enabled &&
		   (phy_status & (EN7581_PHY_INT_LOS | EN7581_PHY_INT_LOF))) {
		if (state == EN7581_GPON_STATE_O5) {
			cancel_delayed_work(&priv->ber_work);
			en7581_gpon_set_state(priv, EN7581_GPON_STATE_O6);
		} else if (state == EN7581_GPON_STATE_O2 ||
			   state == EN7581_GPON_STATE_O3 ||
			   state == EN7581_GPON_STATE_O4) {
			en7581_gpon_reset_session(priv, EN7581_GPON_STATE_O1);
		}
	}

	pma_status = readl(priv->pma + EN7581_PMA_XPON_INT_STA_0);
	if (pma_status & EN7581_PMA_ROGUE_ONU) {
		value = readl(priv->pma + EN7581_PMA_XPON_INT_EN_0);
		writel(value & ~EN7581_PMA_ROGUE_ONU,
		       priv->pma + EN7581_PMA_XPON_INT_EN_0);
		writel(pma_status, priv->pma + EN7581_PMA_XPON_INT_STA_0);
		en7581_gpon_write(priv, EN7581_GPON_INT_ENABLE, 0);
		if (en7581_gpon_reset_session(priv, EN7581_GPON_STATE_O7))
			en7581_gpon_set_state(priv, EN7581_GPON_STATE_O7);
		priv->enabled = false;
	}
	priv->phy_irq_count++;
	mutex_unlock(&priv->lock);

	if (pma_status & EN7581_PMA_ROGUE_ONU)
		dev_crit_ratelimited(priv->dev,
				     "rogue-ONU condition detected; optical TX locked off\n");
	return IRQ_HANDLED;
}

static int en7581_gpon_hw_init(struct en7581_gpon *priv)
{
	int ret;

	priv->init_stage = EN7581_GPON_INIT_MAC_IRQ_MASK;
	ret = en7581_gpon_set_mac_irq_enable(priv, 0);
	if (ret) {
		dev_err(priv->dev, "failed to mask GPON MAC interrupts: %d\n",
			ret);
		return ret;
	}
	en7581_gpon_write(priv, EN7581_GPON_INT_STATUS, U32_MAX);

	priv->init_stage = EN7581_GPON_INIT_SESSION_RESET;
	ret = en7581_gpon_reset_session(priv, EN7581_GPON_STATE_O1);
	if (ret) {
		dev_err(priv->dev, "failed to reset GPON session during init: %d\n",
			ret);
		return ret;
	}
	en7581_gpon_update_bits(priv, EN7581_GPON_GBL_CFG,
				EN7581_GPON_DBA_BLOCK_MASK,
				FIELD_PREP(EN7581_GPON_DBA_BLOCK_MASK,
					   EN7581_GPON_DBA_BLOCK_48));
	en7581_gpon_update_bits(priv, EN7581_GPON_RSP_TIME,
				EN7581_GPON_RSP_TIME_MASK,
				FIELD_PREP(EN7581_GPON_RSP_TIME_MASK,
					   EN7581_GPON_RESPONSE_TIME));
	en7581_gpon_update_bits(priv, EN7581_GPON_DBG_DLY,
				EN7581_GPON_FINE_DLY_MASK,
				FIELD_PREP(EN7581_GPON_FINE_DLY_MASK,
					   EN7581_GPON_FINE_DELAY));
	en7581_gpon_update_bits(priv, EN7581_GPON_DBG_IDLE_GEM_THLD,
				EN7581_GPON_IDLE_GEM_MASK,
				FIELD_PREP(EN7581_GPON_IDLE_GEM_MASK,
					   priv->idle_gem_threshold));
	if (priv->serial_set)
		en7581_gpon_write_serial(priv);
	return 0;
}

static irqreturn_t en7581_gpon_irq(int irq, void *data)
{
	struct en7581_gpon *priv = data;
	u32 enabled, status;

	if (READ_ONCE(priv->xpon_irqs_masked) ||
	    !airoha_xpon_backend_is_active(priv->xpon_backend))
		return IRQ_NONE;
	enabled = en7581_gpon_read(priv, EN7581_GPON_INT_ENABLE);
	status = en7581_gpon_read(priv, EN7581_GPON_INT_STATUS);
	if (!(status & enabled))
		return IRQ_NONE;

	return IRQ_WAKE_THREAD;
}

static void en7581_gpon_drain_ploam(struct en7581_gpon *priv)
{
	u32 fifo_status, word;
	unsigned int messages = 0;
	int i;

	while (messages++ < EN7581_GPON_MAX_IRQ_PLOAMS) {
		fifo_status = en7581_gpon_read(priv,
					      EN7581_GPON_PLOAMD_FIFO_STS);
		if (fifo_status & EN7581_GPON_PLOAMD_OVERRUN)
			priv->fifo_overrun_count++;
		if (FIELD_GET(EN7581_GPON_PLOAMD_USED_MASK, fifo_status) < 3)
			break;

		for (i = 0; i < 3; i++) {
			word = en7581_gpon_read(priv,
						EN7581_GPON_PLOAMD_RDATA);
			put_unaligned_be32(word, priv->last_ploam + i * 4);
		}
		priv->have_last_ploam = true;
		priv->ploam_rx_count++;
		en7581_gpon_handle_ploam(priv, priv->last_ploam);
	}
}

static irqreturn_t en7581_gpon_irq_thread(int irq, void *data)
{
	struct en7581_gpon *priv = data;
	u32 status;

	if (READ_ONCE(priv->xpon_irqs_masked) ||
	    !airoha_xpon_backend_is_active(priv->xpon_backend))
		return IRQ_HANDLED;
	mutex_lock(&priv->lock);
	status = en7581_gpon_read(priv, EN7581_GPON_INT_STATUS) &
		en7581_gpon_read(priv, EN7581_GPON_INT_ENABLE);
	if (!status)
		goto out;

	priv->irq_count++;
	if (status & EN7581_GPON_INT_PLOAMD)
		en7581_gpon_drain_ploam(priv);
	if (status & EN7581_GPON_INT_ERRORS)
		priv->error_irq_count++;
	en7581_gpon_write(priv, EN7581_GPON_INT_STATUS, status);

out:
	mutex_unlock(&priv->lock);
	return IRQ_HANDLED;
}

static ssize_t enabled_show(struct device *dev,
			    struct device_attribute *attribute, char *buf)
{
	struct en7581_gpon *priv = dev_get_drvdata(dev);

	return sysfs_emit(buf, "%u\n", priv->enabled);
}

static ssize_t enabled_store(struct device *dev,
			     struct device_attribute *attribute,
			     const char *buf, size_t count)
{
	struct en7581_gpon *priv = dev_get_drvdata(dev);
	bool enabled;
	int irq_ret, ret;

	ret = kstrtobool(buf, &enabled);
	if (ret)
		return ret;
	ret = airoha_xpon_backend_activation_lock(priv->xpon_backend);
	if (ret)
		return ret;
	if (enabled && !priv->serial_set) {
		ret = -ENODATA;
		goto unlock_owner;
	}
	if (enabled && !priv->safety_ready) {
		ret = -EOPNOTSUPP;
		goto unlock_owner;
	}
	if (enabled && READ_ONCE(priv->rogue_fault)) {
		ret = -EIO;
		goto unlock_owner;
	}

	mutex_lock(&priv->lock);
	if (enabled == priv->enabled && (enabled || !priv->rogue_fault))
		goto out;

	if (enabled) {
		ret = en7581_gpon_hw_init(priv);
		if (ret)
			goto out;
		ret = airoha_en7572_clear_fault(priv->bosa);
		if (ret)
			goto out;
		if (READ_ONCE(priv->rogue_fault)) {
			ret = -EIO;
			goto out;
		}
		ret = en7581_gpon_xpon_unmask_irqs(priv);
		if (ret)
			goto out;
		ret = en7581_gpon_set_mac_irq_enable(priv,
						 EN7581_GPON_INT_MASK);
		if (ret)
			goto out;
		priv->enabled = true;
		if (priv->phy_ready && !priv->los && !priv->lof)
			ret = en7581_gpon_enter_standby(priv);
		else
			ret = 0;
		if (ret) {
			priv->enabled = false;
			en7581_gpon_xpon_mask_irqs(priv);
			en7581_gpon_reset_session(priv,
						 EN7581_GPON_STATE_O1);
			goto out;
		}
		dev_notice(dev, "GPON activation enabled\n");
	} else {
		priv->olt_disabled = false;
		irq_ret = en7581_gpon_set_mac_irq_enable(priv, 0);
		en7581_gpon_write(priv, EN7581_GPON_INT_STATUS, U32_MAX);
		ret = en7581_gpon_reset_session(priv, EN7581_GPON_STATE_O1);
		if (!ret)
			ret = irq_ret;
		priv->enabled = false;
		if (ret)
			goto out;
		priv->rogue_fault = false;
		ret = airoha_en7572_clear_fault(priv->bosa);
		if (ret)
			goto out;
	}

out:
	mutex_unlock(&priv->lock);
	if (ret)
		airoha_en7572_set_tx_enabled(priv->bosa, false);

unlock_owner:
	airoha_xpon_backend_activation_unlock(priv->xpon_backend);
	return ret ? ret : count;
}
static DEVICE_ATTR_RW(enabled);

static ssize_t serial_number_show(struct device *dev,
				  struct device_attribute *attribute, char *buf)
{
	struct en7581_gpon *priv = dev_get_drvdata(dev);

	if (!priv->serial_set)
		return sysfs_emit(buf, "unset\n");

	return sysfs_emit(buf, "%c%c%c%c%02X%02X%02X%02X\n",
			  priv->serial[0], priv->serial[1], priv->serial[2],
			  priv->serial[3], priv->serial[4], priv->serial[5],
			  priv->serial[6], priv->serial[7]);
}

static ssize_t serial_number_store(struct device *dev,
				   struct device_attribute *attribute,
				   const char *buf, size_t count)
{
	struct en7581_gpon *priv = dev_get_drvdata(dev);
	u8 serial[8];
	int ret;

	ret = en7581_gpon_parse_serial(buf, count, serial);
	if (ret)
		return ret;

	mutex_lock(&priv->lock);
	if (priv->enabled) {
		ret = -EBUSY;
		goto out;
	}
	memcpy(priv->serial, serial, sizeof(serial));
	priv->serial_set = true;
	en7581_gpon_write_serial(priv);
	ret = count;

out:
	mutex_unlock(&priv->lock);
	return ret;
}
static DEVICE_ATTR_RW(serial_number);

static ssize_t password_store(struct device *dev,
			      struct device_attribute *attribute,
			      const char *buf, size_t count)
{
	struct en7581_gpon *priv = dev_get_drvdata(dev);
	u8 password[EN7581_GPON_PASSWORD_SIZE];
	bool password_set;
	int ret;

	ret = en7581_gpon_parse_password(buf, count, password, &password_set);
	if (ret)
		return ret;

	mutex_lock(&priv->lock);
	if (priv->enabled) {
		ret = -EBUSY;
		goto out;
	}
	memcpy(priv->password, password, sizeof(priv->password));
	priv->password_set = password_set;
	ret = count;

out:
	mutex_unlock(&priv->lock);
	return ret;
}
static DEVICE_ATTR_WO(password);

static const char *en7581_gpon_state_name(u32 state)
{
	static const char * const names[] = {
		[EN7581_GPON_STATE_O1] = "O1-initial",
		[EN7581_GPON_STATE_O2] = "O2-standby",
		[EN7581_GPON_STATE_O3] = "O3-serial-number",
		[EN7581_GPON_STATE_O4] = "O4-ranging",
		[EN7581_GPON_STATE_O5] = "O5-operation",
		[EN7581_GPON_STATE_O6] = "O6-popup",
		[EN7581_GPON_STATE_O7] = "O7-emergency-stop",
	};

	if (state >= ARRAY_SIZE(names) || !names[state])
		return "unknown";

	return names[state];
}

static ssize_t state_show(struct device *dev,
			  struct device_attribute *attribute, char *buf)
{
	struct en7581_gpon *priv = dev_get_drvdata(dev);
	u32 state = en7581_gpon_get_state(priv);

	return sysfs_emit(buf, "%u %s\n", state,
			  en7581_gpon_state_name(state));
}
static DEVICE_ATTR_RO(state);

static ssize_t onu_id_show(struct device *dev,
			   struct device_attribute *attribute, char *buf)
{
	struct en7581_gpon *priv = dev_get_drvdata(dev);
	u32 value = en7581_gpon_read(priv, EN7581_GPON_ONU_ID);

	if (!(value & EN7581_GPON_ONU_ID_VALID))
		return sysfs_emit(buf, "unassigned\n");

	return sysfs_emit(buf, "%u\n", (u32)FIELD_GET(
			  EN7581_GPON_ONU_ID_MASK, value));
}
static DEVICE_ATTR_RO(onu_id);

static ssize_t omcc_id_show(struct device *dev,
			    struct device_attribute *attribute, char *buf)
{
	struct en7581_gpon *priv = dev_get_drvdata(dev);
	u32 value = en7581_gpon_read(priv, EN7581_GPON_OMCI_ID);

	if (!(value & EN7581_GPON_OMCI_ID_VALID))
		return sysfs_emit(buf, "unassigned\n");

	return sysfs_emit(buf, "%u\n", (u32)FIELD_GET(
			  EN7581_GPON_OMCI_ID_MASK, value));
}
static DEVICE_ATTR_RO(omcc_id);

static ssize_t equalization_delay_show(struct device *dev,
				       struct device_attribute *attribute,
				       char *buf)
{
	struct en7581_gpon *priv = dev_get_drvdata(dev);

	return sysfs_emit(buf, "%u byte_delay=%u bit_delay=%u\n", priv->eqd,
			  (u32)(priv->eqd & ~GENMASK(2, 0)), priv->bit_delay);
}
static DEVICE_ATTR_RO(equalization_delay);

static ssize_t tconts_show(struct device *dev,
			   struct device_attribute *attribute, char *buf)
{
	struct en7581_gpon *priv = dev_get_drvdata(dev);
	ssize_t len = 0;
	int i;

	mutex_lock(&priv->lock);
	for (i = 0; i < EN7581_GPON_MAX_TCONTS; i++) {
		if (!priv->alloc_valid[i])
			continue;
		len += sysfs_emit_at(buf, len, "%s%u:%u",
				     len ? " " : "", i, priv->alloc_ids[i]);
	}
	if (!len)
		len = sysfs_emit(buf, "none");
	len += sysfs_emit_at(buf, len, "\n");
	mutex_unlock(&priv->lock);

	return len;
}
static DEVICE_ATTR_RO(tconts);

static ssize_t qos_show(struct device *dev,
			struct device_attribute *attribute, char *buf)
{
	struct en7581_gpon *priv = dev_get_drvdata(dev);
	struct airoha_gpon_qos_config cfg[AIROHA_GPON_QOS_CHANNELS];
	unsigned int count = ARRAY_SIZE(cfg), i, queue;
	ssize_t len = 0;
	int ret;

	ret = airoha_gpon_get_qos(priv->ethernet_np, cfg, &count);
	if (ret)
		return ret;
	if (!count)
		return sysfs_emit(buf, "off\n");
	for (i = 0; i < count; i++) {
		len += sysfs_emit_at(buf, len, "%s%u:%u",
				     i ? " " : "", cfg[i].channel,
				     cfg[i].scheduler);
		for (queue = 0; queue < AIROHA_GPON_QOS_QUEUES; queue++)
			len += sysfs_emit_at(buf, len, ":%u", cfg[i].weights[queue]);
		len += sysfs_emit_at(buf, len, ":%u:%u:%u:%u", cfg[i].cir,
				     cfg[i].pir, cfg[i].cbs, cfg[i].pbs);
	}
	return sysfs_emit_at(buf, len, "\n");
}

static ssize_t qos_store(struct device *dev,
			 struct device_attribute *attribute,
			 const char *buf, size_t count)
{
	struct en7581_gpon *priv = dev_get_drvdata(dev);
	struct airoha_gpon_qos_config cfg[AIROHA_GPON_QOS_CHANNELS] = {};
	char *copy = NULL, *cursor, *token, *field_cursor, *field;
	char *fields[2 + AIROHA_GPON_QOS_QUEUES + 4];
	unsigned int num = 0, field_count, value, i, queue;
	int ret = 0;

	if (sysfs_streq(buf, "off")) {
		mutex_lock(&priv->lock);
		ret = airoha_gpon_set_qos(priv->ethernet_np, NULL, 0);
		mutex_unlock(&priv->lock);
		return ret ? ret : count;
	}

	copy = kstrdup(buf, GFP_KERNEL);
	if (!copy)
		return -ENOMEM;
	cursor = copy;
	while ((token = strsep(&cursor, " \t\n")) != NULL) {
		if (!*token)
			continue;
		if (num == ARRAY_SIZE(cfg)) {
			ret = -E2BIG;
			goto out;
		}
		field_cursor = token;
		field_count = 0;
		while ((field = strsep(&field_cursor, ":")) != NULL) {
			if (field_count == ARRAY_SIZE(fields)) {
				ret = -EINVAL;
				goto out;
			}
			fields[field_count++] = field;
		}
		if (field_count != ARRAY_SIZE(fields)) {
			ret = -EINVAL;
			goto out;
		}
		for (i = 0; i < field_count; i++) {
			ret = kstrtouint(fields[i], 10, &value);
			if (ret)
				goto out;
			if (i == 0 && value >= AIROHA_GPON_QOS_CHANNELS) {
				ret = -ERANGE;
				goto out;
			}
			if (i == 1 && value > 2) {
				ret = -ERANGE;
				goto out;
			}
			if (i >= 2 && i < 2 + AIROHA_GPON_QOS_QUEUES && value > U8_MAX) {
				ret = -ERANGE;
				goto out;
			}
			if (i >= 2 + AIROHA_GPON_QOS_QUEUES && value > U32_MAX) {
				ret = -ERANGE;
				goto out;
			}
			if (i == 0)
				cfg[num].channel = value;
			else if (i == 1)
				cfg[num].scheduler = value;
			else if (i < 2 + AIROHA_GPON_QOS_QUEUES)
				cfg[num].weights[i - 2] = value;
			else if (i == 2 + AIROHA_GPON_QOS_QUEUES)
				cfg[num].cir = value;
			else if (i == 3 + AIROHA_GPON_QOS_QUEUES)
				cfg[num].pir = value;
			else if (i == 4 + AIROHA_GPON_QOS_QUEUES)
				cfg[num].cbs = value;
			else
				cfg[num].pbs = value;
		}
		num++;
	}
	if (!num) {
		ret = -EINVAL;
		goto out;
	}

	mutex_lock(&priv->lock);
	if (!priv->enabled || en7581_gpon_get_state(priv) != EN7581_GPON_STATE_O5) {
		ret = -ENOLINK;
		goto unlock;
	}
	for (i = 0; i < num; i++) {
		if (!priv->alloc_valid[cfg[i].channel]) {
			ret = -ENOLINK;
			goto unlock;
		}
		for (queue = 0; queue < i; queue++) {
			if (cfg[queue].channel == cfg[i].channel) {
				ret = -EEXIST;
				goto unlock;
			}
		}
	}
	ret = airoha_gpon_set_qos(priv->ethernet_np, cfg, num);
unlock:
	mutex_unlock(&priv->lock);
out:
	kfree(copy);
	return ret ? ret : count;
}
static DEVICE_ATTR_RW(qos);

static int en7581_gpon_apply_data_gems(struct en7581_gpon *priv,
				       const u16 *gems, const u8 *channels,
				       const u8 *directions, unsigned int num)
{
	DECLARE_BITMAP(programmed, EN7581_GPON_MAX_GEMS);
	DECLARE_BITMAP(wanted, EN7581_GPON_MAX_GEMS);
	unsigned long gem;
	unsigned int i;
	int ret;

	if (num > EN7581_GPON_MAX_ACTIVE_GEMS)
		return -E2BIG;
	if (num && (!gems || !channels || !directions))
		return -EINVAL;
	if (num && (!priv->enabled ||
		    en7581_gpon_get_state(priv) != EN7581_GPON_STATE_O5))
		return -ENOLINK;

	bitmap_zero(wanted, EN7581_GPON_MAX_GEMS);
	bitmap_zero(programmed, EN7581_GPON_MAX_GEMS);
	for (i = 0; i < num; i++) {
		if (gems[i] >= EN7581_GPON_MAX_GEMS ||
		    channels[i] >= EN7581_GPON_MAX_TCONTS ||
		    directions[i] < 1 || directions[i] > 3)
			return -ERANGE;
		if (!priv->alloc_valid[channels[i]])
			return -ENOLINK;
		if (test_and_set_bit(gems[i], wanted))
			return -EEXIST;
	}

	for (i = 0; i < num; i++) {
		ret = en7581_gpon_set_gem(priv, gems[i], true,
					   test_bit(gems[i], priv->gem_encrypted));
		if (ret)
			goto rollback;
		__set_bit(gems[i], programmed);
	}

	ret = airoha_gpon_set_gems(priv->ethernet_np, gems, channels,
				    directions, num);
	if (ret)
		goto rollback;

	for_each_set_bit(gem, priv->data_gems, EN7581_GPON_MAX_GEMS)
		if (!test_bit(gem, wanted) && gem != priv->omcc_id)
			en7581_gpon_set_gem(priv, gem, false, false);

	bitmap_copy(priv->data_gems, wanted, EN7581_GPON_MAX_GEMS);
	memset(priv->data_channels, 0, sizeof(priv->data_channels));
	memset(priv->data_directions, 0, sizeof(priv->data_directions));
	for (i = 0; i < num; i++) {
		priv->data_channels[gems[i]] = channels[i];
		priv->data_directions[gems[i]] = directions[i];
	}
	priv->data_gem_count = num;
	priv->data_gem_valid = num == 1;
	if (num == 1) {
		priv->data_gem = gems[0];
		priv->data_channel = channels[0];
	}
	return 0;

rollback:
	for_each_set_bit(gem, programmed, EN7581_GPON_MAX_GEMS)
		if (!test_bit(gem, priv->data_gems) && gem != priv->omcc_id)
			en7581_gpon_set_gem(priv, gem, false, false);
	return ret;
}

static ssize_t data_gem_show(struct device *dev,
			     struct device_attribute *attribute, char *buf)
{
	struct en7581_gpon *priv = dev_get_drvdata(dev);
	ssize_t len;

	mutex_lock(&priv->lock);
	if (!priv->data_gem_count)
		len = sysfs_emit(buf, "off\n");
	else if (priv->data_gem_count != 1)
		len = sysfs_emit(buf, "multi %u\n", priv->data_gem_count);
	else
		len = sysfs_emit(buf, "%u %u %u\n", priv->data_gem,
				 priv->data_channel,
				 test_bit(priv->data_gem,
					  priv->gem_encrypted));
	mutex_unlock(&priv->lock);

	return len;
}

static ssize_t data_gem_store(struct device *dev,
			      struct device_attribute *attribute,
			      const char *buf, size_t count)
{
	struct en7581_gpon *priv = dev_get_drvdata(dev);
	unsigned int gem, channel, encrypted;
	u8 direction = 3;
	u16 gem_id;
	u8 channel_id;
	bool was_valid, was_encrypted;
	int ret;

	mutex_lock(&priv->lock);
	if (sysfs_streq(buf, "off")) {
		ret = en7581_gpon_disable_data_gems(priv);
		if (!ret)
			ret = count;
		goto out;
	}

	if (sscanf(buf, "%u %u %u", &gem, &channel, &encrypted) != 3) {
		ret = -EINVAL;
		goto out;
	}
	if (gem >= EN7581_GPON_MAX_GEMS ||
	    channel >= EN7581_GPON_MAX_TCONTS || encrypted > 1) {
		ret = -ERANGE;
		goto out;
	}
	if (!priv->enabled ||
	    en7581_gpon_get_state(priv) != EN7581_GPON_STATE_O5 ||
	    !priv->alloc_valid[channel]) {
		ret = -ENOLINK;
		goto out;
	}
	gem_id = gem;
	channel_id = channel;
	was_valid = test_bit(gem_id, priv->gem_valid);
	was_encrypted = test_bit(gem_id, priv->gem_encrypted);
	ret = en7581_gpon_set_gem(priv, gem_id, true, encrypted);
	if (ret)
		goto out;
	ret = en7581_gpon_apply_data_gems(priv, &gem_id, &channel_id,
					   &direction, 1);
	if (!ret) {
		ret = count;
	} else if (was_valid) {
		/* Restore the PLOAM-programmed encryption state on apply failure. */
		en7581_gpon_set_gem(priv, gem_id, true, was_encrypted);
	}

out:
	mutex_unlock(&priv->lock);
	return ret;
}
static DEVICE_ATTR_RW(data_gem);

static ssize_t data_gems_show(struct device *dev,
			      struct device_attribute *attribute, char *buf)
{
	struct en7581_gpon *priv = dev_get_drvdata(dev);
	unsigned long gem;
	ssize_t len = 0;

	mutex_lock(&priv->lock);
	for_each_set_bit(gem, priv->data_gems, EN7581_GPON_MAX_GEMS)
		len += sysfs_emit_at(buf, len, "%s%lu:%u:%u", len ? " " : "",
				     gem, priv->data_channels[gem],
				     priv->data_directions[gem]);
	if (!len)
		len = sysfs_emit(buf, "off");
	len += sysfs_emit_at(buf, len, "\n");
	mutex_unlock(&priv->lock);

	return len;
}

static ssize_t data_gems_store(struct device *dev,
			       struct device_attribute *attribute,
			       const char *buf, size_t count)
{
	struct en7581_gpon *priv = dev_get_drvdata(dev);
	u16 *gems = NULL;
	u8 *channels = NULL, *directions = NULL;
	char *copy = NULL, *cursor, *token, trailing;
	unsigned int gem, channel, direction, num = 0;
	int ret = 0;

	if (!sysfs_streq(buf, "off")) {
		gems = kcalloc(EN7581_GPON_MAX_ACTIVE_GEMS, sizeof(*gems),
			       GFP_KERNEL);
		channels = kcalloc(EN7581_GPON_MAX_ACTIVE_GEMS,
				   sizeof(*channels), GFP_KERNEL);
		directions = kcalloc(EN7581_GPON_MAX_ACTIVE_GEMS,
				     sizeof(*directions), GFP_KERNEL);
		copy = kstrdup(buf, GFP_KERNEL);
		if (!gems || !channels || !directions || !copy) {
			ret = -ENOMEM;
			goto free;
		}

		cursor = copy;
		while ((token = strsep(&cursor, " \t\n"))) {
			if (!*token)
				continue;
			if (num == EN7581_GPON_MAX_ACTIVE_GEMS) {
				ret = -E2BIG;
				goto free;
			}
			if (sscanf(token, "%u:%u:%u%c", &gem, &channel,
				   &direction, &trailing) != 3) {
				ret = -EINVAL;
				goto free;
			}
			if (gem >= EN7581_GPON_MAX_GEMS ||
			    channel >= EN7581_GPON_MAX_TCONTS ||
			    direction < 1 || direction > 3) {
				ret = -ERANGE;
				goto free;
			}
			gems[num] = gem;
			channels[num] = channel;
			directions[num] = direction;
			num++;
		}
		if (!num) {
			ret = -EINVAL;
			goto free;
		}
	}

	mutex_lock(&priv->lock);
	ret = en7581_gpon_apply_data_gems(priv, gems, channels, directions,
					   num);
	mutex_unlock(&priv->lock);
	if (!ret)
		ret = count;

free:
	kfree(copy);
	kfree(directions);
	kfree(channels);
	kfree(gems);
	return ret;
}
static DEVICE_ATTR_RW(data_gems);

static ssize_t safety_status_show(struct device *dev,
				  struct device_attribute *attribute,
				  char *buf)
{
	struct en7581_gpon *priv = dev_get_drvdata(dev);

	return sysfs_emit(buf, "ready=%u rogue_fault=%u olt_disabled=%u\n",
				  priv->safety_ready, READ_ONCE(priv->rogue_fault),
				  priv->olt_disabled);
}
static DEVICE_ATTR_RO(safety_status);

static ssize_t init_status_show(struct device *dev,
				struct device_attribute *attribute, char *buf)
{
	struct en7581_gpon *priv = dev_get_drvdata(dev);
	const struct airoha_xpon_mode_descriptor *mode;
	ssize_t len;

	mutex_lock(&priv->lock);
	mode = airoha_xpon_mode_descriptor(priv->init_bosa_mode);
	len = sysfs_emit(buf,
		"attempts=%llu stage=%s error=%d bosa_mode=%s bosa_ready=%u tx_disabled=%u ",
		priv->init_attempts, en7581_gpon_init_stage_name(priv->init_stage),
		priv->init_error, mode ? mode->name : "invalid",
		priv->init_bosa_ready, priv->init_tx_disabled);
	len += sysfs_emit_at(buf, len,
		"phy_setting=0x%08x errcnt_enable=0x%08x pma_setting0=0x%08x pma_setting1=0x%08x pma_interrupt_enable=0x%08x scu_gpio_force=0x%08x\n",
		priv->init_phy_setting, priv->init_errcnt_enable,
		priv->init_pma_setting_0, priv->init_pma_setting_1,
		priv->init_pma_interrupt_enable, priv->init_scu_gpio_force);
	mutex_unlock(&priv->lock);

	return len;
}
static DEVICE_ATTR_RO(init_status);

static ssize_t last_ploam_show(struct device *dev,
			       struct device_attribute *attribute, char *buf)
{
	struct en7581_gpon *priv = dev_get_drvdata(dev);
	ssize_t len;

	mutex_lock(&priv->lock);
	if (!priv->have_last_ploam)
		len = sysfs_emit(buf, "none\n");
	else
		len = sysfs_emit(buf, "%*phN\n", EN7581_GPON_PLOAM_SIZE,
				 priv->last_ploam);
	mutex_unlock(&priv->lock);

	return len;
}
static DEVICE_ATTR_RO(last_ploam);

static ssize_t gem_counters_show(struct device *dev,
				  struct device_attribute *attribute, char *buf)
{
	struct en7581_gpon *priv = dev_get_drvdata(dev);
	u64 rx_frames, rx_bytes, tx_frames, tx_bytes;
	u16 gem;
	int ret;

	mutex_lock(&priv->lock);
	gem = priv->counter_gem;
	if (!test_bit(gem, priv->gem_valid)) {
		ret = -ENOENT;
		goto unlock;
	}
	ret = en7581_gpon_read_gem_counter(priv, gem,
					   EN7581_GPON_GEM_RX_FRAMES,
					   &rx_frames);
	if (ret)
		goto unlock;
	ret = en7581_gpon_read_gem_counter(priv, gem,
					   EN7581_GPON_GEM_RX_PAYLOAD_BYTES,
					   &rx_bytes);
	if (ret)
		goto unlock;
	ret = en7581_gpon_read_gem_counter(priv, gem,
					   EN7581_GPON_GEM_TX_FRAMES,
					   &tx_frames);
	if (ret)
		goto unlock;
	ret = en7581_gpon_read_gem_counter(priv, gem,
					   EN7581_GPON_GEM_TX_PAYLOAD_BYTES,
					   &tx_bytes);
	if (!ret)
		ret = sysfs_emit(buf,
				 "port_id=%u rx_frames=%llu rx_bytes=%llu tx_frames=%llu tx_bytes=%llu\n",
				 gem, rx_frames, rx_bytes, tx_frames, tx_bytes);

unlock:
	mutex_unlock(&priv->lock);
	return ret;
}

static ssize_t gem_counters_store(struct device *dev,
				   struct device_attribute *attribute,
				   const char *buf, size_t count)
{
	struct en7581_gpon *priv = dev_get_drvdata(dev);
	u16 gem;
	int ret;

	ret = kstrtou16(buf, 0, &gem);
	if (ret)
		return ret;
	if (gem >= EN7581_GPON_MAX_GEMS)
		return -ERANGE;

	mutex_lock(&priv->lock);
	if (!test_bit(gem, priv->gem_valid))
		ret = -ENOENT;
	else
		priv->counter_gem = gem;
	mutex_unlock(&priv->lock);

	return ret ? ret : count;
}
static DEVICE_ATTR_RW(gem_counters);

static ssize_t counter_evidence_show(struct device *dev,
				     struct device_attribute *attribute,
				     char *buf)
{
	struct en7581_gpon *priv = dev_get_drvdata(dev);
	u64 rx_frames = 0, rx_bytes = 0, tx_frames = 0, tx_bytes = 0;
	unsigned long gem;
	unsigned int active = 0;
	ssize_t ret;

	mutex_lock(&priv->lock);
	for_each_set_bit(gem, priv->data_gems, EN7581_GPON_MAX_GEMS) {
		u64 value;

		ret = en7581_gpon_read_gem_counter(priv, gem,
						   EN7581_GPON_GEM_RX_FRAMES,
						   &value);
		if (ret)
			goto unlock;
		rx_frames += value;
		ret = en7581_gpon_read_gem_counter(priv, gem,
						   EN7581_GPON_GEM_RX_PAYLOAD_BYTES,
						   &value);
		if (ret)
			goto unlock;
		rx_bytes += value;
		ret = en7581_gpon_read_gem_counter(priv, gem,
						   EN7581_GPON_GEM_TX_FRAMES,
						   &value);
		if (ret)
			goto unlock;
		tx_frames += value;
		ret = en7581_gpon_read_gem_counter(priv, gem,
						   EN7581_GPON_GEM_TX_PAYLOAD_BYTES,
						   &value);
		if (ret)
			goto unlock;
		tx_bytes += value;
		active++;
	}

	ret = sysfs_emit(buf,
		"version=1 counter_reset=0 active_gems=%u rx_frames=%llu rx_payload_bytes=%llu tx_frames=%llu tx_payload_bytes=%llu\n",
		active, rx_frames, rx_bytes, tx_frames, tx_bytes);

unlock:
	mutex_unlock(&priv->lock);
	return ret;
}
static DEVICE_ATTR_RO(counter_evidence);

static ssize_t fec_counters_show(struct device *dev,
				 struct device_attribute *attribute, char *buf)
{
	struct en7581_gpon *priv = dev_get_drvdata(dev);
	u32 corrected_bytes, corrected_codewords, uncorrectable_codewords;
	u32 total_codewords, fec_seconds;
	ssize_t len;

	mutex_lock(&priv->lock);
	writel(EN7581_PHY_ERRCNT_FEC_LATCH,
	       priv->phy_csr + EN7581_PHY_CSR_ERRCNT_CTL);
	corrected_bytes = readl(priv->phy_csr +
				EN7581_PHY_CSR_FEC_CORRECTED_BYTES);
	corrected_codewords = readl(priv->phy_csr +
				    EN7581_PHY_CSR_FEC_CORRECTED_CW);
	uncorrectable_codewords = readl(priv->phy_csr +
					EN7581_PHY_CSR_FEC_UNCORRECTABLE_CW);
	total_codewords = readl(priv->phy_csr + EN7581_PHY_CSR_FEC_TOTAL_CW);
	fec_seconds = readl(priv->phy_csr + EN7581_PHY_CSR_FEC_SECONDS);
	len = sysfs_emit(buf,
			 "corrected_bytes=%u corrected_codewords=%u uncorrectable_codewords=%u total_codewords=%u fec_seconds=%u\n",
			 corrected_bytes, corrected_codewords,
			 uncorrectable_codewords, total_codewords, fec_seconds);
	mutex_unlock(&priv->lock);

	return len;
}
static DEVICE_ATTR_RO(fec_counters);

static ssize_t optical_link_show(struct device *dev,
				 struct device_attribute *attribute, char *buf)
{
	struct en7581_gpon *priv = dev_get_drvdata(dev);
	ssize_t len;

	mutex_lock(&priv->lock);
	len = sysfs_emit(buf, "los=%u lof=%u phy_ready=%u\n",
			 priv->los, priv->lof, priv->phy_ready);
	mutex_unlock(&priv->lock);
	return len;
}
static DEVICE_ATTR_RO(optical_link);

static ssize_t ber_sample_show(struct device *dev,
			       struct device_attribute *attribute, char *buf)
{
	struct en7581_gpon *priv = dev_get_drvdata(dev);
	ssize_t len;

	mutex_lock(&priv->lock);
	if (!priv->ber_sample_valid)
		len = sysfs_emit(buf, "none\n");
	else
		len = sysfs_emit(buf,
				 "sequence=%llu bip_count=%u interval_ms=%u\n",
				 priv->ber_sample_sequence, priv->last_bip_count,
				 priv->ber_interval_ms);
	mutex_unlock(&priv->lock);
	return len;
}
static DEVICE_ATTR_RO(ber_sample);

static ssize_t stats_show(struct device *dev,
			  struct device_attribute *attribute, char *buf)
{
	struct en7581_gpon *priv = dev_get_drvdata(dev);
	ssize_t len;

	len = sysfs_emit(buf, "interrupts=%llu phy_interrupts=%llu ploam_rx=%llu ",
			 priv->irq_count, priv->phy_irq_count,
			 priv->ploam_rx_count);
	len += sysfs_emit_at(buf, len,
			     "ploam_tx=%llu error_interrupts=%llu fifo_overruns=%llu ",
			     priv->ploam_tx_count, priv->error_irq_count,
			     priv->fifo_overrun_count);
	len += sysfs_emit_at(buf, len, "drops=%llu unknown=%llu tx_blocked=%llu\n",
			     priv->ploam_drop_count, priv->ploam_unknown_count,
			     priv->ploam_tx_blocked_count);
	len += sysfs_emit_at(buf, len,
			     "to1_timeouts=%llu to2_timeouts=%llu popup_recoveries=%llu\n",
			     priv->to1_timeout_count, priv->to2_timeout_count,
			     priv->popup_recovery_count);

	return len;
}
static DEVICE_ATTR_RO(stats);

static struct attribute *en7581_gpon_attrs[] = {
	&dev_attr_enabled.attr,
	&dev_attr_serial_number.attr,
	&dev_attr_password.attr,
	&dev_attr_state.attr,
	&dev_attr_onu_id.attr,
	&dev_attr_omcc_id.attr,
	&dev_attr_equalization_delay.attr,
	&dev_attr_tconts.attr,
	&dev_attr_qos.attr,
	&dev_attr_data_gem.attr,
	&dev_attr_data_gems.attr,
	&dev_attr_safety_status.attr,
	&dev_attr_init_status.attr,
	&dev_attr_last_ploam.attr,
	&dev_attr_gem_counters.attr,
	&dev_attr_counter_evidence.attr,
	&dev_attr_fec_counters.attr,
	&dev_attr_optical_link.attr,
	&dev_attr_ber_sample.attr,
	&dev_attr_stats.attr,
	NULL,
};
ATTRIBUTE_GROUPS(en7581_gpon);

static void en7581_gpon_of_node_put(void *data)
{
	of_node_put(data);
}

static void en7581_gpon_bosa_put(void *data)
{
	airoha_en7572_put(data);
}

static int en7581_gpon_board_mode(struct device *dev, bool *selected)
{
	struct device_node *pcs_np;
	const char *mode;
	int ret;

	pcs_np = of_parse_phandle(dev->of_node, "airoha,pcs", 0);
	if (!pcs_np)
		return dev_err_probe(dev, -EINVAL, "missing airoha,pcs phandle\n");
	ret = of_property_read_string(pcs_np, "airoha,pon-mode", &mode);
	of_node_put(pcs_np);
	if (ret)
		return dev_err_probe(dev, ret,
				     "PON PCS has no explicit airoha,pon-mode\n");
	*selected = !strcmp(mode, "gpon");

	return 0;
}

static void __iomem *
en7581_gpon_ioremap_phandle_resource(struct device *dev,
				      const char *phandle,
				      const char *resource_name)
{
	struct device_node *np;
	struct resource resource;
	void __iomem *base;
	int index, ret;

	np = of_parse_phandle(dev->of_node, phandle, 0);
	if (!np)
		return ERR_PTR(-EINVAL);

	index = of_property_match_string(np, "reg-names", resource_name);
	if (index < 0) {
		base = ERR_PTR(index);
		goto out;
	}
	ret = of_address_to_resource(np, index, &resource);
	if (ret) {
		base = ERR_PTR(ret);
		goto out;
	}
	base = devm_ioremap(dev, resource.start, resource_size(&resource));
	if (!base)
		base = ERR_PTR(-ENOMEM);

out:
	of_node_put(np);
	return base;
}

static int en7581_gpon_xpon_block_traffic(void *context)
{
	struct en7581_gpon *priv = context;

	cancel_delayed_work_sync(&priv->activation_work);
	cancel_delayed_work_sync(&priv->ber_work);
	mutex_lock(&priv->lock);
	priv->switch_resume_enabled = priv->enabled;
	priv->enabled = false;
	mutex_unlock(&priv->lock);
	return 0;
}

static int en7581_gpon_xpon_clear_session(void *context)
{
	struct en7581_gpon *priv = context;
	int ret;

	mutex_lock(&priv->lock);
	ret = en7581_gpon_reset_session(priv, EN7581_GPON_STATE_O1);
	mutex_unlock(&priv->lock);
	return ret;
}

static int en7581_gpon_xpon_mask_irqs(void *context)
{
	struct en7581_gpon *priv = context;
	u32 value;

	WRITE_ONCE(priv->xpon_irqs_masked, true);
	en7581_gpon_write(priv, EN7581_GPON_INT_ENABLE, 0);
	writel(0, priv->phy_csr + EN7581_PHY_CSR_XPON_INT_ENABLE);
	value = readl(priv->pma + EN7581_PMA_XPON_INT_EN_0);
	writel(value & ~EN7581_PMA_ROGUE_ONU,
	       priv->pma + EN7581_PMA_XPON_INT_EN_0);
	if (en7581_gpon_read(priv, EN7581_GPON_INT_ENABLE) ||
	    readl(priv->phy_csr + EN7581_PHY_CSR_XPON_INT_ENABLE) ||
	    (readl(priv->pma + EN7581_PMA_XPON_INT_EN_0) &
	     EN7581_PMA_ROGUE_ONU)) {
		en7581_gpon_write(priv, EN7581_GPON_INT_ENABLE, 0);
		writel(0, priv->phy_csr + EN7581_PHY_CSR_XPON_INT_ENABLE);
		value = readl(priv->pma + EN7581_PMA_XPON_INT_EN_0);
		writel(value & ~EN7581_PMA_ROGUE_ONU,
		       priv->pma + EN7581_PMA_XPON_INT_EN_0);
		dev_err(priv->dev, "failed to verify GPON interrupt mask\n");
		return -EIO;
	}
	return 0;
}

static void en7581_gpon_xpon_synchronize_irqs(void *context)
{
	struct en7581_gpon *priv = context;

	synchronize_irq(priv->mac_irq);
	synchronize_irq(priv->phy_irq);
}

static int en7581_gpon_xpon_stop_datapath(void *context)
{
	struct en7581_gpon *priv = context;
	int error = 0, ret;

	ret = airoha_gpon_set_omcc(priv->ethernet_np, 0, false);
	if (ret)
		error = ret;
	ret = airoha_gpon_set_qos(priv->ethernet_np, NULL, 0);
	if (ret && !error)
		error = ret;
	ret = airoha_gpon_set_gems(priv->ethernet_np, NULL, NULL, NULL, 0);
	if (ret && !error)
		error = ret;
	return error;
}

static int en7581_gpon_xpon_stop_mac(void *context)
{
	struct en7581_gpon *priv = context;
	int ret;

	cancel_delayed_work_sync(&priv->activation_work);
	cancel_delayed_work_sync(&priv->ber_work);
	mutex_lock(&priv->lock);
	ret = en7581_gpon_reset_session(priv, EN7581_GPON_STATE_O1);
	priv->safety_ready = false;
	priv->hardware_selected = false;
	mutex_unlock(&priv->lock);
	return ret;
}

static int en7581_gpon_xpon_start_mac(void *context)
{
	struct en7581_gpon *priv = context;
	const struct airoha_xpon_mode_descriptor *mode;
	int ret;

	mutex_lock(&priv->lock);
	priv->init_attempts++;
	priv->init_stage = EN7581_GPON_INIT_PRECHECK;
	priv->init_error = 0;
	priv->init_bosa_mode = airoha_en7572_get_mode(priv->bosa);
	priv->init_bosa_ready = airoha_en7572_is_ready(priv->bosa);
	priv->init_tx_disabled = airoha_en7572_tx_is_disabled(priv->bosa);
	priv->init_phy_setting = 0;
	priv->init_errcnt_enable = 0;
	priv->init_pma_setting_0 = 0;
	priv->init_pma_setting_1 = 0;
	priv->init_pma_interrupt_enable = 0;
	priv->init_scu_gpio_force = 0;
	if (priv->init_bosa_mode != AIROHA_XPON_MODE_GPON ||
	    !priv->init_bosa_ready || !priv->init_tx_disabled) {
		mode = airoha_xpon_mode_descriptor(priv->init_bosa_mode);
		ret = -EIO;
		dev_err(priv->dev,
			"GPON start precheck failed: bosa_mode=%s ready=%u tx_disabled=%u\n",
			mode ? mode->name : "invalid", priv->init_bosa_ready,
			priv->init_tx_disabled);
		goto out;
	}
	priv->hardware_selected = true;
	ret = en7581_gpon_hw_init(priv);
	if (!ret)
		ret = en7581_gpon_phy_init(priv);
	if (ret) {
		priv->hardware_selected = false;
		dev_err(priv->dev, "GPON hardware init failed at %s: %d\n",
			en7581_gpon_init_stage_name(priv->init_stage), ret);
	} else {
		priv->init_stage = EN7581_GPON_INIT_READY;
	}
out:
	priv->init_error = ret;
	mutex_unlock(&priv->lock);
	return ret;
}

static int en7581_gpon_xpon_start_datapath(void *context)
{
	struct en7581_gpon *priv = context;
	int ret;

	ret = airoha_gpon_set_gems(priv->ethernet_np, NULL, NULL, NULL, 0);
	if (ret)
		return ret;
	return airoha_gpon_set_qos(priv->ethernet_np, NULL, 0);
}

static int en7581_gpon_xpon_unmask_irqs(void *context)
{
	struct en7581_gpon *priv = context;
	u32 value;

	if (READ_ONCE(priv->rogue_fault) ||
	    airoha_en7572_fault_locked(priv->bosa))
		return -EIO;
	en7581_gpon_write(priv, EN7581_GPON_INT_STATUS, U32_MAX);
	writel(EN7581_PHY_INT_MASK,
	       priv->phy_csr + EN7581_PHY_CSR_XPON_INT_CLEAR);
	value = readl(priv->pma + EN7581_PMA_XPON_INT_STA_0);
	writel(value | EN7581_PMA_ROGUE_ONU,
	       priv->pma + EN7581_PMA_XPON_INT_STA_0);
	value = readl(priv->pma + EN7581_PMA_XPON_INT_EN_0);
	writel(value | EN7581_PMA_ROGUE_ONU,
	       priv->pma + EN7581_PMA_XPON_INT_EN_0);
	writel(EN7581_PHY_INT_MASK,
	       priv->phy_csr + EN7581_PHY_CSR_XPON_INT_ENABLE);
	if (en7581_gpon_read(priv, EN7581_GPON_INT_ENABLE) ||
	    readl(priv->phy_csr + EN7581_PHY_CSR_XPON_INT_ENABLE) !=
			EN7581_PHY_INT_MASK ||
	    !(readl(priv->pma + EN7581_PMA_XPON_INT_EN_0) &
	      EN7581_PMA_ROGUE_ONU)) {
		WRITE_ONCE(priv->xpon_irqs_masked, true);
		en7581_gpon_write(priv, EN7581_GPON_INT_ENABLE, 0);
		writel(0, priv->phy_csr + EN7581_PHY_CSR_XPON_INT_ENABLE);
		value = readl(priv->pma + EN7581_PMA_XPON_INT_EN_0);
		writel(value & ~EN7581_PMA_ROGUE_ONU,
		       priv->pma + EN7581_PMA_XPON_INT_EN_0);
		dev_err(priv->dev, "failed to verify GPON interrupt enable\n");
		return -EIO;
	}
	WRITE_ONCE(priv->xpon_irqs_masked, false);
	return 0;
}

static int en7581_gpon_xpon_mode_committed(void *context)
{
	struct en7581_gpon *priv = context;
	int ret = 0;

	if (!priv->switch_resume_enabled)
		return 0;
	mutex_lock(&priv->lock);
	if (!priv->serial_set || !priv->safety_ready ||
	    READ_ONCE(priv->rogue_fault) ||
	    airoha_en7572_fault_locked(priv->bosa))
		goto out;
	priv->enabled = true;
	ret = en7581_gpon_set_mac_irq_enable(priv, EN7581_GPON_INT_MASK);
	if (ret) {
		priv->enabled = false;
		goto out;
	}
	if (priv->phy_ready)
		ret = en7581_gpon_enter_standby(priv);
	if (ret) {
		priv->enabled = false;
		en7581_gpon_xpon_mask_irqs(priv);
	}
out:
	mutex_unlock(&priv->lock);
	return ret;
}

static bool en7581_gpon_xpon_activation_enabled(void *context)
{
	struct en7581_gpon *priv = context;

	return READ_ONCE(priv->enabled);
}

static const struct airoha_xpon_backend_ops en7581_gpon_xpon_ops = {
	.block_traffic = en7581_gpon_xpon_block_traffic,
	.clear_session = en7581_gpon_xpon_clear_session,
	.mask_irqs = en7581_gpon_xpon_mask_irqs,
	.synchronize_irqs = en7581_gpon_xpon_synchronize_irqs,
	.stop_datapath = en7581_gpon_xpon_stop_datapath,
	.stop_mac = en7581_gpon_xpon_stop_mac,
	.start_mac = en7581_gpon_xpon_start_mac,
	.start_datapath = en7581_gpon_xpon_start_datapath,
	.unmask_irqs = en7581_gpon_xpon_unmask_irqs,
	.mode_committed = en7581_gpon_xpon_mode_committed,
	.activation_enabled = en7581_gpon_xpon_activation_enabled,
};

static void en7581_gpon_xpon_unregister(void *data)
{
	airoha_xpon_backend_unregister(data);
}

static int en7581_gpon_probe(struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;
	struct en7581_gpon *priv;
	struct device_node *bosa_np;
	struct device_node *controller_np;
	const char *serial;
	bool hardware_selected;
	u32 value;
	int mac_irq, phy_irq, ret;

	ret = en7581_gpon_board_mode(dev, &hardware_selected);
	if (ret)
		return ret;

	priv = devm_kzalloc(dev, sizeof(*priv), GFP_KERNEL);
	if (!priv)
		return -ENOMEM;

	priv->dev = dev;
	priv->hardware_selected = false;
	priv->init_bosa_mode = AIROHA_XPON_MODE_INVALID;
	mutex_init(&priv->lock);
	INIT_DELAYED_WORK(&priv->activation_work,
			  en7581_gpon_activation_work);
	INIT_DELAYED_WORK(&priv->ber_work, en7581_gpon_ber_work);
	priv->base = devm_platform_ioremap_resource_byname(pdev, "mac");
	if (IS_ERR(priv->base))
		return PTR_ERR(priv->base);
	priv->phy_csr = devm_platform_ioremap_resource_byname(pdev, "phy-csr");
	if (IS_ERR(priv->phy_csr))
		return PTR_ERR(priv->phy_csr);
	priv->pma = en7581_gpon_ioremap_phandle_resource(dev, "airoha,pcs",
							 "pcs_pma");
	if (IS_ERR(priv->pma))
		return dev_err_probe(dev, PTR_ERR(priv->pma),
				     "failed to map PON PMA\n");
	priv->chip_scu = syscon_regmap_lookup_by_phandle(dev->of_node,
							 "airoha,chip-scu");
	if (IS_ERR(priv->chip_scu))
		return dev_err_probe(dev, PTR_ERR(priv->chip_scu),
				     "failed to find Chip SCU syscon\n");

	priv->idle_gem_threshold = EN7581_GPON_IDLE_GEM_THRESHOLD;
	ret = device_property_read_u32(dev, "airoha,idle-gem-threshold",
				       &priv->idle_gem_threshold);
	if (ret && ret != -EINVAL)
		return dev_err_probe(dev, ret,
				     "failed to read idle GEM threshold\n");
	if (priv->idle_gem_threshold > EN7581_GPON_IDLE_GEM_MASK)
		return dev_err_probe(dev, -ERANGE,
				     "idle GEM threshold is out of range\n");

	bosa_np = of_parse_phandle(dev->of_node, "bosa-controller", 0);
	if (!bosa_np)
		return dev_err_probe(dev, -EINVAL,
				     "missing bosa-controller phandle\n");
	priv->bosa = airoha_en7572_get(bosa_np);
	of_node_put(bosa_np);
	if (IS_ERR(priv->bosa))
		return dev_err_probe(dev, PTR_ERR(priv->bosa),
				     "BOSA controller is not ready\n");
	ret = devm_add_action_or_reset(dev, en7581_gpon_bosa_put, priv->bosa);
	if (ret)
		return ret;
	if (hardware_selected &&
	    airoha_en7572_get_mode(priv->bosa) != AIROHA_XPON_MODE_GPON)
		return dev_err_probe(dev, -EINVAL,
				     "BOSA mode does not match GPON MAC\n");
	if (!airoha_en7572_is_ready(priv->bosa))
		return dev_err_probe(dev, -EPROBE_DEFER,
				     "BOSA controller is not ready\n");

	ret = airoha_en7572_set_tx_enabled(priv->bosa, false);
	if (ret)
		return dev_err_probe(dev, ret,
				     "failed to disable optical TX\n");

	if (!device_property_read_string(dev, "airoha,serial-number", &serial)) {
		ret = en7581_gpon_parse_serial(serial, strlen(serial),
					       priv->serial);
		if (ret)
			return dev_err_probe(dev, ret,
					     "invalid airoha,serial-number\n");
		priv->serial_set = true;
	}

	priv->ethernet_np = of_parse_phandle(dev->of_node, "ethernet", 0);
	if (!priv->ethernet_np)
		return dev_err_probe(dev, -EINVAL,
				     "missing ethernet phandle\n");
	ret = devm_add_action_or_reset(dev, en7581_gpon_of_node_put,
				       priv->ethernet_np);
	if (ret)
		return ret;
	mac_irq = platform_get_irq_byname(pdev, "mac");
	if (mac_irq < 0)
		return mac_irq;
	phy_irq = platform_get_irq_byname(pdev, "phy");
	if (phy_irq < 0)
		return phy_irq;
	priv->mac_irq = mac_irq;
	priv->phy_irq = phy_irq;

	platform_set_drvdata(pdev, priv);
	en7581_gpon_write(priv, EN7581_GPON_INT_ENABLE, 0);
	priv->xpon_irqs_masked = true;
	value = readl(priv->pma + EN7581_PMA_XPON_INT_EN_0);
	writel(value & ~EN7581_PMA_ROGUE_ONU,
	       priv->pma + EN7581_PMA_XPON_INT_EN_0);
	value = readl(priv->pma + EN7581_PMA_XPON_INT_STA_0);
	writel(value, priv->pma + EN7581_PMA_XPON_INT_STA_0);
	ret = devm_request_threaded_irq(dev, phy_irq, en7581_gpon_phy_irq,
					en7581_gpon_phy_irq_thread,
					IRQF_ONESHOT | IRQF_SHARED,
					"airoha-gpon-phy", priv);
	if (ret)
		return dev_err_probe(dev, ret, "failed to request PHY IRQ\n");
	ret = devm_request_threaded_irq(dev, mac_irq, en7581_gpon_irq,
					en7581_gpon_irq_thread,
					IRQF_ONESHOT | IRQF_SHARED,
					"airoha-gpon-mac", priv);
	if (ret)
		return dev_err_probe(dev, ret, "failed to request MAC IRQ\n");
	controller_np = of_parse_phandle(dev->of_node,
					 "airoha,xpon-controller", 0);
	if (!controller_np)
		return dev_err_probe(dev, -EINVAL,
				     "missing airoha,xpon-controller phandle\n");
	priv->xpon_backend = airoha_xpon_backend_register(
		dev, controller_np, AIROHA_XPON_MODE_GPON,
		&en7581_gpon_xpon_ops, priv);
	of_node_put(controller_np);
	if (IS_ERR(priv->xpon_backend))
		return dev_err_probe(dev, PTR_ERR(priv->xpon_backend),
				     "XPON runtime owner is not ready\n");
	ret = devm_add_action_or_reset(dev, en7581_gpon_xpon_unregister,
				       priv->xpon_backend);
	if (ret)
		return ret;
	ret = airoha_xpon_backend_ready(priv->xpon_backend);
	if (ret)
		return dev_err_probe(dev, ret == -ENODEV ? -EPROBE_DEFER : ret,
				     "failed to claim selected GPON mode\n");

	dev_info(dev, airoha_xpon_backend_is_active(priv->xpon_backend) ?
		 "EN7581 GPON backend active; optical TX disabled until enabled\n" :
		 "EN7581 GPON backend ready for runtime activation\n");
	return 0;
}

static void en7581_gpon_quiesce(struct en7581_gpon *priv)
{
	bool active = airoha_xpon_backend_is_active(priv->xpon_backend);

	en7581_gpon_session_disable_tx(priv);
	en7581_gpon_write(priv, EN7581_GPON_INT_ENABLE, 0);
	en7581_gpon_write(priv, EN7581_GPON_INT_STATUS, U32_MAX);
	if (active) {
		writel(readl(priv->pma + EN7581_PMA_XPON_INT_EN_0) &
		       ~EN7581_PMA_ROGUE_ONU,
		       priv->pma + EN7581_PMA_XPON_INT_EN_0);
		writel(0, priv->phy_csr + EN7581_PHY_CSR_XPON_INT_ENABLE);
	}
	cancel_delayed_work_sync(&priv->activation_work);
	cancel_delayed_work_sync(&priv->ber_work);

	mutex_lock(&priv->lock);
	priv->enabled = false;
	en7581_gpon_reset_session(priv, EN7581_GPON_STATE_O1);
	mutex_unlock(&priv->lock);
}

static void en7581_gpon_remove(struct platform_device *pdev)
{
	struct en7581_gpon *priv = platform_get_drvdata(pdev);

	en7581_gpon_quiesce(priv);
}

static void en7581_gpon_shutdown(struct platform_device *pdev)
{
	struct en7581_gpon *priv = platform_get_drvdata(pdev);

	en7581_gpon_quiesce(priv);
}

static const struct of_device_id en7581_gpon_of_match[] = {
	{ .compatible = "airoha,en7581-gpon" },
	{ }
};
MODULE_DEVICE_TABLE(of, en7581_gpon_of_match);

static struct platform_driver en7581_gpon_driver = {
	.probe = en7581_gpon_probe,
	.remove = en7581_gpon_remove,
	.shutdown = en7581_gpon_shutdown,
	.driver = {
		.name = "airoha-gpon",
		.of_match_table = en7581_gpon_of_match,
		.dev_groups = en7581_gpon_groups,
	},
};
module_platform_driver(en7581_gpon_driver);

MODULE_AUTHOR("OpenWrt contributors");
MODULE_DESCRIPTION("Airoha EN7581 GPON MAC control-plane driver");
MODULE_LICENSE("GPL");
