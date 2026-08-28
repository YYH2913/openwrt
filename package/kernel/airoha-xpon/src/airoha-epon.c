// SPDX-License-Identifier: GPL-2.0-only
/* AN7581 10G-EPON MAC, MPCP and runtime XPON backend. */

#include <linux/bitfield.h>
#include <linux/bitops.h>
#include <linux/capability.h>
#include <linux/compat.h>
#include <linux/delay.h>
#include <linux/etherdevice.h>
#include <linux/interrupt.h>
#include <linux/io.h>
#include <linux/iopoll.h>
#include <linux/list.h>
#include <linux/miscdevice.h>
#include <linux/module.h>
#include <linux/mutex.h>
#include <linux/of.h>
#include <linux/of_device.h>
#include <linux/pcs/pcs-airoha.h>
#include <linux/pcs/pcs.h>
#include <linux/poll.h>
#include <linux/platform_device.h>
#include <linux/property.h>
#include <linux/regmap.h>
#include <linux/mfd/syscon.h>
#include <linux/slab.h>
#include <linux/uaccess.h>
#include <linux/unaligned.h>
#include <linux/workqueue.h>

#include "airoha-en7572.h"
#include "airoha-eth.h"
#include "airoha-epon-dying-gasp.h"
#include "airoha-epon-holdover.h"
#include "airoha-epon-logic-reset.h"
#include "airoha-epon-mac-init.h"
#include "airoha-epon-mac.h"
#include "airoha-epon-oam-abi.h"
#include "airoha-epon-security.h"
#include "airoha-epon-stop.h"
#include "airoha-xpon-core.h"

#define EN7581_EPON_MAC_SIZE		0x700
#define EN7581_EPON_PHY_CSR_SIZE		0x1000
#define EN7581_XEPON_PCS_SIZE		0x1000

#define EN7581_EPON_GLB_CFG		0x000
#define  EN7581_EPON_RXMPI_STOP		BIT(13)
#define  EN7581_EPON_TXMPI_STOP		BIT(12)
#define  EN7581_EPON_RXMBI_STOP		BIT(9)
#define  EN7581_EPON_TXMBI_STOP		BIT(8)
#define  EN7581_EPON_PATH_STOP		(EN7581_EPON_RXMPI_STOP | \
					 EN7581_EPON_TXMPI_STOP | \
					 EN7581_EPON_RXMBI_STOP | \
					 EN7581_EPON_TXMBI_STOP)
#define EN7581_EPON_GLB_CFG2		0x004
#define  EN7581_EPON_ETH_COUNT_BYTES	BIT(15)
#define EN7581_EPON_GLB_STS1		0x008
#define  EN7581_EPON_PATH_STOPPED	GENMASK(29, 26)
#define EN7581_EPON_INT_STATUS		0x010
#define EN7581_EPON_INT_ENABLE		0x014
#define EN7581_EPON_INT_STATUS2		0x018
#define EN7581_EPON_INT_ENABLE2		0x01c
#define EN7581_EPON_INT_STATUS3		0x020
#define EN7581_EPON_INT_ENABLE3		0x024
#define EN7581_EPON_DS_KEY_CHANGE	0x034
#define EN7581_EPON_DS_KEY_CHANGE2	0x038
#define EN7581_EPON_DS_KEY_MISS		0x03c
#define EN7581_EPON_DS_KEY_MISS2		0x040
#define EN7581_EPON_US_KEY_CHANGE	0x044
#define EN7581_EPON_LLID_CFG_BASE	0x050
#define EN7581_EPON_LLID_DISCOVERY_CTRL	0x07c
#define EN7581_EPON_LLID_STATUS_BASE	0x080
#define EN7581_EPON_MAC_ADDRESS_CFG	0x104
#define  EN7581_EPON_MAC_ADDRESS_WRITE	BIT(31)
#define  EN7581_EPON_MAC_ADDRESS_DONE	BIT(16)
#define  EN7581_EPON_MAC_ADDRESS_LLID	GENMASK(5, 1)
#define  EN7581_EPON_MAC_ADDRESS_WORD	BIT(0)
#define EN7581_EPON_MAC_ADDRESS_VALUE	0x108
#define EN7581_EPON_SECURITY_KEY_CFG	0x10c
#define EN7581_EPON_KEY_VALUE		0x110
#define EN7581_EPON_ENCRYPT_KEY_CFG	0x114
#define EN7581_EPON_ENCRYPT_KEY_VALUE	0x118
#define EN7581_EPON_REPORT_CFG		0x124
#define EN7581_EPON_REPORT_CFG2		0x128
#define EN7581_EPON_REPORT_THRESHOLD	0x12c
#define  EN7581_EPON_REPORT_THRESHOLD_WRITE BIT(31)
#define  EN7581_EPON_REPORT_THRESHOLD_DONE BIT(30)
#define  EN7581_EPON_REPORT_THRESHOLD_LLID GENMASK(28, 24)
#define  EN7581_EPON_REPORT_THRESHOLD_VALUE GENMASK(23, 8)
#define  EN7581_EPON_REPORT_THRESHOLD_INDEX GENMASK(7, 6)
#define  EN7581_EPON_REPORT_THRESHOLD_QUEUE GENMASK(2, 0)
#define EN7581_EPON_REPORT_BITMAP	0x134
#define EN7581_EPON_1G_REPORT_QSIZE_ADJUST 0x13c
#define EN7581_EPON_10G_REPORT_QSIZE_ADJUST 0x140
#define EN7581_EPON_LASER_TIME		0x1c0
#define EN7581_EPON_SYNC_TIME		0x1c4
#define  EN7581_EPON_SYNC_UPDATE	BIT(16)
#define EN7581_EPON_MPCP_TIMEOUT		0x1d8
#define EN7581_EPON_TRX_ADJUST1		0x1e8
#define EN7581_EPON_TRX_ADJUST2		0x1ec
#define EN7581_EPON_TRX_ADJUST3		0x1f0
#define EN7581_EPON_TRX_ADJUST4		0x1f4
#define EN7581_EPON_TX_FETCH		0x200
#define EN7581_EPON_TX_CAL		0x204
#define  EN7581_EPON_DEFAULT_OVERHEAD	GENMASK(5, 0)
#define EN7581_EPON_DYING_GASP		0x2ac
#define  EN7581_EPON_DYING_GASP_HW	BIT(31)
#define EN7581_EPON_DYING_GASP_WORD1	0x2b0
#define EN7581_EPON_DYING_GASP_WORD2	0x2b4
#define EN7581_EPON_DYING_GASP_WORD3	0x2b8
#define EN7581_EPON_DYING_GASP_WORD4	0x2bc
#define EN7581_EPON_DYING_GASP_WORD5	0x2c0
#define EN7581_EPON_DYING_GASP_WORD6	0x2c4
#define EN7581_EPON_DYING_GASP_WORD7	0x2c8
#define EN7581_EPON_DYING_GASP_WORD8	0x2cc
#define EN7581_EPON_DYING_GASP_WORD9	0x2d0
#define EN7581_EPON_CRYPT_CFG		0x350
#define  EN7581_EPON_DECRYPT_MODE	GENMASK(2, 0)
#define  EN7581_EPON_ENCRYPT_MODE	GENMASK(5, 4)
#define EN7581_EPON_DECRYPT_SOURCE_CFG	0x360
#define  EN7581_EPON_DECRYPT_SOURCE_WRITE BIT(31)
#define  EN7581_EPON_DECRYPT_SOURCE_DONE BIT(30)
#define  EN7581_EPON_DECRYPT_SOURCE_LLID GENMASK(21, 16)
#define  EN7581_EPON_DECRYPT_SOURCE_HIGH GENMASK(15, 0)
#define EN7581_EPON_DECRYPT_SOURCE_VALUE	0x364

/* AN7581 SDK XEPON MAC statistics block, offsets 0x6500..0x6584. */
#define EN7581_EPON_RX_MBI_ETH_COUNT	0x510
#define EN7581_EPON_RX_MPI_ETH_COUNT	0x514
#define EN7581_EPON_TX_MBI_ETH_COUNT	0x518
#define EN7581_EPON_TX_MPI_ETH_COUNT	0x51c
#define EN7581_EPON_RX_MBI_BYTE_COUNT_HI	0x580
#define EN7581_EPON_RX_MBI_BYTE_COUNT_LO	0x584

#define EN7581_EPON_LLID_DECRYPT_MODE	BIT(1)
#define EN7581_EPON_LLID_DECRYPT_ENABLE	BIT(2)
#define EN7581_EPON_LLID_ENCRYPT_ENABLE	BIT(4)
#define EN7581_EPON_LLID_ENCRYPT_KEY	BIT(5)
#define EN7581_EPON_LLID_SECURITY_MASK	(EN7581_EPON_LLID_DECRYPT_MODE | \
	EN7581_EPON_LLID_DECRYPT_ENABLE | EN7581_EPON_LLID_ENCRYPT_ENABLE | \
	EN7581_EPON_LLID_ENCRYPT_KEY)

#define EN7581_EPON_INT_DISCOVERY_GATE	BIT(0)
#define EN7581_EPON_INT_LLID_LOW		GENMASK(8, 1)
#define EN7581_EPON_INT_GRANT_OVERRUN	AIROHA_EPON_INT_GRANT_OVERRUN
#define EN7581_EPON_INT_TIME_DRIFT	BIT(13)
#define EN7581_EPON_INT_MPCP_TIMEOUT	BIT(14)
#define EN7581_EPON_INT_REPORT_TIMEOUT	BIT(15)
#define EN7581_EPON_INT_TX_UNDERRUN	AIROHA_EPON_INT_TX_UNDERRUN
#define EN7581_EPON_INT_REGISTER_REQ_DONE BIT(24)
#define EN7581_EPON_INT_REGISTER_ACK_DONE BIT(25)
#define EN7581_EPON_INT_DS_KEY_CHANGE	BIT(27)
#define EN7581_EPON_INT_DS_KEY_MISS	BIT(28)
#define EN7581_EPON_INT_US_KEY_CHANGE	BIT(29)
#define EN7581_EPON_INT_COMMON		(EN7581_EPON_INT_DISCOVERY_GATE | \
	EN7581_EPON_INT_GRANT_OVERRUN | EN7581_EPON_INT_TIME_DRIFT | \
	EN7581_EPON_INT_MPCP_TIMEOUT | EN7581_EPON_INT_REPORT_TIMEOUT | \
	EN7581_EPON_INT_TX_UNDERRUN | EN7581_EPON_INT_REGISTER_REQ_DONE | \
	EN7581_EPON_INT_REGISTER_ACK_DONE | EN7581_EPON_INT_DS_KEY_CHANGE | \
	EN7581_EPON_INT_DS_KEY_MISS | EN7581_EPON_INT_US_KEY_CHANGE)
#define EN7581_EPON_INT2_ERRORS		AIROHA_EPON_INT2_DIAGNOSTICS

#define EN7581_EPON_PHY_DUMMY_RX		0x290

/* EN7581 XEPON PCS, SDK REG_BASE_XEPON_PCS (0x1faf1000). */
#define EN7581_XEPON_PCS_RX_CTRL_CFG	0x01c
#define  EN7581_XEPON_PCS_RX_ENABLE	0x80810302
#define  EN7581_XEPON_PCS_RX_DISABLE	0x80810300
#define EN7581_XEPON_PCS_INT_STATUS	0x020
#define EN7581_XEPON_PCS_INT_ENABLE	0x024
#define EN7581_XEPON_PCS_LOGIC_RST	0x034
#define  EN7581_XEPON_PCS_LOGIC_RST_HOLD	0
#define  EN7581_XEPON_PCS_LOGIC_RST_RELEASE	1
#define EN7581_XEPON_PCS_RX_SYNC_STATUS	0x06c
#define  EN7581_XEPON_PCS_RX_SYNC_OK	BIT(31)
#define EN7581_XEPON_PCS_SFP_STATUS	0x224
#define  EN7581_XEPON_PCS_SFP_RX_LOS	BIT(28)
#define  EN7581_XEPON_PCS_INT_SYNC_OK	BIT(31)
#define  EN7581_XEPON_PCS_INT_SYNC_LOSS	BIT(30)
#define  EN7581_XEPON_PCS_INT_LASER_RX_LOSS BIT(24)
#define  EN7581_XEPON_PCS_INT_NOT_LASER_RX_LOSS BIT(5)
#define  EN7581_XEPON_PCS_RX_EVENTS	(EN7581_XEPON_PCS_INT_SYNC_OK | \
	EN7581_XEPON_PCS_INT_SYNC_LOSS | \
	EN7581_XEPON_PCS_INT_LASER_RX_LOSS | \
	EN7581_XEPON_PCS_INT_NOT_LASER_RX_LOSS)

#define EN7581_SCU_SSR3			0x94
#define  EN7581_SCU_EPON_LOGIC_RESET	BIT(10)
#define EN7581_SCU_RSTCTRL1		0x834
#define  EN7581_SCU_PON_MAC_RESET	BIT(31)

#define EN7581_EPON_SYNC_TIME_DEFAULT	0x20
#define EN7581_EPON_MPCP_TIMEOUT_DEFAULT 0x1f4
#define EN7581_EPON_LASER_TIME_DEFAULT	0x2020
#define EN7581_EPON_SILENT_TIME_DEFAULT	60
#define EN7581_EPON_TRANSACTION_TIMEOUT	msecs_to_jiffies(1000)
#define EN7581_EPON_MPCP_WORK_INTERVAL	msecs_to_jiffies(250)
#define EN7581_EPON_PHY_RECOVERY_DELAY_MS 2
#define EN7581_EPON_PHY_SETTLE_MS	8
#define EN7581_EPON_PHY_RETRY_DELAY_MS	50
#define EN7581_EPON_PHY_POLL_DELAY_MS	1500
#define EN7581_EPON_PHY_FAST_RECOVERY_ATTEMPTS 3
#define EN7581_EPON_QUEUE_COUNT		8
#define EN7581_EPON_THRESHOLD_COUNT	3
#define EN7581_EPON_OAM_QUEUE_LIMIT	128
#define EN7581_EPON_OAM_FRAME_MAX	2048
#define EN7581_EPON_KEY_TIMEOUT_US	1000
#define EN7581_EPON_NO_LLID		AIROHA_EPON_LLID_COUNT
#define EN7581_EPON_PM_SAMPLE_MS		10000
#define EN7581_EPON_PM_FRAME_COUNTERS	4

struct en7581_epon_llid {
	enum airoha_epon_mpcp_state state;
	u8 mac[ETH_ALEN];
	u16 llid;
	u8 retries;
	bool valid;
	bool tx_fec;
	bool rx_fec;
	unsigned long deadline;
	unsigned long silent_until;
};

struct en7581_epon_oam_rx {
	struct list_head list;
	size_t length;
	u8 data[];
};

struct en7581_epon_pm_extender {
	u64 total;
	u64 session_base;
	u32 previous;
	bool initialized;
};

enum en7581_epon_pm_frame_counter {
	EN7581_EPON_PM_RX_MBI_FRAMES,
	EN7581_EPON_PM_RX_MPI_FRAMES,
	EN7581_EPON_PM_TX_MBI_FRAMES,
	EN7581_EPON_PM_TX_MPI_FRAMES,
};

struct en7581_epon {
	struct device *dev;
	void __iomem *mac;
	void __iomem *phy_csr;
	void __iomem *xepon_pcs;
	struct phylink_pcs *pcs;
	struct regmap *scu;
	struct airoha_en7572 *bosa;
	struct airoha_xpon_backend *asymmetric_backend;
	struct airoha_xpon_backend *symmetric_backend;
	struct device_node *ethernet_np;
	struct mutex lock;
	spinlock_t irq_lock;
	spinlock_t oam_rx_lock;
	struct list_head oam_rx_queue;
	wait_queue_head_t oam_rx_wait;
	struct miscdevice oam;
	atomic_t oam_opened;
	struct delayed_work mpcp_work;
	struct delayed_work phy_recovery_work;
	struct delayed_work holdover_work;
	struct delayed_work pm_work;
	struct airoha_epon_holdover_state holdover;
	struct en7581_epon_llid llids[AIROHA_EPON_LLID_COUNT];
	enum airoha_xpon_mode active_mode;
	u32 llid_mask;
	u32 datapath_llid_mask;
	u32 datapath_report_fec_mask;
	u32 loopback_mask;
	u32 silent_time;
	u16 sync_time;
	u32 pending_status;
	u32 pending_status2;
	u32 pending_status3;
	u32 pending_ds_key_events;
	u32 pending_us_key_events;
	u32 local_deregister_pending;
	u64 irq_events;
	u64 registration_events;
	u64 deregistration_events;
	u64 error_events;
	u64 timeout_events;
	u64 denied_events;
	u64 local_deregistration_events;
	u64 oam_rx_packets;
	u64 oam_rx_dropped;
	u64 oam_tx_packets;
	u64 oam_tx_errors;
	u64 key_programs;
	u64 key_changes;
	u64 key_misses;
	u64 key_errors;
	u64 phy_irq_events;
	u64 phy_sync_events;
	u64 phy_sync_loss_events;
	u64 phy_los_events;
	u64 phy_no_los_events;
	u64 phy_fake_sync_events;
	u64 phy_recoveries;
	u64 phy_recovery_failures;
	u64 holdover_starts;
	u64 holdover_recoveries;
	u64 holdover_expirations;
	u64 holdover_cancellations;
	u64 holdover_suppressed_irqs;
	struct en7581_epon_pm_extender pm_frames[
		EN7581_EPON_PM_FRAME_COUNTERS];
	u64 pm_rx_bytes_base;
	u64 pm_rx_bytes_previous;
	u64 pm_session_generation;
	u64 pm_snapshot_sequence;
	u32 last_phy_irq_status;
	u32 phy_recovery_attempts;
	unsigned long holdover_started;
	u32 oam_rx_depth;
	int mac_irq;
	int phy_irq;
	u8 next_discovery_llid;
	u8 local_deregister_inflight;
	bool hardware_selected;
	bool mac_initialized;
	bool enabled;
	bool switch_resume_enabled;
	bool irqs_masked;
	bool phy_ready;
	bool phy_los;
	bool phy_recovering;
	bool phy_fault;
	bool holdover_tx_enabled;
	bool holdover_blocked;
	bool pm_rx_bytes_initialized;
	bool pm_counter_reset;
	bool onu_mac_set;
	bool oam_consumer_registered;
	bool oam_registered;
	bool removing;
	u8 onu_mac[ETH_ALEN];
	u8 key_suite[AIROHA_EPON_LLID_COUNT];
	u8 key_index[AIROHA_EPON_LLID_COUNT];
};

static int en7581_epon_clear_sessions_locked(struct en7581_epon *priv);
static long en7581_epon_oam_dba_ioctl(struct en7581_epon *priv,
				      unsigned int command,
				      unsigned long argument);
static long en7581_epon_oam_fec_ioctl(struct en7581_epon *priv,
				      unsigned int command,
				      unsigned long argument);
static long en7581_epon_oam_loopback_ioctl(struct en7581_epon *priv,
					   unsigned int command,
					   unsigned long argument);
static long en7581_epon_oam_holdover_ioctl(struct en7581_epon *priv,
						   unsigned int command,
						   unsigned long argument);
static long en7581_epon_oam_clear_session_ioctl(struct en7581_epon *priv,
							unsigned long argument);

static bool en7581_epon_active(struct en7581_epon *priv)
{
	return airoha_xpon_backend_is_active(priv->asymmetric_backend) ||
	       airoha_xpon_backend_is_active(priv->symmetric_backend);
}

static struct airoha_xpon_backend *
en7581_epon_active_backend(struct en7581_epon *priv)
{
	if (airoha_xpon_backend_is_active(priv->asymmetric_backend))
		return priv->asymmetric_backend;
	if (airoha_xpon_backend_is_active(priv->symmetric_backend))
		return priv->symmetric_backend;

	return NULL;
}

static void en7581_epon_purge_oam_rx(struct en7581_epon *priv)
{
	struct en7581_epon_oam_rx *rx, *next;
	LIST_HEAD(queue);
	unsigned long flags;

	spin_lock_irqsave(&priv->oam_rx_lock, flags);
	list_splice_init(&priv->oam_rx_queue, &queue);
	priv->oam_rx_depth = 0;
	spin_unlock_irqrestore(&priv->oam_rx_lock, flags);
	list_for_each_entry_safe(rx, next, &queue, list)
		kfree(rx);
	wake_up_interruptible(&priv->oam_rx_wait);
}

static void en7581_epon_clear_key_events(struct en7581_epon *priv)
{
	unsigned long flags;

	spin_lock_irqsave(&priv->irq_lock, flags);
	priv->pending_ds_key_events = 0;
	priv->pending_us_key_events = 0;
	spin_unlock_irqrestore(&priv->irq_lock, flags);
	wake_up_interruptible(&priv->oam_rx_wait);
}

static void en7581_epon_oam_receive(
	void *context, struct sk_buff *skb,
	const struct airoha_epon_oam_metadata *metadata)
{
	struct en7581_epon *priv = context;
	struct en7581_epon_oam_rx *rx;
	unsigned long flags;
	size_t length;

	if (!metadata || metadata->llid_index >= AIROHA_EPON_LLID_COUNT ||
	    !skb->len || skb->len > EN7581_EPON_OAM_FRAME_MAX ||
	    READ_ONCE(priv->removing))
		goto drop;
	length = sizeof(__be16) + skb->len;
	rx = kmalloc(struct_size(rx, data, length), GFP_ATOMIC);
	if (!rx)
		goto drop;
	rx->length = length;
	put_unaligned_be16(metadata->llid_index, rx->data);
	if (skb_copy_bits(skb, 0, rx->data + sizeof(__be16), skb->len)) {
		kfree(rx);
		goto drop;
	}

	spin_lock_irqsave(&priv->oam_rx_lock, flags);
	if (priv->oam_rx_depth >= EN7581_EPON_OAM_QUEUE_LIMIT ||
	    READ_ONCE(priv->removing)) {
		spin_unlock_irqrestore(&priv->oam_rx_lock, flags);
		kfree(rx);
		goto drop;
	}
	list_add_tail(&rx->list, &priv->oam_rx_queue);
	priv->oam_rx_depth++;
	priv->oam_rx_packets++;
	spin_unlock_irqrestore(&priv->oam_rx_lock, flags);
	dev_kfree_skb_any(skb);
	wake_up_interruptible(&priv->oam_rx_wait);
	return;

drop:
	spin_lock_irqsave(&priv->oam_rx_lock, flags);
	priv->oam_rx_dropped++;
	spin_unlock_irqrestore(&priv->oam_rx_lock, flags);
	dev_kfree_skb_any(skb);
}

static const struct airoha_epon_oam_ops en7581_epon_oam_ops = {
	.receive = en7581_epon_oam_receive,
};

static int en7581_epon_attach_oam(struct en7581_epon *priv)
{
	int ret;

	if (priv->oam_consumer_registered)
		return 0;
	ret = airoha_epon_oam_register(priv->ethernet_np,
				       &en7581_epon_oam_ops, priv);
	if (!ret)
		priv->oam_consumer_registered = true;
	return ret;
}

static void en7581_epon_detach_oam(struct en7581_epon *priv)
{
	if (priv->oam_consumer_registered) {
		airoha_epon_oam_unregister(priv->ethernet_np, priv);
		priv->oam_consumer_registered = false;
	}
	en7581_epon_purge_oam_rx(priv);
	en7581_epon_clear_key_events(priv);
}

static int en7581_epon_oam_open(struct inode *inode, struct file *file)
{
	struct miscdevice *misc = file->private_data;
	struct en7581_epon *priv = container_of(misc, struct en7581_epon, oam);

	if (READ_ONCE(priv->removing))
		return -ENODEV;
	if (atomic_cmpxchg(&priv->oam_opened, 0, 1))
		return -EBUSY;
	file->private_data = priv;
	return nonseekable_open(inode, file);
}

static int en7581_epon_oam_release(struct inode *inode, struct file *file)
{
	struct en7581_epon *priv = file->private_data;
	bool disable_tx = false;

	mutex_lock(&priv->lock);
	if (en7581_epon_active(priv) && priv->mac_initialized) {
		if (en7581_epon_clear_sessions_locked(priv))
			priv->error_events++;
		disable_tx = true;
	}
	mutex_unlock(&priv->lock);
	if (disable_tx)
		airoha_en7572_set_tx_enabled(priv->bosa, false);

	atomic_set(&priv->oam_opened, 0);
	return 0;
}

static ssize_t en7581_epon_oam_read(struct file *file, char __user *buffer,
				    size_t count, loff_t *offset)
{
	struct en7581_epon *priv = file->private_data;
	struct en7581_epon_oam_rx *rx;
	unsigned long flags;
	int ret;

	for (;;) {
		spin_lock_irqsave(&priv->oam_rx_lock, flags);
		if (!list_empty(&priv->oam_rx_queue)) {
			rx = list_first_entry(&priv->oam_rx_queue,
				struct en7581_epon_oam_rx, list);
			if (count < rx->length) {
				spin_unlock_irqrestore(&priv->oam_rx_lock, flags);
				return -EMSGSIZE;
			}
			list_del(&rx->list);
			priv->oam_rx_depth--;
			spin_unlock_irqrestore(&priv->oam_rx_lock, flags);
			break;
		}
		spin_unlock_irqrestore(&priv->oam_rx_lock, flags);
		if (READ_ONCE(priv->removing))
			return -ENODEV;
		if (file->f_flags & O_NONBLOCK)
			return -EAGAIN;
		ret = wait_event_interruptible(priv->oam_rx_wait,
			READ_ONCE(priv->oam_rx_depth) || READ_ONCE(priv->removing));
		if (ret)
			return ret;
	}
	if (copy_to_user(buffer, rx->data, rx->length))
		ret = -EFAULT;
	else
		ret = rx->length;
	kfree(rx);
	return ret;
}

static ssize_t en7581_epon_oam_write(struct file *file,
				     const char __user *buffer, size_t count,
				     loff_t *offset)
{
	struct en7581_epon *priv = file->private_data;
	struct airoha_epon_oam_tx_metadata metadata;
	__be16 encoded_index;
	size_t frame_length;
	u8 *frame;
	int ret;

	if (count < sizeof(encoded_index) + ETH_HLEN ||
	    count > sizeof(encoded_index) + EN7581_EPON_OAM_FRAME_MAX)
		return -EMSGSIZE;
	if (copy_from_user(&encoded_index, buffer, sizeof(encoded_index)))
		return -EFAULT;
	metadata.llid_index = be16_to_cpu(encoded_index);
	if (metadata.llid_index >= AIROHA_EPON_LLID_COUNT)
		return -ERANGE;
	frame_length = count - sizeof(encoded_index);
	frame = memdup_user(buffer + sizeof(encoded_index), frame_length);
	if (IS_ERR(frame))
		return PTR_ERR(frame);

	mutex_lock(&priv->lock);
	if (!en7581_epon_active(priv) || !priv->mac_initialized ||
	    !priv->oam_consumer_registered ||
	    !(priv->datapath_llid_mask & BIT(metadata.llid_index)))
		ret = -EHOSTDOWN;
	else
		ret = airoha_epon_oam_transmit(priv->ethernet_np, frame,
			frame_length, &metadata);
	if (ret)
		priv->oam_tx_errors++;
	else
		priv->oam_tx_packets++;
	mutex_unlock(&priv->lock);
	kfree(frame);
	return ret ? ret : count;
}

static __poll_t en7581_epon_oam_poll(struct file *file, poll_table *wait)
{
	struct en7581_epon *priv = file->private_data;
	__poll_t events = 0;

	poll_wait(file, &priv->oam_rx_wait, wait);
	if (READ_ONCE(priv->oam_rx_depth))
		events |= EPOLLIN | EPOLLRDNORM;
	if (READ_ONCE(priv->pending_ds_key_events) ||
	    READ_ONCE(priv->pending_us_key_events))
		events |= EPOLLPRI;
	if (READ_ONCE(priv->removing))
		return events | EPOLLERR | EPOLLHUP;
	if (READ_ONCE(priv->datapath_llid_mask) &&
	    READ_ONCE(priv->oam_consumer_registered) &&
	    airoha_epon_oam_tx_available(priv->ethernet_np))
		events |= EPOLLOUT | EPOLLWRNORM;
	return events;
}

static long en7581_epon_oam_key_events_ioctl(
		struct en7581_epon *priv, unsigned long argument)
{
	struct airoha_epon_oam_key_events events;
	unsigned long flags;

	if (copy_from_user(&events, (void __user *)argument, sizeof(events)))
		return -EFAULT;
	if (events.version != AIROHA_EPON_OAM_KEY_EVENTS_VERSION ||
	    memchr_inv(events.reserved, 0, sizeof(events.reserved)) ||
	    memchr_inv(events.reserved2, 0, sizeof(events.reserved2)))
		return -EINVAL;

	spin_lock_irqsave(&priv->irq_lock, flags);
	events.downstream_llids = priv->pending_ds_key_events;
	events.upstream_llids = priv->pending_us_key_events;
	priv->pending_ds_key_events = 0;
	priv->pending_us_key_events = 0;
	spin_unlock_irqrestore(&priv->irq_lock, flags);
	if (!copy_to_user((void __user *)argument, &events, sizeof(events)))
		return 0;

	/* A bad userspace pointer must not silently discard a key event. */
	spin_lock_irqsave(&priv->irq_lock, flags);
	priv->pending_ds_key_events |= events.downstream_llids;
	priv->pending_us_key_events |= events.upstream_llids;
	spin_unlock_irqrestore(&priv->irq_lock, flags);
	wake_up_interruptible(&priv->oam_rx_wait);
	return -EFAULT;
}

static void en7581_epon_write(struct en7581_epon *priv, u32 reg, u32 value)
{
	writel(value, priv->mac + reg);
}

static u32 en7581_epon_read(struct en7581_epon *priv, u32 reg)
{
	return readl(priv->mac + reg);
}

static u64 en7581_epon_read_rx_mbi_bytes(struct en7581_epon *priv)
{
	u32 high, low, confirm;

	do {
		high = en7581_epon_read(priv,
			EN7581_EPON_RX_MBI_BYTE_COUNT_HI);
		low = en7581_epon_read(priv,
			EN7581_EPON_RX_MBI_BYTE_COUNT_LO);
		confirm = en7581_epon_read(priv,
			EN7581_EPON_RX_MBI_BYTE_COUNT_HI);
	} while (high != confirm);

	return (u64)confirm << 32 | low;
}

static void en7581_epon_pm_extend(struct en7581_epon_pm_extender *counter,
				  u32 value)
{
	if (!counter->initialized) {
		counter->previous = value;
		counter->initialized = true;
		return;
	}
	counter->total += (u32)(value - counter->previous);
	counter->previous = value;
}

static void en7581_epon_pm_update_locked(struct en7581_epon *priv)
{
	static const u16 registers[EN7581_EPON_PM_FRAME_COUNTERS] = {
		[EN7581_EPON_PM_RX_MBI_FRAMES] = EN7581_EPON_RX_MBI_ETH_COUNT,
		[EN7581_EPON_PM_RX_MPI_FRAMES] = EN7581_EPON_RX_MPI_ETH_COUNT,
		[EN7581_EPON_PM_TX_MBI_FRAMES] = EN7581_EPON_TX_MBI_ETH_COUNT,
		[EN7581_EPON_PM_TX_MPI_FRAMES] = EN7581_EPON_TX_MPI_ETH_COUNT,
	};
	u64 rx_bytes;
	unsigned int i;

	for (i = 0; i < ARRAY_SIZE(registers); i++)
		en7581_epon_pm_extend(&priv->pm_frames[i],
			en7581_epon_read(priv, registers[i]));

	rx_bytes = en7581_epon_read_rx_mbi_bytes(priv);
	if (priv->pm_rx_bytes_initialized &&
	    rx_bytes < priv->pm_rx_bytes_previous)
		priv->pm_counter_reset = true;
	priv->pm_rx_bytes_previous = rx_bytes;
	priv->pm_rx_bytes_initialized = true;
}

static void en7581_epon_pm_reset_session_locked(struct en7581_epon *priv)
{
	unsigned int i;

	en7581_epon_pm_update_locked(priv);
	for (i = 0; i < ARRAY_SIZE(priv->pm_frames); i++)
		priv->pm_frames[i].session_base = priv->pm_frames[i].total;
	priv->pm_rx_bytes_base = priv->pm_rx_bytes_previous;
	priv->pm_counter_reset = false;
	priv->pm_session_generation++;
}

static u64 en7581_epon_pm_frame_value(
	const struct en7581_epon *priv, enum en7581_epon_pm_frame_counter index)
{
	return priv->pm_frames[index].total -
	       priv->pm_frames[index].session_base;
}

static void en7581_epon_pm_work(struct work_struct *work)
{
	struct en7581_epon *priv = container_of(
		to_delayed_work(work), struct en7581_epon, pm_work);
	bool reschedule;

	mutex_lock(&priv->lock);
	reschedule = !priv->removing && priv->hardware_selected &&
		     priv->mac_initialized;
	if (reschedule)
		en7581_epon_pm_update_locked(priv);
	mutex_unlock(&priv->lock);
	if (reschedule)
		mod_delayed_work(system_wq, &priv->pm_work,
			msecs_to_jiffies(EN7581_EPON_PM_SAMPLE_MS));
}

static int en7581_epon_key_access_locked(struct en7581_epon *priv,
					 bool encrypt, bool write, u8 llid,
					 u8 key_index, u8 word, u32 *value)
{
	u32 command, status;
	u32 config = encrypt ? EN7581_EPON_ENCRYPT_KEY_CFG :
		EN7581_EPON_SECURITY_KEY_CFG;
	u32 value_reg = encrypt ? EN7581_EPON_ENCRYPT_KEY_VALUE :
		EN7581_EPON_KEY_VALUE;
	int ret;

	if (!value || llid >= AIROHA_EPON_LLID_COUNT || key_index > 1 || word > 3)
		return -EINVAL;
	if (write)
		en7581_epon_write(priv, value_reg, *value);
	command = airoha_epon_key_command(write, llid, key_index, word);
	en7581_epon_write(priv, config, command);
	ret = readl_poll_timeout(priv->mac + config, status,
		status & AIROHA_EPON_KEY_COMMAND_DONE, 1,
		EN7581_EPON_KEY_TIMEOUT_US);
	if (!ret && !write)
		*value = en7581_epon_read(priv, value_reg);
	return ret;
}

static int en7581_epon_write_key_word_locked(struct en7581_epon *priv,
					      bool encrypt, u8 llid,
					      u8 key_index, u8 word,
					      u32 value)
{
	u32 readback = 0;
	int ret;

	ret = en7581_epon_key_access_locked(priv, encrypt, true, llid,
		key_index, word, &value);
	if (ret)
		return ret;
	ret = en7581_epon_key_access_locked(priv, encrypt, false, llid,
		key_index, word, &readback);
	if (ret)
		return ret;
	return readback == value ? 0 : -EIO;
}

static void en7581_epon_set_llid_security_locked(struct en7581_epon *priv,
						 u8 llid, u8 bits)
{
	u32 reg = EN7581_EPON_LLID_CFG_BASE + llid / 4 * sizeof(u32);
	u8 shift = llid % 4 * 8;
	u32 value = en7581_epon_read(priv, reg);

	value &= ~((u32)EN7581_EPON_LLID_SECURITY_MASK << shift);
	value |= (u32)(bits & EN7581_EPON_LLID_SECURITY_MASK) << shift;
	en7581_epon_write(priv, reg, value);
}

static int en7581_epon_set_decrypt_source_locked(struct en7581_epon *priv,
						  u8 llid, const u8 *mac)
{
	u32 command, status;

	en7581_epon_write(priv, EN7581_EPON_DECRYPT_SOURCE_VALUE,
		get_unaligned_be32(mac + 2));
	command = EN7581_EPON_DECRYPT_SOURCE_WRITE |
		FIELD_PREP(EN7581_EPON_DECRYPT_SOURCE_LLID, llid) |
		FIELD_PREP(EN7581_EPON_DECRYPT_SOURCE_HIGH,
			   get_unaligned_be16(mac));
	en7581_epon_write(priv, EN7581_EPON_DECRYPT_SOURCE_CFG, command);
	return readl_poll_timeout(priv->mac + EN7581_EPON_DECRYPT_SOURCE_CFG,
		status, status & EN7581_EPON_DECRYPT_SOURCE_DONE, 1,
		EN7581_EPON_KEY_TIMEOUT_US);
}

static int en7581_epon_clear_key_slot_locked(struct en7581_epon *priv,
					      u8 llid, u8 key_index)
{
	u8 word;
	int ret, first_error = 0;

	for (word = 0; word < 4; word++) {
		ret = en7581_epon_write_key_word_locked(priv, false, llid,
			key_index, word, 0);
		if (ret && !first_error)
			first_error = ret;
		ret = en7581_epon_write_key_word_locked(priv, true, llid,
			key_index, word, 0);
		if (ret && !first_error)
			first_error = ret;
	}
	return first_error;
}

static int en7581_epon_clear_llid_keys_locked(struct en7581_epon *priv, u8 llid)
{
	u8 zero_mac[ETH_ALEN] = {};
	int ret, first_error = 0;
	u8 key_index;

	en7581_epon_set_llid_security_locked(priv, llid, 0);
	for (key_index = 0; key_index < 2; key_index++) {
		ret = en7581_epon_clear_key_slot_locked(priv, llid, key_index);
		if (ret && !first_error)
			first_error = ret;
	}
	ret = en7581_epon_set_decrypt_source_locked(priv, llid, zero_mac);
	if (ret && !first_error)
		first_error = ret;
	priv->key_suite[llid] = 0;
	priv->key_index[llid] = 0;
	return first_error;
}

static int en7581_epon_clear_all_keys_locked(struct en7581_epon *priv)
{
	u32 crypt = en7581_epon_read(priv, EN7581_EPON_CRYPT_CFG);
	int ret, first_error = 0;
	u8 llid;

	crypt &= ~(EN7581_EPON_DECRYPT_MODE | EN7581_EPON_ENCRYPT_MODE);
	en7581_epon_write(priv, EN7581_EPON_CRYPT_CFG, crypt);
	for (llid = 0; llid < AIROHA_EPON_LLID_COUNT; llid++) {
		ret = en7581_epon_clear_llid_keys_locked(priv, llid);
		if (ret && !first_error)
			first_error = ret;
	}
	return first_error;
}

static int en7581_epon_program_ctc_key_locked(struct en7581_epon *priv,
					       const struct airoha_epon_oam_key *key)
{
	u8 word;
	int ret;

	for (word = 0; word < 3; word++) {
		ret = en7581_epon_write_key_word_locked(priv, false,
			key->llid_index, key->key_index, word,
			airoha_epon_ctc_key_word(key->key, word));
		if (ret)
			return ret;
	}
	en7581_epon_set_llid_security_locked(priv, key->llid_index,
		EN7581_EPON_LLID_DECRYPT_ENABLE);
	return 0;
}

static int en7581_epon_program_dpoe_key_locked(struct en7581_epon *priv,
						const struct airoha_epon_oam_key *key)
{
	u32 crypt;
	u8 word;
	int ret;

	ret = en7581_epon_set_decrypt_source_locked(priv, key->llid_index,
		key->olt_mac);
	if (ret)
		return ret;
	for (word = 0; word < 4; word++) {
		u8 hardware_word = 3 - word;
		u32 value = airoha_epon_dpoe_key_word(key->key, word);

		ret = en7581_epon_write_key_word_locked(priv, false,
			key->llid_index, key->key_index, hardware_word, value);
		if (ret)
			return ret;
		if (priv->active_mode != AIROHA_XPON_MODE_EPON_10G_10G)
			continue;
		ret = en7581_epon_write_key_word_locked(priv, true,
			key->llid_index, key->key_index, hardware_word, value);
		if (ret)
			return ret;
	}
	crypt = en7581_epon_read(priv, EN7581_EPON_CRYPT_CFG);
	crypt &= ~(EN7581_EPON_DECRYPT_MODE | EN7581_EPON_ENCRYPT_MODE);
	crypt |= FIELD_PREP(EN7581_EPON_DECRYPT_MODE, 2);
	if (priv->active_mode == AIROHA_XPON_MODE_EPON_10G_10G)
		crypt |= FIELD_PREP(EN7581_EPON_ENCRYPT_MODE, 2);
	en7581_epon_write(priv, EN7581_EPON_CRYPT_CFG, crypt);
	en7581_epon_set_llid_security_locked(priv, key->llid_index,
		EN7581_EPON_LLID_DECRYPT_MODE |
		EN7581_EPON_LLID_DECRYPT_ENABLE |
		(priv->active_mode == AIROHA_XPON_MODE_EPON_10G_10G ?
		 EN7581_EPON_LLID_ENCRYPT_ENABLE |
		 (key->key_index ? EN7581_EPON_LLID_ENCRYPT_KEY : 0) : 0));
	return 0;
}

static long en7581_epon_oam_ioctl(struct file *file, unsigned int command,
				  unsigned long argument)
{
	struct en7581_epon *priv = file->private_data;
	struct airoha_epon_oam_key key;
	int ret = -ENOTTY;

	if (command != AIROHA_EPON_OAM_IOC_SET_KEY &&
	    command != AIROHA_EPON_OAM_IOC_GET_DBA &&
	    command != AIROHA_EPON_OAM_IOC_SET_DBA &&
	    command != AIROHA_EPON_OAM_IOC_GET_KEY_EVENTS &&
	    command != AIROHA_EPON_OAM_IOC_GET_FEC &&
	    command != AIROHA_EPON_OAM_IOC_SET_FEC &&
	    command != AIROHA_EPON_OAM_IOC_GET_LOOPBACK &&
	    command != AIROHA_EPON_OAM_IOC_SET_LOOPBACK &&
	    command != AIROHA_EPON_OAM_IOC_GET_HOLDOVER &&
	    command != AIROHA_EPON_OAM_IOC_SET_HOLDOVER &&
	    command != AIROHA_EPON_OAM_IOC_CLEAR_SESSION)
		return -ENOTTY;
	if (!capable(CAP_NET_ADMIN))
		return -EPERM;
	if (command == AIROHA_EPON_OAM_IOC_GET_KEY_EVENTS)
		return en7581_epon_oam_key_events_ioctl(priv, argument);
	if (command == AIROHA_EPON_OAM_IOC_GET_FEC ||
	    command == AIROHA_EPON_OAM_IOC_SET_FEC)
		return en7581_epon_oam_fec_ioctl(priv, command, argument);
	if (command == AIROHA_EPON_OAM_IOC_GET_LOOPBACK ||
	    command == AIROHA_EPON_OAM_IOC_SET_LOOPBACK)
		return en7581_epon_oam_loopback_ioctl(priv, command, argument);
	if (command == AIROHA_EPON_OAM_IOC_GET_HOLDOVER ||
	    command == AIROHA_EPON_OAM_IOC_SET_HOLDOVER)
		return en7581_epon_oam_holdover_ioctl(priv, command, argument);
	if (command == AIROHA_EPON_OAM_IOC_CLEAR_SESSION)
		return en7581_epon_oam_clear_session_ioctl(priv, argument);
	if (command != AIROHA_EPON_OAM_IOC_SET_KEY)
		return en7581_epon_oam_dba_ioctl(priv, command, argument);
	if (copy_from_user(&key, (void __user *)argument, sizeof(key)))
		return -EFAULT;
	if (key.version != AIROHA_EPON_OAM_ABI_VERSION ||
	    key.llid_index >= AIROHA_EPON_LLID_COUNT || key.key_index > 1 ||
	    memchr_inv(key.reserved, 0, sizeof(key.reserved)) ||
	    memchr_inv(key.reserved2, 0, sizeof(key.reserved2))) {
		ret = -EINVAL;
		goto out_wipe;
	}
	if (key.suite == AIROHA_EPON_OAM_KEY_CTC_TRIPLE_CHURNING) {
		if (key.key_length != 9 || key.flags) {
			ret = -EINVAL;
			goto out_wipe;
		}
	} else if (key.suite == AIROHA_EPON_OAM_KEY_DPOE_AES_128) {
		if (key.key_length != sizeof(key.key) ||
		    key.flags != AIROHA_EPON_OAM_KEY_F_OLT_MAC ||
		    !is_valid_ether_addr(key.olt_mac)) {
			ret = -EINVAL;
			goto out_wipe;
		}
	} else {
		ret = -EOPNOTSUPP;
		goto out_wipe;
	}

	mutex_lock(&priv->lock);
	if (!en7581_epon_active(priv) || !priv->mac_initialized ||
	    !priv->oam_consumer_registered ||
	    priv->llids[key.llid_index].state != AIROHA_EPON_MPCP_REGISTERED ||
	    !priv->llids[key.llid_index].valid) {
		ret = -EHOSTDOWN;
		goto out_unlock;
	}
	if (key.suite == AIROHA_EPON_OAM_KEY_CTC_TRIPLE_CHURNING)
		ret = en7581_epon_program_ctc_key_locked(priv, &key);
	else
		ret = en7581_epon_program_dpoe_key_locked(priv, &key);
	if (ret) {
		priv->key_errors++;
		(void)en7581_epon_clear_llid_keys_locked(priv, key.llid_index);
	} else {
		priv->key_suite[key.llid_index] = key.suite;
		priv->key_index[key.llid_index] = key.key_index;
		priv->key_programs++;
	}
out_unlock:
	mutex_unlock(&priv->lock);
out_wipe:
	memzero_explicit(&key, sizeof(key));
	return ret;
}

static const struct file_operations en7581_epon_oam_fops = {
	.owner = THIS_MODULE,
	.open = en7581_epon_oam_open,
	.release = en7581_epon_oam_release,
	.read = en7581_epon_oam_read,
	.write = en7581_epon_oam_write,
	.poll = en7581_epon_oam_poll,
	.unlocked_ioctl = en7581_epon_oam_ioctl,
	.compat_ioctl = compat_ptr_ioctl,
};

static int en7581_epon_set_report_threshold(struct en7581_epon *priv,
					     u8 llid, u8 queue, u8 index,
					     u16 value)
{
	u32 status;

	en7581_epon_write(priv, EN7581_EPON_REPORT_THRESHOLD,
		EN7581_EPON_REPORT_THRESHOLD_WRITE |
		FIELD_PREP(EN7581_EPON_REPORT_THRESHOLD_LLID, llid) |
		FIELD_PREP(EN7581_EPON_REPORT_THRESHOLD_VALUE, value) |
		FIELD_PREP(EN7581_EPON_REPORT_THRESHOLD_INDEX, index) |
		FIELD_PREP(EN7581_EPON_REPORT_THRESHOLD_QUEUE, queue));
	return readl_poll_timeout(priv->mac + EN7581_EPON_REPORT_THRESHOLD,
		status, status & EN7581_EPON_REPORT_THRESHOLD_DONE, 1, 10);
}

static int en7581_epon_get_report_threshold(struct en7581_epon *priv,
					     u8 llid, u8 queue, u8 index,
					     u16 *value)
{
	u32 status;
	int ret;

	en7581_epon_write(priv, EN7581_EPON_REPORT_THRESHOLD,
		FIELD_PREP(EN7581_EPON_REPORT_THRESHOLD_LLID, llid) |
		FIELD_PREP(EN7581_EPON_REPORT_THRESHOLD_INDEX, index) |
		FIELD_PREP(EN7581_EPON_REPORT_THRESHOLD_QUEUE, queue));
	ret = readl_poll_timeout(priv->mac + EN7581_EPON_REPORT_THRESHOLD,
		status, status & EN7581_EPON_REPORT_THRESHOLD_DONE, 1, 10);
	if (!ret)
		*value = FIELD_GET(EN7581_EPON_REPORT_THRESHOLD_VALUE, status);
	return ret;
}

static u8 en7581_epon_get_queue_set_count(struct en7581_epon *priv, u8 llid)
{
	u32 reg = llid < 16 ? EN7581_EPON_REPORT_CFG :
				 EN7581_EPON_REPORT_CFG2;
	u8 shift = (llid % 16) * 2;

	return ((en7581_epon_read(priv, reg) >> shift) & 0x3) + 1;
}

static int en7581_epon_set_queue_set_count(struct en7581_epon *priv, u8 llid,
					    u8 count)
{
	u32 reg = llid < 16 ? EN7581_EPON_REPORT_CFG :
				 EN7581_EPON_REPORT_CFG2;
	u8 shift = (llid % 16) * 2;
	u32 value = en7581_epon_read(priv, reg);

	value &= ~(0x3U << shift);
	value |= (u32)(count - 1) << shift;
	en7581_epon_write(priv, reg, value);
	return en7581_epon_get_queue_set_count(priv, llid) == count ? 0 : -EIO;
}

static int en7581_epon_snapshot_dba_locked(struct en7581_epon *priv,
					   u8 llid,
					   struct airoha_epon_oam_dba *dba)
{
	u8 set, queue;
	int ret;

	memset(dba, 0, sizeof(*dba));
	dba->version = AIROHA_EPON_OAM_ABI_VERSION;
	dba->llid_index = llid;
	dba->queue_set_count = en7581_epon_get_queue_set_count(priv, llid);
	for (set = 0; set < AIROHA_EPON_DBA_THRESHOLD_SET_COUNT; set++) {
		dba->report_bitmap[set] =
			en7581_epon_read(priv, EN7581_EPON_REPORT_BITMAP) & 0xff;
		for (queue = 0; queue < AIROHA_EPON_DBA_QUEUE_COUNT; queue++) {
			ret = en7581_epon_get_report_threshold(priv, llid, queue,
				set, &dba->threshold[set][queue]);
			if (ret)
				return ret;
		}
	}
	return 0;
}

static int en7581_epon_apply_dba_locked(struct en7581_epon *priv,
					const struct airoha_epon_oam_dba *dba)
{
	struct airoha_epon_oam_dba previous;
	u8 set, queue;
	int ret;

	ret = en7581_epon_snapshot_dba_locked(priv, dba->llid_index, &previous);
	if (ret)
		return ret;
	for (set = 0; set + 1 < dba->queue_set_count; set++)
		for (queue = 0; queue < AIROHA_EPON_DBA_QUEUE_COUNT; queue++) {
			u16 readback;

			if (!(dba->report_bitmap[set] & BIT(queue)))
				continue;
			ret = en7581_epon_set_report_threshold(priv,
				dba->llid_index, queue, set,
				dba->threshold[set][queue]);
			if (ret)
				goto rollback;
			ret = en7581_epon_get_report_threshold(priv,
				dba->llid_index, queue, set, &readback);
			if (ret || readback != dba->threshold[set][queue]) {
				ret = ret ?: -EIO;
				goto rollback;
			}
		}
	ret = en7581_epon_set_queue_set_count(priv, dba->llid_index,
						 dba->queue_set_count);
	if (!ret)
		return 0;

rollback:
	for (set = 0; set < AIROHA_EPON_DBA_THRESHOLD_SET_COUNT; set++)
		for (queue = 0; queue < AIROHA_EPON_DBA_QUEUE_COUNT; queue++)
			(void)en7581_epon_set_report_threshold(priv,
				previous.llid_index, queue, set,
				previous.threshold[set][queue]);
	(void)en7581_epon_set_queue_set_count(priv, previous.llid_index,
						 previous.queue_set_count);
	return ret;
}

static long en7581_epon_oam_dba_ioctl(struct en7581_epon *priv,
				      unsigned int command,
				      unsigned long argument)
{
	struct airoha_epon_oam_dba dba;
	int ret;

	if (copy_from_user(&dba, (void __user *)argument, sizeof(dba)))
		return -EFAULT;
	if (dba.version != AIROHA_EPON_OAM_ABI_VERSION ||
	    dba.llid_index >= AIROHA_EPON_LLID_COUNT || dba.reserved ||
	    dba.reserved2) {
		ret = -EINVAL;
		goto out;
	}
	if (command == AIROHA_EPON_OAM_IOC_SET_DBA &&
	    (dba.queue_set_count < 2 ||
	     dba.queue_set_count > AIROHA_EPON_DBA_QUEUE_SET_COUNT)) {
		ret = -EINVAL;
		goto out;
	}
	mutex_lock(&priv->lock);
	if (!en7581_epon_active(priv) || !priv->mac_initialized ||
	    !priv->oam_consumer_registered ||
	    priv->llids[dba.llid_index].state != AIROHA_EPON_MPCP_REGISTERED ||
	    !priv->llids[dba.llid_index].valid) {
		ret = -EHOSTDOWN;
		goto out_unlock;
	}
	if (command == AIROHA_EPON_OAM_IOC_GET_DBA)
		ret = en7581_epon_snapshot_dba_locked(priv, dba.llid_index, &dba);
	else
		ret = en7581_epon_apply_dba_locked(priv, &dba);
out_unlock:
	mutex_unlock(&priv->lock);
	if (!ret && command == AIROHA_EPON_OAM_IOC_GET_DBA &&
	    copy_to_user((void __user *)argument, &dba, sizeof(dba)))
		ret = -EFAULT;
out:
	memset(&dba, 0, sizeof(dba));
	return ret;
}

static int en7581_epon_clear_report_thresholds(struct en7581_epon *priv)
{
	u8 llid, queue, index;
	int ret;

	for (llid = 0; llid < AIROHA_EPON_LLID_COUNT; llid++)
		for (index = 0; index < EN7581_EPON_THRESHOLD_COUNT; index++)
			for (queue = 0; queue < EN7581_EPON_QUEUE_COUNT; queue++) {
				ret = en7581_epon_set_report_threshold(priv, llid,
					queue, index, 0);
				if (ret)
					return ret;
			}
	return 0;
}

static void en7581_epon_set_llid_tx_fec(struct en7581_epon *priv,
					u8 index, bool enabled)
{
	u32 reg = EN7581_EPON_LLID_CFG_BASE + index / 4 * sizeof(u32);
	u32 mask = BIT(3 + index % 4 * 8);
	u32 value = en7581_epon_read(priv, reg);

	en7581_epon_write(priv, reg, enabled ? value | mask : value & ~mask);
	priv->llids[index].tx_fec = enabled;
}

static bool en7581_epon_get_llid_tx_fec(struct en7581_epon *priv, u8 index)
{
	u32 reg = EN7581_EPON_LLID_CFG_BASE + index / 4 * sizeof(u32);
	u32 mask = BIT(3 + index % 4 * 8);

	return en7581_epon_read(priv, reg) & mask;
}

static void en7581_epon_set_hw_state(struct en7581_epon *priv, u8 index,
				     u8 state)
{
	u32 reg = EN7581_EPON_LLID_STATUS_BASE + index * sizeof(u32);
	u32 value = en7581_epon_read(priv, reg);

	en7581_epon_write(priv, reg,
		airoha_epon_llid_set_discovery_state(value, state));
}

static int en7581_epon_wait_mac_address(struct en7581_epon *priv)
{
	u32 value;

	return readl_poll_timeout(priv->mac + EN7581_EPON_MAC_ADDRESS_CFG,
		value, !(value & EN7581_EPON_MAC_ADDRESS_DONE), 1, 1000);
}

static int en7581_epon_program_mac_word(struct en7581_epon *priv, u8 index,
					u8 word, u32 value)
{
	u32 command;
	int ret;

	ret = en7581_epon_wait_mac_address(priv);
	if (ret)
		return ret;
	en7581_epon_write(priv, EN7581_EPON_MAC_ADDRESS_VALUE, value);
	command = EN7581_EPON_MAC_ADDRESS_WRITE |
		FIELD_PREP(EN7581_EPON_MAC_ADDRESS_LLID, index) |
		(word ? EN7581_EPON_MAC_ADDRESS_WORD : 0);
	en7581_epon_write(priv, EN7581_EPON_MAC_ADDRESS_CFG, command);
	return en7581_epon_wait_mac_address(priv);
}

static int en7581_epon_program_llid_mac(struct en7581_epon *priv, u8 index)
{
	struct en7581_epon_llid *llid = &priv->llids[index];
	u32 low = get_unaligned_be32(llid->mac + 2);
	u16 high = get_unaligned_be16(llid->mac);
	int ret;

	ret = en7581_epon_program_mac_word(priv, index, 0, low);
	return ret ? ret : en7581_epon_program_mac_word(priv, index, 1, high);
}

static void en7581_epon_derive_llid_mac(struct en7581_epon *priv, u8 index)
{
	u32 tail;

	ether_addr_copy(priv->llids[index].mac, priv->onu_mac);
	tail = get_unaligned_be32(priv->llids[index].mac + 2) + index;
	put_unaligned_be32(tail, priv->llids[index].mac + 2);
}

static void en7581_epon_reset_llid(struct en7581_epon *priv, u8 index)
{
	struct en7581_epon_llid *llid = &priv->llids[index];

	if (priv->mac_initialized && priv->key_suite[index] &&
	    en7581_epon_clear_llid_keys_locked(priv, index))
		priv->key_errors++;
	llid->state = AIROHA_EPON_MPCP_REGISTERING;
	llid->llid = 0;
	llid->valid = false;
	llid->retries = 3;
	llid->rx_fec = false;
	llid->deadline = 0;
	llid->silent_until = 0;
	en7581_epon_set_hw_state(priv, index,
		AIROHA_EPON_DISCOVERY_STATE_REGISTERING);
}

static int en7581_epon_sync_datapath_locked(struct en7581_epon *priv);

static int en7581_epon_clear_sessions_locked(struct en7581_epon *priv)
{
	int ret, key_ret = 0;
	u8 index;

	if (priv->mac_initialized)
		key_ret = en7581_epon_clear_all_keys_locked(priv);
	en7581_epon_clear_key_events(priv);
	priv->local_deregister_pending = 0;
	priv->local_deregister_inflight = EN7581_EPON_NO_LLID;
	for (index = 0; index < AIROHA_EPON_LLID_COUNT; index++) {
		priv->llids[index].state = AIROHA_EPON_MPCP_WAIT;
		priv->llids[index].llid = 0;
		priv->llids[index].valid = false;
		priv->llids[index].retries = 0;
		priv->llids[index].rx_fec = false;
		priv->llids[index].deadline = 0;
		priv->llids[index].silent_until = 0;
		en7581_epon_set_hw_state(priv, index,
			AIROHA_EPON_DISCOVERY_STATE_UNREGISTERED);
	}
	ret = en7581_epon_sync_datapath_locked(priv);
	return key_ret ? key_ret : ret;
}

static bool en7581_epon_any_registered(struct en7581_epon *priv)
{
	u8 index;

	for (index = 0; index < AIROHA_EPON_LLID_COUNT; index++)
		if (priv->llids[index].state == AIROHA_EPON_MPCP_REGISTERED)
			return true;
	return false;
}

static int en7581_epon_sync_datapath_locked(struct en7581_epon *priv)
{
	const struct airoha_epon_mode_profile *profile;
	u32 llid_mask = 0, report_fec_mask = 0;
	u8 index;
	int ret;

	profile = airoha_epon_mode_profile(priv->active_mode);
	for (index = 0; index < AIROHA_EPON_LLID_COUNT; index++) {
		if (!airoha_epon_mpcp_data_ready(priv->llids[index].state,
					  priv->llids[index].valid))
			continue;
		llid_mask |= BIT(index);
		if (profile && profile->qdma_report_fec &&
		    priv->llids[index].tx_fec)
			report_fec_mask |= BIT(index);
	}
	if (llid_mask == priv->datapath_llid_mask &&
	    report_fec_mask == priv->datapath_report_fec_mask)
		return 0;
	ret = airoha_epon_qdma_llids_apply(priv->ethernet_np, llid_mask,
					  report_fec_mask);
	if (!ret) {
		priv->datapath_llid_mask = llid_mask;
		priv->datapath_report_fec_mask = report_fec_mask;
		priv->loopback_mask &= llid_mask;
	}
	return ret;
}

static long en7581_epon_oam_fec_ioctl(struct en7581_epon *priv,
				      unsigned int command,
				      unsigned long argument)
{
	struct airoha_epon_oam_fec fec;
	bool previous_tx, previous_rx;
	int ret = 0;

	if (copy_from_user(&fec, (void __user *)argument, sizeof(fec)))
		return -EFAULT;
	if (fec.version != AIROHA_EPON_OAM_ABI_VERSION ||
	    fec.llid_index >= AIROHA_EPON_LLID_COUNT ||
	    fec.reserved2 ||
	    (command == AIROHA_EPON_OAM_IOC_GET_FEC &&
	     (fec.tx_enabled || fec.rx_enabled)) ||
	    (command == AIROHA_EPON_OAM_IOC_SET_FEC &&
	     (fec.tx_enabled > 1 || fec.rx_enabled > 1))) {
		ret = -EINVAL;
		goto out;
	}

	mutex_lock(&priv->lock);
	if (!en7581_epon_active(priv) || !priv->mac_initialized ||
	    !priv->oam_consumer_registered ||
	    priv->llids[fec.llid_index].state != AIROHA_EPON_MPCP_REGISTERED ||
	    !priv->llids[fec.llid_index].valid) {
		ret = -EHOSTDOWN;
		goto out_unlock;
	}
	previous_tx = en7581_epon_get_llid_tx_fec(priv, fec.llid_index);
	previous_rx = priv->llids[fec.llid_index].rx_fec;
	if (command == AIROHA_EPON_OAM_IOC_GET_FEC) {
		if (priv->active_mode == AIROHA_XPON_MODE_EPON_10G_10G &&
		    !previous_tx) {
			ret = -EIO;
			goto out_unlock;
		}
		fec.tx_enabled = previous_tx;
		fec.rx_enabled = previous_rx;
		goto out_unlock;
	}
	if (priv->active_mode == AIROHA_XPON_MODE_EPON_10G_10G &&
	    !fec.tx_enabled) {
		ret = -EOPNOTSUPP;
		goto out_unlock;
	}
	if (previous_tx == !!fec.tx_enabled) {
		priv->llids[fec.llid_index].rx_fec = fec.rx_enabled;
		goto out_unlock;
	}

	en7581_epon_set_llid_tx_fec(priv, fec.llid_index, fec.tx_enabled);
	if (en7581_epon_get_llid_tx_fec(priv, fec.llid_index) !=
	    !!fec.tx_enabled) {
		ret = -EIO;
		goto rollback;
	}
	ret = en7581_epon_sync_datapath_locked(priv);
	if (!ret) {
		priv->llids[fec.llid_index].rx_fec = fec.rx_enabled;
		goto out_unlock;
	}

rollback:
	priv->llids[fec.llid_index].rx_fec = previous_rx;
	en7581_epon_set_llid_tx_fec(priv, fec.llid_index, previous_tx);
	if (en7581_epon_get_llid_tx_fec(priv, fec.llid_index) != previous_tx)
		ret = -EIO;
out_unlock:
	mutex_unlock(&priv->lock);
	if (!ret && command == AIROHA_EPON_OAM_IOC_GET_FEC &&
	    copy_to_user((void __user *)argument, &fec, sizeof(fec)))
		ret = -EFAULT;
out:
	memzero_explicit(&fec, sizeof(fec));
	return ret;
}

static long en7581_epon_oam_loopback_ioctl(struct en7581_epon *priv,
					   unsigned int command,
					   unsigned long argument)
{
	struct airoha_epon_oam_loopback loopback;
	int ret = 0;

	if (copy_from_user(&loopback, (void __user *)argument,
			   sizeof(loopback)))
		return -EFAULT;
	if (loopback.version != AIROHA_EPON_OAM_ABI_VERSION ||
	    loopback.llid_index >= AIROHA_EPON_LLID_COUNT ||
	    loopback.reserved || loopback.reserved2 ||
	    (command == AIROHA_EPON_OAM_IOC_GET_LOOPBACK &&
	     loopback.enabled) ||
	    (command == AIROHA_EPON_OAM_IOC_SET_LOOPBACK &&
	     loopback.enabled > 1)) {
		ret = -EINVAL;
		goto out;
	}

	mutex_lock(&priv->lock);
	if (!en7581_epon_active(priv) || !priv->mac_initialized ||
	    !priv->oam_consumer_registered ||
	    priv->llids[loopback.llid_index].state !=
		AIROHA_EPON_MPCP_REGISTERED ||
	    !priv->llids[loopback.llid_index].valid) {
		ret = -EHOSTDOWN;
		goto out_unlock;
	}
	if (command == AIROHA_EPON_OAM_IOC_GET_LOOPBACK) {
		loopback.enabled = !!(priv->loopback_mask &
					 BIT(loopback.llid_index));
		goto out_unlock;
	}
	ret = airoha_epon_loopback_set(priv->ethernet_np,
		loopback.llid_index, loopback.enabled);
	if (!ret) {
		if (loopback.enabled)
			priv->loopback_mask |= BIT(loopback.llid_index);
		else
			priv->loopback_mask &= ~BIT(loopback.llid_index);
	}
out_unlock:
	mutex_unlock(&priv->lock);
	if (!ret && command == AIROHA_EPON_OAM_IOC_GET_LOOPBACK &&
	    copy_to_user((void __user *)argument, &loopback, sizeof(loopback)))
		ret = -EFAULT;
out:
	memzero_explicit(&loopback, sizeof(loopback));
	return ret;
}

static long en7581_epon_oam_clear_session_ioctl(struct en7581_epon *priv,
							unsigned long argument)
{
	struct airoha_epon_oam_session session;
	unsigned long flags;
	u32 llid_bit;
	int ret, first_error = 0;

	if (copy_from_user(&session, (void __user *)argument, sizeof(session)))
		return -EFAULT;
	if (session.version != AIROHA_EPON_OAM_ABI_VERSION ||
	    session.llid_index >= AIROHA_EPON_LLID_COUNT ||
	    memchr_inv(session.reserved, 0, sizeof(session.reserved))) {
		ret = -EINVAL;
		goto out;
	}

	llid_bit = BIT(session.llid_index);
	mutex_lock(&priv->lock);
	if (!en7581_epon_active(priv) || !priv->mac_initialized ||
	    !priv->oam_consumer_registered ||
	    priv->llids[session.llid_index].state != AIROHA_EPON_MPCP_REGISTERED ||
	    !priv->llids[session.llid_index].valid) {
		first_error = -EHOSTDOWN;
		goto out_unlock;
	}

	ret = airoha_epon_loopback_set(priv->ethernet_np,
					 session.llid_index, false);
	if (ret)
		first_error = ret;
	priv->loopback_mask &= ~llid_bit;

	/* Key removal remains fail-closed even if loopback teardown failed. */
	ret = en7581_epon_clear_llid_keys_locked(priv, session.llid_index);
	if (ret && !first_error)
		first_error = ret;
	if (ret)
		priv->key_errors++;

	spin_lock_irqsave(&priv->irq_lock, flags);
	priv->pending_ds_key_events &= ~llid_bit;
	priv->pending_us_key_events &= ~llid_bit;
	spin_unlock_irqrestore(&priv->irq_lock, flags);
	wake_up_interruptible(&priv->oam_rx_wait);

out_unlock:
	mutex_unlock(&priv->lock);
	ret = first_error;
out:
	memzero_explicit(&session, sizeof(session));
	return ret;
}

static int en7581_epon_holdover_configure(struct en7581_epon *priv,
						  bool enabled,
						  unsigned int time_ms)
{
	bool expire_now = false;
	int ret = 0;

	if (!airoha_epon_holdover_config_valid(enabled, time_ms))
		return -ERANGE;
	mutex_lock(&priv->lock);
	if (priv->removing) {
		ret = -ENODEV;
	} else {
		priv->holdover.enabled = enabled;
		priv->holdover.time_ms = time_ms;
		expire_now = !enabled && priv->holdover.active;
	}
	mutex_unlock(&priv->lock);
	if (expire_now)
		mod_delayed_work(system_wq, &priv->holdover_work, 0);
	return ret;
}

static long en7581_epon_oam_holdover_ioctl(struct en7581_epon *priv,
					   unsigned int command,
					   unsigned long argument)
{
	struct airoha_epon_oam_holdover holdover;
	int ret = 0;

	if (copy_from_user(&holdover, (void __user *)argument,
			   sizeof(holdover)))
		return -EFAULT;
	if (holdover.version != AIROHA_EPON_OAM_ABI_VERSION ||
	    holdover.reserved || holdover.reserved2 ||
	    (command == AIROHA_EPON_OAM_IOC_GET_HOLDOVER &&
	     (holdover.enabled || holdover.active || holdover.time_ms)) ||
	    (command == AIROHA_EPON_OAM_IOC_SET_HOLDOVER &&
	     (holdover.enabled > 1 || holdover.active ||
	      !airoha_epon_holdover_config_valid(holdover.enabled,
						    holdover.time_ms)))) {
		ret = -EINVAL;
		goto out;
	}

	if (command == AIROHA_EPON_OAM_IOC_SET_HOLDOVER) {
		ret = en7581_epon_holdover_configure(priv, holdover.enabled,
						       holdover.time_ms);
		goto out;
	}
	mutex_lock(&priv->lock);
	holdover.enabled = priv->holdover.enabled;
	holdover.active = priv->holdover.active;
	holdover.time_ms = priv->holdover.time_ms;
	mutex_unlock(&priv->lock);
	if (copy_to_user((void __user *)argument, &holdover, sizeof(holdover)))
		ret = -EFAULT;
out:
	memzero_explicit(&holdover, sizeof(holdover));
	return ret;
}

static int en7581_epon_send_command(struct en7581_epon *priv, u8 command,
				    u8 index, bool deregister, bool ack)
{
	u32 value = en7581_epon_read(priv, EN7581_EPON_LLID_DISCOVERY_CTRL);

	if (!(value & AIROHA_EPON_MPCP_COMMAND_DONE) &&
	    FIELD_GET(AIROHA_EPON_MPCP_COMMAND, value))
		return -EBUSY;
	en7581_epon_write(priv, EN7581_EPON_LLID_DISCOVERY_CTRL,
		airoha_epon_mpcp_command(command, index, deregister, ack));
	return 0;
}

static int en7581_epon_start_next_deregister_locked(struct en7581_epon *priv)
{
	u8 index;
	int ret;

	if (priv->local_deregister_inflight != EN7581_EPON_NO_LLID ||
	    !priv->local_deregister_pending)
		return 0;
	index = __ffs(priv->local_deregister_pending);
	ret = en7581_epon_send_command(priv,
		AIROHA_EPON_MPCP_CMD_NORMAL_REQUEST, index, true, false);
	if (ret == -EBUSY)
		return 0;
	if (ret)
		return ret;
	priv->local_deregister_pending &= ~BIT(index);
	priv->local_deregister_inflight = index;
	priv->llids[index].deadline = jiffies +
		EN7581_EPON_TRANSACTION_TIMEOUT;
	return 0;
}

static void en7581_epon_set_tx_burst_mode(struct en7581_epon *priv,
						   bool burst)
{
	u32 value = readl(priv->phy_csr + EN7581_EPON_PHY_DUMMY_RX);

	writel(airoha_epon_phy_tx_mode(value, burst),
	       priv->phy_csr + EN7581_EPON_PHY_DUMMY_RX);
}

static void en7581_epon_apply_sync_time_locked(struct en7581_epon *priv)
{
	struct airoha_epon_mpcp_sync_update update;
	u16 observed;

	observed = FIELD_GET(AIROHA_EPON_MPCP_SYNC_TIME,
			     en7581_epon_read(priv, EN7581_EPON_SYNC_TIME));
	update = airoha_epon_mpcp_sync_update(
		observed, priv->sync_time, en7581_epon_any_registered(priv));
	if (!update.program_timestamp_adjust)
		return;
	if (update.program_sync_time)
		en7581_epon_write(priv, EN7581_EPON_SYNC_TIME,
				    update.sync_time);
	en7581_epon_write(priv, EN7581_EPON_TRX_ADJUST1,
			    update.timestamp_adjust);
	priv->sync_time = update.sync_time;
}

static int en7581_epon_handle_discovery(struct en7581_epon *priv)
{
	u8 index, offset;
	int ret;

	if (!priv->onu_mac_set || !priv->phy_ready ||
	    !airoha_en7572_is_ready(priv->bosa) ||
	    airoha_en7572_fault_locked(priv->bosa))
		return -EAGAIN;

	for (offset = 0; offset < AIROHA_EPON_LLID_COUNT; offset++) {
		struct en7581_epon_llid *llid;

		index = (priv->next_discovery_llid + offset) %
			AIROHA_EPON_LLID_COUNT;
		llid = &priv->llids[index];

		if (!(priv->llid_mask & BIT(index)) ||
		    (llid->state != AIROHA_EPON_MPCP_REGISTERING &&
		     llid->state != AIROHA_EPON_MPCP_REGISTER_REQUEST))
			continue;
		if (!llid->retries) {
			llid->retries = 3;
			continue;
		}
		llid->retries--;
		priv->next_discovery_llid = (index + 1) %
			AIROHA_EPON_LLID_COUNT;
		ret = airoha_en7572_set_tx_enabled(priv->bosa, true);
		if (ret)
			return ret;
		en7581_epon_set_tx_burst_mode(priv, true);
		en7581_epon_apply_sync_time_locked(priv);
		ret = en7581_epon_send_command(priv,
			AIROHA_EPON_MPCP_CMD_DISCOVERY_REQUEST, index,
			false, false);
		if (!ret)
			llid->deadline = jiffies +
				EN7581_EPON_TRANSACTION_TIMEOUT;
		if (ret && !en7581_epon_any_registered(priv))
			airoha_en7572_set_tx_enabled(priv->bosa, false);
		return ret;
	}
	return 0;
}

static void en7581_epon_handle_register(struct en7581_epon *priv, u8 index)
{
	struct en7581_epon_llid *llid = &priv->llids[index];
	u32 status = en7581_epon_read(priv,
		EN7581_EPON_LLID_STATUS_BASE + index * sizeof(u32));
	u8 flag = FIELD_GET(AIROHA_EPON_LLID_REGISTER_FLAG, status);
	bool valid = status & AIROHA_EPON_LLID_VALID;

	if (!airoha_epon_mpcp_register_flag_allowed(llid->state, flag,
						       valid)) {
		priv->error_events++;
		return;
	}

	switch (flag) {
	case AIROHA_EPON_REGISTER_FLAG_REREGISTER:
	case AIROHA_EPON_REGISTER_FLAG_ACK:
		llid->llid = FIELD_GET(AIROHA_EPON_LLID_VALUE, status);
		llid->valid = true;
		llid->state = AIROHA_EPON_MPCP_REGISTER_PENDING;
		llid->deadline = jiffies + EN7581_EPON_TRANSACTION_TIMEOUT;
		en7581_epon_set_hw_state(priv, index,
			AIROHA_EPON_DISCOVERY_STATE_REGISTERING);
		if (en7581_epon_sync_datapath_locked(priv)) {
			llid->state = AIROHA_EPON_MPCP_RETRY;
			llid->valid = false;
			priv->error_events++;
			airoha_en7572_set_tx_enabled(priv->bosa, false);
			break;
		}
		en7581_epon_set_tx_burst_mode(priv, false);
		if (en7581_epon_send_command(priv,
			    AIROHA_EPON_MPCP_CMD_REGISTER_ACK, index,
			    false, true)) {
			llid->state = AIROHA_EPON_MPCP_RETRY;
			llid->deadline = jiffies +
				EN7581_EPON_TRANSACTION_TIMEOUT;
		}
		break;
	case AIROHA_EPON_REGISTER_FLAG_DEREGISTER:
		priv->deregistration_events++;
		llid->state = AIROHA_EPON_MPCP_REMOTE_DEREGISTER;
		llid->valid = false;
		llid->llid = 0;
		llid->deadline = jiffies + EN7581_EPON_MPCP_WORK_INTERVAL;
		en7581_epon_set_hw_state(priv, index,
			AIROHA_EPON_DISCOVERY_STATE_UNREGISTERED);
		if (en7581_epon_clear_llid_keys_locked(priv, index))
			priv->key_errors++;
		if (en7581_epon_sync_datapath_locked(priv)) {
			priv->error_events++;
			airoha_en7572_set_tx_enabled(priv->bosa, false);
		}
		break;
	case AIROHA_EPON_REGISTER_FLAG_NACK:
		llid->state = AIROHA_EPON_MPCP_DENIED;
		llid->valid = false;
		llid->llid = 0;
		llid->deadline = 0;
		llid->silent_until = jiffies + priv->silent_time * HZ;
		priv->denied_events++;
		en7581_epon_set_hw_state(priv, index,
			AIROHA_EPON_DISCOVERY_STATE_UNREGISTERED);
		if (en7581_epon_clear_llid_keys_locked(priv, index))
			priv->key_errors++;
		if (en7581_epon_sync_datapath_locked(priv)) {
			priv->error_events++;
			airoha_en7572_set_tx_enabled(priv->bosa, false);
		}
		break;
	}
}

static int en7581_epon_local_deregister_locked(struct en7581_epon *priv,
						u8 index)
{
	struct en7581_epon_llid *llid = &priv->llids[index];
	int ret, start_ret;

	if (!(priv->llid_mask & BIT(index)) ||
	    llid->state != AIROHA_EPON_MPCP_REGISTERED)
		return -EINVAL;
	llid->state = AIROHA_EPON_MPCP_LOCAL_DEREGISTER;
	llid->deadline = 0;
	priv->local_deregister_pending |= BIT(index);
	priv->local_deregistration_events++;
	if (en7581_epon_clear_llid_keys_locked(priv, index))
		priv->key_errors++;
	ret = en7581_epon_sync_datapath_locked(priv);
	if (ret)
		airoha_en7572_set_tx_enabled(priv->bosa, false);
	start_ret = en7581_epon_start_next_deregister_locked(priv);
	return ret ? ret : start_ret;
}

static int en7581_epon_handle_mpcp_timeout_locked(struct en7581_epon *priv)
{
	int ret, first_error = 0;
	u8 index;

	priv->local_deregister_pending = 0;
	priv->local_deregister_inflight = EN7581_EPON_NO_LLID;
	for (index = 0; index < AIROHA_EPON_LLID_COUNT; index++) {
		struct en7581_epon_llid *llid = &priv->llids[index];

		switch (airoha_epon_mpcp_timeout_action(
				priv->llid_mask & BIT(index), llid->state,
				llid->valid)) {
		case AIROHA_EPON_MPCP_TIMEOUT_IGNORE:
			break;
		case AIROHA_EPON_MPCP_TIMEOUT_REINITIALIZE:
			en7581_epon_reset_llid(priv, index);
			break;
		case AIROHA_EPON_MPCP_TIMEOUT_DEREGISTER:
			llid->state = AIROHA_EPON_MPCP_LOCAL_DEREGISTER;
			llid->deadline = 0;
			priv->local_deregister_pending |= BIT(index);
			priv->local_deregistration_events++;
			ret = en7581_epon_clear_llid_keys_locked(priv, index);
			if (ret && !first_error)
				first_error = ret;
			if (ret)
				priv->key_errors++;
			break;
		}
	}
	ret = en7581_epon_sync_datapath_locked(priv);
	if (ret && !first_error)
		first_error = ret;
	ret = en7581_epon_start_next_deregister_locked(priv);
	if (ret && !first_error)
		first_error = ret;
	return first_error;
}

static void en7581_epon_mpcp_work(struct work_struct *work)
{
	struct en7581_epon *priv = container_of(to_delayed_work(work),
		struct en7581_epon, mpcp_work);
	unsigned long now = jiffies;
	bool changed = false;
	bool reschedule;
	u8 index;

	mutex_lock(&priv->lock);
	reschedule = !priv->removing && priv->enabled && priv->mac_initialized &&
		en7581_epon_active(priv);
	if (!reschedule || priv->holdover.active)
		goto out;

	for (index = 0; index < AIROHA_EPON_LLID_COUNT; index++) {
		struct en7581_epon_llid *llid = &priv->llids[index];

		if (!(priv->llid_mask & BIT(index)))
			continue;
		switch (llid->state) {
		case AIROHA_EPON_MPCP_DENIED:
			if (llid->silent_until &&
			    time_after_eq(now, llid->silent_until)) {
				en7581_epon_reset_llid(priv, index);
				changed = true;
			}
			break;
		case AIROHA_EPON_MPCP_REGISTER_REQUEST:
		case AIROHA_EPON_MPCP_REGISTER_PENDING:
		case AIROHA_EPON_MPCP_RETRY:
		case AIROHA_EPON_MPCP_REGISTERING:
			if (llid->deadline &&
			    time_after_eq(now, llid->deadline)) {
				priv->timeout_events++;
				priv->error_events++;
				en7581_epon_reset_llid(priv, index);
				changed = true;
			}
			break;
		case AIROHA_EPON_MPCP_REMOTE_DEREGISTER:
			if (llid->deadline &&
			    time_after_eq(now, llid->deadline)) {
				en7581_epon_reset_llid(priv, index);
				changed = true;
			}
			break;
		case AIROHA_EPON_MPCP_LOCAL_DEREGISTER:
			if (llid->deadline &&
			    time_after_eq(now, llid->deadline)) {
				priv->local_deregister_pending &= ~BIT(index);
				if (priv->local_deregister_inflight == index)
					priv->local_deregister_inflight =
						EN7581_EPON_NO_LLID;
				en7581_epon_reset_llid(priv, index);
				priv->timeout_events++;
				priv->error_events++;
				changed = true;
			}
			break;
		default:
			break;
		}
	}
	if (changed) {
		if (en7581_epon_sync_datapath_locked(priv))
			priv->error_events++;
		if (!en7581_epon_any_registered(priv) &&
		    !priv->local_deregister_pending &&
		    priv->local_deregister_inflight == EN7581_EPON_NO_LLID)
			airoha_en7572_set_tx_enabled(priv->bosa, false);
	}
out:
	mutex_unlock(&priv->lock);
	if (changed)
		kobject_uevent(&priv->dev->kobj, KOBJ_CHANGE);
	if (reschedule)
		mod_delayed_work(system_wq, &priv->mpcp_work,
			EN7581_EPON_MPCP_WORK_INTERVAL);
}

static irqreturn_t en7581_epon_irq(int irq, void *data)
{
	struct en7581_epon *priv = data;
	unsigned long flags;
	u32 status, status2, status3;

	if (!en7581_epon_active(priv) || !READ_ONCE(priv->enabled) ||
	    READ_ONCE(priv->irqs_masked))
		return IRQ_NONE;
	status = en7581_epon_read(priv, EN7581_EPON_INT_STATUS) &
		en7581_epon_read(priv, EN7581_EPON_INT_ENABLE);
	status2 = en7581_epon_read(priv, EN7581_EPON_INT_STATUS2) &
		en7581_epon_read(priv, EN7581_EPON_INT_ENABLE2);
	status3 = en7581_epon_read(priv, EN7581_EPON_INT_STATUS3) &
		en7581_epon_read(priv, EN7581_EPON_INT_ENABLE3);
	if (!(status | status2 | status3))
		return IRQ_NONE;

	en7581_epon_write(priv, EN7581_EPON_INT_STATUS, status);
	en7581_epon_write(priv, EN7581_EPON_INT_STATUS2, status2);
	en7581_epon_write(priv, EN7581_EPON_INT_STATUS3, status3);
	spin_lock_irqsave(&priv->irq_lock, flags);
	priv->pending_status |= status;
	priv->pending_status2 |= status2;
	priv->pending_status3 |= status3;
	spin_unlock_irqrestore(&priv->irq_lock, flags);
	return IRQ_WAKE_THREAD;
}

static irqreturn_t en7581_epon_irq_thread(int irq, void *data)
{
	struct en7581_epon *priv = data;
	unsigned long flags;
	u32 command, key_low, key_high, status, status2, status3;
	unsigned int diagnostic_events;
	bool wake_key_events = false;
	u8 index;

	if (READ_ONCE(priv->irqs_masked) || !en7581_epon_active(priv))
		return IRQ_HANDLED;
	spin_lock_irqsave(&priv->irq_lock, flags);
	status = priv->pending_status;
	status2 = priv->pending_status2;
	status3 = priv->pending_status3;
	priv->pending_status = 0;
	priv->pending_status2 = 0;
	priv->pending_status3 = 0;
	spin_unlock_irqrestore(&priv->irq_lock, flags);

	mutex_lock(&priv->lock);
	if (!priv->enabled || !priv->mac_initialized) {
		mutex_unlock(&priv->lock);
		return IRQ_HANDLED;
	}
	priv->irq_events++;
	if (priv->holdover.active) {
		priv->holdover_suppressed_irqs += hweight32(status) +
			hweight32(status2) + hweight32(status3);
		mutex_unlock(&priv->lock);
		return IRQ_HANDLED;
	}
	if (status & EN7581_EPON_INT_REGISTER_REQ_DONE) {
		u8 request;

		command = en7581_epon_read(priv,
			EN7581_EPON_LLID_DISCOVERY_CTRL);
		index = FIELD_GET(AIROHA_EPON_MPCP_LLID_INDEX, command);
		request = FIELD_GET(AIROHA_EPON_MPCP_COMMAND, command);
		if (priv->llid_mask & BIT(index)) {
			struct en7581_epon_llid *llid = &priv->llids[index];

			if (request == AIROHA_EPON_MPCP_CMD_NORMAL_REQUEST &&
			    llid->state == AIROHA_EPON_MPCP_LOCAL_DEREGISTER &&
			    priv->local_deregister_inflight == index) {
				priv->local_deregister_inflight =
					EN7581_EPON_NO_LLID;
				en7581_epon_reset_llid(priv, index);
				priv->deregistration_events++;
			} else if (request ==
				   AIROHA_EPON_MPCP_CMD_DISCOVERY_REQUEST &&
				   llid->state ==
				   AIROHA_EPON_MPCP_REGISTERING) {
				llid->state = AIROHA_EPON_MPCP_REGISTER_REQUEST;
				llid->deadline = jiffies +
					EN7581_EPON_TRANSACTION_TIMEOUT;
			} else {
				priv->error_events++;
			}
		}
		if (en7581_epon_start_next_deregister_locked(priv)) {
			priv->error_events++;
			airoha_en7572_set_tx_enabled(priv->bosa, false);
		}
		/* The SDK suppresses a simultaneous gate to avoid FSM confusion. */
		status &= ~EN7581_EPON_INT_DISCOVERY_GATE;
	}
	for (index = 0; index < 8; index++)
		if (status & BIT(index + 1))
			en7581_epon_handle_register(priv, index);
	for (index = 8; index < AIROHA_EPON_LLID_COUNT; index++)
		if (status3 & BIT(index - 8))
			en7581_epon_handle_register(priv, index);
	if (status & EN7581_EPON_INT_REGISTER_ACK_DONE) {
		int ret;

		command = en7581_epon_read(priv,
			EN7581_EPON_LLID_DISCOVERY_CTRL);
		index = FIELD_GET(AIROHA_EPON_MPCP_LLID_INDEX, command);
		if ((priv->llid_mask & BIT(index)) &&
		    priv->llids[index].state ==
			AIROHA_EPON_MPCP_REGISTER_PENDING) {
			priv->llids[index].state = AIROHA_EPON_MPCP_REGISTERED;
			priv->llids[index].deadline = 0;
			priv->llids[index].silent_until = 0;
			en7581_epon_set_hw_state(priv, index,
				AIROHA_EPON_DISCOVERY_STATE_REGISTERED);
			ret = en7581_epon_sync_datapath_locked(priv);
			if (ret) {
				priv->llids[index].state = AIROHA_EPON_MPCP_RETRY;
				priv->llids[index].valid = false;
				priv->llids[index].deadline = jiffies +
					EN7581_EPON_TRANSACTION_TIMEOUT;
				en7581_epon_set_hw_state(priv, index,
					AIROHA_EPON_DISCOVERY_STATE_UNREGISTERED);
				priv->error_events++;
				airoha_en7572_set_tx_enabled(priv->bosa, false);
			} else {
				priv->registration_events++;
				kobject_uevent(&priv->dev->kobj, KOBJ_CHANGE);
			}
		}
		if (en7581_epon_start_next_deregister_locked(priv)) {
			priv->error_events++;
			airoha_en7572_set_tx_enabled(priv->bosa, false);
		}
	}
	if (status & EN7581_EPON_INT_DISCOVERY_GATE)
		en7581_epon_handle_discovery(priv);
	if (status & EN7581_EPON_INT_DS_KEY_CHANGE) {
		key_low = en7581_epon_read(priv, EN7581_EPON_DS_KEY_CHANGE);
		key_high = en7581_epon_read(priv, EN7581_EPON_DS_KEY_CHANGE2);
		en7581_epon_write(priv, EN7581_EPON_DS_KEY_CHANGE, key_low);
		en7581_epon_write(priv, EN7581_EPON_DS_KEY_CHANGE2, key_high);
		priv->key_changes += hweight32(key_low) + hweight32(key_high);
		spin_lock_irqsave(&priv->irq_lock, flags);
		priv->pending_ds_key_events |= key_low & priv->llid_mask;
		spin_unlock_irqrestore(&priv->irq_lock, flags);
		wake_key_events = !!(key_low & priv->llid_mask);
	}
	if (status & EN7581_EPON_INT_US_KEY_CHANGE) {
		key_low = en7581_epon_read(priv, EN7581_EPON_US_KEY_CHANGE);
		en7581_epon_write(priv, EN7581_EPON_US_KEY_CHANGE, key_low);
		priv->key_changes += hweight32(key_low);
		spin_lock_irqsave(&priv->irq_lock, flags);
		priv->pending_us_key_events |= key_low & priv->llid_mask;
		spin_unlock_irqrestore(&priv->irq_lock, flags);
		wake_key_events |= !!(key_low & priv->llid_mask);
	}
	if (status & EN7581_EPON_INT_DS_KEY_MISS) {
		key_low = en7581_epon_read(priv, EN7581_EPON_DS_KEY_MISS);
		key_high = en7581_epon_read(priv, EN7581_EPON_DS_KEY_MISS2);
		en7581_epon_write(priv, EN7581_EPON_DS_KEY_MISS, key_low);
		en7581_epon_write(priv, EN7581_EPON_DS_KEY_MISS2, key_high);
		priv->key_misses += hweight32(key_low) + hweight32(key_high);
		for (index = 0; index < AIROHA_EPON_LLID_COUNT; index++) {
			if (!(key_low & BIT(index)))
				continue;
			priv->llids[index].state = AIROHA_EPON_MPCP_RETRY;
			priv->llids[index].valid = false;
			priv->llids[index].deadline = jiffies +
				EN7581_EPON_MPCP_WORK_INTERVAL;
			if (en7581_epon_clear_llid_keys_locked(priv, index))
				priv->key_errors++;
		}
		if (en7581_epon_sync_datapath_locked(priv))
			priv->error_events++;
		priv->key_errors++;
		if (!en7581_epon_any_registered(priv))
			airoha_en7572_set_tx_enabled(priv->bosa, false);
	}
	if (status & EN7581_EPON_INT_MPCP_TIMEOUT) {
		priv->error_events++;
		priv->timeout_events++;
		if (en7581_epon_handle_mpcp_timeout_locked(priv))
			priv->error_events++;
	}
	if (status & EN7581_EPON_INT_REPORT_TIMEOUT)
		priv->timeout_events++;
	/* Match the SDK ISR: these conditions are diagnostic and must not
	 * revoke an otherwise registered link or disable the optical TX path. */
	diagnostic_events = airoha_epon_nonfatal_irq_count(status, status2);
	priv->error_events += diagnostic_events;
	if (!en7581_epon_any_registered(priv)) {
		bool pending = false;

		for (index = 0; index < AIROHA_EPON_LLID_COUNT; index++)
			pending |= priv->llids[index].state ==
				AIROHA_EPON_MPCP_REGISTER_REQUEST ||
				priv->llids[index].state ==
				AIROHA_EPON_MPCP_REGISTER_PENDING ||
				priv->llids[index].state ==
				AIROHA_EPON_MPCP_LOCAL_DEREGISTER;
		if (!pending)
			airoha_en7572_set_tx_enabled(priv->bosa, false);
	}
	mutex_unlock(&priv->lock);
	if (wake_key_events)
		wake_up_interruptible(&priv->oam_rx_wait);
	return IRQ_HANDLED;
}

static bool en7581_epon_phy_live_ready(struct en7581_epon *priv)
{
	return !(readl(priv->xepon_pcs + EN7581_XEPON_PCS_SFP_STATUS) &
		 EN7581_XEPON_PCS_SFP_RX_LOS) &&
	       (readl(priv->xepon_pcs + EN7581_XEPON_PCS_RX_SYNC_STATUS) &
		EN7581_XEPON_PCS_RX_SYNC_OK);
}

static void en7581_epon_phy_set_rx(struct en7581_epon *priv, bool enabled)
{
	writel(enabled ? EN7581_XEPON_PCS_RX_ENABLE :
			  EN7581_XEPON_PCS_RX_DISABLE,
	       priv->xepon_pcs + EN7581_XEPON_PCS_RX_CTRL_CFG);
}

static void en7581_epon_phy_set_logic_reset(struct en7581_epon *priv,
						    bool hold)
{
	writel(hold ? EN7581_XEPON_PCS_LOGIC_RST_HOLD :
		       EN7581_XEPON_PCS_LOGIC_RST_RELEASE,
	       priv->xepon_pcs + EN7581_XEPON_PCS_LOGIC_RST);
}

static const u32
en7581_epon_mac_init_offsets[AIROHA_EPON_MAC_INIT_REGISTER_COUNT] = {
	[AIROHA_EPON_MAC_INIT_REPORT_BITMAP] = EN7581_EPON_REPORT_BITMAP,
	[AIROHA_EPON_MAC_INIT_REPORT_CONFIG] = EN7581_EPON_REPORT_CFG,
	[AIROHA_EPON_MAC_INIT_REPORT_CONFIG2] = EN7581_EPON_REPORT_CFG2,
	[AIROHA_EPON_MAC_INIT_LASER_TIME] = EN7581_EPON_LASER_TIME,
	[AIROHA_EPON_MAC_INIT_SYNC_TIME] = EN7581_EPON_SYNC_TIME,
	[AIROHA_EPON_MAC_INIT_MPCP_TIMEOUT] = EN7581_EPON_MPCP_TIMEOUT,
	[AIROHA_EPON_MAC_INIT_TRX_ADJUST1] = EN7581_EPON_TRX_ADJUST1,
	[AIROHA_EPON_MAC_INIT_TRX_ADJUST2] = EN7581_EPON_TRX_ADJUST2,
	[AIROHA_EPON_MAC_INIT_TRX_ADJUST3] = EN7581_EPON_TRX_ADJUST3,
	[AIROHA_EPON_MAC_INIT_TRX_ADJUST4] = EN7581_EPON_TRX_ADJUST4,
	[AIROHA_EPON_MAC_INIT_TX_FETCH] = EN7581_EPON_TX_FETCH,
	[AIROHA_EPON_MAC_INIT_TX_CAL] = EN7581_EPON_TX_CAL,
	[AIROHA_EPON_MAC_INIT_DYING_GASP] = EN7581_EPON_DYING_GASP,
	[AIROHA_EPON_MAC_INIT_DYING_GASP_WORD1] =
		EN7581_EPON_DYING_GASP_WORD1,
	[AIROHA_EPON_MAC_INIT_DYING_GASP_WORD2] =
		EN7581_EPON_DYING_GASP_WORD2,
	[AIROHA_EPON_MAC_INIT_DYING_GASP_WORD3] =
		EN7581_EPON_DYING_GASP_WORD3,
	[AIROHA_EPON_MAC_INIT_DYING_GASP_WORD4] =
		EN7581_EPON_DYING_GASP_WORD4,
	[AIROHA_EPON_MAC_INIT_DYING_GASP_WORD5] =
		EN7581_EPON_DYING_GASP_WORD5,
	[AIROHA_EPON_MAC_INIT_DYING_GASP_WORD6] =
		EN7581_EPON_DYING_GASP_WORD6,
	[AIROHA_EPON_MAC_INIT_DYING_GASP_WORD7] =
		EN7581_EPON_DYING_GASP_WORD7,
	[AIROHA_EPON_MAC_INIT_DYING_GASP_WORD8] =
		EN7581_EPON_DYING_GASP_WORD8,
	[AIROHA_EPON_MAC_INIT_DYING_GASP_WORD9] =
		EN7581_EPON_DYING_GASP_WORD9,
};

static int en7581_epon_mac_init_offset(
	struct en7581_epon *priv, enum airoha_epon_mac_init_register reg,
	u32 *offset)
{
	if (reg >= AIROHA_EPON_MAC_INIT_REGISTER_COUNT)
		return -EINVAL;
	if (reg != AIROHA_EPON_MAC_INIT_REPORT_QSIZE_ADJUST) {
		*offset = en7581_epon_mac_init_offsets[reg];
		return 0;
	}
	if (priv->active_mode == AIROHA_XPON_MODE_EPON_10G_1G)
		*offset = EN7581_EPON_1G_REPORT_QSIZE_ADJUST;
	else if (priv->active_mode == AIROHA_XPON_MODE_EPON_10G_10G)
		*offset = EN7581_EPON_10G_REPORT_QSIZE_ADJUST;
	else
		return -EINVAL;
	return 0;
}

static int en7581_epon_mac_init_read(
	void *context, enum airoha_epon_mac_init_register reg,
	unsigned int *value)
{
	struct en7581_epon *priv = context;
	u32 offset;
	int ret;

	ret = en7581_epon_mac_init_offset(priv, reg, &offset);
	if (ret)
		return ret;
	*value = en7581_epon_read(priv, offset);
	return 0;
}

static int en7581_epon_mac_init_update(
	void *context, enum airoha_epon_mac_init_register reg,
	unsigned int mask, unsigned int value)
{
	struct en7581_epon *priv = context;
	u32 offset, register_value, expected;
	int ret;

	ret = en7581_epon_mac_init_offset(priv, reg, &offset);
	if (ret)
		return ret;
	register_value = en7581_epon_read(priv, offset);
	expected = (register_value & ~mask) | (value & mask);
	en7581_epon_write(priv, offset, expected);
	return (en7581_epon_read(priv, offset) & mask) ==
	       (expected & mask) ? 0 : -EIO;
}

static void en7581_epon_mac_init_fail_closed(void *context)
{
	struct en7581_epon *priv = context;
	u32 value;

	priv->mac_initialized = false;
	priv->phy_ready = false;
	priv->phy_fault = true;
	en7581_epon_write(priv, EN7581_EPON_INT_ENABLE, 0);
	en7581_epon_write(priv, EN7581_EPON_INT_ENABLE2, 0);
	en7581_epon_write(priv, EN7581_EPON_INT_ENABLE3, 0);
	value = en7581_epon_read(priv, EN7581_EPON_GLB_CFG);
	en7581_epon_write(priv, EN7581_EPON_GLB_CFG,
			  value | EN7581_EPON_PATH_STOP);
	regmap_update_bits(priv->scu, EN7581_SCU_SSR3,
			   EN7581_SCU_EPON_LOGIC_RESET,
			   EN7581_SCU_EPON_LOGIC_RESET);
	en7581_epon_phy_set_rx(priv, false);
	en7581_epon_phy_set_logic_reset(priv, true);
	if (priv->bosa)
		airoha_en7572_emergency_disable(priv->bosa);
}

static const struct airoha_epon_mac_init_ops
en7581_epon_mac_init_ops = {
	.read = en7581_epon_mac_init_read,
	.update = en7581_epon_mac_init_update,
	.fail_closed = en7581_epon_mac_init_fail_closed,
};

static int en7581_epon_dying_gasp_update(void *context, bool enabled)
{
	struct en7581_epon *priv = context;
	u32 value;

	value = en7581_epon_read(priv, EN7581_EPON_DYING_GASP);
	if (enabled)
		value |= EN7581_EPON_DYING_GASP_HW;
	else
		value &= ~EN7581_EPON_DYING_GASP_HW;
	en7581_epon_write(priv, EN7581_EPON_DYING_GASP, value);
	return 0;
}

static int en7581_epon_dying_gasp_read(void *context, bool *enabled)
{
	struct en7581_epon *priv = context;

	*enabled = en7581_epon_read(priv, EN7581_EPON_DYING_GASP) &
		EN7581_EPON_DYING_GASP_HW;
	return 0;
}

static const struct airoha_epon_dying_gasp_ops
en7581_epon_dying_gasp_ops = {
	.update = en7581_epon_dying_gasp_update,
	.read = en7581_epon_dying_gasp_read,
	.fail_closed = en7581_epon_mac_init_fail_closed,
};

static int en7581_epon_scu_pon_mac_reset_update(void *context, bool hold)
{
	struct en7581_epon *priv = context;

	return regmap_update_bits(priv->scu, EN7581_SCU_RSTCTRL1,
				  EN7581_SCU_PON_MAC_RESET,
				  hold ? EN7581_SCU_PON_MAC_RESET : 0);
}

static int en7581_epon_scu_pon_mac_reset_read(void *context, bool *hold)
{
	struct en7581_epon *priv = context;
	unsigned int value;
	int ret;

	ret = regmap_read(priv->scu, EN7581_SCU_RSTCTRL1, &value);
	if (!ret)
		*hold = value & EN7581_SCU_PON_MAC_RESET;
	return ret;
}

static int en7581_epon_scu_logic_reset_update(void *context, bool hold)
{
	struct en7581_epon *priv = context;

	return regmap_update_bits(priv->scu, EN7581_SCU_SSR3,
				  EN7581_SCU_EPON_LOGIC_RESET,
				  hold ? EN7581_SCU_EPON_LOGIC_RESET : 0);
}

static int en7581_epon_scu_logic_reset_read(void *context, bool *hold)
{
	struct en7581_epon *priv = context;
	unsigned int value;
	int ret;

	ret = regmap_read(priv->scu, EN7581_SCU_SSR3, &value);
	if (!ret)
		*hold = value & EN7581_SCU_EPON_LOGIC_RESET;
	return ret;
}

static void en7581_epon_scu_reset_delay(void *context)
{
	(void)context;
	udelay(1);
}

static const struct airoha_epon_logic_reset_ops
en7581_epon_scu_pon_mac_reset_ops = {
	.update = en7581_epon_scu_pon_mac_reset_update,
	.read = en7581_epon_scu_pon_mac_reset_read,
	.delay = en7581_epon_scu_reset_delay,
	.fail_closed = en7581_epon_mac_init_fail_closed,
};

static const struct airoha_epon_logic_reset_ops
en7581_epon_scu_logic_reset_ops = {
	.update = en7581_epon_scu_logic_reset_update,
	.read = en7581_epon_scu_logic_reset_read,
	.delay = en7581_epon_scu_reset_delay,
	.fail_closed = en7581_epon_mac_init_fail_closed,
};

static int en7581_epon_pcs_mode(struct en7581_epon *priv,
				 enum airoha_pcs_xpon_mode *mode)
{
	switch (priv->active_mode) {
	case AIROHA_XPON_MODE_EPON_10G_1G:
		*mode = AIROHA_PCS_XPON_MODE_EPON_10G_1G;
		return 0;
	case AIROHA_XPON_MODE_EPON_10G_10G:
		*mode = AIROHA_PCS_XPON_MODE_EPON_10G_10G;
		return 0;
	default:
		return -EINVAL;
	}
}

static void en7581_epon_phy_schedule_recovery_locked(
	struct en7581_epon *priv, unsigned long delay)
{
	if (priv->removing || !priv->hardware_selected || !priv->enabled ||
	    priv->phy_los)
		return;
	priv->phy_ready = false;
	priv->phy_recovering = true;
	mod_delayed_work(system_wq, &priv->phy_recovery_work, delay);
}

static void en7581_epon_holdover_shift_deadlines_locked(
	struct en7581_epon *priv)
{
	unsigned long elapsed = jiffies - priv->holdover_started;
	u8 index;

	for (index = 0; index < AIROHA_EPON_LLID_COUNT; index++) {
		if (priv->llids[index].deadline)
			priv->llids[index].deadline += elapsed;
		if (priv->llids[index].silent_until)
			priv->llids[index].silent_until += elapsed;
	}
}

static bool en7581_epon_holdover_start_locked(struct en7581_epon *priv)
{
	if (priv->holdover_blocked ||
	    airoha_epon_holdover_loss(&priv->holdover) !=
	    AIROHA_EPON_HOLDOVER_START)
		return false;

	priv->holdover_started = jiffies;
	priv->holdover_tx_enabled =
		!airoha_en7572_tx_is_disabled(priv->bosa);
	priv->holdover_starts++;
	priv->phy_los = readl(priv->xepon_pcs +
			       EN7581_XEPON_PCS_SFP_STATUS) &
			EN7581_XEPON_PCS_SFP_RX_LOS;
	priv->phy_ready = false;
	priv->phy_recovering = false;
	priv->phy_fault = false;
	priv->phy_recovery_attempts = 0;
	en7581_epon_phy_set_rx(priv, true);
	mod_delayed_work(system_wq, &priv->holdover_work,
		msecs_to_jiffies(priv->holdover.time_ms));
	return true;
}

static bool en7581_epon_holdover_restore_locked(struct en7581_epon *priv)
{
	bool restore_tx = priv->holdover_tx_enabled;

	en7581_epon_holdover_shift_deadlines_locked(priv);
	priv->holdover_tx_enabled = false;
	priv->phy_los = false;
	priv->phy_ready = true;
	priv->phy_recovering = false;
	priv->phy_fault = false;
	priv->phy_recovery_attempts = 0;
	priv->holdover_blocked = false;
	priv->holdover_recoveries++;
	return restore_tx;
}

static void en7581_epon_holdover_work(struct work_struct *work)
{
	struct en7581_epon *priv = container_of(
		to_delayed_work(work), struct en7581_epon, holdover_work);
	enum airoha_epon_holdover_action action;
	bool restore_tx = false, schedule_recovery = false;
	bool sfp_los = false, forced = false;
	int ret = 0;

	mutex_lock(&priv->lock);
	if (!priv->holdover.active) {
		mutex_unlock(&priv->lock);
		return;
	}
	if (priv->removing || !priv->hardware_selected || !priv->enabled ||
	    !priv->mac_initialized || !en7581_epon_active(priv)) {
		if (airoha_epon_holdover_cancel(&priv->holdover))
			priv->holdover_cancellations++;
		priv->holdover_tx_enabled = false;
		priv->holdover_blocked = false;
		mutex_unlock(&priv->lock);
		return;
	}
	forced = !priv->holdover.enabled;
	action = airoha_epon_holdover_timeout(&priv->holdover,
					      en7581_epon_phy_live_ready(priv));
	if (action == AIROHA_EPON_HOLDOVER_RESTORE) {
		restore_tx = en7581_epon_holdover_restore_locked(priv);
	} else if (action == AIROHA_EPON_HOLDOVER_CLEAR) {
		if (forced)
			priv->holdover_cancellations++;
		else
			priv->holdover_expirations++;
		priv->holdover_tx_enabled = false;
		priv->phy_ready = false;
		priv->phy_recovering = false;
		priv->phy_fault = false;
		priv->phy_recovery_attempts = 0;
		priv->holdover_blocked = !forced;
		sfp_los = readl(priv->xepon_pcs + EN7581_XEPON_PCS_SFP_STATUS) &
			  EN7581_XEPON_PCS_SFP_RX_LOS;
		priv->phy_los = sfp_los;
		if (en7581_epon_clear_sessions_locked(priv))
			priv->error_events++;
		en7581_epon_phy_set_rx(priv, false);
		schedule_recovery = !sfp_los;
	}
	if (restore_tx)
		ret = airoha_en7572_set_tx_enabled(priv->bosa, true);
	else if (action == AIROHA_EPON_HOLDOVER_CLEAR)
		ret = airoha_en7572_set_tx_enabled(priv->bosa, false);
	if (ret) {
		priv->phy_fault = true;
		priv->phy_recovery_failures++;
	}
	mutex_unlock(&priv->lock);

	if (!ret && action == AIROHA_EPON_HOLDOVER_CLEAR) {
		ret = airoha_pcs_xpon_quiesce(priv->pcs);
		if (ret) {
			mutex_lock(&priv->lock);
			priv->phy_fault = true;
			priv->phy_recovery_failures++;
			mutex_unlock(&priv->lock);
		}
	}
	if (ret) {
		dev_err_ratelimited(priv->dev,
			"failed to complete 10G-EPON holdover: %d\n", ret);
	} else if (schedule_recovery) {
		mutex_lock(&priv->lock);
		en7581_epon_phy_schedule_recovery_locked(priv,
			msecs_to_jiffies(EN7581_EPON_PHY_RECOVERY_DELAY_MS));
		mutex_unlock(&priv->lock);
	}
	kobject_uevent(&priv->dev->kobj, KOBJ_CHANGE);
}

static void en7581_epon_phy_recovery_work(struct work_struct *work)
{
	struct en7581_epon *priv = container_of(
		to_delayed_work(work), struct en7581_epon, phy_recovery_work);
	enum airoha_pcs_xpon_mode mode;
	unsigned long retry_delay;
	bool retry = false, start_holdover = false;
	int ret;

	mutex_lock(&priv->lock);
	if (priv->removing || !priv->hardware_selected || !priv->enabled ||
	    !priv->pcs || !priv->xepon_pcs) {
		priv->phy_recovering = false;
		mutex_unlock(&priv->lock);
		return;
	}
	if (priv->holdover.active) {
		priv->phy_recovering = false;
		en7581_epon_phy_set_rx(priv, true);
		mutex_unlock(&priv->lock);
		return;
	}
	if (en7581_epon_phy_live_ready(priv)) {
		priv->phy_los = false;
		priv->phy_ready = true;
		priv->phy_recovering = false;
		priv->phy_fault = false;
		priv->phy_recovery_attempts = 0;
		priv->holdover_blocked = false;
		mutex_unlock(&priv->lock);
		return;
	}
	if (priv->holdover.enabled && !priv->holdover_blocked) {
		start_holdover = en7581_epon_holdover_start_locked(priv);
		mutex_unlock(&priv->lock);
		if (start_holdover) {
			airoha_en7572_set_tx_enabled(priv->bosa, false);
			kobject_uevent(&priv->dev->kobj, KOBJ_CHANGE);
		}
		return;
	}
	if (readl(priv->xepon_pcs + EN7581_XEPON_PCS_SFP_STATUS) &
	    EN7581_XEPON_PCS_SFP_RX_LOS) {
		priv->phy_los = true;
		priv->phy_ready = false;
		priv->phy_recovering = false;
		priv->phy_recovery_attempts = 0;
		en7581_epon_phy_set_rx(priv, false);
		mutex_unlock(&priv->lock);
		airoha_en7572_set_tx_enabled(priv->bosa, false);
		return;
	}
	ret = en7581_epon_pcs_mode(priv, &mode);
	if (ret) {
		priv->phy_recovering = false;
		mutex_unlock(&priv->lock);
		return;
	}

	priv->phy_ready = false;
	priv->phy_los = false;
	priv->phy_recovering = true;
	priv->phy_recovery_attempts++;
	if (en7581_epon_clear_sessions_locked(priv))
		priv->error_events++;
	en7581_epon_phy_set_rx(priv, false);
	mutex_unlock(&priv->lock);

	/* The SDK resets the PMA when NOT_LASER_RX_LOSS arrives.  Rebuild the
	 * complete same-mode PMA transaction so asymmetric and symmetric EPON
	 * restore their distinct upstream rates before releasing the datapath. */
	ret = airoha_en7572_set_tx_enabled(priv->bosa, false);
	if (!ret) {
		en7581_epon_phy_set_logic_reset(priv, true);
		msleep(1);
		ret = airoha_pcs_xpon_quiesce(priv->pcs);
	}
	if (!ret)
		ret = airoha_pcs_xpon_select_wan(priv->pcs, mode);
	if (!ret)
		ret = airoha_pcs_xpon_set_mode(priv->pcs, mode);
	if (!ret)
		ret = airoha_pcs_xpon_recover(priv->pcs);
	if (!ret) {
		msleep(1);
		en7581_epon_phy_set_logic_reset(priv, false);
		msleep(1);
		en7581_epon_phy_set_rx(priv, true);
		msleep(EN7581_EPON_PHY_SETTLE_MS);
	}

	mutex_lock(&priv->lock);
	if (priv->removing || !priv->hardware_selected || !priv->enabled) {
		priv->phy_recovering = false;
		mutex_unlock(&priv->lock);
		return;
	}
	if (ret) {
		priv->phy_fault = true;
		priv->phy_recovery_failures++;
		retry = true;
	} else if (readl(priv->xepon_pcs + EN7581_XEPON_PCS_SFP_STATUS) &
		   EN7581_XEPON_PCS_SFP_RX_LOS) {
		priv->phy_los = true;
		priv->phy_ready = false;
		priv->phy_recovering = false;
		priv->phy_recovery_attempts = 0;
		en7581_epon_phy_set_rx(priv, false);
	} else if (en7581_epon_phy_live_ready(priv)) {
		priv->phy_los = false;
		priv->phy_ready = true;
		priv->phy_recovering = false;
		priv->phy_fault = false;
		priv->phy_recovery_attempts = 0;
		priv->holdover_blocked = false;
		priv->phy_recoveries++;
	} else {
		priv->phy_fake_sync_events++;
		retry = true;
	}
	if (retry) {
		retry_delay = msecs_to_jiffies(
			priv->phy_recovery_attempts <
			EN7581_EPON_PHY_FAST_RECOVERY_ATTEMPTS ?
			EN7581_EPON_PHY_RETRY_DELAY_MS :
			EN7581_EPON_PHY_POLL_DELAY_MS);
		if (priv->phy_recovery_attempts >=
		    EN7581_EPON_PHY_FAST_RECOVERY_ATTEMPTS)
			priv->phy_recovery_attempts = 0;
		mod_delayed_work(system_wq, &priv->phy_recovery_work,
				 retry_delay);
	}
	mutex_unlock(&priv->lock);

	if (ret)
		dev_err_ratelimited(priv->dev,
			"failed to recover 10G-EPON PMA/PCS: %d\n", ret);
}

static irqreturn_t en7581_epon_phy_irq(int irq, void *data)
{
	struct en7581_epon *priv = data;
	u32 status;

	if (!en7581_epon_active(priv) || !READ_ONCE(priv->enabled) ||
	    READ_ONCE(priv->irqs_masked))
		return IRQ_NONE;
	status = readl(priv->xepon_pcs + EN7581_XEPON_PCS_INT_STATUS) &
		 readl(priv->xepon_pcs + EN7581_XEPON_PCS_INT_ENABLE) &
		 EN7581_XEPON_PCS_RX_EVENTS;
	return status ? IRQ_WAKE_THREAD : IRQ_NONE;
}

static irqreturn_t en7581_epon_phy_irq_thread(int irq, void *data)
{
	struct en7581_epon *priv = data;
	enum airoha_epon_holdover_action holdover_action;
	bool cancel_holdover = false, disable_tx = false, restore_tx = false;
	bool quiesce = false, schedule_recovery = false, start_holdover = false;
	bool live_ready, loss_event, sfp_los;
	u32 status;
	int ret = 0;

	if (READ_ONCE(priv->irqs_masked) || !en7581_epon_active(priv))
		return IRQ_HANDLED;
	status = readl(priv->xepon_pcs + EN7581_XEPON_PCS_INT_STATUS) &
		 readl(priv->xepon_pcs + EN7581_XEPON_PCS_INT_ENABLE) &
		 EN7581_XEPON_PCS_RX_EVENTS;
	if (!status)
		return IRQ_HANDLED;
	/* XEPON INT_STATUS is write-one-to-clear in the vendor ISR. */
	writel(status, priv->xepon_pcs + EN7581_XEPON_PCS_INT_STATUS);
	mutex_lock(&priv->lock);
	if (priv->removing || !priv->enabled || !priv->hardware_selected ||
	    !priv->mac_initialized || !en7581_epon_active(priv)) {
		mutex_unlock(&priv->lock);
		return IRQ_HANDLED;
	}
	priv->last_phy_irq_status = status;
	priv->phy_irq_events++;
	if (status & EN7581_XEPON_PCS_INT_SYNC_OK)
		priv->phy_sync_events++;
	if (status & EN7581_XEPON_PCS_INT_SYNC_LOSS)
		priv->phy_sync_loss_events++;
	if (status & EN7581_XEPON_PCS_INT_LASER_RX_LOSS)
		priv->phy_los_events++;
	if (status & EN7581_XEPON_PCS_INT_NOT_LASER_RX_LOSS)
		priv->phy_no_los_events++;

	sfp_los = readl(priv->xepon_pcs + EN7581_XEPON_PCS_SFP_STATUS) &
		   EN7581_XEPON_PCS_SFP_RX_LOS;
	live_ready = en7581_epon_phy_live_ready(priv);
	loss_event = sfp_los ||
		(!live_ready &&
		 (status & (EN7581_XEPON_PCS_INT_SYNC_LOSS |
			    EN7581_XEPON_PCS_INT_LASER_RX_LOSS)));

	if (priv->holdover.active) {
		if (live_ready) {
			holdover_action =
				airoha_epon_holdover_ready(&priv->holdover);
			if (holdover_action == AIROHA_EPON_HOLDOVER_RESTORE) {
				restore_tx =
					en7581_epon_holdover_restore_locked(priv);
				cancel_holdover = true;
			}
		} else {
			priv->phy_los = sfp_los;
			priv->phy_ready = false;
			priv->phy_recovering = false;
			priv->phy_fault = false;
			priv->phy_recovery_attempts = 0;
			disable_tx = true;
		}
	} else if (loss_event && priv->holdover.enabled &&
		   !priv->holdover_blocked) {
		start_holdover = en7581_epon_holdover_start_locked(priv);
		disable_tx = start_holdover;
	} else if (sfp_los) {
		priv->phy_los = true;
		priv->phy_ready = false;
		priv->phy_recovering = false;
		priv->phy_fault = false;
		priv->phy_recovery_attempts = 0;
		if (en7581_epon_clear_sessions_locked(priv))
			priv->error_events++;
		en7581_epon_phy_set_rx(priv, false);
		quiesce = true;
	} else if (status & (EN7581_XEPON_PCS_INT_NOT_LASER_RX_LOSS |
			     EN7581_XEPON_PCS_INT_LASER_RX_LOSS)) {
		priv->phy_los = false;
		priv->phy_ready = false;
		if (en7581_epon_clear_sessions_locked(priv))
			priv->error_events++;
		en7581_epon_phy_set_rx(priv, false);
		quiesce = true;
		schedule_recovery = true;
	} else if ((status & EN7581_XEPON_PCS_INT_SYNC_LOSS) &&
		   !live_ready) {
		priv->phy_los = false;
		priv->phy_ready = false;
		if (en7581_epon_clear_sessions_locked(priv))
			priv->error_events++;
		en7581_epon_phy_set_rx(priv, false);
		quiesce = true;
		schedule_recovery = true;
	} else if (status & EN7581_XEPON_PCS_INT_SYNC_OK) {
		priv->phy_los = false;
		priv->phy_ready = live_ready;
		if (live_ready) {
			priv->phy_recovering = false;
			priv->phy_fault = false;
			priv->phy_recovery_attempts = 0;
			priv->holdover_blocked = false;
		} else {
			priv->phy_fake_sync_events++;
			schedule_recovery = true;
		}
	}
	if (disable_tx)
		ret = airoha_en7572_set_tx_enabled(priv->bosa, false);
	else if (restore_tx)
		ret = airoha_en7572_set_tx_enabled(priv->bosa, true);
	if (ret) {
		priv->phy_fault = true;
		priv->phy_recovery_failures++;
	}
	mutex_unlock(&priv->lock);

	if (start_holdover)
		cancel_delayed_work_sync(&priv->phy_recovery_work);
	if (cancel_holdover)
		cancel_delayed_work_sync(&priv->holdover_work);
	if (ret) {
		dev_err_ratelimited(priv->dev,
			"failed to update optical TX for 10G-EPON holdover: %d\n",
			ret);
		return IRQ_HANDLED;
	}
	if (start_holdover || cancel_holdover)
		kobject_uevent(&priv->dev->kobj, KOBJ_CHANGE);
	if (sfp_los)
		cancel_delayed_work(&priv->phy_recovery_work);
	if (quiesce || schedule_recovery) {
		ret = airoha_en7572_set_tx_enabled(priv->bosa, false);
		if (!ret)
			ret = airoha_pcs_xpon_quiesce(priv->pcs);
		if (ret) {
			mutex_lock(&priv->lock);
			priv->phy_fault = true;
			priv->phy_recovering = false;
			priv->phy_recovery_failures++;
			mutex_unlock(&priv->lock);
			dev_err_ratelimited(priv->dev,
				"failed to quiesce 10G-EPON PMA: %d\n", ret);
		}
	}
	if (schedule_recovery && !ret) {
		mutex_lock(&priv->lock);
		en7581_epon_phy_schedule_recovery_locked(priv,
			msecs_to_jiffies(EN7581_EPON_PHY_RECOVERY_DELAY_MS));
		mutex_unlock(&priv->lock);
	}
	return IRQ_HANDLED;
}

static int en7581_epon_hw_initialize(struct en7581_epon *priv)
{
	u32 value;
	u8 index;
	int ret;

	if (!airoha_epon_mode_profile(priv->active_mode) || !priv->onu_mac_set)
		return -EINVAL;
	en7581_epon_phy_set_rx(priv, false);
	en7581_epon_phy_set_logic_reset(priv, true);
	udelay(1);
	priv->local_deregister_pending = 0;
	priv->local_deregister_inflight = EN7581_EPON_NO_LLID;
	priv->sync_time = AIROHA_EPON_MPCP_SYNC_TIME_DEFAULT;

	/* SDK pulses the shared reset, then holds EPON logic reset through init. */
	ret = airoha_epon_logic_reset_pulse(
		&en7581_epon_scu_pon_mac_reset_ops, priv);
	if (ret)
		return ret;
	ret = airoha_epon_logic_reset_set(
		&en7581_epon_scu_logic_reset_ops, priv, true);
	if (ret)
		return ret;
	value = en7581_epon_read(priv, EN7581_EPON_GLB_CFG2) |
		EN7581_EPON_ETH_COUNT_BYTES;
	en7581_epon_write(priv, EN7581_EPON_GLB_CFG2, value);
	if (!(en7581_epon_read(priv, EN7581_EPON_GLB_CFG2) &
	      EN7581_EPON_ETH_COUNT_BYTES)) {
		ret = -EIO;
		goto fail_closed;
	}

	en7581_epon_write(priv, EN7581_EPON_INT_ENABLE, 0);
	en7581_epon_write(priv, EN7581_EPON_INT_ENABLE2, 0);
	en7581_epon_write(priv, EN7581_EPON_INT_ENABLE3, 0);
	en7581_epon_write(priv, EN7581_EPON_INT_STATUS, U32_MAX);
	en7581_epon_write(priv, EN7581_EPON_INT_STATUS2, U32_MAX);
	en7581_epon_write(priv, EN7581_EPON_INT_STATUS3, U32_MAX);
	en7581_epon_write(priv, EN7581_EPON_DS_KEY_CHANGE, U32_MAX);
	en7581_epon_write(priv, EN7581_EPON_DS_KEY_CHANGE2, U32_MAX);
	en7581_epon_write(priv, EN7581_EPON_DS_KEY_MISS, U32_MAX);
	en7581_epon_write(priv, EN7581_EPON_DS_KEY_MISS2, U32_MAX);
	en7581_epon_write(priv, EN7581_EPON_US_KEY_CHANGE, U32_MAX);
	ret = airoha_epon_mac_init_transaction(
		&en7581_epon_mac_init_ops, priv, priv->active_mode);
	if (ret)
		goto fail_closed;
	ret = en7581_epon_clear_report_thresholds(priv);
	if (ret)
		goto fail_closed;

	for (index = 0; index < AIROHA_EPON_LLID_COUNT; index++) {
		if (!(priv->llid_mask & BIT(index))) {
			en7581_epon_set_hw_state(priv, index,
				AIROHA_EPON_DISCOVERY_STATE_UNREGISTERED);
			continue;
		}
		en7581_epon_derive_llid_mac(priv, index);
		ret = en7581_epon_program_llid_mac(priv, index);
		if (ret)
			goto fail_closed;
		en7581_epon_reset_llid(priv, index);
		en7581_epon_set_llid_tx_fec(priv, index,
			priv->active_mode == AIROHA_XPON_MODE_EPON_10G_10G);
	}
	value = en7581_epon_read(priv, EN7581_EPON_GLB_CFG);
	en7581_epon_write(priv, EN7581_EPON_GLB_CFG,
			value | EN7581_EPON_PATH_STOP);
	/* SDK epon_phy_ready_hw_init() releases logic reset only after init. */
	ret = airoha_epon_logic_reset_set(
		&en7581_epon_scu_logic_reset_ops, priv, false);
	if (ret)
		return ret;
	en7581_epon_set_tx_burst_mode(priv, true);
	ret = airoha_epon_dying_gasp_set(
		&en7581_epon_dying_gasp_ops, priv, true);
	if (ret)
		return ret;
	/* Both 10G/1G and 10G/10G receive through the 10G XEPON PCS. */
	en7581_epon_phy_set_logic_reset(priv, false);
	udelay(1);
	en7581_epon_phy_set_rx(priv, true);
	priv->phy_los = readl(priv->xepon_pcs +
		EN7581_XEPON_PCS_SFP_STATUS) & EN7581_XEPON_PCS_SFP_RX_LOS;
	priv->phy_ready = en7581_epon_phy_live_ready(priv);
	priv->phy_recovering = false;
	priv->phy_fault = false;
	priv->phy_recovery_attempts = 0;
	priv->holdover.active = false;
	priv->holdover_tx_enabled = false;
	priv->holdover_blocked = false;
	memset(priv->key_suite, 0, sizeof(priv->key_suite));
	memset(priv->key_index, 0, sizeof(priv->key_index));
	priv->mac_initialized = true;
	en7581_epon_pm_reset_session_locked(priv);
	return 0;

fail_closed:
	en7581_epon_mac_init_fail_closed(priv);
	return ret;
}

static void en7581_epon_cancel_holdover_sync(struct en7581_epon *priv)
{
	cancel_delayed_work_sync(&priv->holdover_work);
	mutex_lock(&priv->lock);
	if (airoha_epon_holdover_cancel(&priv->holdover))
		priv->holdover_cancellations++;
	priv->holdover_tx_enabled = false;
	priv->holdover_blocked = false;
	mutex_unlock(&priv->lock);
}

static int en7581_epon_stop_path(struct en7581_epon *priv)
{
	u32 value;

	value = en7581_epon_read(priv, EN7581_EPON_GLB_CFG);
	en7581_epon_write(priv, EN7581_EPON_GLB_CFG,
		value | EN7581_EPON_PATH_STOP);
	return readl_poll_timeout(priv->mac + EN7581_EPON_GLB_STS1, value,
		(value & EN7581_EPON_PATH_STOPPED) == EN7581_EPON_PATH_STOPPED,
		1, 1000);
}

static int en7581_epon_block_traffic(void *context)
{
	struct en7581_epon *priv = context;

	mutex_lock(&priv->lock);
	priv->switch_resume_enabled = priv->enabled;
	priv->enabled = false;
	mutex_unlock(&priv->lock);
	en7581_epon_cancel_holdover_sync(priv);
	return en7581_epon_stop_path(priv);
}

static int en7581_epon_clear_session(void *context)
{
	struct en7581_epon *priv = context;
	int ret;

	en7581_epon_cancel_holdover_sync(priv);
	mutex_lock(&priv->lock);
	ret = en7581_epon_clear_sessions_locked(priv);
	mutex_unlock(&priv->lock);
	en7581_epon_purge_oam_rx(priv);
	return ret;
}

static int en7581_epon_mask_irqs(void *context)
{
	struct en7581_epon *priv = context;

	WRITE_ONCE(priv->irqs_masked, true);
	en7581_epon_write(priv, EN7581_EPON_INT_ENABLE, 0);
	en7581_epon_write(priv, EN7581_EPON_INT_ENABLE2, 0);
	en7581_epon_write(priv, EN7581_EPON_INT_ENABLE3, 0);
	writel(0, priv->xepon_pcs + EN7581_XEPON_PCS_INT_ENABLE);
	if (en7581_epon_read(priv, EN7581_EPON_INT_ENABLE) ||
	    en7581_epon_read(priv, EN7581_EPON_INT_ENABLE2) ||
	    en7581_epon_read(priv, EN7581_EPON_INT_ENABLE3) ||
	    readl(priv->xepon_pcs + EN7581_XEPON_PCS_INT_ENABLE)) {
		en7581_epon_write(priv, EN7581_EPON_INT_ENABLE, 0);
		en7581_epon_write(priv, EN7581_EPON_INT_ENABLE2, 0);
		en7581_epon_write(priv, EN7581_EPON_INT_ENABLE3, 0);
		writel(0, priv->xepon_pcs + EN7581_XEPON_PCS_INT_ENABLE);
		dev_err(priv->dev, "failed to verify EPON interrupt mask\n");
		return -EIO;
	}
	return 0;
}

static void en7581_epon_synchronize_irqs(void *context)
{
	struct en7581_epon *priv = context;

	synchronize_irq(priv->mac_irq);
	synchronize_irq(priv->phy_irq);
}

static int en7581_epon_stop_datapath(void *context)
{
	struct en7581_epon *priv = context;

	en7581_epon_cancel_holdover_sync(priv);
	return en7581_epon_stop_path(priv);
}

static int en7581_epon_stop_clear_keys(void *context)
{
	struct en7581_epon *priv = context;

	if (!priv->mac_initialized)
		return 0;
	return en7581_epon_clear_all_keys_locked(priv);
}

static int en7581_epon_stop_clear_report_thresholds(void *context)
{
	return en7581_epon_clear_report_thresholds(context);
}

static int en7581_epon_stop_clear_qdma(void *context)
{
	struct en7581_epon *priv = context;
	u32 retire_mask = priv->datapath_llid_mask;
	int ret;

	ret = airoha_epon_qdma_llids_clear(priv->ethernet_np);
	if (!ret)
		ret = airoha_xpon_qdma_channels_retire(priv->ethernet_np,
						       retire_mask);
	if (!ret) {
		priv->datapath_llid_mask = 0;
		priv->datapath_report_fec_mask = 0;
		priv->loopback_mask = 0;
	}
	return ret;
}

static const struct airoha_epon_stop_ops en7581_epon_stop_ops = {
	.clear_keys = en7581_epon_stop_clear_keys,
	.clear_report_thresholds = en7581_epon_stop_clear_report_thresholds,
	.clear_qdma = en7581_epon_stop_clear_qdma,
};

static int en7581_epon_stop_mac(void *context)
{
	struct en7581_epon *priv = context;
	int ret;

	cancel_delayed_work_sync(&priv->mpcp_work);
	cancel_delayed_work_sync(&priv->phy_recovery_work);
	cancel_delayed_work_sync(&priv->pm_work);
	en7581_epon_cancel_holdover_sync(priv);
	en7581_epon_phy_set_rx(priv, false);
	en7581_epon_phy_set_logic_reset(priv, true);
	mutex_lock(&priv->lock);
	ret = airoha_epon_stop_cleanup(&en7581_epon_stop_ops, priv);
	priv->mac_initialized = false;
	priv->hardware_selected = false;
	priv->phy_ready = false;
	priv->phy_los = false;
	priv->phy_recovering = false;
	priv->phy_fault = false;
	priv->phy_recovery_attempts = 0;
	priv->holdover_tx_enabled = false;
	priv->holdover_blocked = false;
	priv->active_mode = AIROHA_XPON_MODE_INVALID;
	priv->local_deregister_pending = 0;
	priv->local_deregister_inflight = EN7581_EPON_NO_LLID;
	mutex_unlock(&priv->lock);
	en7581_epon_detach_oam(priv);
	return ret;
}

static int en7581_epon_start_mac(void *context)
{
	struct en7581_epon *priv = context;
	enum airoha_xpon_mode mode = airoha_en7572_get_mode(priv->bosa);
	int ret;

	if (!priv->onu_mac_set)
		return -EAGAIN;
	if (!airoha_epon_mode_profile(mode) ||
	    !airoha_en7572_is_ready(priv->bosa) ||
	    !airoha_en7572_tx_is_disabled(priv->bosa))
		return -EIO;
	ret = en7581_epon_attach_oam(priv);
	if (ret)
		return ret;
	mutex_lock(&priv->lock);
	ret = airoha_epon_qdma_llids_clear(priv->ethernet_np);
	if (ret)
		goto out;
	priv->datapath_llid_mask = 0;
	priv->datapath_report_fec_mask = 0;
	priv->loopback_mask = 0;
	priv->active_mode = mode;
	priv->hardware_selected = true;
	ret = en7581_epon_hw_initialize(priv);
	if (ret) {
		priv->hardware_selected = false;
		priv->active_mode = AIROHA_XPON_MODE_INVALID;
	}
out:
	mutex_unlock(&priv->lock);
	if (ret) {
		en7581_epon_detach_oam(priv);
	} else {
		mod_delayed_work(system_wq, &priv->pm_work,
			msecs_to_jiffies(EN7581_EPON_PM_SAMPLE_MS));
	}
	return ret;
}

static int en7581_epon_start_datapath(void *context)
{
	struct en7581_epon *priv = context;
	u32 value;

	if (!priv->mac_initialized)
		return -EIO;
	value = en7581_epon_read(priv, EN7581_EPON_GLB_CFG);
	en7581_epon_write(priv, EN7581_EPON_GLB_CFG,
			value & ~EN7581_EPON_PATH_STOP);
	return en7581_epon_read(priv, EN7581_EPON_GLB_CFG) &
	       EN7581_EPON_PATH_STOP ? -EIO : 0;
}

static int en7581_epon_unmask_irqs(void *context)
{
	struct en7581_epon *priv = context;
	u32 low_mask = priv->llid_mask & 0xff;
	u32 high_mask = priv->llid_mask >> 8;
	u32 primary_mask = EN7581_EPON_INT_COMMON | (low_mask << 1);

	if (!priv->mac_initialized || airoha_en7572_fault_locked(priv->bosa))
		return -EIO;
	if (!READ_ONCE(priv->enabled))
		return 0;
	en7581_epon_write(priv, EN7581_EPON_INT_STATUS, U32_MAX);
	en7581_epon_write(priv, EN7581_EPON_INT_STATUS2, U32_MAX);
	en7581_epon_write(priv, EN7581_EPON_INT_STATUS3, U32_MAX);
	writel(U32_MAX, priv->xepon_pcs + EN7581_XEPON_PCS_INT_STATUS);
	en7581_epon_write(priv, EN7581_EPON_INT_ENABLE, primary_mask);
	en7581_epon_write(priv, EN7581_EPON_INT_ENABLE2,
		EN7581_EPON_INT2_ERRORS);
	en7581_epon_write(priv, EN7581_EPON_INT_ENABLE3, high_mask);
	writel(EN7581_XEPON_PCS_RX_EVENTS,
	       priv->xepon_pcs + EN7581_XEPON_PCS_INT_ENABLE);
	if (en7581_epon_read(priv, EN7581_EPON_INT_ENABLE) != primary_mask ||
	    en7581_epon_read(priv, EN7581_EPON_INT_ENABLE2) !=
			EN7581_EPON_INT2_ERRORS ||
	    en7581_epon_read(priv, EN7581_EPON_INT_ENABLE3) != high_mask ||
	    readl(priv->xepon_pcs + EN7581_XEPON_PCS_INT_ENABLE) !=
			EN7581_XEPON_PCS_RX_EVENTS) {
		en7581_epon_write(priv, EN7581_EPON_INT_ENABLE, 0);
		en7581_epon_write(priv, EN7581_EPON_INT_ENABLE2, 0);
		en7581_epon_write(priv, EN7581_EPON_INT_ENABLE3, 0);
		writel(0, priv->xepon_pcs + EN7581_XEPON_PCS_INT_ENABLE);
		WRITE_ONCE(priv->irqs_masked, true);
		dev_err(priv->dev, "failed to verify EPON interrupt enable\n");
		return -EIO;
	}
	WRITE_ONCE(priv->irqs_masked, false);
	return 0;
}

static int en7581_epon_mode_committed(void *context)
{
	struct en7581_epon *priv = context;
	bool enabled;
	int ret = 0;

	mutex_lock(&priv->lock);
	if (priv->switch_resume_enabled)
		priv->enabled = true;
	enabled = priv->enabled;
	priv->switch_resume_enabled = false;
	mutex_unlock(&priv->lock);
	if (enabled) {
		ret = en7581_epon_unmask_irqs(priv);
		if (ret) {
			mutex_lock(&priv->lock);
			priv->enabled = false;
			mutex_unlock(&priv->lock);
			return ret;
		}
		mod_delayed_work(system_wq, &priv->mpcp_work, 0);
		mutex_lock(&priv->lock);
		if (!priv->phy_ready && !priv->phy_los)
			en7581_epon_phy_schedule_recovery_locked(priv,
				msecs_to_jiffies(
					EN7581_EPON_PHY_POLL_DELAY_MS));
		mutex_unlock(&priv->lock);
	}
	return 0;
}

static bool en7581_epon_activation_enabled(void *context)
{
	struct en7581_epon *priv = context;

	return READ_ONCE(priv->enabled);
}

static const struct airoha_xpon_backend_ops en7581_epon_ops = {
	.block_traffic = en7581_epon_block_traffic,
	.clear_session = en7581_epon_clear_session,
	.mask_irqs = en7581_epon_mask_irqs,
	.synchronize_irqs = en7581_epon_synchronize_irqs,
	.stop_datapath = en7581_epon_stop_datapath,
	.stop_mac = en7581_epon_stop_mac,
	.start_mac = en7581_epon_start_mac,
	.start_datapath = en7581_epon_start_datapath,
	.unmask_irqs = en7581_epon_unmask_irqs,
	.mode_committed = en7581_epon_mode_committed,
	.activation_enabled = en7581_epon_activation_enabled,
};

static ssize_t enabled_show(struct device *dev,
			    struct device_attribute *attribute, char *buf)
{
	struct en7581_epon *priv = dev_get_drvdata(dev);

	return sysfs_emit(buf, "%u\n", READ_ONCE(priv->enabled));
}

static ssize_t enabled_store(struct device *dev,
			     struct device_attribute *attribute,
			     const char *buf, size_t count)
{
	struct en7581_epon *priv = dev_get_drvdata(dev);
	struct airoha_xpon_backend *backend;
	bool active, enabled;
	int ret = 0;

	ret = kstrtobool(buf, &enabled);
	if (ret)
		return ret;
	backend = en7581_epon_active_backend(priv);
	ret = airoha_xpon_backend_activation_lock(backend);
	if (ret)
		return ret;
	mutex_lock(&priv->lock);
	active = en7581_epon_active(priv) && priv->mac_initialized;
	if (enabled && active &&
	    (!airoha_en7572_is_ready(priv->bosa) ||
	     !airoha_en7572_tx_is_disabled(priv->bosa) ||
	     airoha_en7572_fault_locked(priv->bosa))) {
		ret = -EIO;
	} else {
		priv->enabled = enabled;
		if (!active)
			priv->switch_resume_enabled = false;
	}
	mutex_unlock(&priv->lock);
	if (ret || !active)
		goto out;

	if (enabled) {
		ret = en7581_epon_unmask_irqs(priv);
		if (!ret) {
			mod_delayed_work(system_wq, &priv->mpcp_work, 0);
			mutex_lock(&priv->lock);
			if (!priv->phy_ready && !priv->phy_los)
				en7581_epon_phy_schedule_recovery_locked(priv,
					msecs_to_jiffies(
						EN7581_EPON_PHY_POLL_DELAY_MS));
			mutex_unlock(&priv->lock);
		}
		if (ret) {
			mutex_lock(&priv->lock);
			priv->enabled = false;
			mutex_unlock(&priv->lock);
		}
		goto out;
	}

	en7581_epon_mask_irqs(priv);
	cancel_delayed_work_sync(&priv->mpcp_work);
	cancel_delayed_work_sync(&priv->phy_recovery_work);
	en7581_epon_cancel_holdover_sync(priv);
	en7581_epon_synchronize_irqs(priv);
	mutex_lock(&priv->lock);
	ret = en7581_epon_clear_sessions_locked(priv);
	mutex_unlock(&priv->lock);
	airoha_en7572_set_tx_enabled(priv->bosa, false);

out:
	airoha_xpon_backend_activation_unlock(backend);
	return ret ? ret : count;
}
static DEVICE_ATTR_RW(enabled);

static ssize_t onu_mac_show(struct device *dev,
			    struct device_attribute *attribute, char *buf)
{
	struct en7581_epon *priv = dev_get_drvdata(dev);

	return priv->onu_mac_set ? sysfs_emit(buf, "%pM\n", priv->onu_mac) :
		sysfs_emit(buf, "unset\n");
}

static ssize_t onu_mac_store(struct device *dev,
			     struct device_attribute *attribute,
			     const char *buf, size_t count)
{
	struct en7581_epon *priv = dev_get_drvdata(dev);
	u8 address[ETH_ALEN];
	int ret;

	if (!mac_pton(buf, address) || !is_valid_ether_addr(address))
		return -EINVAL;
	mutex_lock(&priv->lock);
	if (en7581_epon_active(priv) && priv->mac_initialized) {
		mutex_unlock(&priv->lock);
		return -EBUSY;
	}
	ether_addr_copy(priv->onu_mac, address);
	priv->onu_mac_set = true;
	mutex_unlock(&priv->lock);
	ret = airoha_xpon_backend_ready(priv->asymmetric_backend);
	if (!ret)
		ret = airoha_xpon_backend_ready(priv->symmetric_backend);
	if (ret)
		return ret;
	return count;
}
static DEVICE_ATTR_RW(onu_mac);

static ssize_t llid_mask_show(struct device *dev,
			      struct device_attribute *attribute, char *buf)
{
	struct en7581_epon *priv = dev_get_drvdata(dev);

	return sysfs_emit(buf, "0x%08x\n", priv->llid_mask);
}

static ssize_t llid_mask_store(struct device *dev,
			       struct device_attribute *attribute,
			       const char *buf, size_t count)
{
	struct en7581_epon *priv = dev_get_drvdata(dev);
	u32 value;
	int ret;

	ret = kstrtou32(buf, 0, &value);
	if (ret || !value)
		return ret ? ret : -EINVAL;
	mutex_lock(&priv->lock);
	if (en7581_epon_active(priv) && priv->mac_initialized) {
		mutex_unlock(&priv->lock);
		return -EBUSY;
	}
	priv->llid_mask = value;
	mutex_unlock(&priv->lock);
	return count;
}
static DEVICE_ATTR_RW(llid_mask);

static ssize_t silent_time_show(struct device *dev,
				struct device_attribute *attribute, char *buf)
{
	struct en7581_epon *priv = dev_get_drvdata(dev);

	return sysfs_emit(buf, "%u\n", priv->silent_time);
}

static ssize_t silent_time_store(struct device *dev,
				 struct device_attribute *attribute,
				 const char *buf, size_t count)
{
	struct en7581_epon *priv = dev_get_drvdata(dev);
	u32 value;
	int ret;

	ret = kstrtou32(buf, 0, &value);
	if (ret)
		return ret;
	if (!value || value > 3600)
		return -ERANGE;
	WRITE_ONCE(priv->silent_time, value);
	return count;
}
static DEVICE_ATTR_RW(silent_time);

static ssize_t holdover_show(struct device *dev,
			     struct device_attribute *attribute, char *buf)
{
	struct en7581_epon *priv = dev_get_drvdata(dev);
	unsigned int enabled, active, time_ms;

	mutex_lock(&priv->lock);
	enabled = priv->holdover.enabled;
	active = priv->holdover.active;
	time_ms = priv->holdover.time_ms;
	mutex_unlock(&priv->lock);
	return sysfs_emit(buf, "%u %u %u\n", enabled, time_ms, active);
}

static ssize_t holdover_store(struct device *dev,
			      struct device_attribute *attribute,
			      const char *buf, size_t count)
{
	struct en7581_epon *priv = dev_get_drvdata(dev);
	unsigned int enabled, time_ms;
	char extra;
	int ret;

	if (sscanf(buf, "%u %u %c", &enabled, &time_ms, &extra) != 2 ||
	    enabled > 1)
		return -EINVAL;
	ret = en7581_epon_holdover_configure(priv, enabled, time_ms);
	return ret ? ret : count;
}
static DEVICE_ATTR_RW(holdover);

static ssize_t deregister_store(struct device *dev,
				struct device_attribute *attribute,
				const char *buf, size_t count)
{
	struct en7581_epon *priv = dev_get_drvdata(dev);
	u8 index;
	int ret;

	ret = kstrtou8(buf, 0, &index);
	if (ret)
		return ret;
	if (index >= AIROHA_EPON_LLID_COUNT)
		return -ERANGE;
	mutex_lock(&priv->lock);
	if (!en7581_epon_active(priv) || !priv->mac_initialized)
		ret = -EHOSTDOWN;
	else
		ret = en7581_epon_local_deregister_locked(priv, index);
	mutex_unlock(&priv->lock);
	return ret ? ret : count;
}
static DEVICE_ATTR_WO(deregister);

static ssize_t mpcp_state_show(struct device *dev,
			       struct device_attribute *attribute, char *buf)
{
	struct en7581_epon *priv = dev_get_drvdata(dev);
	ssize_t length = 0;
	u8 index;

	mutex_lock(&priv->lock);
	for (index = 0; index < AIROHA_EPON_LLID_COUNT; index++) {
		if (!(priv->llid_mask & BIT(index)))
			continue;
		length += sysfs_emit_at(buf, length, "%u:%u:%04x:%u ",
			index, priv->llids[index].state,
			priv->llids[index].llid, priv->llids[index].tx_fec);
	}
	length += sysfs_emit_at(buf, length, "\n");
	mutex_unlock(&priv->lock);
	return length;
}
static DEVICE_ATTR_RO(mpcp_state);

static ssize_t counter_evidence_show(struct device *dev,
				     struct device_attribute *attribute,
				     char *buf)
{
	struct en7581_epon *priv = dev_get_drvdata(dev);
	u64 rx_bytes, rx_mbi_frames, rx_mpi_frames;
	u64 tx_mbi_frames, tx_mpi_frames;
	u64 generation, sequence;
	bool counter_reset, byte_count_enabled;

	mutex_lock(&priv->lock);
	if (!priv->hardware_selected || !priv->mac_initialized) {
		mutex_unlock(&priv->lock);
		return -EOPNOTSUPP;
	}
	en7581_epon_pm_update_locked(priv);
	priv->pm_snapshot_sequence += 2;
	sequence = priv->pm_snapshot_sequence;
	generation = priv->pm_session_generation;
	counter_reset = priv->pm_counter_reset;
	byte_count_enabled = en7581_epon_read(priv, EN7581_EPON_GLB_CFG2) &
		EN7581_EPON_ETH_COUNT_BYTES;
	rx_bytes = !counter_reset && priv->pm_rx_bytes_previous >=
		priv->pm_rx_bytes_base ?
		priv->pm_rx_bytes_previous - priv->pm_rx_bytes_base : 0;
	rx_mbi_frames = en7581_epon_pm_frame_value(priv,
		EN7581_EPON_PM_RX_MBI_FRAMES);
	rx_mpi_frames = en7581_epon_pm_frame_value(priv,
		EN7581_EPON_PM_RX_MPI_FRAMES);
	tx_mbi_frames = en7581_epon_pm_frame_value(priv,
		EN7581_EPON_PM_TX_MBI_FRAMES);
	tx_mpi_frames = en7581_epon_pm_frame_value(priv,
		EN7581_EPON_PM_TX_MPI_FRAMES);
	mutex_unlock(&priv->lock);

	return sysfs_emit(buf,
		"version=1 semantics=kernel-mode-session-monotonic "
		"generation=%llu sequence=%llu sampling_interval_ms=%u "
		"sampling_assumption=single-frame-counter-wrap-per-interval "
		"counter_reset=%u eth_byte_count_enabled=%u "
		"rx_ethernet_bytes=%llu rx_mbi_ethernet_frames=%llu "
		"rx_mpi_ethernet_frames=%llu tx_mbi_ethernet_frames=%llu "
		"tx_mpi_ethernet_frames=%llu\n",
		(unsigned long long)generation,
		(unsigned long long)sequence, EN7581_EPON_PM_SAMPLE_MS,
		counter_reset, byte_count_enabled,
		(unsigned long long)rx_bytes,
		(unsigned long long)rx_mbi_frames,
		(unsigned long long)rx_mpi_frames,
		(unsigned long long)tx_mbi_frames,
		(unsigned long long)tx_mpi_frames);
}
static DEVICE_ATTR_RO(counter_evidence);

static ssize_t statistics_show(struct device *dev,
			       struct device_attribute *attribute, char *buf)
{
	struct en7581_epon *priv = dev_get_drvdata(dev);

	return sysfs_emit(buf,
		"irq=%llu registered=%llu deregistered=%llu local_deregistered=%llu denied=%llu timeouts=%llu errors=%llu oam_rx=%llu oam_rx_dropped=%llu oam_tx=%llu oam_tx_errors=%llu key_programs=%llu key_changes=%llu key_misses=%llu key_errors=%llu phy_irq=%llu phy_sync=%llu phy_sync_loss=%llu phy_los=%llu phy_no_los=%llu phy_fake_sync=%llu phy_recoveries=%llu phy_recovery_failures=%llu holdover_starts=%llu holdover_recoveries=%llu holdover_expirations=%llu holdover_cancellations=%llu holdover_suppressed_irqs=%llu phy_irq_status=%#x phy_recovery_attempt=%u phy_ready=%u phy_los_now=%u phy_recovering=%u phy_fault=%u holdover_enabled=%u holdover_time_ms=%u holdover_active=%u tx_disabled=%u\n",
		priv->irq_events, priv->registration_events,
		priv->deregistration_events,
		priv->local_deregistration_events, priv->denied_events,
		priv->timeout_events, priv->error_events, priv->oam_rx_packets,
		priv->oam_rx_dropped, priv->oam_tx_packets, priv->oam_tx_errors,
		priv->key_programs, priv->key_changes, priv->key_misses,
		priv->key_errors, priv->phy_irq_events, priv->phy_sync_events,
		priv->phy_sync_loss_events, priv->phy_los_events,
		priv->phy_no_los_events, priv->phy_fake_sync_events,
		priv->phy_recoveries, priv->phy_recovery_failures,
		priv->holdover_starts, priv->holdover_recoveries,
		priv->holdover_expirations, priv->holdover_cancellations,
		priv->holdover_suppressed_irqs,
		priv->last_phy_irq_status, priv->phy_recovery_attempts,
		priv->phy_ready, priv->phy_los, priv->phy_recovering,
		priv->phy_fault, priv->holdover.enabled,
		priv->holdover.time_ms, priv->holdover.active,
		airoha_en7572_tx_is_disabled(priv->bosa));
}
static DEVICE_ATTR_RO(statistics);

static struct attribute *en7581_epon_attrs[] = {
	&dev_attr_enabled.attr,
	&dev_attr_onu_mac.attr,
	&dev_attr_llid_mask.attr,
	&dev_attr_silent_time.attr,
	&dev_attr_holdover.attr,
	&dev_attr_deregister.attr,
	&dev_attr_mpcp_state.attr,
	&dev_attr_counter_evidence.attr,
	&dev_attr_statistics.attr,
	NULL,
};
ATTRIBUTE_GROUPS(en7581_epon);

static void en7581_epon_backend_unregister(void *data)
{
	airoha_xpon_backend_unregister(data);
}

static void en7581_epon_misc_deregister(void *data)
{
	struct en7581_epon *priv = data;

	if (!priv->oam_registered)
		return;
	misc_deregister(&priv->oam);
	priv->oam_registered = false;
}

static void en7581_epon_bosa_put(void *data)
{
	airoha_en7572_put(data);
}

static void en7581_epon_of_node_put(void *data)
{
	of_node_put(data);
}

static int en7581_epon_probe(struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;
	struct device_node *bosa_np, *controller_np;
	struct en7581_epon *priv;
	struct resource *resource;
	u32 llid_mask = 1;
	int ready_ret, ret;

	priv = devm_kzalloc(dev, sizeof(*priv), GFP_KERNEL);
	if (!priv)
		return -ENOMEM;
	priv->dev = dev;
	priv->active_mode = AIROHA_XPON_MODE_INVALID;
	priv->irqs_masked = true;
	priv->local_deregister_inflight = EN7581_EPON_NO_LLID;
	priv->silent_time = EN7581_EPON_SILENT_TIME_DEFAULT;
	mutex_init(&priv->lock);
	spin_lock_init(&priv->irq_lock);
	spin_lock_init(&priv->oam_rx_lock);
	INIT_LIST_HEAD(&priv->oam_rx_queue);
	init_waitqueue_head(&priv->oam_rx_wait);
	atomic_set(&priv->oam_opened, 0);
	INIT_DELAYED_WORK(&priv->mpcp_work, en7581_epon_mpcp_work);
	INIT_DELAYED_WORK(&priv->phy_recovery_work,
			  en7581_epon_phy_recovery_work);
	INIT_DELAYED_WORK(&priv->holdover_work, en7581_epon_holdover_work);
	INIT_DELAYED_WORK(&priv->pm_work, en7581_epon_pm_work);
	priv->holdover.time_ms = AIROHA_EPON_HOLDOVER_TIME_DEFAULT_MS;
	platform_set_drvdata(pdev, priv);

	priv->mac = devm_platform_ioremap_resource_byname(pdev, "mac");
	if (IS_ERR(priv->mac))
		return PTR_ERR(priv->mac);
	resource = platform_get_resource_byname(pdev, IORESOURCE_MEM, "phy-csr");
	if (!resource || resource_size(resource) != EN7581_EPON_PHY_CSR_SIZE)
		return dev_err_probe(dev, -EINVAL,
			"invalid shared EPON PHY CSR resource\n");
	/* GPON, XG(S)-PON and EPON share this CSR page. */
	priv->phy_csr = devm_ioremap(dev, resource->start,
				    resource_size(resource));
	if (!priv->phy_csr)
		return -ENOMEM;
	resource = platform_get_resource_byname(pdev, IORESOURCE_MEM,
					"xepon-pcs");
	if (!resource || resource_size(resource) != EN7581_XEPON_PCS_SIZE)
		return dev_err_probe(dev, -EINVAL,
			"invalid XEPON PCS resource\n");
	priv->xepon_pcs = devm_ioremap_resource(dev, resource);
	if (IS_ERR(priv->xepon_pcs))
		return PTR_ERR(priv->xepon_pcs);
	priv->pcs = fwnode_pcs_get(dev_fwnode(dev), 0);
	if (IS_ERR(priv->pcs))
		return dev_err_probe(dev, PTR_ERR(priv->pcs),
			"EPON PCS is not ready\n");
	priv->scu = syscon_regmap_lookup_by_phandle(dev->of_node, "airoha,scu");
	if (IS_ERR(priv->scu))
		return dev_err_probe(dev, PTR_ERR(priv->scu),
			"failed to find SCU syscon\n");
	priv->ethernet_np = of_parse_phandle(dev->of_node, "ethernet", 0);
	if (!priv->ethernet_np)
		return dev_err_probe(dev, -EINVAL,
			"missing ethernet phandle\n");
	ret = devm_add_action_or_reset(dev, en7581_epon_of_node_put,
				       priv->ethernet_np);
	if (ret)
		return ret;

	bosa_np = of_parse_phandle(dev->of_node, "bosa-controller", 0);
	if (!bosa_np)
		return dev_err_probe(dev, -EINVAL,
			"missing bosa-controller phandle\n");
	priv->bosa = airoha_en7572_get(bosa_np);
	of_node_put(bosa_np);
	if (IS_ERR(priv->bosa))
		return dev_err_probe(dev, PTR_ERR(priv->bosa),
			"BOSA controller is not ready\n");
	ret = devm_add_action_or_reset(dev, en7581_epon_bosa_put, priv->bosa);
	if (ret)
		return ret;
	ret = airoha_en7572_set_tx_enabled(priv->bosa, false);
	if (ret)
		return dev_err_probe(dev, ret, "failed to disable optical TX\n");

	device_property_read_u32(dev, "airoha,llid-mask", &llid_mask);
	if (!llid_mask)
		return dev_err_probe(dev, -EINVAL, "LLID mask must not be empty\n");
	priv->llid_mask = llid_mask;
	device_property_read_u32(dev, "airoha,silent-time",
				 &priv->silent_time);
	if (!priv->silent_time || priv->silent_time > 3600)
		return dev_err_probe(dev, -ERANGE,
			"silent time must be between 1 and 3600 seconds\n");
	ret = device_get_mac_address(dev, priv->onu_mac);
	if (!ret && is_valid_ether_addr(priv->onu_mac))
		priv->onu_mac_set = true;

	priv->mac_irq = platform_get_irq_byname(pdev, "mac");
	if (priv->mac_irq < 0)
		return priv->mac_irq;
	priv->phy_irq = platform_get_irq_byname(pdev, "phy");
	if (priv->phy_irq < 0)
		return priv->phy_irq;
	en7581_epon_write(priv, EN7581_EPON_INT_ENABLE, 0);
	en7581_epon_write(priv, EN7581_EPON_INT_ENABLE2, 0);
	en7581_epon_write(priv, EN7581_EPON_INT_ENABLE3, 0);
	writel(0, priv->xepon_pcs + EN7581_XEPON_PCS_INT_ENABLE);
	ret = devm_request_threaded_irq(dev, priv->mac_irq, en7581_epon_irq,
		en7581_epon_irq_thread, IRQF_ONESHOT | IRQF_SHARED,
		"airoha-epon-mac", priv);
	if (ret)
		return dev_err_probe(dev, ret, "failed to request EPON MAC IRQ\n");
	ret = devm_request_threaded_irq(dev, priv->phy_irq, en7581_epon_phy_irq,
		en7581_epon_phy_irq_thread, IRQF_ONESHOT | IRQF_SHARED,
		"airoha-epon-phy", priv);
	if (ret)
		return dev_err_probe(dev, ret, "failed to request EPON PHY IRQ\n");

	controller_np = of_parse_phandle(dev->of_node,
					 "airoha,xpon-controller", 0);
	if (!controller_np)
		return dev_err_probe(dev, -EINVAL,
			"missing airoha,xpon-controller phandle\n");
	priv->asymmetric_backend = airoha_xpon_backend_register(dev,
		controller_np, AIROHA_XPON_MODE_EPON_10G_1G,
		&en7581_epon_ops, priv);
	if (IS_ERR(priv->asymmetric_backend)) {
		of_node_put(controller_np);
		return dev_err_probe(dev, PTR_ERR(priv->asymmetric_backend),
			"XPON owner rejected 10G/1G EPON backend\n");
	}
	ret = devm_add_action_or_reset(dev, en7581_epon_backend_unregister,
		priv->asymmetric_backend);
	if (ret) {
		of_node_put(controller_np);
		return ret;
	}
	priv->symmetric_backend = airoha_xpon_backend_register(dev,
		controller_np, AIROHA_XPON_MODE_EPON_10G_10G,
		&en7581_epon_ops, priv);
	of_node_put(controller_np);
	if (IS_ERR(priv->symmetric_backend))
		return dev_err_probe(dev, PTR_ERR(priv->symmetric_backend),
			"XPON owner rejected 10G/10G EPON backend\n");
	ret = devm_add_action_or_reset(dev, en7581_epon_backend_unregister,
		priv->symmetric_backend);
	if (ret)
		return ret;

	priv->oam.minor = MISC_DYNAMIC_MINOR;
	priv->oam.name = "airoha-epon-oam";
	priv->oam.fops = &en7581_epon_oam_fops;
	priv->oam.parent = dev;
	priv->oam.mode = 0600;
	ret = misc_register(&priv->oam);
	if (ret)
		return dev_err_probe(dev, ret,
			"failed to register EPON OAM endpoint\n");
	priv->oam_registered = true;
	ret = devm_add_action_or_reset(dev, en7581_epon_misc_deregister, priv);
	if (ret)
		return ret;

	ret = airoha_xpon_backend_ready(priv->asymmetric_backend);
	if (ret && ret != -EAGAIN)
		return dev_err_probe(dev, ret == -ENODEV ? -EPROBE_DEFER : ret,
			"failed to claim selected EPON mode\n");
	ready_ret = airoha_xpon_backend_ready(priv->symmetric_backend);
	if (ready_ret && ready_ret != -EAGAIN)
		return dev_err_probe(dev,
			ready_ret == -ENODEV ? -EPROBE_DEFER : ready_ret,
			"failed to claim selected EPON mode\n");
	if (ret == -EAGAIN || ready_ret == -EAGAIN)
		dev_info(dev,
			 "selected EPON mode is waiting for a valid ONU MAC\n");

	dev_info(dev,
		 "AN7581 EPON backends and dedicated OAM endpoint registered; ONU MAC and mode calibration gate TX\n");
	return 0;
}

static void en7581_epon_remove(struct platform_device *pdev)
{
	struct en7581_epon *priv = platform_get_drvdata(pdev);
	int ret;

	priv->removing = true;
	en7581_epon_misc_deregister(priv);
	cancel_delayed_work_sync(&priv->mpcp_work);
	cancel_delayed_work_sync(&priv->phy_recovery_work);
	cancel_delayed_work_sync(&priv->pm_work);
	en7581_epon_cancel_holdover_sync(priv);
	en7581_epon_mask_irqs(priv);
	en7581_epon_synchronize_irqs(priv);
	airoha_en7572_set_tx_enabled(priv->bosa, false);
	en7581_epon_detach_oam(priv);
	mutex_lock(&priv->lock);
	ret = priv->mac_initialized ? en7581_epon_clear_sessions_locked(priv) :
		airoha_epon_qdma_llids_clear(priv->ethernet_np);
	if (!ret) {
		priv->datapath_llid_mask = 0;
		priv->datapath_report_fec_mask = 0;
		priv->loopback_mask = 0;
	}
	mutex_unlock(&priv->lock);
	if (ret)
		dev_warn(priv->dev, "failed to release EPON data path: %d\n",
			 ret);
}

static void en7581_epon_shutdown(struct platform_device *pdev)
{
	struct en7581_epon *priv = platform_get_drvdata(pdev);
	int ret;

	priv->removing = true;
	cancel_delayed_work_sync(&priv->mpcp_work);
	cancel_delayed_work_sync(&priv->phy_recovery_work);
	cancel_delayed_work_sync(&priv->pm_work);
	en7581_epon_cancel_holdover_sync(priv);
	en7581_epon_mask_irqs(priv);
	en7581_epon_synchronize_irqs(priv);
	airoha_en7572_set_tx_enabled(priv->bosa, false);
	en7581_epon_detach_oam(priv);
	mutex_lock(&priv->lock);
	ret = priv->mac_initialized ? en7581_epon_clear_sessions_locked(priv) :
		airoha_epon_qdma_llids_clear(priv->ethernet_np);
	if (!ret) {
		priv->datapath_llid_mask = 0;
		priv->datapath_report_fec_mask = 0;
		priv->loopback_mask = 0;
	}
	mutex_unlock(&priv->lock);
	if (ret)
		dev_warn(priv->dev, "failed to release EPON data path: %d\n",
			 ret);
}

static const struct of_device_id en7581_epon_of_match[] = {
	{ .compatible = "airoha,en7581-epon" },
	{ }
};
MODULE_DEVICE_TABLE(of, en7581_epon_of_match);

static struct platform_driver en7581_epon_driver = {
	.probe = en7581_epon_probe,
	.remove = en7581_epon_remove,
	.shutdown = en7581_epon_shutdown,
	.driver = {
		.name = "airoha-en7581-epon",
		.of_match_table = en7581_epon_of_match,
		.dev_groups = en7581_epon_groups,
	},
};
module_platform_driver(en7581_epon_driver);

MODULE_AUTHOR("OpenWrt XG2010G maintainers");
MODULE_DESCRIPTION("Airoha AN7581 10G-EPON MAC and MPCP driver");
MODULE_LICENSE("GPL");
