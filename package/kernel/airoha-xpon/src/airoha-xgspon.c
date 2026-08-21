// SPDX-License-Identifier: GPL-2.0-only
/*
 * EN7581 XGS-PON control-plane resource owner and secure OMCC ABI endpoint.
 *
 * Security capabilities and optical TX are advertised only after the
 * clean-room TC/PLOAM/key implementation and runtime activation gates pass.
 */

#include <linux/bitfield.h>
#include <linux/ctype.h>
#include <linux/compat.h>
#include <crypto/utils.h>
#include <linux/fs.h>
#include <linux/interrupt.h>
#include <linux/io.h>
#include <linux/iopoll.h>
#include <linux/kref.h>
#include <linux/miscdevice.h>
#include <linux/module.h>
#include <linux/mutex.h>
#include <linux/netdevice.h>
#include <linux/of.h>
#include <linux/pcs/pcs-airoha.h>
#include <linux/pcs/pcs.h>
#include <linux/platform_device.h>
#include <linux/poll.h>
#include <linux/random.h>
#include <linux/skbuff.h>
#include <linux/string.h>
#include <linux/sysfs.h>
#include <linux/unaligned.h>
#include <linux/uaccess.h>
#include <linux/wait.h>
#include <linux/workqueue.h>

#include "airoha-en7572.h"
#include "airoha-eth.h"
#include "airoha-xgs-eqd.h"
#include "airoha-xgs-mac-init.h"
#include "airoha-xgs-omcc.h"
#include "airoha-xgs-security.h"
#include "airoha-xgs-to1.h"
#include "airoha-xpon-core.h"

#define EN7581_XGSPON_MAC_SIZE	0xff8
#define EN7581_XGSPON_PHY_CSR_SIZE	0x1000

#define EN7581_XGSPON_SW_RST		0x000
#define EN7581_XGSPON_MBI_MPI_STOP	0x004
#define EN7581_XGSPON_VENDOR_ID		0x00c
#define EN7581_XGSPON_VS_SN		0x010
#define EN7581_XGSPON_ONU_ID		0x014
#define EN7581_XGSPON_REGISTRATION_ID	0x018
#define EN7581_XGSPON_INT_ENABLE		0x040
#define EN7581_XGSPON_INT_STATUS		0x044
#define EN7581_XGSPON_FIFO_ERR_STS	0x050
#define EN7581_XGSPON_TX_ERR_STS		0x054
#define EN7581_XGSPON_RX_ERR_STS		0x058
#define EN7581_XGSPON_O23_O4_PLOAMU_CTRL	0x100
#define EN7581_XGSPON_ACTIVATION_ST	0x104
#define EN7581_XGSPON_RSP_TIME		0x108
#define EN7581_XGSPON_RDM_DLY		0x10c
#define EN7581_XGSPON_EQD			0x114
#define EN7581_XGSPON_US_PROF_VLD	0x11c
#define EN7581_XGSPON_US_PROF_PSB_LEN_0_1	0x120
#define EN7581_XGSPON_US_PROF_PSB_LEN_2_3	0x124
#define EN7581_XGSPON_TCONT_ID_CFG	0x250
#define EN7581_XGSPON_TCONT_ID_STS	0x254
#define EN7581_XGSPON_GEM_PORT_CFG	0x274
#define EN7581_XGSPON_GEM_PORT_STS	0x278
#define EN7581_XGSPON_IDLE_GEM_CTRL	0x280
#define EN7581_XGSPON_US_DYING_GASP_CTRL	0x284
#define EN7581_XGSPON_EPDROP_EN		0x2f0
#define EN7581_XGSPON_PLOAMU_FIFO_STS	0x300
#define EN7581_XGSPON_PLOAMU_WDATA	0x304
#define EN7581_XGSPON_PLOAMD_FIFO_STS	0x308
#define EN7581_XGSPON_PLOAMD_RDATA	0x30c
#define EN7581_XGSPON_CUR_KIDX		0x318
#define EN7581_XGSPON_PIK0		0x360
#define EN7581_XGSPON_PIK1		0x370
#define EN7581_XGSPON_OIK0		0x380
#define EN7581_XGSPON_OIK1		0x390
#define EN7581_XGSPON_KEK0		0x3a0
#define EN7581_XGSPON_KEK1		0x3b0
#define EN7581_XGSPON_PON_TAG0		0x3c0
#define EN7581_XGSPON_PON_TAG1		0x3c4
#define EN7581_XGSPON_SW_SET_KIDX	0x3e8
#define EN7581_XGSPON_US_AES_KEY_CTRL	0x200
#define EN7581_XGSPON_DS_AES_KEY_VALID	0x204
#define EN7581_XGSPON_AES_UC_KEY0	0x210
#define EN7581_XGSPON_AES_UC_KEY1	0x220
#define EN7581_XGSPON_MIB_CTRL		0x500
#define EN7581_XGSPON_DBG_CAP_SETTING	0x800
#define EN7581_XGSPON_BWMAP_CHECK_STS	0x808
#define EN7581_XGSPON_DBG_RESYNC		0x82c
#define EN7581_XGSPON_DBG_CAP_SETTING1	0x868

/* SDK XGS-PON PHY CSR offsets relative to physical base 0x1faf0000. */
#define EN7581_XGSPON_PHY_RX_SYNC_CTRL	0xa04
#define EN7581_XGSPON_PHY_RESET		0xa0c
#define EN7581_XGSPON_PHY_INT_STATUS	0xa10
#define EN7581_XGSPON_PHY_INT_ENABLE	0xa14
#define EN7581_XGSPON_PHY_PREAMBLE_BASE	0xa18
#define EN7581_XGSPON_PHY_DELIMITER_BASE	0xa38
#define EN7581_XGSPON_PHY_TX_FEC_CTRL	0xa58
#define EN7581_XGSPON_PHY_PSBU_INFO_BASE	0xa5c
#define EN7581_XGSPON_PHY_RX_SYNC_STATUS	0xb1c
#define EN7581_XGSPON_PHY_SFP_STATUS	0xb4c
#define EN7581_XGSPON_PHY_STATUS		0xb54

#define EN7581_XGSPON_INT_PLOAMD		BIT(0)
#define EN7581_XGSPON_INT_PLOAMU		BIT(1)
#define EN7581_XGSPON_INT_O23_SN_REQUEST	BIT(2)
#define EN7581_XGSPON_INT_O23_SN_SENT	BIT(3)
#define EN7581_XGSPON_INT_O4_RANGING_REQUEST	BIT(4)
#define EN7581_XGSPON_INT_O4_REGISTRATION_SENT	BIT(5)
#define EN7581_XGSPON_INT_US_NO_MESSAGE	BIT(6)
#define EN7581_XGSPON_INT_US_KEY_SWITCH_DONE	BIT(7)
#define EN7581_XGSPON_INT_BWMAP_CHECK_ERROR	BIT(13)
#define EN7581_XGSPON_INT_FIFO_ERROR	BIT(15)
#define EN7581_XGSPON_INT_TX_ERROR	BIT(16)
#define EN7581_XGSPON_INT_RX_ERROR	BIT(17)
#define EN7581_XGSPON_INT_UPSTREAM_EVENTS \
	(EN7581_XGSPON_INT_PLOAMU | EN7581_XGSPON_INT_O23_SN_REQUEST | \
	 EN7581_XGSPON_INT_O23_SN_SENT | \
	 EN7581_XGSPON_INT_O4_RANGING_REQUEST | \
	 EN7581_XGSPON_INT_O4_REGISTRATION_SENT | \
	 EN7581_XGSPON_INT_US_NO_MESSAGE | EN7581_XGSPON_INT_FIFO_ERROR | \
	 EN7581_XGSPON_INT_TX_ERROR)
#define EN7581_XGSPON_INT_DOWNSTREAM_ERRORS \
	(EN7581_XGSPON_INT_BWMAP_CHECK_ERROR | EN7581_XGSPON_INT_RX_ERROR)
#define EN7581_XGSPON_INT_SUPPORTED \
	(EN7581_XGSPON_INT_PLOAMD | EN7581_XGSPON_INT_UPSTREAM_EVENTS | \
	 EN7581_XGSPON_INT_DOWNSTREAM_ERRORS)
/* PLOAMd and the SDK receive/BWmap error summaries are enabled at probe.
 * Upstream events are enabled only for an explicit userspace activation
 * request after the credential/PHY gates pass.
 */
#define EN7581_XGSPON_INT_ENABLED	(EN7581_XGSPON_INT_PLOAMD | \
					 EN7581_XGSPON_INT_DOWNSTREAM_ERRORS)
#define EN7581_XGSPON_INT_ACTIVE	(EN7581_XGSPON_INT_ENABLED | \
					 EN7581_XGSPON_INT_UPSTREAM_EVENTS)
#define EN7581_XGSPON_RDM_DLY_MAX_ENABLE	BIT(27)
#define EN7581_XGSPON_FIFO_ERR_TX_PLOAMU_OVERRUN	BIT(2)
#define EN7581_XGSPON_TX_ERR_BURST_SIGNAL_MISMATCH	BIT(0)
#define EN7581_XGSPON_TX_ERR_LATE_START		BIT(1)
#define EN7581_XGSPON_TX_ERR_PROFILE_INVALID	BIT(2)
#define EN7581_XGSPON_TX_ERR_ALL		GENMASK(2, 0)
#define EN7581_XGSPON_RX_ERR_LOSS_OF_GEM_DELINEATION	BIT(5)
#define EN7581_XGSPON_RX_ERR_ALL		GENMASK(13, 0)
#define EN7581_XGSPON_BWMAP_CHECK_ERR_ALL	GENMASK(17, 0)
#define EN7581_XGSPON_MAC_RESET_RELEASE	BIT(0)
#define EN7581_XGSPON_MBI_RX_STOP	BIT(0)
#define EN7581_XGSPON_MBI_TX_STOP	BIT(8)
#define EN7581_XGSPON_MBI_RX_STOP_DONE	BIT(14)
#define EN7581_XGSPON_MBI_TX_STOP_DONE	BIT(15)
#define EN7581_XGSPON_MPI_RX_STOP	BIT(16)
#define EN7581_XGSPON_MPI_TX_STOP	BIT(24)
#define EN7581_XGSPON_MPI_RX_STOP_DONE	BIT(30)
#define EN7581_XGSPON_MPI_TX_STOP_DONE	BIT(31)
#define EN7581_XGSPON_PATH_STOP_MASK \
	(EN7581_XGSPON_MBI_RX_STOP | EN7581_XGSPON_MBI_TX_STOP | \
	 EN7581_XGSPON_MPI_RX_STOP | EN7581_XGSPON_MPI_TX_STOP)
#define EN7581_XGSPON_PATH_STOP_DONE_MASK \
	(EN7581_XGSPON_MBI_RX_STOP_DONE | EN7581_XGSPON_MBI_TX_STOP_DONE | \
	 EN7581_XGSPON_MPI_RX_STOP_DONE | EN7581_XGSPON_MPI_TX_STOP_DONE)
#define EN7581_XGSPON_LODS_STOP_MASK \
	(EN7581_XGSPON_MBI_TX_STOP | EN7581_XGSPON_MPI_RX_STOP | \
	 EN7581_XGSPON_MPI_TX_STOP)
#define EN7581_XGSPON_LODS_STOP_DONE_MASK \
	(EN7581_XGSPON_MBI_TX_STOP_DONE | \
	 EN7581_XGSPON_MPI_RX_STOP_DONE | EN7581_XGSPON_MPI_TX_STOP_DONE)
#define EN7581_XGSPON_O23_O4_PLOAMU_SOFTWARE	BIT(0)
#define EN7581_XGSPON_ACTIVATION_STATE	GENMASK(3, 0)
#define EN7581_XGSPON_ACTIVATION_O1	1
#define EN7581_XGSPON_ACTIVATION_O2_3	2
#define EN7581_XGSPON_ACTIVATION_O4	4
#define EN7581_XGSPON_ACTIVATION_O5	5
#define EN7581_XGSPON_ACTIVATION_O6	6
#define EN7581_XGSPON_ACTIVATION_O7	7
#define EN7581_XGSPON_MAX_RANDOM_DELAY	GENMASK(27, 16)
/* max_rdm_dly is a 12-bit field; bit 11 is the SDK enable flag. */
#define EN7581_XGSPON_RANDOM_DELAY_ENABLE_FIELD	BIT(11)
#define EN7581_XGSPON_RANDOM_DELAY_WORDS	GENMASK(10, 0)
#define EN7581_XGSPON_RANDOM_DELAY_WORD_BITS	(32U * 8U)
#define EN7581_XGSPON_TX_SYNC_READY	BIT(31)
#define EN7581_XGSPON_SW_RESYNC_ENABLE	BIT(8)
#define EN7581_XGSPON_SW_RESYNC_START	BIT(0)
#define EN7581_XGSPON_US_PROFILE_VALID_MASK \
	(BIT(0) | BIT(8) | BIT(16) | BIT(24))
#define EN7581_XGSPON_PROFILE_REGISTER_STRIDE	8
#define EN7581_XGSPON_PROFILE_INFO_STRIDE	4
#define EN7581_XGSPON_PHY_INT_PHYA_READY	BIT(31)
#define EN7581_XGSPON_PHY_INT_RX_READY	BIT(9)
#define EN7581_XGSPON_PHY_INT_RX_LOF	BIT(2)
#define EN7581_XGSPON_PHY_INT_RX_SYNC_OK	BIT(1)
#define EN7581_XGSPON_PHY_INT_RX_LOS	BIT(0)
#define EN7581_XGSPON_PHY_RX_EVENTS \
	(EN7581_XGSPON_PHY_INT_PHYA_READY | \
	 EN7581_XGSPON_PHY_INT_RX_READY | EN7581_XGSPON_PHY_INT_RX_LOF | \
	 EN7581_XGSPON_PHY_INT_RX_SYNC_OK | EN7581_XGSPON_PHY_INT_RX_LOS)
#define EN7581_XGSPON_PHY_RX_SYNC_STATE	GENMASK(1, 0)
#define EN7581_XGSPON_PHY_RX_SYNC	BIT(1)
#define EN7581_XGSPON_PHY_SFP_RX_LOS	BIT(0)
#define EN7581_XGSPON_PHY_PHYA_READY	BIT(0)
#define EN7581_XGSPON_PHY_RX_ENABLE	BIT(16)
#define EN7581_XGSPON_PHY_RESET_HOLD	0
#define EN7581_XGSPON_PHY_RESET_RELEASE	GENMASK(1, 0)
#define EN7581_XGSPON_PHY_RECOVERY_DELAY_MS	2
#define EN7581_XGSPON_PHY_SETTLE_MS	8
#define EN7581_XGSPON_PHY_RETRY_DELAY_MS	50
#define EN7581_XGSPON_PHY_MAX_RECOVERY_ATTEMPTS	3
#define EN7581_XGSPON_TO1_MS		10000
#define EN7581_XGSPON_LODS_TIMEOUT_MS	100
#define EN7581_XGSPON_MAC_STOP_TIMEOUT_US	3000
#define EN7581_XGSPON_PLOAMU_AVAILABLE	GENMASK(7, 0)
#define EN7581_XGSPON_PLOAMU_OVERRUN	BIT(31)
#define EN7581_XGSPON_PLOAMD_USED		GENMASK(7, 0)
#define EN7581_XGSPON_PLOAMD_OVERRUN	BIT(31)
#define EN7581_XGSPON_PROFILE_MESSAGE_ID	0x01
#define EN7581_XGSPON_ASSIGN_ONU_ID_MESSAGE_ID	0x03
#define EN7581_XGSPON_RANGING_TIME_MESSAGE_ID	0x04
#define EN7581_XGSPON_DEACTIVATE_ONU_ID_MESSAGE_ID	0x05
#define EN7581_XGSPON_DISABLE_SERIAL_NUMBER_MESSAGE_ID	0x06
#define EN7581_XGSPON_REQUEST_REGISTRATION_MESSAGE_ID	0x09
#define EN7581_XGSPON_ASSIGN_ALLOC_ID_MESSAGE_ID	0x0a
#define EN7581_XGSPON_SLEEP_ALLOW_MESSAGE_ID	0x12
#define EN7581_XGSPON_CALIBRATION_REQUEST_MESSAGE_ID	0x13
#define EN7581_XGSPON_ADJUST_TX_WAVELENGTH_MESSAGE_ID	0x14
#define EN7581_XGSPON_TUNING_CONTROL_MESSAGE_ID	0x15
#define EN7581_XGSPON_SYSTEM_PROFILE_MESSAGE_ID	0x17
#define EN7581_XGSPON_CHANNEL_PROFILE_MESSAGE_ID	0x18
#define EN7581_XGSPON_PROTECTION_CONTROL_MESSAGE_ID	0x19
#define EN7581_XGSPON_CHANGE_POWER_LEVEL_MESSAGE_ID	0x1a
#define EN7581_XGSPON_POWER_CONSUMPTION_INQUIRE_MESSAGE_ID	0x1b
#define EN7581_XGSPON_RATE_CONTROL_MESSAGE_ID	0x1c
#define EN7581_XGSPON_MAX_IRQ_PLOAMS	32
#define EN7581_XGSPON_ACK_QUEUE_DEPTH	4
#define EN7581_XGSPON_KEY_REPORT_QUEUE_DEPTH	4
#define EN7581_XGSPON_KEY_TK4_MS	100
#define EN7581_XGSPON_KEY_TK5_MS	20
#define EN7581_XGSPON_KEY_SWITCH_TIMEOUT_US	3000

#define EN7581_XGSPON_ONU_ID_VALUE	GENMASK(9, 0)
#define EN7581_XGSPON_ONU_ID_VALID	BIT(15)
#define EN7581_XGPON_BROADCAST_ONU_ID	0x03ff
#define EN7581_XGSPON_BROADCAST_ONU_ID	0x03fe
#define EN7581_XGSPON_NOKIA_BROADCAST_ONU_ID	0x07ff

#define EN7581_XGSPON_TCONT_COMMAND_WRITE	BIT(31)
#define EN7581_XGSPON_TCONT_INDEX		GENMASK(24, 20)
#define EN7581_XGSPON_TCONT_VALID		BIT(16)
#define EN7581_XGSPON_TCONT_ALLOC_ID		GENMASK(13, 0)
#define EN7581_XGSPON_TCONT_COMMAND_DONE	BIT(31)
#define EN7581_XGSPON_TCONT_STATUS_VALID	BIT(16)
#define EN7581_XGSPON_TCONT_STATUS_ALLOC_ID	GENMASK(13, 0)
#define EN7581_XGSPON_MAX_TCONTS		32
#define EN7581_XGSPON_UNASSIGNED_ALLOC_ID	0x03ff
#define EN7581_XGSPON_NO_TCONT			U8_MAX
#define EN7581_XGSPON_TABLE_TIMEOUT_US		3000

#define EN7581_XGSPON_GEM_COMMAND_WRITE	BIT(31)
#define EN7581_XGSPON_GEM_VALID		BIT(18)
#define EN7581_XGSPON_GEM_UNICAST	BIT(17)
#define EN7581_XGSPON_GEM_US_ENCRYPT	BIT(16)
#define EN7581_XGSPON_GEM_PORT_ID	GENMASK(15, 0)
#define EN7581_XGSPON_GEM_COMMAND_DONE	BIT(31)
#define EN7581_XGSPON_GEM_STATUS_VALID	BIT(2)
#define EN7581_XGSPON_GEM_STATUS_UNICAST	BIT(1)
#define EN7581_XGSPON_GEM_STATUS_US_ENCRYPT	BIT(0)
#define EN7581_XGSPON_GEM_STATUS_MASK	GENMASK(2, 0)

#define EN7581_XGSPON_CUR_PIK_IDX	BIT(0)
#define EN7581_XGSPON_CUR_OIK_IDX	BIT(16)
#define EN7581_XGSPON_SET_PIK_IDX	BIT(0)
#define EN7581_XGSPON_SET_OIK_IDX	BIT(8)
#define EN7581_XGSPON_SET_PIK_EN	BIT(16)
#define EN7581_XGSPON_SET_OIK_EN	BIT(24)
#define EN7581_XGSPON_US_AES_KEY_INDEX	BIT(0)
#define EN7581_XGSPON_US_AES_KEY_VALID	BIT(31)
#define EN7581_XGSPON_DS_AES_UC_KEY0_VALID	BIT(0)
#define EN7581_XGSPON_DS_AES_UC_KEY1_VALID	BIT(1)
#define EN7581_XGSPON_DS_AES_UC_KEYS_VALID	GENMASK(1, 0)
#define EN7581_XGSPON_HW_DS_OMCI_MIC	BIT(4)
#define EN7581_XGSPON_HW_US_OMCI_MIC	BIT(3)
#define EN7581_XGSPON_OMCC_RX_QUEUE_LIMIT	64
#define EN7581_XGSPON_OMCC_READY_QUEUE_LIMIT	64
#define EN7581_XGSPON_ACTIVATION_RETRY_MS	50
#define EN7581_XGSPON_PM_SAMPLE_MS		10

/* SDK logical register addresses are based at 0x5000. */
#define EN7581_XGSPON_RX_HLEND_HEC_CNT		0x8fc
#define EN7581_XGSPON_RX_ALLOC_HEC_CNT		0x900
#define EN7581_XGSPON_RX_HDR_HEC_CNT		0x904
#define EN7581_XGSPON_RX_PHY_HEC_ERR_CNT	0x908
#define EN7581_XGSPON_RX_MIC_ERR_CNT		0x90c
#define EN7581_XGSPON_RX_KEY_ERR_CNT		0x918
#define EN7581_XGSPON_RX_LOST_WCNT		0x91c
#define EN7581_XGSPON_INVLD_PROF_BST_GNT_CNT	0x920
#define EN7581_XGSPON_RX_PLOAMD_CNT		0x950
#define EN7581_XGSPON_TX_PLOAMU_CNT		0x954
#define EN7581_XGSPON_RX_OMCI_CNT		0x960
#define EN7581_XGSPON_TX_OMCI_CNT		0x964
#define EN7581_XGSPON_TX_XGEM_CNT		0x96c
#define EN7581_XGSPON_RX_NON_IDLE_BCNT		0x978
#define EN7581_XGSPON_TX_NON_IDLE_BCNT		0x97c
#define EN7581_XGSPON_TX_NLF_XGEM_CNT		0x980

#define EN7581_XGSPON_HEC_1ERR		GENMASK(7, 0)
#define EN7581_XGSPON_HEC_2ERR		GENMASK(15, 8)
#define EN7581_XGSPON_HEC_3ERR		GENMASK(23, 16)
#define EN7581_XGSPON_PHY_SFC_HEC_ERR	GENMASK(15, 0)
#define EN7581_XGSPON_PHY_PON_ID_HEC_ERR	GENMASK(31, 16)
#define EN7581_XGSPON_PLOAM_MIC_ERR	GENMASK(15, 0)
#define EN7581_XGSPON_OMCI_MIC_ERR	GENMASK(31, 16)
#define EN7581_XGSPON_OMCI_MAC_CNT	GENMASK(15, 0)
#define EN7581_XGSPON_OMCI_FE_CNT	GENMASK(31, 16)

/* G.988 class 344/345/346 field positions in performance.XGSPONCounters. */
#define EN7581_XGSPON_TC_HW_VALID	GENMASK(9, 0)
#define EN7581_XGSPON_DS_HW_VALID	(BIT(0) | BIT(1) | BIT(13))
#define EN7581_XGSPON_US_HW_VALID	BIT(0)

/* Complete G.988 field masks and the subset owned by the kernel snapshot. */
#define EN7581_XGSPON_TC_PM_REQUIRED	GENMASK(12, 0)
#define EN7581_XGSPON_DS_PM_REQUIRED	GENMASK(13, 0)
#define EN7581_XGSPON_US_PM_REQUIRED	GENMASK(5, 0)
#define EN7581_XGSPON_TC_PM_VALID	EN7581_XGSPON_TC_PM_REQUIRED
#define EN7581_XGSPON_DS_PM_VALID	(EN7581_XGSPON_DS_HW_VALID | \
						 GENMASK(9, 2) | BIT(12))
/*
 * The SDK exposes the Sleep Request slot through gponDevGetUsMgntCounter(),
 * while its sole upstream PLOAM owner has no Sleep Request sender. This driver
 * has the same ownership boundary and deliberately implements no low-power
 * sender, making bit 5 an authoritative constant zero rather than unknown.
 */
#define EN7581_XGSPON_US_PM_VALID	EN7581_XGSPON_US_PM_REQUIRED

#define EN7581_XGSPON_BLOCKERS \
	"phy calibration tc-ploam secure-omcc xgem"
#define EN7581_XGSPON_INACTIVE_BLOCKERS \
	"board-mode phy calibration tc-ploam secure-omcc xgem"

enum en7581_xgspon_rx_state {
	EN7581_XGSPON_RX_WAIT_PROFILE,
	EN7581_XGSPON_RX_WAIT_ASSIGN_ONU_ID,
	EN7581_XGSPON_RX_ONU_ID_ASSIGNED,
};

enum en7581_xgspon_tx_state {
	EN7581_XGSPON_TX_BLOCKED,
	EN7581_XGSPON_TX_SERIAL_READY,
	EN7581_XGSPON_TX_SERIAL_QUEUED,
	EN7581_XGSPON_TX_WAIT_REGISTRATION,
	EN7581_XGSPON_TX_REGISTRATION_QUEUED,
	EN7581_XGSPON_TX_REGISTERED,
	EN7581_XGSPON_TX_FAULT,
};

struct en7581_xgspon_omcc_rx {
	struct list_head list;
	struct sk_buff *skb;
	struct airoha_xgs_omcc_metadata metadata;
	u64 generation;
};

struct en7581_xgspon_omcc_ready {
	struct list_head list;
	u64 generation;
	size_t size;
	u8 record[];
};

struct en7581_xgspon_service_work {
	struct airoha_xgs_service_tcont normalized_tconts[
		AIROHA_XGS_SERVICE_MAX_TCONTS];
	struct airoha_xgs_service_xgem normalized_xgems[
		AIROHA_XGS_SERVICE_MAX_XGEMS];
	struct airoha_xgs_service_tcont old_tconts[
		AIROHA_XGS_SERVICE_MAX_TCONTS];
	struct airoha_xgs_service_xgem old_xgems[
		AIROHA_XGS_SERVICE_MAX_XGEMS];
};

struct en7581_xgspon_ack {
	u64 generation;
	u8 source_message_id;
	u8 sequence;
	u8 completion_code;
	u8 key_index;
};

struct en7581_xgspon_key_report {
	u64 generation;
	u64 exchange_generation;
	u8 sequence;
	u8 report_type;
	u8 key_index;
	u8 fragment[AIROHA_XGS_KEY_REPORT_FRAGMENT_SIZE];
};

struct en7581_xgspon_data_key_snapshot {
	u32 upstream_control;
	u32 downstream_valid;
	u8 keys[2][AIROHA_XGS_KEY_SIZE];
	u8 active_index;
	bool confirmed;
};

enum en7581_xgspon_pm_hw_counter {
	EN7581_XGSPON_PM_HLEND_HEC_1,
	EN7581_XGSPON_PM_HLEND_HEC_2,
	EN7581_XGSPON_PM_HLEND_HEC_3,
	EN7581_XGSPON_PM_ALLOC_HEC_1,
	EN7581_XGSPON_PM_ALLOC_HEC_2,
	EN7581_XGSPON_PM_ALLOC_HEC_3,
	EN7581_XGSPON_PM_HEADER_HEC_1,
	EN7581_XGSPON_PM_HEADER_HEC_2,
	EN7581_XGSPON_PM_HEADER_HEC_3,
	EN7581_XGSPON_PM_PHY_SFC_HEC,
	EN7581_XGSPON_PM_PHY_PON_ID_HEC,
	EN7581_XGSPON_PM_UNKNOWN_PROFILES,
	EN7581_XGSPON_PM_TX_XGEM,
	EN7581_XGSPON_PM_FRAGMENT_XGEM,
	EN7581_XGSPON_PM_XGEM_LOST_WORDS,
	EN7581_XGSPON_PM_XGEM_KEY_ERRORS,
	EN7581_XGSPON_PM_TX_NON_IDLE_BYTES,
	EN7581_XGSPON_PM_RX_NON_IDLE_BYTES,
	EN7581_XGSPON_PM_PLOAM_MIC_ERRORS,
	EN7581_XGSPON_PM_RX_PLOAM,
	EN7581_XGSPON_PM_OMCI_MIC_ERRORS,
	EN7581_XGSPON_PM_TX_PLOAM,
	EN7581_XGSPON_PM_HW_COUNTERS,
};

struct en7581_xgspon_pm_extender {
	u64 total;
	u64 session_base;
	u32 previous;
	u32 mask;
	bool initialized;
};

struct en7581_xgspon_pm_lods {
	u64 events;
	u64 restored;
	u64 reactivations;
};

struct en7581_xgspon {
	struct device *dev;
	struct kref refcount;
	struct mutex lock;
	struct mutex omcc_read_lock;
	spinlock_t omcc_rx_lock;
	struct list_head omcc_rx_queue;
	struct list_head omcc_ready_queue;
	wait_queue_head_t omcc_rx_wait;
	struct work_struct omcc_rx_work;
	struct delayed_work phy_recovery_work;
	struct delayed_work activation_work;
	struct delayed_work to1_work;
	struct delayed_work key_tk4_work;
	struct delayed_work key_tk5_work;
	struct delayed_work pm_work;
	void __iomem *mac;
	void __iomem *phy_csr;
	struct phylink_pcs *pcs;
	struct airoha_en7572 *bosa;
	struct airoha_xpon_backend *xgpon_backend;
	struct airoha_xpon_backend *xgspon_backend;
	struct device_node *ethernet_np;
	int mac_irq;
	int phy_irq;
	bool hardware_selected;
	bool enabled;
	bool optical_tx_enabled;
	bool removing;
	bool mac_irq_requested;
	bool phy_irq_requested;
	bool omcc_registered;
	bool xpon_irqs_masked;
	bool switch_resume_enabled;
	enum airoha_xpon_mode active_mode;
	bool phy_ready;
	bool phy_los;
	bool phy_lof;
	bool phy_recovering;
	bool phy_fault;
	bool lods_session_preserved;
	bool serial_set;
	bool registration_id_set;
	bool registration_msk_set;
	bool session_keys_set;
	bool session_keys_programmed;
	bool hardware_state_valid;
	bool mac_initialized;
	bool onu_id_set;
	bool omcc_xgem_programmed;
	bool omcc_consumer_registered;
	bool hardware_ds_omci_mic;
	bool request_registration_authenticated;
	bool registration_response_pending;
	bool ranging_time_authenticated;
	bool equalization_delay_programmed;
	bool assign_alloc_id_authenticated;
	bool assign_alloc_id_committed;
	/* Temporary permission for the Serial Number/Registration PLOAM. */
	bool tx_armed;
	bool tx_authorized;
	bool serial_disabled;
	bool registration_key_switch_verified;
	bool ack_in_flight;
	bool key_report_in_flight;
	bool data_key_pending;
	bool data_key_confirm_pending;
	bool data_key_confirmed;
	bool data_key_snapshot_valid;
	u16 onu_id;
	u8 assign_onu_id_sequence;
	u8 request_registration_sequence;
	u8 last_deactivate_sequence;
	u8 last_tcont_index;
	u8 ack_head;
	u8 ack_count;
	u8 key_report_head;
	u8 key_report_count;
	u8 data_key_index;
	u8 data_key_sequence;
	u32 equalization_delay;
	u32 lods_us_profile_valid;
	u32 lods_key_indices;
	unsigned long lods_deadline;
	unsigned long to1_deadline;
	u64 to1_generation;
	u32 to1_expected_state;
	u16 alloc_ids[EN7581_XGSPON_MAX_TCONTS];
	bool alloc_valid[EN7581_XGSPON_MAX_TCONTS];
	struct airoha_xgs_service_tcont service_tconts[
		AIROHA_XGS_SERVICE_MAX_TCONTS];
	struct airoha_xgs_service_xgem service_xgems[
		AIROHA_XGS_SERVICE_MAX_XGEMS];
	u8 service_tcont_hw_index[AIROHA_XGS_SERVICE_MAX_TCONTS];
	u16 service_tcont_count;
	u16 service_xgem_count;
	struct airoha_xgs_service_tcont pending_tconts[
		AIROHA_XGS_SERVICE_MAX_TCONTS];
	struct airoha_xgs_service_xgem pending_xgems[
		AIROHA_XGS_SERVICE_MAX_XGEMS];
	u16 pending_tcont_count;
	u16 pending_xgem_count;
	struct airoha_xgs_service_tcont rollback_tconts[
		AIROHA_XGS_SERVICE_MAX_TCONTS];
	struct airoha_xgs_service_xgem rollback_xgems[
		AIROHA_XGS_SERVICE_MAX_XGEMS];
	u16 rollback_tcont_count;
	u16 rollback_xgem_count;
	bool service_rollback_valid;
	u32 service_generation;
	struct airoha_xgs_ranging_time last_ranging_time;
	struct airoha_xgs_alloc_id_assignment last_alloc_id_assignment;
	struct en7581_xgspon_ack ack_queue[EN7581_XGSPON_ACK_QUEUE_DEPTH];
	struct en7581_xgspon_ack active_ack;
	struct en7581_xgspon_key_report key_report_queue[
		EN7581_XGSPON_KEY_REPORT_QUEUE_DEPTH];
	struct en7581_xgspon_key_report active_key_report;
	struct en7581_xgspon_data_key_snapshot data_key_snapshot;
	enum en7581_xgspon_rx_state rx_state;
	enum en7581_xgspon_tx_state tx_state;
	u64 ploamu_send_events;
	u64 serial_number_request_events;
	u64 serial_number_send_events;
	u64 ranging_request_events;
	u64 registration_send_events;
	u64 acknowledge_queued_events;
	u64 acknowledge_send_events;
	u64 acknowledge_coalesced_events;
	u64 acknowledge_dropped_events;
	u64 key_control_generate_events;
	u64 key_control_confirm_events;
	u64 key_report_queued_events;
	u64 key_report_send_events;
	u64 key_report_coalesced_events;
	u64 key_report_dropped_events;
	u64 data_key_exchange_generation;
	u64 key_exchange_timeout_events;
	u64 key_exchange_rollback_events;
	u64 to1_timeout_events;
	u64 deactivate_events;
	u64 disable_serial_events;
	u64 allow_serial_events;
	u64 disable_discovery_events;
	u64 sleep_allow_events;
	u64 sleep_allow_type_one_events;
	/* SDK-style PLOAM accounting: recognized downstream parser dispatches. */
	u64 profile_messages_received;
	u64 assign_onu_id_messages_received;
	u64 ranging_time_messages_received;
	u64 deactivate_onu_id_messages_received;
	u64 disable_serial_number_messages_received;
	u64 request_registration_messages_received;
	u64 assign_alloc_id_messages_received;
	u64 key_control_messages_received;
	u64 sleep_allow_messages_received;
	u64 reboot_onu_messages_received;
	u64 reboot_request_events;
	u64 reboot_request_generation;
	struct airoha_xgs_reboot_request last_reboot_request;
	/* Session-scoped, successfully accepted downstream PM events. */
	u64 pm_profile_messages;
	u64 pm_assign_onu_id_messages;
	u64 pm_ranging_time_messages;
	u64 pm_deactivate_onu_id_messages;
	u64 pm_disable_serial_number_messages;
	u64 pm_request_registration_messages;
	u64 pm_assign_alloc_id_messages;
	u64 pm_key_control_messages;
	u64 pm_sleep_allow_messages;
	/* Session-scoped, successfully completed upstream PM events. */
	u64 pm_serial_number_messages;
	u64 pm_registration_messages;
	u64 pm_key_report_messages;
	u64 pm_acknowledge_messages;
	struct en7581_xgspon_pm_lods pm_lods;
	struct en7581_xgspon_pm_extender pm_hw[
		EN7581_XGSPON_PM_HW_COUNTERS];
	u64 pm_snapshot_sequence;
	u64 recognized_noop_ploam_events;
	u64 ngpon2_control_ignored_events;
	u64 unsupported_ploam_events;
	u64 no_message_events;
	u64 fifo_error_events;
	u64 tx_error_events;
	u64 rx_error_events;
	u64 bwmap_check_error_events;
	u64 loss_of_gem_delineation_events;
	u64 upstream_fifo_overrun_events;
	u64 unexpected_upstream_events;
	u32 last_upstream_irq_status;
	u32 last_fifo_error_status;
	u32 last_tx_error_status;
	u32 last_rx_error_status;
	u32 last_bwmap_check_error_status;
	u32 last_phy_irq_status;
	u32 phy_recovery_attempts;
	u64 phy_irq_events;
	u64 phy_los_events;
	u64 phy_lof_events;
	u64 phy_sync_events;
	u64 phy_rx_ready_events;
	u64 phy_phya_ready_events;
	u64 phy_fake_sync_events;
	u64 phy_recoveries;
	u64 phy_recovery_failures;
	u32 omcc_rx_queue_depth;
	u32 omcc_ready_queue_depth;
	u64 instance_generation;
	u64 session_generation;
	atomic64_t omcc_rx_queued;
	atomic64_t omcc_rx_hw_verified;
	atomic64_t omcc_rx_sw_verified;
	atomic64_t omcc_rx_ready_queued;
	atomic64_t omcc_rx_delivered;
	atomic64_t omcc_rx_dropped_no_session;
	atomic64_t omcc_rx_dropped_wrong_xgem;
	atomic64_t omcc_rx_dropped_queue_full;
	atomic64_t omcc_rx_dropped_removing;
	atomic64_t omcc_rx_dropped_ready_full;
	atomic64_t omcc_rx_dropped_ready_alloc;
	atomic64_t omcc_rx_dropped_session_reset;
	atomic64_t omcc_rx_copy_faults;
	atomic64_t omcc_rx_dropped_invalid;
	atomic64_t omcc_rx_dropped_mic;
	atomic64_t omcc_rx_dropped_stale;
	u8 serial[AIROHA_XGS_SERIAL_SIZE];
	u8 registration_id[AIROHA_XGS_REGISTRATION_ID_SIZE];
	u8 registration_msk[AIROHA_XGS_KEY_SIZE];
	u8 pon_tag[AIROHA_XGS_PON_TAG_SIZE];
	struct airoha_xgs_security_keys session_keys;
	u8 data_keys[2][AIROHA_XGS_KEY_SIZE];
	struct miscdevice omcc;
};

static bool en7581_xgspon_phy_trusted_locked(struct en7581_xgspon *priv);
static void en7581_xgspon_activation_work(struct work_struct *work);
static void en7581_xgspon_activation_fault_locked(
	struct en7581_xgspon *priv);
static int en7581_xgspon_lods_release_hardware_locked(
	struct en7581_xgspon *priv);

static bool en7581_xgspon_backend_active(struct en7581_xgspon *priv)
{
	return airoha_xpon_backend_is_active(priv->xgpon_backend) ||
	       airoha_xpon_backend_is_active(priv->xgspon_backend);
}

static int en7581_xgspon_set_mac_irq_enable(struct en7581_xgspon *priv,
						    u32 mask)
{
	if (!priv->mac)
		return -ENODEV;

	writel(mask, priv->mac + EN7581_XGSPON_INT_ENABLE);
	if (readl(priv->mac + EN7581_XGSPON_INT_ENABLE) == mask)
		return 0;

	WRITE_ONCE(priv->xpon_irqs_masked, true);
	writel(0, priv->mac + EN7581_XGSPON_INT_ENABLE);
	if (priv->phy_csr)
		writel(0, priv->phy_csr + EN7581_XGSPON_PHY_INT_ENABLE);
	priv->hardware_state_valid = false;
	dev_err(priv->dev,
		"failed to verify XG/XGS-PON MAC interrupt enable %#x\n", mask);
	return -EIO;
}

static const char *en7581_xgspon_tx_state_name(
	enum en7581_xgspon_tx_state state)
{
	switch (state) {
	case EN7581_XGSPON_TX_BLOCKED:
		return "blocked";
	case EN7581_XGSPON_TX_SERIAL_READY:
		return "serial-ready";
	case EN7581_XGSPON_TX_SERIAL_QUEUED:
		return "serial-queued";
	case EN7581_XGSPON_TX_WAIT_REGISTRATION:
		return "wait-registration";
	case EN7581_XGSPON_TX_REGISTRATION_QUEUED:
		return "registration-queued";
	case EN7581_XGSPON_TX_REGISTERED:
		return "registered";
	case EN7581_XGSPON_TX_FAULT:
		return "fault";
	}

	return "invalid";
}

static bool en7581_xgspon_omcc_session_ready(struct en7581_xgspon *priv,
					      u16 xgem_id)
{
	return !priv->removing && priv->enabled && priv->hardware_selected &&
	       priv->hardware_state_valid &&
	       priv->phy_ready && !priv->phy_los && !priv->phy_recovering &&
	       !priv->phy_fault &&
	       priv->session_keys_set && priv->session_keys_programmed &&
	       priv->registration_key_switch_verified && priv->onu_id_set &&
	       priv->omcc_xgem_programmed && xgem_id == priv->onu_id;
}

static bool en7581_xgspon_omcc_tx_ready_locked(
					struct en7581_xgspon *priv)
{
	return en7581_xgspon_omcc_session_ready(priv, priv->onu_id) &&
	       priv->tx_authorized && priv->registration_key_switch_verified &&
	       priv->tx_state != EN7581_XGSPON_TX_FAULT &&
	       FIELD_GET(EN7581_XGSPON_ACTIVATION_STATE,
			 readl(priv->mac + EN7581_XGSPON_ACTIVATION_ST)) ==
			 EN7581_XGSPON_ACTIVATION_O5 &&
	       !(readl(priv->mac + EN7581_XGSPON_CUR_KIDX) &
		 EN7581_XGSPON_CUR_OIK_IDX);
}

static bool en7581_xgspon_ack_tx_ready_locked(struct en7581_xgspon *priv)
{
	return priv->tx_armed && priv->tx_authorized &&
	       priv->tx_state == EN7581_XGSPON_TX_REGISTERED &&
	       en7581_xgspon_phy_trusted_locked(priv) &&
	       priv->hardware_state_valid && priv->session_keys_set &&
	       priv->session_keys_programmed &&
	       priv->registration_key_switch_verified && priv->onu_id_set &&
	       FIELD_GET(EN7581_XGSPON_ACTIVATION_STATE,
			 readl(priv->mac + EN7581_XGSPON_ACTIVATION_ST)) ==
			 EN7581_XGSPON_ACTIVATION_O5 &&
	       !(readl(priv->mac + EN7581_XGSPON_CUR_KIDX) &
		 EN7581_XGSPON_CUR_PIK_IDX);
}

static bool en7581_xgspon_ack_matches(const struct en7581_xgspon_ack *ack,
				      u64 generation, u8 source_message_id,
				      u8 sequence, u8 completion_code,
				      u8 key_index)
{
	return ack->generation == generation &&
	       ack->source_message_id == source_message_id &&
	       ack->sequence == sequence &&
	       ack->completion_code == completion_code &&
	       (ack->key_index == key_index ||
		source_message_id == EN7581_XGSPON_PROFILE_MESSAGE_ID);
}

/* The SDK sends the O4 ranging ACK after moving to O5 but before
 * Registration, using the still-active default PLOAM key in slot 1. This
 * permission is deliberately narrower than operational PLOAM/OMCI access. */
static bool en7581_xgspon_ranging_ack_tx_ready_locked(
	struct en7581_xgspon *priv)
{
	return priv->tx_armed && !priv->tx_authorized &&
	       (priv->tx_state == EN7581_XGSPON_TX_WAIT_REGISTRATION ||
		priv->tx_state == EN7581_XGSPON_TX_REGISTRATION_QUEUED) &&
	       en7581_xgspon_phy_trusted_locked(priv) &&
	       priv->hardware_state_valid && priv->session_keys_set &&
	       priv->session_keys_programmed && priv->onu_id_set &&
	       priv->ranging_time_authenticated &&
	       priv->equalization_delay_programmed &&
	       FIELD_GET(EN7581_XGSPON_ACTIVATION_STATE,
			 readl(priv->mac + EN7581_XGSPON_ACTIVATION_ST)) ==
			 EN7581_XGSPON_ACTIVATION_O5 &&
	       (readl(priv->mac + EN7581_XGSPON_CUR_KIDX) &
		EN7581_XGSPON_CUR_PIK_IDX);
}

/* The SDK accepts directed Profile updates in O4 and O5. Their ACK uses the
 * PLOAM key selected by hardware at transmission time, including the brief
 * pre-Registration interval in which the default key can still be active. */
static bool en7581_xgspon_profile_ack_tx_ready_locked(
	struct en7581_xgspon *priv, u8 key_index)
{
	u32 activation_state, current_index;

	if (key_index > AIROHA_XGS_PLOAM_KEY_INDEX_1 || !priv->tx_armed ||
	    !en7581_xgspon_phy_trusted_locked(priv) ||
	    !priv->hardware_state_valid || !priv->session_keys_set ||
	    !priv->session_keys_programmed || !priv->onu_id_set)
		return false;

	activation_state = FIELD_GET(
		EN7581_XGSPON_ACTIVATION_STATE,
		readl(priv->mac + EN7581_XGSPON_ACTIVATION_ST));
	if (activation_state != EN7581_XGSPON_ACTIVATION_O4 &&
	    activation_state != EN7581_XGSPON_ACTIVATION_O5)
		return false;
	current_index = !!(readl(priv->mac + EN7581_XGSPON_CUR_KIDX) &
			  EN7581_XGSPON_CUR_PIK_IDX);
	if (current_index != key_index)
		return false;

	if (priv->tx_state == EN7581_XGSPON_TX_WAIT_REGISTRATION ||
	    priv->tx_state == EN7581_XGSPON_TX_REGISTRATION_QUEUED)
		return !priv->tx_authorized;

	return activation_state == EN7581_XGSPON_ACTIVATION_O5 &&
	       priv->tx_state == EN7581_XGSPON_TX_REGISTERED &&
	       priv->tx_authorized && priv->registration_key_switch_verified &&
	       key_index == AIROHA_XGS_PLOAM_KEY_INDEX_0;
}

static bool en7581_xgspon_ack_key_ready_locked(
	struct en7581_xgspon *priv, u8 source_message_id, u8 key_index)
{
	if (source_message_id == EN7581_XGSPON_PROFILE_MESSAGE_ID)
		return en7581_xgspon_profile_ack_tx_ready_locked(priv,
							      key_index);
	if (key_index == AIROHA_XGS_PLOAM_KEY_INDEX_0)
		return en7581_xgspon_ack_tx_ready_locked(priv);

	return key_index == AIROHA_XGS_PLOAM_KEY_INDEX_1 &&
	       source_message_id == EN7581_XGSPON_RANGING_TIME_MESSAGE_ID &&
	       en7581_xgspon_ranging_ack_tx_ready_locked(priv);
}

static bool en7581_xgspon_key_report_matches(
	const struct en7581_xgspon_key_report *report, u64 generation,
	u64 exchange_generation, u8 sequence, u8 report_type, u8 key_index,
	const u8 *fragment)
{
	return report->generation == generation &&
	       report->exchange_generation == exchange_generation &&
	       report->sequence == sequence &&
	       report->report_type == report_type &&
	       report->key_index == key_index &&
	       !crypto_memneq(report->fragment, fragment,
			       sizeof(report->fragment));
}

static int en7581_xgspon_schedule_key_report_locked(
	struct en7581_xgspon *priv, u8 sequence, u8 report_type, u8 key_index,
	const u8 fragment[AIROHA_XGS_KEY_REPORT_FRAGMENT_SIZE])
{
	struct en7581_xgspon_key_report *report;
	u8 index;
	int i;

	if (!en7581_xgspon_ack_tx_ready_locked(priv))
		return -EAGAIN;
	if (priv->key_report_in_flight &&
	    en7581_xgspon_key_report_matches(&priv->active_key_report,
		priv->session_generation, priv->data_key_exchange_generation,
		sequence, report_type, key_index,
		fragment)) {
		priv->key_report_coalesced_events++;
		return 0;
	}
	for (i = 0; i < priv->key_report_count; i++) {
		index = (priv->key_report_head + i) %
			EN7581_XGSPON_KEY_REPORT_QUEUE_DEPTH;
		if (en7581_xgspon_key_report_matches(
			&priv->key_report_queue[index], priv->session_generation,
			priv->data_key_exchange_generation, sequence, report_type,
			key_index, fragment)) {
			priv->key_report_coalesced_events++;
			return 0;
		}
	}
	if (priv->key_report_count == EN7581_XGSPON_KEY_REPORT_QUEUE_DEPTH) {
		priv->key_report_dropped_events++;
		return -ENOSPC;
	}
	index = (priv->key_report_head + priv->key_report_count) %
		EN7581_XGSPON_KEY_REPORT_QUEUE_DEPTH;
	report = &priv->key_report_queue[index];
	report->generation = priv->session_generation;
	report->exchange_generation = priv->data_key_exchange_generation;
	report->sequence = sequence;
	report->report_type = report_type;
	report->key_index = key_index;
	memcpy(report->fragment, fragment, sizeof(report->fragment));
	priv->key_report_count++;
	priv->key_report_queued_events++;
	if (priv->enabled)
		mod_delayed_work(system_wq, &priv->activation_work, 0);
	return 0;
}

static void en7581_xgspon_drop_queued_key_reports_locked(
	struct en7581_xgspon *priv, u64 exchange_generation)
{
	struct en7581_xgspon_key_report retained[
		EN7581_XGSPON_KEY_REPORT_QUEUE_DEPTH] = {};
	u8 retained_count = 0;
	u8 index;
	int i;

	for (i = 0; i < priv->key_report_count; i++) {
		index = (priv->key_report_head + i) %
			EN7581_XGSPON_KEY_REPORT_QUEUE_DEPTH;
		if (priv->key_report_queue[index].exchange_generation !=
		    exchange_generation)
			retained[retained_count++] = priv->key_report_queue[index];
	}
	memzero_explicit(priv->key_report_queue,
			 sizeof(priv->key_report_queue));
	memcpy(priv->key_report_queue, retained,
	       retained_count * sizeof(retained[0]));
	priv->key_report_head = 0;
	priv->key_report_count = retained_count;
	memzero_explicit(retained, sizeof(retained));
}

static bool en7581_xgspon_ack_already_scheduled_locked(
	struct en7581_xgspon *priv, u8 source_message_id, u8 sequence,
	u8 completion_code, u8 key_index)
{
	u8 index;
	int i;

	if (priv->ack_in_flight &&
	    en7581_xgspon_ack_matches(&priv->active_ack,
				       priv->session_generation,
				       source_message_id, sequence,
				       completion_code, key_index))
		return true;
	for (i = 0; i < priv->ack_count; i++) {
		index = (priv->ack_head + i) % EN7581_XGSPON_ACK_QUEUE_DEPTH;
		if (en7581_xgspon_ack_matches(&priv->ack_queue[index],
					       priv->session_generation,
					       source_message_id, sequence,
					       completion_code, key_index))
			return true;
	}

	return false;
}

static int en7581_xgspon_ack_schedule_status_locked(
	struct en7581_xgspon *priv, u8 source_message_id, u8 sequence,
	u8 completion_code, u8 key_index)
{
	if (!en7581_xgspon_ack_key_ready_locked(priv, source_message_id,
						 key_index))
		return -EAGAIN;
	if (en7581_xgspon_ack_already_scheduled_locked(
		priv, source_message_id, sequence, completion_code, key_index))
		return 0;
	if (priv->ack_count == EN7581_XGSPON_ACK_QUEUE_DEPTH)
		return -ENOSPC;

	return 0;
}

static int en7581_xgspon_commit_ack_locked(
	struct en7581_xgspon *priv, u8 source_message_id, u8 sequence,
	u8 completion_code, u8 key_index)
{
	struct en7581_xgspon_ack *ack;
	u8 index;

	if (en7581_xgspon_ack_already_scheduled_locked(
		priv, source_message_id, sequence, completion_code, key_index)) {
		priv->acknowledge_coalesced_events++;
		return 0;
	}
	if (priv->ack_count == EN7581_XGSPON_ACK_QUEUE_DEPTH) {
		priv->acknowledge_dropped_events++;
		return -ENOSPC;
	}

	index = (priv->ack_head + priv->ack_count) %
		EN7581_XGSPON_ACK_QUEUE_DEPTH;
	ack = &priv->ack_queue[index];
	ack->generation = priv->session_generation;
	ack->source_message_id = source_message_id;
	ack->sequence = sequence;
	ack->completion_code = completion_code;
	ack->key_index = key_index;
	priv->ack_count++;
	priv->acknowledge_queued_events++;
	if (priv->enabled)
		mod_delayed_work(system_wq, &priv->activation_work, 0);

	return 0;
}

static int en7581_xgspon_schedule_ack_with_key_locked(
	struct en7581_xgspon *priv, u8 source_message_id, u8 sequence,
	u8 completion_code, u8 key_index)
{
	int ret;

	ret = en7581_xgspon_ack_schedule_status_locked(
		priv, source_message_id, sequence, completion_code, key_index);
	if (ret) {
		if (ret == -ENOSPC)
			priv->acknowledge_dropped_events++;
		return ret;
	}

	return en7581_xgspon_commit_ack_locked(
		priv, source_message_id, sequence, completion_code, key_index);
}

static int en7581_xgspon_schedule_ack_locked(struct en7581_xgspon *priv,
					      u8 source_message_id,
					      u8 sequence,
					      u8 completion_code)
{
	u8 key_index;

	if (en7581_xgspon_ack_tx_ready_locked(priv))
		key_index = AIROHA_XGS_PLOAM_KEY_INDEX_0;
	else if (source_message_id == EN7581_XGSPON_RANGING_TIME_MESSAGE_ID &&
		 en7581_xgspon_ranging_ack_tx_ready_locked(priv))
		key_index = AIROHA_XGS_PLOAM_KEY_INDEX_1;
	else
		return -EAGAIN;

	return en7581_xgspon_schedule_ack_with_key_locked(
		priv, source_message_id, sequence, completion_code, key_index);
}

static void en7581_xgspon_free_omcc_ready_queue(struct list_head *queue)
{
	struct en7581_xgspon_omcc_ready *ready, *next;

	list_for_each_entry_safe(ready, next, queue, list) {
		memzero_explicit(ready->record, ready->size);
		kfree(ready);
	}
}

static void en7581_xgspon_pm_read_hardware_locked(
	struct en7581_xgspon *priv, u32 values[EN7581_XGSPON_PM_HW_COUNTERS])
{
	u32 hlend, alloc, header, phy, mic;

	memset(values, 0, sizeof(u32) * EN7581_XGSPON_PM_HW_COUNTERS);
	if (!priv->hardware_selected || !priv->mac)
		return;

	hlend = readl(priv->mac + EN7581_XGSPON_RX_HLEND_HEC_CNT);
	alloc = readl(priv->mac + EN7581_XGSPON_RX_ALLOC_HEC_CNT);
	header = readl(priv->mac + EN7581_XGSPON_RX_HDR_HEC_CNT);
	phy = readl(priv->mac + EN7581_XGSPON_RX_PHY_HEC_ERR_CNT);
	mic = readl(priv->mac + EN7581_XGSPON_RX_MIC_ERR_CNT);
	values[EN7581_XGSPON_PM_HLEND_HEC_1] =
		FIELD_GET(EN7581_XGSPON_HEC_1ERR, hlend);
	values[EN7581_XGSPON_PM_HLEND_HEC_2] =
		FIELD_GET(EN7581_XGSPON_HEC_2ERR, hlend);
	values[EN7581_XGSPON_PM_HLEND_HEC_3] =
		FIELD_GET(EN7581_XGSPON_HEC_3ERR, hlend);
	values[EN7581_XGSPON_PM_ALLOC_HEC_1] =
		FIELD_GET(EN7581_XGSPON_HEC_1ERR, alloc);
	values[EN7581_XGSPON_PM_ALLOC_HEC_2] =
		FIELD_GET(EN7581_XGSPON_HEC_2ERR, alloc);
	values[EN7581_XGSPON_PM_ALLOC_HEC_3] =
		FIELD_GET(EN7581_XGSPON_HEC_3ERR, alloc);
	values[EN7581_XGSPON_PM_HEADER_HEC_1] =
		FIELD_GET(EN7581_XGSPON_HEC_1ERR, header);
	values[EN7581_XGSPON_PM_HEADER_HEC_2] =
		FIELD_GET(EN7581_XGSPON_HEC_2ERR, header);
	values[EN7581_XGSPON_PM_HEADER_HEC_3] =
		FIELD_GET(EN7581_XGSPON_HEC_3ERR, header);
	values[EN7581_XGSPON_PM_PHY_SFC_HEC] =
		FIELD_GET(EN7581_XGSPON_PHY_SFC_HEC_ERR, phy);
	values[EN7581_XGSPON_PM_PHY_PON_ID_HEC] =
		FIELD_GET(EN7581_XGSPON_PHY_PON_ID_HEC_ERR, phy);
	values[EN7581_XGSPON_PM_UNKNOWN_PROFILES] =
		readl(priv->mac + EN7581_XGSPON_INVLD_PROF_BST_GNT_CNT);
	values[EN7581_XGSPON_PM_TX_XGEM] =
		readl(priv->mac + EN7581_XGSPON_TX_XGEM_CNT);
	values[EN7581_XGSPON_PM_FRAGMENT_XGEM] =
		readl(priv->mac + EN7581_XGSPON_TX_NLF_XGEM_CNT);
	values[EN7581_XGSPON_PM_XGEM_LOST_WORDS] =
		readl(priv->mac + EN7581_XGSPON_RX_LOST_WCNT);
	values[EN7581_XGSPON_PM_XGEM_KEY_ERRORS] =
		readl(priv->mac + EN7581_XGSPON_RX_KEY_ERR_CNT);
	values[EN7581_XGSPON_PM_TX_NON_IDLE_BYTES] =
		readl(priv->mac + EN7581_XGSPON_TX_NON_IDLE_BCNT);
	values[EN7581_XGSPON_PM_RX_NON_IDLE_BYTES] =
		readl(priv->mac + EN7581_XGSPON_RX_NON_IDLE_BCNT);
	values[EN7581_XGSPON_PM_PLOAM_MIC_ERRORS] =
		FIELD_GET(EN7581_XGSPON_PLOAM_MIC_ERR, mic);
	values[EN7581_XGSPON_PM_RX_PLOAM] =
		readl(priv->mac + EN7581_XGSPON_RX_PLOAMD_CNT);
	values[EN7581_XGSPON_PM_OMCI_MIC_ERRORS] =
		FIELD_GET(EN7581_XGSPON_OMCI_MIC_ERR, mic);
	values[EN7581_XGSPON_PM_TX_PLOAM] =
		readl(priv->mac + EN7581_XGSPON_TX_PLOAMU_CNT);
}

static void en7581_xgspon_pm_extend(
	struct en7581_xgspon_pm_extender *counter, u32 value)
{
	value &= counter->mask;
	if (!counter->initialized) {
		counter->previous = value;
		counter->initialized = true;
		return;
	}
	counter->total += (value - counter->previous) & counter->mask;
	counter->previous = value;
}

static bool en7581_xgspon_pm_extender_selftest(void)
{
	struct en7581_xgspon_pm_extender counter = { .mask = U8_MAX };

	en7581_xgspon_pm_extend(&counter, 250);
	if (counter.total || counter.previous != 250 || !counter.initialized)
		return false;
	en7581_xgspon_pm_extend(&counter, 5);
	if (counter.total != 11 || counter.previous != 5)
		return false;
	counter.session_base = counter.total;
	en7581_xgspon_pm_extend(&counter, 9);
	return counter.total - counter.session_base == 4;
}

static void en7581_xgspon_pm_update_hardware_locked(
	struct en7581_xgspon *priv)
{
	u32 values[EN7581_XGSPON_PM_HW_COUNTERS];
	struct en7581_xgspon_pm_extender *counter;
	unsigned int i;

	en7581_xgspon_pm_read_hardware_locked(priv, values);
	for (i = 0; i < EN7581_XGSPON_PM_HW_COUNTERS; i++) {
		counter = &priv->pm_hw[i];
		en7581_xgspon_pm_extend(counter, values[i]);
	}
}

static void en7581_xgspon_pm_reset_session_locked(
	struct en7581_xgspon *priv)
{
	unsigned int i;

	en7581_xgspon_pm_update_hardware_locked(priv);
	for (i = 0; i < EN7581_XGSPON_PM_HW_COUNTERS; i++)
		priv->pm_hw[i].session_base = priv->pm_hw[i].total;
	priv->pm_profile_messages = 0;
	priv->pm_assign_onu_id_messages = 0;
	priv->pm_ranging_time_messages = 0;
	priv->pm_deactivate_onu_id_messages = 0;
	priv->pm_disable_serial_number_messages = 0;
	priv->pm_request_registration_messages = 0;
	priv->pm_assign_alloc_id_messages = 0;
	priv->pm_key_control_messages = 0;
	priv->pm_sleep_allow_messages = 0;
	priv->pm_serial_number_messages = 0;
	priv->pm_registration_messages = 0;
	priv->pm_key_report_messages = 0;
	priv->pm_acknowledge_messages = 0;
	memset(&priv->pm_lods, 0, sizeof(priv->pm_lods));
}

static u64 en7581_xgspon_pm_hardware_value(
	const struct en7581_xgspon *priv, enum en7581_xgspon_pm_hw_counter index)
{
	return priv->pm_hw[index].total - priv->pm_hw[index].session_base;
}

static void en7581_xgspon_pm_init(struct en7581_xgspon *priv)
{
	unsigned int i;

	for (i = EN7581_XGSPON_PM_HLEND_HEC_1;
	     i <= EN7581_XGSPON_PM_HEADER_HEC_3; i++)
		priv->pm_hw[i].mask = U8_MAX;
	priv->pm_hw[EN7581_XGSPON_PM_PHY_SFC_HEC].mask = U16_MAX;
	priv->pm_hw[EN7581_XGSPON_PM_PHY_PON_ID_HEC].mask = U16_MAX;
	for (i = EN7581_XGSPON_PM_UNKNOWN_PROFILES;
	     i <= EN7581_XGSPON_PM_RX_NON_IDLE_BYTES; i++)
		priv->pm_hw[i].mask = U32_MAX;
	priv->pm_hw[EN7581_XGSPON_PM_PLOAM_MIC_ERRORS].mask = U16_MAX;
	priv->pm_hw[EN7581_XGSPON_PM_RX_PLOAM].mask = U32_MAX;
	priv->pm_hw[EN7581_XGSPON_PM_OMCI_MIC_ERRORS].mask = U16_MAX;
	priv->pm_hw[EN7581_XGSPON_PM_TX_PLOAM].mask = U32_MAX;
}

static void en7581_xgspon_pm_record_accepted_locked(
	struct en7581_xgspon *priv, u8 message_id)
{
	switch (message_id) {
	case EN7581_XGSPON_PROFILE_MESSAGE_ID:
		priv->pm_profile_messages++;
		break;
	case EN7581_XGSPON_ASSIGN_ONU_ID_MESSAGE_ID:
		priv->pm_assign_onu_id_messages++;
		break;
	case EN7581_XGSPON_RANGING_TIME_MESSAGE_ID:
		priv->pm_ranging_time_messages++;
		break;
	case EN7581_XGSPON_DEACTIVATE_ONU_ID_MESSAGE_ID:
		priv->pm_deactivate_onu_id_messages++;
		break;
	case EN7581_XGSPON_DISABLE_SERIAL_NUMBER_MESSAGE_ID:
		priv->pm_disable_serial_number_messages++;
		break;
	case EN7581_XGSPON_REQUEST_REGISTRATION_MESSAGE_ID:
		priv->pm_request_registration_messages++;
		break;
	case EN7581_XGSPON_ASSIGN_ALLOC_ID_MESSAGE_ID:
		priv->pm_assign_alloc_id_messages++;
		break;
	case AIROHA_XGS_KEY_CONTROL_MESSAGE_ID:
		priv->pm_key_control_messages++;
		break;
	case EN7581_XGSPON_SLEEP_ALLOW_MESSAGE_ID:
		priv->pm_sleep_allow_messages++;
		break;
	}
}

static void en7581_xgspon_pm_work(struct work_struct *work)
{
	struct en7581_xgspon *priv = container_of(
		to_delayed_work(work), struct en7581_xgspon, pm_work);
	bool reschedule;

	mutex_lock(&priv->lock);
	reschedule = !priv->removing && priv->hardware_selected && priv->mac;
	if (reschedule)
		en7581_xgspon_pm_update_hardware_locked(priv);
	mutex_unlock(&priv->lock);
	if (reschedule)
		mod_delayed_work(system_wq, &priv->pm_work,
			msecs_to_jiffies(EN7581_XGSPON_PM_SAMPLE_MS));
}

static void en7581_xgspon_advance_session_locked(
	struct en7581_xgspon *priv)
{
	struct en7581_xgspon_omcc_rx *rx, *next;
	u32 dropped_pending, dropped_ready;
	LIST_HEAD(pending_queue);
	LIST_HEAD(ready_queue);
	unsigned long flags;

	spin_lock_irqsave(&priv->omcc_rx_lock, flags);
	priv->session_generation++;
	list_splice_init(&priv->omcc_rx_queue, &pending_queue);
	dropped_pending = priv->omcc_rx_queue_depth;
	priv->omcc_rx_queue_depth = 0;
	list_splice_init(&priv->omcc_ready_queue, &ready_queue);
	dropped_ready = priv->omcc_ready_queue_depth;
	priv->omcc_ready_queue_depth = 0;
	spin_unlock_irqrestore(&priv->omcc_rx_lock, flags);
	en7581_xgspon_pm_reset_session_locked(priv);
	priv->ack_head = 0;
	priv->ack_count = 0;
	priv->ack_in_flight = false;
	memset(priv->ack_queue, 0, sizeof(priv->ack_queue));
	memset(&priv->active_ack, 0, sizeof(priv->active_ack));
	priv->key_report_head = 0;
	priv->key_report_count = 0;
	priv->key_report_in_flight = false;
	memzero_explicit(priv->key_report_queue,
			 sizeof(priv->key_report_queue));
	memzero_explicit(&priv->active_key_report,
			 sizeof(priv->active_key_report));

	list_for_each_entry_safe(rx, next, &pending_queue, list) {
		dev_kfree_skb_any(rx->skb);
		kfree(rx);
	}
	en7581_xgspon_free_omcc_ready_queue(&ready_queue);
	if (dropped_pending || dropped_ready)
		atomic64_add(dropped_pending + dropped_ready,
			     &priv->omcc_rx_dropped_session_reset);
	wake_up_interruptible(&priv->omcc_rx_wait);
}

static struct en7581_xgspon_omcc_ready *
en7581_xgspon_build_omcc_record(const u8 *content, size_t content_len,
				 u64 instance_generation, u64 generation)
{
	struct en7581_xgspon_omcc_ready *ready;
	struct airoha_xgs_omcc_record *record;
	size_t record_size;

	if (!content || content_len < 4 ||
	    content_len > AIROHA_XGS_OMCC_MAX_CONTENTS)
		return NULL;
	record_size = sizeof(*record) + content_len;
	ready = kmalloc(sizeof(*ready) + record_size, GFP_KERNEL);
	if (!ready)
		return NULL;

	ready->generation = generation;
	ready->size = record_size;
	record = (struct airoha_xgs_omcc_record *)ready->record;
	record->magic = cpu_to_be32(AIROHA_XGS_OMCC_MAGIC);
	record->abi_version = AIROHA_XGS_OMCC_ABI_VERSION;
	record->direction = AIROHA_XGS_OMCC_DIRECTION_RX;
	record->flags = cpu_to_be16(AIROHA_XGS_OMCC_FLAG_MIC_VERIFIED |
				    AIROHA_XGS_OMCC_FLAG_TRAILER_STRIPPED);
	record->length = cpu_to_be16(content_len);
	record->reserved = 0;
	record->instance_generation = cpu_to_be64(instance_generation);
	record->session_generation = cpu_to_be64(generation);
	memcpy(record->contents, content, content_len);

	return ready;
}

static void en7581_xgspon_process_omcc_rx(
	struct en7581_xgspon *priv, struct en7581_xgspon_omcc_rx *rx)
{
	struct en7581_xgspon_omcc_ready *ready = NULL;
	u8 omci_key[AIROHA_XGS_KEY_SIZE];
	size_t content_len = 0;
	u64 generation = 0;
	unsigned long flags;
	bool delivered = false;
	bool ready_full = false;
	int ret;

	memset(omci_key, 0, sizeof(omci_key));
	mutex_lock(&priv->lock);
	if (rx->generation != priv->session_generation) {
		atomic64_inc(&priv->omcc_rx_dropped_stale);
		goto unlock;
	}
	if (rx->metadata.xgem_id != priv->onu_id && priv->onu_id_set) {
		atomic64_inc(&priv->omcc_rx_dropped_wrong_xgem);
		goto unlock;
	}
	if (!en7581_xgspon_omcc_session_ready(priv,
					      rx->metadata.xgem_id)) {
		atomic64_inc(&priv->omcc_rx_dropped_no_session);
		goto unlock;
	}
	if (!rx->metadata.no_mic && !priv->hardware_ds_omci_mic) {
		atomic64_inc(&priv->omcc_rx_dropped_invalid);
		goto unlock;
	}
	memcpy(omci_key, priv->session_keys.omci, sizeof(omci_key));
	generation = rx->generation;
	mutex_unlock(&priv->lock);

	ret = skb_linearize(rx->skb);
	if (ret) {
		atomic64_inc(&priv->omcc_rx_dropped_invalid);
		goto out;
	}
	ret = airoha_xgs_omci_content_length(rx->skb->data, rx->skb->len,
					     &content_len);
	if (ret) {
		atomic64_inc(&priv->omcc_rx_dropped_invalid);
		goto out;
	}

	if (rx->metadata.no_mic) {
		if (rx->skb->len != content_len + AIROHA_XGS_OMCI_MIC_SIZE) {
			atomic64_inc(&priv->omcc_rx_dropped_invalid);
			goto out;
		}
		ret = airoha_xgs_authenticate_downstream_omci(
			omci_key, rx->skb->data, rx->skb->len, rx->skb->data,
			rx->skb->len, &content_len);
		if (ret) {
			if (ret == -EBADMSG)
				atomic64_inc(&priv->omcc_rx_dropped_mic);
			else
				atomic64_inc(&priv->omcc_rx_dropped_invalid);
			goto out;
		}
	}

	ready = en7581_xgspon_build_omcc_record(rx->skb->data, content_len,
					       priv->instance_generation,
					       generation);
	if (!ready) {
		atomic64_inc(&priv->omcc_rx_dropped_ready_alloc);
		goto out;
	}

	mutex_lock(&priv->lock);
	if (generation != priv->session_generation ||
	    !en7581_xgspon_omcc_session_ready(priv,
					      rx->metadata.xgem_id)) {
		atomic64_inc(&priv->omcc_rx_dropped_stale);
	} else if (rx->metadata.no_mic) {
		atomic64_inc(&priv->omcc_rx_sw_verified);
		spin_lock_irqsave(&priv->omcc_rx_lock, flags);
		if (priv->omcc_ready_queue_depth <
		    EN7581_XGSPON_OMCC_READY_QUEUE_LIMIT) {
			list_add_tail(&ready->list, &priv->omcc_ready_queue);
			priv->omcc_ready_queue_depth++;
			delivered = true;
		} else
			ready_full = true;
		spin_unlock_irqrestore(&priv->omcc_rx_lock, flags);
	} else if (priv->hardware_ds_omci_mic) {
		atomic64_inc(&priv->omcc_rx_hw_verified);
		spin_lock_irqsave(&priv->omcc_rx_lock, flags);
		if (priv->omcc_ready_queue_depth <
		    EN7581_XGSPON_OMCC_READY_QUEUE_LIMIT) {
			list_add_tail(&ready->list, &priv->omcc_ready_queue);
			priv->omcc_ready_queue_depth++;
			delivered = true;
		} else
			ready_full = true;
		spin_unlock_irqrestore(&priv->omcc_rx_lock, flags);
	} else {
		atomic64_inc(&priv->omcc_rx_dropped_stale);
	}
	mutex_unlock(&priv->lock);
	if (delivered) {
		atomic64_inc(&priv->omcc_rx_ready_queued);
		wake_up_interruptible(&priv->omcc_rx_wait);
		ready = NULL;
	} else if (ready_full) {
		atomic64_inc(&priv->omcc_rx_dropped_ready_full);
	}
	goto out;

unlock:
	mutex_unlock(&priv->lock);
out:
	if (ready) {
		memzero_explicit(ready->record, ready->size);
		kfree(ready);
	}
	memzero_explicit(omci_key, sizeof(omci_key));
	dev_kfree_skb_any(rx->skb);
}

static void en7581_xgspon_omcc_rx_work(struct work_struct *work)
{
	struct en7581_xgspon *priv = container_of(
		work, struct en7581_xgspon, omcc_rx_work);
	struct en7581_xgspon_omcc_rx *rx;
	unsigned long flags;

	for (;;) {
		spin_lock_irqsave(&priv->omcc_rx_lock, flags);
		if (list_empty(&priv->omcc_rx_queue)) {
			spin_unlock_irqrestore(&priv->omcc_rx_lock, flags);
			break;
		}
		rx = list_first_entry(&priv->omcc_rx_queue,
				      struct en7581_xgspon_omcc_rx, list);
		list_del(&rx->list);
		priv->omcc_rx_queue_depth--;
		spin_unlock_irqrestore(&priv->omcc_rx_lock, flags);

		en7581_xgspon_process_omcc_rx(priv, rx);
		kfree(rx);
	}
}

static void en7581_xgspon_omcc_receive(
	void *context, struct sk_buff *skb,
	const struct airoha_xgs_omcc_metadata *metadata)
{
	struct en7581_xgspon *priv = context;
	struct en7581_xgspon_omcc_rx *rx;
	unsigned long flags;
	bool queued = false;
	bool removing = false;

	if (READ_ONCE(priv->removing))
		goto drop_removing;

	rx = kmalloc(sizeof(*rx), GFP_ATOMIC);
	if (!rx)
		goto drop;
	rx->skb = skb;
	rx->metadata = *metadata;

	spin_lock_irqsave(&priv->omcc_rx_lock, flags);
	if (priv->removing) {
		removing = true;
	} else if (priv->omcc_rx_queue_depth <
		   EN7581_XGSPON_OMCC_RX_QUEUE_LIMIT) {
		rx->generation = priv->session_generation;
		list_add_tail(&rx->list, &priv->omcc_rx_queue);
		priv->omcc_rx_queue_depth++;
		queued = true;
	}
	spin_unlock_irqrestore(&priv->omcc_rx_lock, flags);
	if (!queued) {
		kfree(rx);
		if (removing)
			goto drop_removing;
		goto drop;
	}

	atomic64_inc(&priv->omcc_rx_queued);
	schedule_work(&priv->omcc_rx_work);
	return;

drop:
	atomic64_inc(&priv->omcc_rx_dropped_queue_full);
	dev_kfree_skb_any(skb);
	return;

drop_removing:
	atomic64_inc(&priv->omcc_rx_dropped_removing);
	dev_kfree_skb_any(skb);
}

static const struct airoha_xgs_omcc_ops en7581_xgspon_omcc_ops = {
	.receive = en7581_xgspon_omcc_receive,
};

static int en7581_xgspon_attach_omcc(struct en7581_xgspon *priv)
{
	int ret;

	if (priv->omcc_consumer_registered)
		return 0;
	ret = airoha_xgs_omcc_register(priv->ethernet_np,
				       &en7581_xgspon_omcc_ops, priv);
	if (!ret)
		priv->omcc_consumer_registered = true;
	return ret;
}

static void en7581_xgspon_purge_omcc_rx(struct en7581_xgspon *priv)
{
	struct en7581_xgspon_omcc_rx *rx, *next;
	LIST_HEAD(queue);
	unsigned long flags;

	cancel_work_sync(&priv->omcc_rx_work);
	spin_lock_irqsave(&priv->omcc_rx_lock, flags);
	list_splice_init(&priv->omcc_rx_queue, &queue);
	priv->omcc_rx_queue_depth = 0;
	spin_unlock_irqrestore(&priv->omcc_rx_lock, flags);
	list_for_each_entry_safe(rx, next, &queue, list) {
		dev_kfree_skb_any(rx->skb);
		kfree(rx);
	}
}

static void en7581_xgspon_detach_omcc(void *data)
{
	struct en7581_xgspon *priv = data;

	if (priv->omcc_consumer_registered) {
		airoha_xgs_omcc_unregister(priv->ethernet_np, priv);
		priv->omcc_consumer_registered = false;
	}
	en7581_xgspon_purge_omcc_rx(priv);
}

static int en7581_xgspon_tx_transition(enum en7581_xgspon_tx_state state,
				       bool armed, u32 interrupt_status,
				       u32 fifo_error_status,
				       u32 tx_error_status, u32 current_index,
				       enum en7581_xgspon_tx_state *next,
				       bool *key_switch_verified)
{
	if (!next || !key_switch_verified)
		return -EINVAL;
	*next = state;
	*key_switch_verified = false;

	if (!(interrupt_status & EN7581_XGSPON_INT_UPSTREAM_EVENTS))
		return 0;
	/* Serial Number and Registration are allowed while tx_armed is set.
	 * tx_authorized is deliberately asserted only after Registration has
	 * completed and both hardware key selectors read back as slot 0.
	 */
	if (!armed ||
	    (interrupt_status & (EN7581_XGSPON_INT_FIFO_ERROR |
				 EN7581_XGSPON_INT_TX_ERROR)) ||
	    fifo_error_status || tx_error_status)
		goto fault;
	if ((interrupt_status & EN7581_XGSPON_INT_US_NO_MESSAGE) &&
	    (state == EN7581_XGSPON_TX_SERIAL_QUEUED ||
	     state == EN7581_XGSPON_TX_REGISTRATION_QUEUED))
		goto fault;
	if (interrupt_status & EN7581_XGSPON_INT_O23_SN_SENT) {
		if (state != EN7581_XGSPON_TX_SERIAL_QUEUED)
			goto fault;
		state = EN7581_XGSPON_TX_WAIT_REGISTRATION;
	}
	/* The SDK uses the O4 Ranging Request event to synchronize its software
	 * key indices with the MAC. Both integrity-key selectors must have moved
	 * from the default slot before this becomes authorization evidence. */
	if (interrupt_status & EN7581_XGSPON_INT_O4_RANGING_REQUEST) {
		if ((state != EN7581_XGSPON_TX_REGISTRATION_QUEUED &&
		     state != EN7581_XGSPON_TX_REGISTERED) ||
		    (current_index & (EN7581_XGSPON_CUR_PIK_IDX |
				      EN7581_XGSPON_CUR_OIK_IDX)))
			goto fault;
		*key_switch_verified = true;
	}
	if (interrupt_status & EN7581_XGSPON_INT_O4_REGISTRATION_SENT) {
		if ((state != EN7581_XGSPON_TX_REGISTRATION_QUEUED &&
		     state != EN7581_XGSPON_TX_REGISTERED) ||
		    (current_index & (EN7581_XGSPON_CUR_PIK_IDX |
				      EN7581_XGSPON_CUR_OIK_IDX)))
			goto fault;
		state = EN7581_XGSPON_TX_REGISTERED;
		*key_switch_verified = true;
	}

	*next = state;
	return 0;

fault:
	*next = EN7581_XGSPON_TX_FAULT;
	return -EPROTO;
}

static bool en7581_xgspon_tx_state_selftest(void)
{
	enum en7581_xgspon_tx_state next;
	bool key_switch_verified;
	int ret;

	ret = en7581_xgspon_tx_transition(
		EN7581_XGSPON_TX_SERIAL_QUEUED, true,
		EN7581_XGSPON_INT_O23_SN_SENT, 0, 0,
		EN7581_XGSPON_CUR_PIK_IDX | EN7581_XGSPON_CUR_OIK_IDX,
		&next, &key_switch_verified);
	if (ret || next != EN7581_XGSPON_TX_WAIT_REGISTRATION ||
	    key_switch_verified)
		return false;

	ret = en7581_xgspon_tx_transition(
		EN7581_XGSPON_TX_REGISTRATION_QUEUED, true,
		EN7581_XGSPON_INT_O4_RANGING_REQUEST, 0, 0, 0,
		&next, &key_switch_verified);
	if (ret || next != EN7581_XGSPON_TX_REGISTRATION_QUEUED ||
	    !key_switch_verified)
		return false;

	ret = en7581_xgspon_tx_transition(
		EN7581_XGSPON_TX_REGISTRATION_QUEUED, true,
		EN7581_XGSPON_INT_O4_REGISTRATION_SENT, 0, 0, 0,
		&next, &key_switch_verified);
	if (ret || next != EN7581_XGSPON_TX_REGISTERED ||
	    !key_switch_verified)
		return false;

	ret = en7581_xgspon_tx_transition(
		EN7581_XGSPON_TX_REGISTERED, true,
		EN7581_XGSPON_INT_O4_REGISTRATION_SENT, 0, 0, 0,
		&next, &key_switch_verified);
	if (ret || next != EN7581_XGSPON_TX_REGISTERED ||
	    !key_switch_verified)
		return false;

	ret = en7581_xgspon_tx_transition(
		EN7581_XGSPON_TX_REGISTRATION_QUEUED, true,
		EN7581_XGSPON_INT_O4_REGISTRATION_SENT, 0, 0,
		EN7581_XGSPON_CUR_PIK_IDX, &next, &key_switch_verified);
	if (ret != -EPROTO || next != EN7581_XGSPON_TX_FAULT ||
	    key_switch_verified)
		return false;

	ret = en7581_xgspon_tx_transition(
		EN7581_XGSPON_TX_SERIAL_QUEUED, false,
		EN7581_XGSPON_INT_O23_SN_SENT, 0, 0, 0,
		&next, &key_switch_verified);
	return ret == -EPROTO && next == EN7581_XGSPON_TX_FAULT &&
	       !key_switch_verified;
}

static bool en7581_xgspon_authorize_o5_locked(struct en7581_xgspon *priv)
{
	u32 current_index;

	if (priv->tx_authorized)
		return true;
	if (!priv->tx_armed ||
	    priv->tx_state != EN7581_XGSPON_TX_REGISTERED ||
	    !priv->registration_key_switch_verified ||
	    !priv->ranging_time_authenticated ||
	    !priv->equalization_delay_programmed ||
	    !en7581_xgspon_phy_trusted_locked(priv) ||
	    !priv->hardware_state_valid || !priv->session_keys_set ||
	    !priv->session_keys_programmed || !priv->onu_id_set ||
	    FIELD_GET(EN7581_XGSPON_ACTIVATION_STATE,
		      readl(priv->mac + EN7581_XGSPON_ACTIVATION_ST)) !=
			EN7581_XGSPON_ACTIVATION_O5)
		return false;

	current_index = readl(priv->mac + EN7581_XGSPON_CUR_KIDX);
	if (current_index & (EN7581_XGSPON_CUR_PIK_IDX |
			     EN7581_XGSPON_CUR_OIK_IDX))
		return false;

	priv->tx_authorized = true;
	return true;
}

static u32 en7581_xgspon_serial_random_delay(u32 random_delay_register)
{
	u32 maximum = FIELD_GET(EN7581_XGSPON_MAX_RANDOM_DELAY,
				 random_delay_register);

	return (maximum & EN7581_XGSPON_RANDOM_DELAY_WORDS) *
	       EN7581_XGSPON_RANDOM_DELAY_WORD_BITS;
}

static void en7581_xgspon_bosa_put(void *data)
{
	airoha_en7572_put(data);
}

static void en7581_xgspon_disable_tx(struct en7581_xgspon *priv)
{
	if (priv->bosa && airoha_en7572_set_tx_enabled(priv->bosa, false))
		airoha_en7572_emergency_disable(priv->bosa);
}

static void en7581_xgspon_of_node_put(void *data)
{
	of_node_put(data);
}

static int en7581_xgspon_board_mode(struct device *dev,
				    enum airoha_xpon_mode *initial_mode,
				    bool *selected)
{
	struct device_node *pcs_np;
	const struct airoha_xpon_mode_descriptor *descriptor;
	const char *mode;
	unsigned int candidate;
	int ret;

	pcs_np = of_parse_phandle(dev->of_node, "airoha,pcs", 0);
	if (!pcs_np)
		return dev_err_probe(dev, -EINVAL, "missing airoha,pcs phandle\n");
	ret = of_property_read_string(pcs_np, "airoha,pon-mode", &mode);
	of_node_put(pcs_np);
	if (ret)
		return dev_err_probe(dev, ret,
				     "PON PCS has no explicit airoha,pon-mode\n");
	for (candidate = 0; candidate < AIROHA_XPON_MODE_COUNT; candidate++) {
		descriptor = airoha_xpon_mode_descriptor(candidate);
		if (!strcmp(mode, descriptor->name)) {
			*initial_mode = candidate;
			*selected = candidate == AIROHA_XPON_MODE_XGPON ||
				    candidate == AIROHA_XPON_MODE_XGSPON;
			return 0;
		}
	}
	return dev_err_probe(dev, -EINVAL,
			     "unsupported board PON mode %s\n", mode);
}

static const u32
en7581_xgspon_mac_init_offsets[AIROHA_XGS_MAC_INIT_REGISTER_COUNT] = {
	[AIROHA_XGS_MAC_INIT_PLOAM_CONTROL] =
		EN7581_XGSPON_O23_O4_PLOAMU_CTRL,
	[AIROHA_XGS_MAC_INIT_ACTIVATION] = EN7581_XGSPON_ACTIVATION_ST,
	[AIROHA_XGS_MAC_INIT_RSP_TIME] = EN7581_XGSPON_RSP_TIME,
	[AIROHA_XGS_MAC_INIT_DS_FEC] = EN7581_XGSPON_DBG_CAP_SETTING1,
	[AIROHA_XGS_MAC_INIT_DEBUG_CAP] = EN7581_XGSPON_DBG_CAP_SETTING,
	[AIROHA_XGS_MAC_INIT_DYING_GASP] = EN7581_XGSPON_US_DYING_GASP_CTRL,
	[AIROHA_XGS_MAC_INIT_PLOAM_DROP] = EN7581_XGSPON_EPDROP_EN,
	[AIROHA_XGS_MAC_INIT_TX_RESYNC] = EN7581_XGSPON_DBG_RESYNC,
	[AIROHA_XGS_MAC_INIT_IDLE_GEM] = EN7581_XGSPON_IDLE_GEM_CTRL,
	[AIROHA_XGS_MAC_INIT_MIB] = EN7581_XGSPON_MIB_CTRL,
};

static int en7581_xgspon_mac_init_read(
	void *context, enum airoha_xgs_mac_init_register reg,
	unsigned int *value)
{
	struct en7581_xgspon *priv = context;

	if (reg >= AIROHA_XGS_MAC_INIT_REGISTER_COUNT)
		return -EINVAL;
	*value = readl(priv->mac + en7581_xgspon_mac_init_offsets[reg]);
	return 0;
}

static int en7581_xgspon_mac_init_update(
	void *context, enum airoha_xgs_mac_init_register reg,
	unsigned int mask, unsigned int value)
{
	struct en7581_xgspon *priv = context;
	u32 offset, register_value, expected;

	if (reg >= AIROHA_XGS_MAC_INIT_REGISTER_COUNT)
		return -EINVAL;
	offset = en7581_xgspon_mac_init_offsets[reg];
	register_value = readl(priv->mac + offset);
	expected = (register_value & ~mask) | (value & mask);
	writel(expected, priv->mac + offset);
	return (readl(priv->mac + offset) & mask) == (expected & mask) ?
		0 : -EIO;
}

static void en7581_xgspon_mac_init_fail_closed(void *context)
{
	struct en7581_xgspon *priv = context;

	priv->hardware_state_valid = false;
	priv->mac_initialized = false;
	priv->hardware_ds_omci_mic = false;
	writel(0, priv->mac + EN7581_XGSPON_INT_ENABLE);
	if (priv->bosa)
		airoha_en7572_emergency_disable(priv->bosa);
}

static const struct airoha_xgs_mac_init_ops en7581_xgspon_mac_init_ops = {
	.read = en7581_xgspon_mac_init_read,
	.update = en7581_xgspon_mac_init_update,
	.fail_closed = en7581_xgspon_mac_init_fail_closed,
};

static int en7581_xgspon_initialize_mac(struct en7581_xgspon *priv)
{
	return airoha_xgs_mac_init_transaction_mode(
		&en7581_xgspon_mac_init_ops, priv, priv->active_mode);
}

static u32 en7581_xgspon_reverse_word(const u8 *value, size_t size,
				      unsigned int word)
{
	return get_unaligned_be32(value + size - (word + 1) * sizeof(u32));
}

static u32 en7581_xgspon_key_word(const u8 key[AIROHA_XGS_KEY_SIZE],
				  unsigned int word)
{
	return en7581_xgspon_reverse_word(key, AIROHA_XGS_KEY_SIZE, word);
}

static u32 en7581_xgspon_pon_tag_word(
	const u8 pon_tag[AIROHA_XGS_PON_TAG_SIZE], unsigned int word)
{
	return get_unaligned_be32(pon_tag + AIROHA_XGS_PON_TAG_SIZE -
					(word + 1) * sizeof(u32));
}

static bool en7581_xgspon_register_layout_selftest(void)
{
	static const u8 key[AIROHA_XGS_KEY_SIZE] = {
		0x00, 0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07,
		0x08, 0x09, 0x0a, 0x0b, 0x0c, 0x0d, 0x0e, 0x0f,
	};
	static const u8 pon_tag[AIROHA_XGS_PON_TAG_SIZE] = {
		0x10, 0x11, 0x12, 0x13, 0x14, 0x15, 0x16, 0x17,
	};
	static const u8 registration_id[AIROHA_XGS_REGISTRATION_ID_SIZE] = {
		0x00, 0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07,
		0x08, 0x09, 0x0a, 0x0b, 0x0c, 0x0d, 0x0e, 0x0f,
		0x10, 0x11, 0x12, 0x13, 0x14, 0x15, 0x16, 0x17,
		0x18, 0x19, 0x1a, 0x1b, 0x1c, 0x1d, 0x1e, 0x1f,
		0x20, 0x21, 0x22, 0x23,
	};

	return en7581_xgspon_key_word(key, 0) == 0x0c0d0e0f &&
	       en7581_xgspon_key_word(key, 1) == 0x08090a0b &&
	       en7581_xgspon_key_word(key, 2) == 0x04050607 &&
	       en7581_xgspon_key_word(key, 3) == 0x00010203 &&
	       en7581_xgspon_pon_tag_word(pon_tag, 0) == 0x14151617 &&
	       en7581_xgspon_pon_tag_word(pon_tag, 1) == 0x10111213 &&
	       en7581_xgspon_reverse_word(registration_id,
					AIROHA_XGS_REGISTRATION_ID_SIZE, 0) ==
			0x20212223 &&
	       en7581_xgspon_reverse_word(registration_id,
					AIROHA_XGS_REGISTRATION_ID_SIZE, 8) ==
			0x00010203;
}

static int en7581_xgspon_program_serial(
	struct en7581_xgspon *priv,
	const u8 serial[AIROHA_XGS_SERIAL_SIZE])
{
	u32 vendor = get_unaligned_be32(serial);
	u32 vendor_serial = get_unaligned_be32(serial + sizeof(u32));

	if (!priv->hardware_selected || !priv->mac)
		return 0;
	writel(vendor, priv->mac + EN7581_XGSPON_VENDOR_ID);
	writel(vendor_serial, priv->mac + EN7581_XGSPON_VS_SN);
	if (readl(priv->mac + EN7581_XGSPON_VENDOR_ID) != vendor ||
	    readl(priv->mac + EN7581_XGSPON_VS_SN) != vendor_serial)
		return -EIO;

	return 0;
}

static int en7581_xgspon_program_registration_id(
	struct en7581_xgspon *priv,
	const u8 registration_id[AIROHA_XGS_REGISTRATION_ID_SIZE])
{
	unsigned int words = AIROHA_XGS_REGISTRATION_ID_SIZE / sizeof(u32);
	unsigned int i;

	if (!priv->hardware_selected || !priv->mac)
		return 0;
	for (i = 0; i < words; i++)
		writel(en7581_xgspon_reverse_word(registration_id,
						  AIROHA_XGS_REGISTRATION_ID_SIZE,
						  i),
		       priv->mac + EN7581_XGSPON_REGISTRATION_ID +
				i * sizeof(u32));
	for (i = 0; i < words; i++)
		if (readl(priv->mac + EN7581_XGSPON_REGISTRATION_ID +
			  i * sizeof(u32)) !=
		    en7581_xgspon_reverse_word(registration_id,
						AIROHA_XGS_REGISTRATION_ID_SIZE,
						i))
			return -EIO;

	return 0;
}

static void en7581_xgspon_restore_serial(
	struct en7581_xgspon *priv,
	const u8 serial[AIROHA_XGS_SERIAL_SIZE])
{
	if (en7581_xgspon_program_serial(priv, serial))
		dev_err(priv->dev, "failed to restore XGS-PON serial registers\n");
}

static void en7581_xgspon_restore_registration_id(
	struct en7581_xgspon *priv,
	const u8 registration_id[AIROHA_XGS_REGISTRATION_ID_SIZE])
{
	if (en7581_xgspon_program_registration_id(priv, registration_id))
		dev_err(priv->dev,
			"failed to restore XGS-PON Registration-ID registers\n");
}

static int en7581_xgspon_clear_hardware_identity(
	struct en7581_xgspon *priv)
{
	static const u8 zero_serial[AIROHA_XGS_SERIAL_SIZE];
	static const u8 zero_registration_id[AIROHA_XGS_REGISTRATION_ID_SIZE];
	int ret;

	ret = en7581_xgspon_program_serial(priv, zero_serial);
	if (ret)
		return ret;

	return en7581_xgspon_program_registration_id(priv,
						      zero_registration_id);
}

static int en7581_xgspon_program_onu_id(struct en7581_xgspon *priv,
						u16 onu_id, bool valid)
{
	u32 value;

	if (!priv->mac)
		return -ENODEV;
	if (onu_id & ~EN7581_XGSPON_ONU_ID_VALUE)
		return -ERANGE;
	value = readl(priv->mac + EN7581_XGSPON_ONU_ID);
	value &= ~(EN7581_XGSPON_ONU_ID_VALUE | EN7581_XGSPON_ONU_ID_VALID);
	value |= FIELD_PREP(EN7581_XGSPON_ONU_ID_VALUE, onu_id);
	if (valid)
		value |= EN7581_XGSPON_ONU_ID_VALID;
	writel(value, priv->mac + EN7581_XGSPON_ONU_ID);
	if ((readl(priv->mac + EN7581_XGSPON_ONU_ID) &
	     (EN7581_XGSPON_ONU_ID_VALUE | EN7581_XGSPON_ONU_ID_VALID)) !=
	    (value & (EN7581_XGSPON_ONU_ID_VALUE |
		      EN7581_XGSPON_ONU_ID_VALID)))
		return -EIO;

	return 0;
}

static int en7581_xgspon_read_tcont(struct en7581_xgspon *priv, u8 index,
					   bool *valid, u16 *alloc_id)
{
	u32 status;
	int ret;

	if (!priv->mac)
		return -ENODEV;
	if (index >= EN7581_XGSPON_MAX_TCONTS || !valid || !alloc_id)
		return -EINVAL;

	writel(FIELD_PREP(EN7581_XGSPON_TCONT_INDEX, index),
	       priv->mac + EN7581_XGSPON_TCONT_ID_CFG);
	ret = readl_poll_timeout(priv->mac + EN7581_XGSPON_TCONT_ID_STS,
				 status,
				 status & EN7581_XGSPON_TCONT_COMMAND_DONE,
				 1, EN7581_XGSPON_TABLE_TIMEOUT_US);
	if (ret)
		return ret;

	*valid = !!(status & EN7581_XGSPON_TCONT_STATUS_VALID);
	*alloc_id = FIELD_GET(EN7581_XGSPON_TCONT_STATUS_ALLOC_ID, status);
	return 0;
}

static int en7581_xgspon_program_tcont(struct en7581_xgspon *priv, u8 index,
					      u16 alloc_id, bool valid)
{
	bool read_valid;
	u16 read_alloc_id;
	u32 status;
	u32 value;
	int ret;

	if (!priv->mac)
		return -ENODEV;
	if (index >= EN7581_XGSPON_MAX_TCONTS ||
	    alloc_id > FIELD_MAX(EN7581_XGSPON_TCONT_ALLOC_ID))
		return -EINVAL;

	value = EN7581_XGSPON_TCONT_COMMAND_WRITE |
		FIELD_PREP(EN7581_XGSPON_TCONT_INDEX, index) |
		FIELD_PREP(EN7581_XGSPON_TCONT_ALLOC_ID, alloc_id);
	if (valid)
		value |= EN7581_XGSPON_TCONT_VALID;
	writel(value, priv->mac + EN7581_XGSPON_TCONT_ID_CFG);
	ret = readl_poll_timeout(priv->mac + EN7581_XGSPON_TCONT_ID_STS,
				 status,
				 status & EN7581_XGSPON_TCONT_COMMAND_DONE,
				 1, EN7581_XGSPON_TABLE_TIMEOUT_US);
	if (ret)
		return ret;

	ret = en7581_xgspon_read_tcont(priv, index, &read_valid,
					       &read_alloc_id);
	if (ret)
		return ret;
	if (read_valid != valid || read_alloc_id != alloc_id)
		return -EIO;

	return 0;
}

static void en7581_xgspon_reset_activation_state(
	struct en7581_xgspon *priv)
{
	int i;

	priv->ranging_time_authenticated = false;
	priv->equalization_delay_programmed = false;
	priv->equalization_delay = 0;
	priv->lods_session_preserved = false;
	priv->lods_us_profile_valid = 0;
	priv->lods_key_indices = 0;
	priv->lods_deadline = 0;
	priv->to1_deadline = 0;
	priv->to1_generation = 0;
	priv->to1_expected_state = 0;
	memset(&priv->last_ranging_time, 0, sizeof(priv->last_ranging_time));
	priv->assign_alloc_id_authenticated = false;
	priv->assign_alloc_id_committed = false;
	priv->tx_armed = false;
	priv->tx_authorized = false;
	priv->optical_tx_enabled = false;
	priv->ack_head = 0;
	priv->ack_count = 0;
	priv->ack_in_flight = false;
	memset(priv->ack_queue, 0, sizeof(priv->ack_queue));
	memset(&priv->active_ack, 0, sizeof(priv->active_ack));
	priv->key_report_head = 0;
	priv->key_report_count = 0;
	priv->key_report_in_flight = false;
	memzero_explicit(priv->key_report_queue,
			 sizeof(priv->key_report_queue));
	memzero_explicit(&priv->active_key_report,
			 sizeof(priv->active_key_report));
	priv->last_tcont_index = EN7581_XGSPON_NO_TCONT;
	memset(&priv->last_alloc_id_assignment, 0,
	       sizeof(priv->last_alloc_id_assignment));
	for (i = 0; i < EN7581_XGSPON_MAX_TCONTS; i++) {
		priv->alloc_ids[i] = EN7581_XGSPON_UNASSIGNED_ALLOC_ID;
		priv->alloc_valid[i] = false;
	}
}

static int en7581_xgspon_set_activation_state(
	struct en7581_xgspon *priv, u32 state)
{
	u32 value, previous, readback;

	if (!priv->mac || state > FIELD_MAX(EN7581_XGSPON_ACTIVATION_STATE))
		return -ENODEV;
	value = readl(priv->mac + EN7581_XGSPON_ACTIVATION_ST);
	previous = FIELD_GET(EN7581_XGSPON_ACTIVATION_STATE, value);
	value &= ~EN7581_XGSPON_ACTIVATION_STATE;
	value |= FIELD_PREP(EN7581_XGSPON_ACTIVATION_STATE, state);
	writel(value, priv->mac + EN7581_XGSPON_ACTIVATION_ST);
	readback = FIELD_GET(EN7581_XGSPON_ACTIVATION_STATE,
			     readl(priv->mac + EN7581_XGSPON_ACTIVATION_ST));
	if (readback != state)
		return -EIO;

	/* Count only SDK-defined LODS transitions after the state readback. */
	if (previous == EN7581_XGSPON_ACTIVATION_O5 &&
	    state == EN7581_XGSPON_ACTIVATION_O6)
		priv->pm_lods.events++;
	else if (previous == EN7581_XGSPON_ACTIVATION_O6 &&
		 state == EN7581_XGSPON_ACTIVATION_O5)
		priv->pm_lods.restored++;
	else if (previous == EN7581_XGSPON_ACTIVATION_O6 &&
		 state == EN7581_XGSPON_ACTIVATION_O1)
		priv->pm_lods.reactivations++;

	return 0;
}

static void en7581_xgspon_cancel_to1_locked(
	struct en7581_xgspon *priv)
{
	cancel_delayed_work(&priv->to1_work);
	priv->to1_deadline = 0;
	priv->to1_generation = 0;
	priv->to1_expected_state = 0;
}

static void en7581_xgspon_start_to1_locked(
	struct en7581_xgspon *priv)
{
	unsigned long delay = msecs_to_jiffies(EN7581_XGSPON_TO1_MS);

	priv->to1_deadline = jiffies + delay;
	priv->to1_generation = priv->session_generation;
	priv->to1_expected_state = EN7581_XGSPON_ACTIVATION_O4;
	mod_delayed_work(system_wq, &priv->to1_work, delay);
}

static int en7581_xgspon_set_ploam_reply_mode(
	struct en7581_xgspon *priv, bool software)
{
	u32 value;

	if (!priv->mac)
		return -ENODEV;
	value = readl(priv->mac + EN7581_XGSPON_O23_O4_PLOAMU_CTRL);
	if (software)
		value |= EN7581_XGSPON_O23_O4_PLOAMU_SOFTWARE;
	else
		value &= ~EN7581_XGSPON_O23_O4_PLOAMU_SOFTWARE;
	writel(value, priv->mac + EN7581_XGSPON_O23_O4_PLOAMU_CTRL);
	value = readl(priv->mac + EN7581_XGSPON_O23_O4_PLOAMU_CTRL);

	return !!(value & EN7581_XGSPON_O23_O4_PLOAMU_SOFTWARE) == software ?
		0 : -EIO;
}

/* Upstream completion/error IRQs are armed only with the temporary
 * Serial Number/Registration permission.  Keep PLOAMd enabled while the
 * readback proves that every upstream event bit was accepted by hardware. */
static int en7581_xgspon_enable_upstream_irqs_locked(
	struct en7581_xgspon *priv)
{
	u32 status;

	if (!priv->mac)
		return -ENODEV;

	/* Do not let an event left by firmware or a previous activation session
	 * satisfy or fault the new Serial Number/Registration transaction. */
	status = readl(priv->mac + EN7581_XGSPON_INT_STATUS) &
		 EN7581_XGSPON_INT_UPSTREAM_EVENTS;
	if (status)
		writel(status, priv->mac + EN7581_XGSPON_INT_STATUS);
	status = readl(priv->mac + EN7581_XGSPON_FIFO_ERR_STS);
	if (status)
		writel(status, priv->mac + EN7581_XGSPON_FIFO_ERR_STS);
	status = readl(priv->mac + EN7581_XGSPON_TX_ERR_STS) &
		 EN7581_XGSPON_TX_ERR_ALL;
	if (status)
		writel(status, priv->mac + EN7581_XGSPON_TX_ERR_STS);

	return en7581_xgspon_set_mac_irq_enable(priv,
						 EN7581_XGSPON_INT_ACTIVE);
}

static bool en7581_xgspon_tx_sync_ready_locked(
	struct en7581_xgspon *priv)
{
	return priv->mac &&
		(readl(priv->mac + EN7581_XGSPON_DBG_RESYNC) &
		 EN7581_XGSPON_TX_SYNC_READY);
}

static bool en7581_xgspon_activation_ready_locked(
	struct en7581_xgspon *priv)
{
	return priv->hardware_selected && priv->hardware_state_valid &&
		priv->bosa && airoha_en7572_is_ready(priv->bosa) &&
		priv->serial_set &&
		!priv->serial_disabled &&
		priv->registration_id_set && en7581_xgspon_phy_trusted_locked(priv) &&
		en7581_xgspon_tx_sync_ready_locked(priv);
}

static int en7581_xgspon_clear_activation_hardware(
	struct en7581_xgspon *priv)
{
	int first_error = 0;
	int i, ret;

	if (!priv->mac) {
		en7581_xgspon_reset_activation_state(priv);
		return 0;
	}

	ret = en7581_xgspon_set_ploam_reply_mode(priv, false);
	if (ret)
		first_error = ret;
	ret = en7581_xgspon_set_activation_state(
		priv, EN7581_XGSPON_ACTIVATION_O1);
	if (ret && !first_error)
		first_error = ret;

	/* T-CONT 0 is the ONU-ID shadow and must never be written directly. */
	for (i = 1; i < EN7581_XGSPON_MAX_TCONTS; i++) {
		ret = en7581_xgspon_program_tcont(
			priv, i, EN7581_XGSPON_UNASSIGNED_ALLOC_ID, false);
		if (ret && !first_error)
			first_error = ret;
	}

	writel(0, priv->mac + EN7581_XGSPON_EQD);
	if (readl(priv->mac + EN7581_XGSPON_EQD) && !first_error)
		first_error = -EIO;

	en7581_xgspon_reset_activation_state(priv);
	return first_error;
}

static int en7581_xgspon_eqd_read(void *context, unsigned int *value)
{
	struct en7581_xgspon *priv = context;

	if (!priv->mac)
		return -ENODEV;
	*value = readl(priv->mac + EN7581_XGSPON_EQD);
	return 0;
}

static int en7581_xgspon_eqd_write(void *context, unsigned int value)
{
	struct en7581_xgspon *priv = context;

	writel(value, priv->mac + EN7581_XGSPON_EQD);
	return 0;
}

static void en7581_xgspon_eqd_fail_closed(void *context)
{
	struct en7581_xgspon *priv = context;

	priv->hardware_state_valid = false;
}

static const struct airoha_xgs_eqd_ops en7581_xgspon_eqd_ops = {
	.read = en7581_xgspon_eqd_read,
	.write = en7581_xgspon_eqd_write,
	.fail_closed = en7581_xgspon_eqd_fail_closed,
};

static int en7581_xgspon_program_equalization_delay(
	struct en7581_xgspon *priv, u32 requested, bool absolute, bool negative,
	u32 *resolved)
{
	return airoha_xgs_eqd_transaction(
		&en7581_xgspon_eqd_ops, priv, priv->active_mode,
		priv->equalization_delay, requested, absolute, negative, resolved);
}

static int en7581_xgspon_snapshot_tconts(
	struct en7581_xgspon *priv,
	u16 alloc_ids[EN7581_XGSPON_MAX_TCONTS],
	bool alloc_valid[EN7581_XGSPON_MAX_TCONTS])
{
	int i, ret;

	alloc_ids[0] = priv->onu_id_set ? priv->onu_id :
		EN7581_XGSPON_UNASSIGNED_ALLOC_ID;
	alloc_valid[0] = priv->onu_id_set;
	for (i = 1; i < EN7581_XGSPON_MAX_TCONTS; i++) {
		ret = en7581_xgspon_read_tcont(priv, i, &alloc_valid[i],
						&alloc_ids[i]);
		if (ret)
			return ret;
	}

	return 0;
}

static int en7581_xgspon_commit_alloc_id(
	struct en7581_xgspon *priv,
	const struct airoha_xgs_alloc_id_assignment *assignment,
	u8 *tcont_index)
{
	bool alloc_valid[EN7581_XGSPON_MAX_TCONTS] = {};
	u16 alloc_ids[EN7581_XGSPON_MAX_TCONTS] = {};
	u8 free_index = EN7581_XGSPON_NO_TCONT;
	u8 match_index = EN7581_XGSPON_NO_TCONT;
	int rollback_ret;
	int i, ret;

	ret = en7581_xgspon_snapshot_tconts(priv, alloc_ids, alloc_valid);
	if (ret) {
		priv->hardware_state_valid = false;
		return ret;
	}

	for (i = 1; i < EN7581_XGSPON_MAX_TCONTS; i++) {
		if (!alloc_valid[i]) {
			if (free_index == EN7581_XGSPON_NO_TCONT)
				free_index = i;
			continue;
		}
		if (alloc_ids[i] != assignment->alloc_id)
			continue;
		if (match_index != EN7581_XGSPON_NO_TCONT) {
			priv->hardware_state_valid = false;
			return -EUCLEAN;
		}
		match_index = i;
	}

	/* The SDK treats the ONU-ID allocation as the read-only T-CONT 0 shadow. */
	if (assignment->alloc_id == priv->onu_id) {
		*tcont_index = 0;
		goto committed;
	}

	if (assignment->operation == AIROHA_XGS_ALLOC_ID_ASSIGN) {
		if (match_index != EN7581_XGSPON_NO_TCONT) {
			*tcont_index = match_index;
			goto committed;
		}
		if (free_index == EN7581_XGSPON_NO_TCONT)
			return -ENOSPC;

		ret = en7581_xgspon_program_tcont(priv, free_index,
						     assignment->alloc_id, true);
		if (ret) {
			rollback_ret = en7581_xgspon_program_tcont(
				priv, free_index, alloc_ids[free_index],
				alloc_valid[free_index]);
			if (rollback_ret)
				priv->hardware_state_valid = false;
			return ret;
		}
		alloc_ids[free_index] = assignment->alloc_id;
		alloc_valid[free_index] = true;
		*tcont_index = free_index;
	} else if (assignment->operation == AIROHA_XGS_ALLOC_ID_DEALLOCATE) {
		if (match_index == EN7581_XGSPON_NO_TCONT) {
			*tcont_index = EN7581_XGSPON_NO_TCONT;
			goto committed;
		}

		ret = en7581_xgspon_program_tcont(
			priv, match_index, EN7581_XGSPON_UNASSIGNED_ALLOC_ID,
			false);
		if (ret) {
			rollback_ret = en7581_xgspon_program_tcont(
				priv, match_index, assignment->alloc_id, true);
			if (rollback_ret)
				priv->hardware_state_valid = false;
			return ret;
		}
		alloc_ids[match_index] = EN7581_XGSPON_UNASSIGNED_ALLOC_ID;
		alloc_valid[match_index] = false;
		*tcont_index = match_index;
	} else {
		return -EINVAL;
	}

committed:
	memcpy(priv->alloc_ids, alloc_ids, sizeof(priv->alloc_ids));
	memcpy(priv->alloc_valid, alloc_valid, sizeof(priv->alloc_valid));
	return 0;
}

static int en7581_xgspon_read_xgem(struct en7581_xgspon *priv, u16 xgem_id,
					 u32 *status)
{
	u32 value;
	int ret;

	if (!priv->mac)
		return -ENODEV;

	writel(FIELD_PREP(EN7581_XGSPON_GEM_PORT_ID, xgem_id),
	       priv->mac + EN7581_XGSPON_GEM_PORT_CFG);
	ret = readl_poll_timeout(priv->mac + EN7581_XGSPON_GEM_PORT_STS,
				 value, value & EN7581_XGSPON_GEM_COMMAND_DONE,
				 1, 1000);
	if (ret)
		return ret;

	*status = value & EN7581_XGSPON_GEM_STATUS_MASK;
	return 0;
}

static int en7581_xgspon_program_xgem(struct en7581_xgspon *priv, u16 xgem_id,
					    bool valid, bool unicast,
					    bool upstream_encrypted)
{
	u32 expected = 0;
	u32 status;
	u32 value;
	int ret;

	if (!priv->mac)
		return -ENODEV;

	value = EN7581_XGSPON_GEM_COMMAND_WRITE |
		FIELD_PREP(EN7581_XGSPON_GEM_PORT_ID, xgem_id);
	if (valid)
		value |= EN7581_XGSPON_GEM_VALID;
	if (unicast)
		value |= EN7581_XGSPON_GEM_UNICAST;
	if (upstream_encrypted)
		value |= EN7581_XGSPON_GEM_US_ENCRYPT;
	writel(value, priv->mac + EN7581_XGSPON_GEM_PORT_CFG);
	ret = readl_poll_timeout(priv->mac + EN7581_XGSPON_GEM_PORT_STS,
				 status, status & EN7581_XGSPON_GEM_COMMAND_DONE,
				 1, 1000);
	if (ret)
		return ret;

	ret = en7581_xgspon_read_xgem(priv, xgem_id, &status);
	if (ret)
		return ret;
	if (valid)
		expected |= EN7581_XGSPON_GEM_STATUS_VALID;
	if (unicast)
		expected |= EN7581_XGSPON_GEM_STATUS_UNICAST;
	if (upstream_encrypted)
		expected |= EN7581_XGSPON_GEM_STATUS_US_ENCRYPT;

	return status == expected ? 0 : -EIO;
}

static int en7581_xgspon_validate_service(
	const struct airoha_xgs_service_config *config)
{
	unsigned int i, j;

	if (!config || config->version != AIROHA_XGS_SERVICE_ABI_VERSION ||
	    config->tcont_count > AIROHA_XGS_SERVICE_MAX_TCONTS ||
	    config->xgem_count > AIROHA_XGS_SERVICE_MAX_XGEMS ||
	    (config->tcont_count && !config->tconts) ||
	    (config->xgem_count && !config->xgems))
		return -EINVAL;
	for (i = 0; i < config->tcont_count; i++) {
		const struct airoha_xgs_service_tcont *t = &config->tconts[i];

		if (t->index < AIROHA_XGS_SERVICE_FIRST_TCONT ||
		    t->index >= EN7581_XGSPON_MAX_TCONTS || !t->valid ||
		    t->alloc_id > AIROHA_XGS_SERVICE_MAX_ALLOC_ID ||
		    t->scheduler > 2)
			return -ERANGE;
		if (t->scheduler == 2) {
			bool have_weight = false;
			for (j = 0; j < ARRAY_SIZE(t->queue_weights); j++)
				have_weight |= t->queue_weights[j] != 0;
			if (!have_weight)
				return -EINVAL;
		}
		for (j = 0; j < i; j++)
			if (config->tconts[j].index == t->index ||
			    config->tconts[j].alloc_id == t->alloc_id)
				return -EEXIST;
	}
	for (i = 0; i < config->xgem_count; i++) {
		const struct airoha_xgs_service_xgem *x = &config->xgems[i];
		bool have_tcont = false;

		if (!x->valid || !x->xgem_id ||
		    x->xgem_id > AIROHA_XGS_SERVICE_MAX_XGEM_ID ||
		    x->tcont_index < AIROHA_XGS_SERVICE_FIRST_TCONT ||
		    x->tcont_index >= EN7581_XGSPON_MAX_TCONTS ||
		    x->direction < AIROHA_XGS_SERVICE_DIRECTION_DOWNSTREAM ||
		    x->direction > AIROHA_XGS_SERVICE_DIRECTION_BIDIRECTIONAL ||
		    x->channel > AIROHA_XGS_SERVICE_MAX_CHANNEL ||
		    x->nboq > AIROHA_XGS_SERVICE_MAX_NBOQ || x->mic_idx > 1)
			return -ERANGE;
		for (j = 0; j < config->tcont_count; j++)
			if (config->tconts[j].index == x->tcont_index)
				have_tcont = true;
		if (!have_tcont)
			return -ENOENT;
		for (j = 0; j < i; j++)
			if (config->xgems[j].xgem_id == x->xgem_id)
				return -EEXIST;
	}
	return 0;
}

static int en7581_xgspon_program_service_locked(
	struct en7581_xgspon *priv,
	const struct airoha_xgs_service_config *config)
{
	unsigned int i;
	unsigned int j;
	int ret;

	/* The ONU-ID shadow and the OMCC XGEM are owned by activation. */
	for (i = 0; i < config->xgem_count; i++) {
		const struct airoha_xgs_service_xgem *x = &config->xgems[i];

		if (x->xgem_id == priv->onu_id)
			return -EBUSY;
	}
	for (i = 0; i < config->tcont_count; i++) {
		const struct airoha_xgs_service_tcont *t = &config->tconts[i];

		ret = en7581_xgspon_program_tcont(priv, t->index, t->alloc_id, true);
		if (ret)
			return ret;
	}
	for (i = 0; i < priv->service_xgem_count; i++) {
		bool wanted = false;

		for (j = 0; j < config->xgem_count; j++)
			if (config->xgems[j].xgem_id ==
			    priv->service_xgems[i].xgem_id)
				wanted = true;
		if (!wanted) {
			ret = en7581_xgspon_program_xgem(
				priv, priv->service_xgems[i].xgem_id, false,
				false, false);
			if (ret)
				return ret;
		}
	}
	for (i = 1; i < EN7581_XGSPON_MAX_TCONTS; i++) {
		bool wanted = false;
		unsigned int j;

		for (j = 0; j < config->tcont_count; j++)
			if (config->tconts[j].index == i)
				wanted = true;
		if (!wanted) {
			ret = en7581_xgspon_program_tcont(
				priv, i, EN7581_XGSPON_UNASSIGNED_ALLOC_ID, false);
			if (ret)
				return ret;
		}
	}
	for (i = 0; i < config->xgem_count; i++) {
		const struct airoha_xgs_service_xgem *x = &config->xgems[i];

		ret = en7581_xgspon_program_xgem(priv, x->xgem_id, true,
						 x->unicast, x->upstream_encrypted);
		if (ret)
			return ret;
	}
	return 0;
}

static int en7581_xgspon_restore_service_locked(
	struct en7581_xgspon *priv,
	const struct airoha_xgs_service_tcont *tconts, u16 tcont_count,
	const struct airoha_xgs_service_xgem *xgems, u16 xgem_count)
{
	struct airoha_xgs_service_config config = {
		.version = AIROHA_XGS_SERVICE_ABI_VERSION,
		.tcont_count = tcont_count,
		.xgem_count = xgem_count,
		.tconts = tconts,
		.xgems = xgems,
	};
	unsigned int i, j;
	int ret;

	for (i = 1; i < EN7581_XGSPON_MAX_TCONTS; i++)
		if ((ret = en7581_xgspon_program_tcont(
			priv, i, EN7581_XGSPON_UNASSIGNED_ALLOC_ID, false)))
			return ret;
	/* The table has no bulk clear command.  Invalidate every XGEM touched by
	 * either generation before restoring the old snapshot. */
	for (i = 0; i < priv->service_xgem_count; i++) {
		ret = en7581_xgspon_program_xgem(priv,
			priv->service_xgems[i].xgem_id, false, false, false);
		if (ret)
			return ret;
	}
	for (i = 0; i < xgem_count; i++) {
		for (j = 0; j < priv->service_xgem_count; j++)
			if (xgems[i].xgem_id == priv->service_xgems[j].xgem_id)
				break;
		if (j == priv->service_xgem_count) {
			ret = en7581_xgspon_program_xgem(priv, xgems[i].xgem_id,
							false, false, false);
			if (ret)
				return ret;
		}
	}
	/* program_service restores T-CONTs before it re-enables the XGEMs. */
	return en7581_xgspon_program_service_locked(priv, &config);
}

static int en7581_xgspon_apply_service_locked(
	struct en7581_xgspon *priv,
	const struct airoha_xgs_service_config *config)
{
	struct en7581_xgspon_service_work *work;
	struct airoha_xgs_service_tcont *normalized_tconts;
	struct airoha_xgs_service_xgem *normalized_xgems;
	struct airoha_xgs_service_tcont *old_tconts;
	struct airoha_xgs_service_xgem *old_xgems;
	u16 old_tcont_count = priv->service_tcont_count;
	u16 old_xgem_count = priv->service_xgem_count;
	unsigned int i, j;
	int ret, rollback_ret;
	struct airoha_xgs_service_config normalized;

	ret = en7581_xgspon_validate_service(config);
	if (ret)
		return ret;
	for (i = 0; i < config->xgem_count; i++)
		if (config->xgems[i].upstream_encrypted &&
		    !priv->data_key_confirmed)
			return -ENOKEY;
	if (!priv->hardware_selected || !priv->mac || !priv->hardware_state_valid ||
	    !priv->ethernet_np)
		return -ENODEV;
	work = kzalloc(sizeof(*work), GFP_KERNEL);
	if (!work)
		return -ENOMEM;
	normalized_tconts = work->normalized_tconts;
	normalized_xgems = work->normalized_xgems;
	old_tconts = work->old_tconts;
	old_xgems = work->old_xgems;
	for (i = 0; i < config->tcont_count; i++) {
		const struct airoha_xgs_service_tcont *t = &config->tconts[i];
		unsigned int hw_index = EN7581_XGSPON_NO_TCONT;

		for (j = 1; j < EN7581_XGSPON_MAX_TCONTS; j++)
			if (priv->alloc_valid[j] && priv->alloc_ids[j] == t->alloc_id) {
				hw_index = j;
				break;
			}
		if (hw_index == EN7581_XGSPON_NO_TCONT) {
			ret = -ENOLINK;
			goto out;
		}
		for (j = 0; j < i; j++)
			if (normalized_tconts[j].index == hw_index) {
				ret = -EEXIST;
				goto out;
			}
		normalized_tconts[i] = *t;
		normalized_tconts[i].index = hw_index;
	}
	for (i = 0; i < config->xgem_count; i++) {
		const struct airoha_xgs_service_xgem *x = &config->xgems[i];
		unsigned int tcont_position = AIROHA_XGS_SERVICE_MAX_TCONTS;

		for (j = 0; j < config->tcont_count; j++)
			if (config->tconts[j].index == x->tcont_index) {
				tcont_position = j;
				break;
			}
		if (tcont_position == AIROHA_XGS_SERVICE_MAX_TCONTS) {
			ret = -ENOENT;
			goto out;
		}
		normalized_xgems[i] = *x;
		normalized_xgems[i].tcont_index =
			normalized_tconts[tcont_position].index;
		normalized_xgems[i].channel =
			normalized_tconts[tcont_position].index;
		normalized_xgems[i].nboq =
			normalized_tconts[tcont_position].index;
	}
	normalized.version = AIROHA_XGS_SERVICE_ABI_VERSION;
	normalized.tcont_count = config->tcont_count;
	normalized.xgem_count = config->xgem_count;
	normalized.tconts = normalized_tconts;
	normalized.xgems = normalized_xgems;
	memcpy(old_tconts, priv->service_tconts,
	       old_tcont_count * sizeof(*old_tconts));
	memcpy(old_xgems, priv->service_xgems,
	       old_xgem_count * sizeof(*old_xgems));
	ret = en7581_xgspon_program_service_locked(priv, &normalized);
	if (ret) {
		rollback_ret = en7581_xgspon_restore_service_locked(
			priv, old_tconts, old_tcont_count, old_xgems, old_xgem_count);
		if (rollback_ret)
			priv->hardware_state_valid = false;
		goto out;
	}
	ret = airoha_xgs_qdma_service_apply(priv->ethernet_np, &normalized);
	if (ret) {
		rollback_ret = en7581_xgspon_restore_service_locked(
			priv, old_tconts, old_tcont_count, old_xgems, old_xgem_count);
		if (!rollback_ret) {
			if (old_tcont_count || old_xgem_count)
				rollback_ret = airoha_xgs_qdma_service_apply(
					priv->ethernet_np,
					&(struct airoha_xgs_service_config){
					.version = AIROHA_XGS_SERVICE_ABI_VERSION,
					.tcont_count = old_tcont_count,
					.xgem_count = old_xgem_count,
					.tconts = old_tconts,
					.xgems = old_xgems,
				});
			else
				rollback_ret = airoha_xgs_qdma_service_clear(
					priv->ethernet_np);
		}
		if (rollback_ret)
			priv->hardware_state_valid = false;
		goto out;
	}
	memcpy(priv->rollback_tconts, old_tconts,
	       old_tcont_count * sizeof(*old_tconts));
	memcpy(priv->rollback_xgems, old_xgems,
	       old_xgem_count * sizeof(*old_xgems));
	priv->rollback_tcont_count = old_tcont_count;
	priv->rollback_xgem_count = old_xgem_count;
	priv->service_rollback_valid = true;
	memcpy(priv->service_tconts, normalized_tconts,
	       normalized.tcont_count * sizeof(*normalized.tconts));
	memcpy(priv->service_xgems, normalized_xgems,
	       normalized.xgem_count * sizeof(*normalized.xgems));
	priv->service_tcont_count = normalized.tcont_count;
	priv->service_xgem_count = normalized.xgem_count;
	memset(priv->service_tcont_hw_index, 0,
	       sizeof(priv->service_tcont_hw_index));
	for (i = 0; i < normalized.tcont_count; i++)
		priv->service_tcont_hw_index[i] = normalized_tconts[i].index;
	priv->service_generation++;
	ret = 0;
out:
	kfree(work);
	return ret;
}

static int en7581_xgspon_program_omcc_xgem(struct en7581_xgspon *priv,
						 u16 onu_id, bool valid)
{
	/* The SDK creates the default OMCC as unicast and unencrypted. */
	return en7581_xgspon_program_xgem(priv, onu_id, valid, true, false);
}

static int en7581_xgspon_rollback_onu_assignment(
	struct en7581_xgspon *priv, u16 onu_id)
{
	int onu_ret, xgem_ret;

	xgem_ret = en7581_xgspon_program_omcc_xgem(priv, onu_id, false);
	onu_ret = en7581_xgspon_program_onu_id(priv, 0, false);
	priv->omcc_xgem_programmed = false;

	return xgem_ret ? xgem_ret : onu_ret;
}

static int en7581_xgspon_reset_onu_id(struct en7581_xgspon *priv)
{
	u16 assigned_onu_id = priv->onu_id;
	u32 hardware_onu_id = 0;
	int activation_ret;
	int onu_ret = 0;
	int ret = 0;

	activation_ret = en7581_xgspon_clear_activation_hardware(priv);
	if (priv->mac) {
		hardware_onu_id = readl(priv->mac + EN7581_XGSPON_ONU_ID);
		if (!priv->omcc_xgem_programmed)
			assigned_onu_id = FIELD_GET(EN7581_XGSPON_ONU_ID_VALUE,
						    hardware_onu_id);
		ret = en7581_xgspon_program_omcc_xgem(priv, assigned_onu_id,
						      false);
		onu_ret = en7581_xgspon_program_onu_id(priv, 0, false);
		if (!ret)
			ret = onu_ret;
	}
	if (!ret)
		ret = activation_ret;
	if (ret) {
		dev_err(priv->dev,
			"failed to invalidate XGS-PON ONU-ID/OMCC XGEM assignment\n");
		priv->hardware_state_valid = false;
	}
	priv->onu_id = 0x03ff;
	priv->onu_id_set = false;
	priv->omcc_xgem_programmed = false;
	priv->assign_onu_id_sequence = 0;
	priv->request_registration_authenticated = false;
	priv->registration_response_pending = false;
	priv->request_registration_sequence = 0;
	priv->tx_authorized = false;
	priv->registration_key_switch_verified = false;
	priv->tx_state = EN7581_XGSPON_TX_BLOCKED;
	priv->rx_state = EN7581_XGSPON_RX_WAIT_PROFILE;

	return ret;
}

static void en7581_xgspon_write_key(struct en7581_xgspon *priv, u32 offset,
				    const u8 key[AIROHA_XGS_KEY_SIZE])
{
	int i;

	/* The SDK places bytes 12..15 in the lowest-addressed key word. */
	for (i = 0; i < AIROHA_XGS_KEY_SIZE / sizeof(u32); i++)
		writel(en7581_xgspon_key_word(key, i),
		       priv->mac + offset + i * sizeof(u32));
}

static bool en7581_xgspon_key_matches(struct en7581_xgspon *priv, u32 offset,
				      const u8 key[AIROHA_XGS_KEY_SIZE]);

static int en7581_xgspon_program_data_key_locked(
	struct en7581_xgspon *priv, u8 slot,
	const u8 key[AIROHA_XGS_KEY_SIZE])
{
	u32 offset;

	if (slot > 1 || !priv->mac)
		return -EINVAL;
	offset = slot ? EN7581_XGSPON_AES_UC_KEY1 :
			EN7581_XGSPON_AES_UC_KEY0;
	en7581_xgspon_write_key(priv, offset, key);
	return en7581_xgspon_key_matches(priv, offset, key) ? 0 : -EIO;
}

static int en7581_xgspon_set_ds_data_key_valid_locked(
	struct en7581_xgspon *priv, u8 slot, bool valid)
{
	u32 mask, value;

	if (slot > 1 || !priv->mac)
		return -EINVAL;
	mask = slot ? EN7581_XGSPON_DS_AES_UC_KEY1_VALID :
		      EN7581_XGSPON_DS_AES_UC_KEY0_VALID;
	value = readl(priv->mac + EN7581_XGSPON_DS_AES_KEY_VALID);
	if (valid)
		value |= mask;
	else
		value &= ~mask;
	writel(value, priv->mac + EN7581_XGSPON_DS_AES_KEY_VALID);
	return !!(readl(priv->mac + EN7581_XGSPON_DS_AES_KEY_VALID) & mask) ==
		valid ? 0 : -EIO;
}

static int en7581_xgspon_snapshot_data_keys_locked(
	struct en7581_xgspon *priv)
{
	int slot, word;

	if (!priv->mac)
		return -ENODEV;
	priv->data_key_snapshot.upstream_control =
		readl(priv->mac + EN7581_XGSPON_US_AES_KEY_CTRL);
	priv->data_key_snapshot.downstream_valid =
		readl(priv->mac + EN7581_XGSPON_DS_AES_KEY_VALID);
	priv->data_key_snapshot.active_index = priv->data_key_index;
	priv->data_key_snapshot.confirmed = priv->data_key_confirmed;
	for (slot = 0; slot < 2; slot++)
		for (word = 0; word < AIROHA_XGS_KEY_SIZE / sizeof(u32); word++)
			put_unaligned_be32(readl(priv->mac +
				(slot ? EN7581_XGSPON_AES_UC_KEY1 :
					EN7581_XGSPON_AES_UC_KEY0) +
				word * sizeof(u32)),
				priv->data_key_snapshot.keys[slot] +
				AIROHA_XGS_KEY_SIZE - (word + 1) * sizeof(u32));
	priv->data_key_snapshot_valid = true;
	return 0;
}

static int en7581_xgspon_restore_data_keys_locked(
	struct en7581_xgspon *priv)
{
	int ret, slot;

	if (!priv->data_key_snapshot_valid)
		return -EINVAL;
	for (slot = 0; slot < 2; slot++) {
		ret = en7581_xgspon_program_data_key_locked(
			priv, slot, priv->data_key_snapshot.keys[slot]);
		if (ret)
			return ret;
	}
	writel(priv->data_key_snapshot.downstream_valid,
	       priv->mac + EN7581_XGSPON_DS_AES_KEY_VALID);
	writel(priv->data_key_snapshot.upstream_control,
	       priv->mac + EN7581_XGSPON_US_AES_KEY_CTRL);
	if (readl(priv->mac + EN7581_XGSPON_DS_AES_KEY_VALID) !=
	    priv->data_key_snapshot.downstream_valid ||
	    readl(priv->mac + EN7581_XGSPON_US_AES_KEY_CTRL) !=
	    priv->data_key_snapshot.upstream_control)
		return -EIO;
	priv->key_exchange_rollback_events++;
	memcpy(priv->data_keys, priv->data_key_snapshot.keys,
	       sizeof(priv->data_keys));
	priv->data_key_index = priv->data_key_snapshot.active_index;
	priv->data_key_confirmed = priv->data_key_snapshot.confirmed;
	return 0;
}

static int en7581_xgspon_rollback_data_key_exchange_locked(
	struct en7581_xgspon *priv)
{
	int ret = 0;

	cancel_delayed_work(&priv->key_tk4_work);
	cancel_delayed_work(&priv->key_tk5_work);
	en7581_xgspon_drop_queued_key_reports_locked(
		priv, priv->data_key_exchange_generation);
	if (priv->data_key_snapshot_valid)
		ret = en7581_xgspon_restore_data_keys_locked(priv);
	if (ret)
		memzero_explicit(priv->data_keys, sizeof(priv->data_keys));
	memzero_explicit(&priv->data_key_snapshot,
			 sizeof(priv->data_key_snapshot));
	priv->data_key_snapshot_valid = false;
	priv->data_key_pending = false;
	priv->data_key_confirm_pending = false;
	if (ret)
		priv->data_key_index = 0;
	priv->data_key_sequence = 0;
	return ret;
}

static int en7581_xgspon_wipe_data_keys_locked(struct en7581_xgspon *priv)
{
	static const u8 zero[AIROHA_XGS_KEY_SIZE];
	u32 control, valid;
	int ret, slot;

	if (!priv->mac)
		goto clear_state;
	control = readl(priv->mac + EN7581_XGSPON_US_AES_KEY_CTRL) &
		  ~EN7581_XGSPON_US_AES_KEY_VALID;
	writel(control, priv->mac + EN7581_XGSPON_US_AES_KEY_CTRL);
	valid = readl(priv->mac + EN7581_XGSPON_DS_AES_KEY_VALID) &
		~EN7581_XGSPON_DS_AES_UC_KEYS_VALID;
	writel(valid, priv->mac + EN7581_XGSPON_DS_AES_KEY_VALID);
	for (slot = 0; slot < 2; slot++) {
		ret = en7581_xgspon_program_data_key_locked(priv, slot, zero);
		if (ret)
			return ret;
	}
	if ((readl(priv->mac + EN7581_XGSPON_US_AES_KEY_CTRL) &
	     EN7581_XGSPON_US_AES_KEY_VALID) ||
	    (readl(priv->mac + EN7581_XGSPON_DS_AES_KEY_VALID) &
	     EN7581_XGSPON_DS_AES_UC_KEYS_VALID))
		return -EIO;
clear_state:
	memzero_explicit(priv->data_keys, sizeof(priv->data_keys));
	memzero_explicit(&priv->data_key_snapshot,
			 sizeof(priv->data_key_snapshot));
	priv->data_key_snapshot_valid = false;
	priv->data_key_pending = false;
	priv->data_key_confirm_pending = false;
	priv->data_key_confirmed = false;
	priv->data_key_index = 0;
	priv->data_key_sequence = 0;
	cancel_delayed_work(&priv->key_tk4_work);
	cancel_delayed_work(&priv->key_tk5_work);
	return 0;
}

static bool en7581_xgspon_key_matches(struct en7581_xgspon *priv, u32 offset,
				      const u8 key[AIROHA_XGS_KEY_SIZE])
{
	int i;

	for (i = 0; i < AIROHA_XGS_KEY_SIZE / sizeof(u32); i++)
		if (readl(priv->mac + offset + i * sizeof(u32)) !=
		    en7581_xgspon_key_word(key, i))
			return false;

	return true;
}

static void en7581_xgspon_write_pon_tag(
	struct en7581_xgspon *priv,
	const u8 pon_tag[AIROHA_XGS_PON_TAG_SIZE])
{
	writel(en7581_xgspon_pon_tag_word(pon_tag, 0),
	       priv->mac + EN7581_XGSPON_PON_TAG0);
	writel(en7581_xgspon_pon_tag_word(pon_tag, 1),
	       priv->mac + EN7581_XGSPON_PON_TAG1);
}

static bool en7581_xgspon_pon_tag_matches(
	struct en7581_xgspon *priv,
	const u8 pon_tag[AIROHA_XGS_PON_TAG_SIZE])
{
	return readl(priv->mac + EN7581_XGSPON_PON_TAG0) ==
		en7581_xgspon_pon_tag_word(pon_tag, 0) &&
	       readl(priv->mac + EN7581_XGSPON_PON_TAG1) ==
		en7581_xgspon_pon_tag_word(pon_tag, 1);
}

static int en7581_xgspon_wipe_hardware_keys(struct en7581_xgspon *priv)
{
	static const u8 zero_key[AIROHA_XGS_KEY_SIZE];
	static const u8 zero_tag[AIROHA_XGS_PON_TAG_SIZE];
	bool cleared;

	if (!priv->mac) {
		priv->session_keys_programmed = false;
		return 0;
	}
	en7581_xgspon_write_key(priv, EN7581_XGSPON_PIK0, zero_key);
	en7581_xgspon_write_key(priv, EN7581_XGSPON_PIK1, zero_key);
	en7581_xgspon_write_key(priv, EN7581_XGSPON_OIK0, zero_key);
	en7581_xgspon_write_key(priv, EN7581_XGSPON_OIK1, zero_key);
	en7581_xgspon_write_key(priv, EN7581_XGSPON_KEK0, zero_key);
	en7581_xgspon_write_key(priv, EN7581_XGSPON_KEK1, zero_key);
	en7581_xgspon_write_pon_tag(priv, zero_tag);
	cleared = en7581_xgspon_key_matches(priv, EN7581_XGSPON_PIK0,
					     zero_key) &&
		  en7581_xgspon_key_matches(priv, EN7581_XGSPON_PIK1,
					     zero_key) &&
		  en7581_xgspon_key_matches(priv, EN7581_XGSPON_OIK0,
					     zero_key) &&
		  en7581_xgspon_key_matches(priv, EN7581_XGSPON_OIK1,
					     zero_key) &&
		  en7581_xgspon_key_matches(priv, EN7581_XGSPON_KEK0,
					     zero_key) &&
		  en7581_xgspon_key_matches(priv, EN7581_XGSPON_KEK1,
					     zero_key) &&
		  en7581_xgspon_pon_tag_matches(priv, zero_tag);
	priv->session_keys_programmed = false;
	if (!cleared) {
		priv->hardware_state_valid = false;
		dev_err(priv->dev, "failed to verify XGS-PON hardware key wipe\n");
		return -EIO;
	}

	return 0;
}

static int en7581_xgspon_clear_service_locked(struct en7581_xgspon *priv)
{
	struct airoha_xgs_service_config empty = {
		.version = AIROHA_XGS_SERVICE_ABI_VERSION,
	};
	struct airoha_xgs_service_config old = {
		.version = AIROHA_XGS_SERVICE_ABI_VERSION,
		.tcont_count = priv->service_tcont_count,
		.xgem_count = priv->service_xgem_count,
		.tconts = priv->service_tconts,
		.xgems = priv->service_xgems,
	};
	bool active = priv->service_tcont_count || priv->service_xgem_count;
	u32 retire_mask = BIT(0);
	unsigned int i;
	int ret;
	int rollback_ret;

	for (i = 0; i < priv->service_tcont_count; i++)
		retire_mask |= BIT(priv->service_tcont_hw_index[i]);

	if (active) {
		ret = en7581_xgspon_program_service_locked(priv, &empty);
		if (ret)
			goto rollback_mac;
		if (!priv->ethernet_np) {
			ret = -ENODEV;
			goto rollback_mac;
		}
		ret = airoha_xgs_qdma_service_clear(priv->ethernet_np);
		if (ret)
			goto rollback_both;
	}
	if (!priv->ethernet_np)
		return -ENODEV;
	ret = airoha_xpon_qdma_channels_retire(priv->ethernet_np, retire_mask);
	if (ret) {
		if (active)
			goto rollback_both;
		return ret;
	}

	memzero_explicit(priv->service_tconts,
			 sizeof(priv->service_tconts));
	memzero_explicit(priv->service_xgems, sizeof(priv->service_xgems));
	memzero_explicit(priv->service_tcont_hw_index,
			 sizeof(priv->service_tcont_hw_index));
	priv->service_tcont_count = 0;
	priv->service_xgem_count = 0;
	memzero_explicit(priv->pending_tconts,
			 sizeof(priv->pending_tconts));
	memzero_explicit(priv->pending_xgems, sizeof(priv->pending_xgems));
	priv->pending_tcont_count = 0;
	priv->pending_xgem_count = 0;
	memzero_explicit(priv->rollback_tconts,
			 sizeof(priv->rollback_tconts));
	memzero_explicit(priv->rollback_xgems,
			 sizeof(priv->rollback_xgems));
	priv->rollback_tcont_count = 0;
	priv->rollback_xgem_count = 0;
	priv->service_rollback_valid = false;
	if (active)
		priv->service_generation++;

	return 0;

rollback_both:
	rollback_ret = en7581_xgspon_restore_service_locked(priv, old.tconts,
							    old.tcont_count,
							    old.xgems, old.xgem_count);
	if (!rollback_ret)
		rollback_ret = airoha_xgs_qdma_service_apply(priv->ethernet_np,
							      &old);
	if (rollback_ret)
		priv->hardware_state_valid = false;
	return ret;

rollback_mac:
	rollback_ret = en7581_xgspon_restore_service_locked(priv, old.tconts,
							    old.tcont_count,
							    old.xgems, old.xgem_count);
	if (rollback_ret)
		priv->hardware_state_valid = false;
	return ret;
}

static int en7581_xgspon_program_session_keys(
	struct en7581_xgspon *priv,
	const struct airoha_xgs_security_keys *keys,
	const struct airoha_xgs_security_keys *default_keys,
	const u8 default_ploam[AIROHA_XGS_KEY_SIZE],
	const u8 pon_tag[AIROHA_XGS_PON_TAG_SIZE])
{
	u32 key_index;

	if (!priv->mac)
		return -ENODEV;

	en7581_xgspon_write_key(priv, EN7581_XGSPON_PIK0, keys->ploam);
	en7581_xgspon_write_key(priv, EN7581_XGSPON_PIK1, default_ploam);
	en7581_xgspon_write_key(priv, EN7581_XGSPON_OIK0, keys->omci);
	en7581_xgspon_write_key(priv, EN7581_XGSPON_OIK1, default_keys->omci);
	en7581_xgspon_write_key(priv, EN7581_XGSPON_KEK0, keys->kek);
	en7581_xgspon_write_key(priv, EN7581_XGSPON_KEK1, default_keys->kek);
	en7581_xgspon_write_pon_tag(priv, pon_tag);

	if (!en7581_xgspon_key_matches(priv, EN7581_XGSPON_PIK0,
					  keys->ploam) ||
	    !en7581_xgspon_key_matches(priv, EN7581_XGSPON_PIK1,
					  default_ploam) ||
	    !en7581_xgspon_key_matches(priv, EN7581_XGSPON_OIK0, keys->omci) ||
	    !en7581_xgspon_key_matches(priv, EN7581_XGSPON_OIK1,
					  default_keys->omci) ||
	    !en7581_xgspon_key_matches(priv, EN7581_XGSPON_KEK0, keys->kek) ||
	    !en7581_xgspon_key_matches(priv, EN7581_XGSPON_KEK1,
					  default_keys->kek) ||
	    !en7581_xgspon_pon_tag_matches(priv, pon_tag)) {
		en7581_xgspon_wipe_hardware_keys(priv);
		return -EIO;
	}

	/* Match the SDK's independent read-modify-write selector operations. */
	key_index = readl(priv->mac + EN7581_XGSPON_SW_SET_KIDX);
	key_index |= EN7581_XGSPON_SET_PIK_IDX | EN7581_XGSPON_SET_PIK_EN;
	writel(key_index, priv->mac + EN7581_XGSPON_SW_SET_KIDX);
	key_index = readl(priv->mac + EN7581_XGSPON_SW_SET_KIDX);
	key_index |= EN7581_XGSPON_SET_OIK_IDX | EN7581_XGSPON_SET_OIK_EN;
	writel(key_index, priv->mac + EN7581_XGSPON_SW_SET_KIDX);
	/* Registration PLOAM later causes hardware to switch both indices to 0. */
	if (readl_poll_timeout(priv->mac + EN7581_XGSPON_CUR_KIDX, key_index,
			       (key_index & (EN7581_XGSPON_CUR_PIK_IDX |
					   EN7581_XGSPON_CUR_OIK_IDX)) ==
			       (EN7581_XGSPON_CUR_PIK_IDX |
				EN7581_XGSPON_CUR_OIK_IDX),
			       1, 1000)) {
		en7581_xgspon_wipe_hardware_keys(priv);
		return -EIO;
	}

	priv->session_keys_programmed = true;
	return 0;
}

static int en7581_xgspon_clear_session_keys(struct en7581_xgspon *priv)
{
	int data_key_ret, key_ret, lods_ret = 0, service_ret, ret;

	/* The workers may already be running. They observe the cleared state under
	 * priv->lock; cancel_delayed_work_sync() is not safe while locked. */
	cancel_delayed_work(&priv->activation_work);
	en7581_xgspon_cancel_to1_locked(priv);
	/* O6 holds the MAC in reset and stops the packet interfaces. Release that
	 * transaction before issuing table/key cleanup commands. */
	if (priv->lods_session_preserved) {
		lods_ret = en7581_xgspon_lods_release_hardware_locked(priv);
		priv->lods_session_preserved = false;
		priv->lods_deadline = 0;
		if (lods_ret)
			priv->hardware_state_valid = false;
	}
	service_ret = en7581_xgspon_clear_service_locked(priv);
	ret = en7581_xgspon_reset_onu_id(priv);
	data_key_ret = en7581_xgspon_wipe_data_keys_locked(priv);
	key_ret = en7581_xgspon_wipe_hardware_keys(priv);
	memzero_explicit(priv->pon_tag, sizeof(priv->pon_tag));
	memzero_explicit(&priv->session_keys, sizeof(priv->session_keys));
	priv->session_keys_set = false;
	en7581_xgspon_advance_session_locked(priv);
	if (!ret)
		ret = lods_ret;
	if (!ret)
		ret = service_ret;
	if (!ret)
		ret = data_key_ret;
	if (!ret)
		ret = key_ret;
	if (ret)
		priv->hardware_state_valid = false;

	return ret;
}

static int en7581_xgspon_to1_disable_tx(void *context)
{
	struct en7581_xgspon *priv = context;
	int ret;

	priv->tx_armed = false;
	priv->tx_authorized = false;
	priv->optical_tx_enabled = false;
	ret = priv->bosa ? airoha_en7572_set_tx_enabled(priv->bosa, false) :
		-ENODEV;
	if (ret)
		priv->hardware_state_valid = false;

	return ret;
}

static int en7581_xgspon_to1_clear_session(void *context)
{
	return en7581_xgspon_clear_session_keys(context);
}

static const struct airoha_xgs_to1_cleanup_ops en7581_xgspon_to1_cleanup_ops = {
	.disable_tx = en7581_xgspon_to1_disable_tx,
	.clear_session = en7581_xgspon_to1_clear_session,
};

static void en7581_xgspon_to1_work(struct work_struct *work)
{
	struct en7581_xgspon *priv = container_of(
		to_delayed_work(work), struct en7581_xgspon, to1_work);
	struct airoha_xgs_to1_cleanup_result cleanup_result;
	struct airoha_xgs_to1_state to1_state;
	enum airoha_xgs_to1_decision decision;
	unsigned long remaining;
	u64 generation;
	bool emergency_disable = false;
	bool restart_discovery = false;
	int cleanup_ret = 0;

	mutex_lock(&priv->lock);
	to1_state = (struct airoha_xgs_to1_state) {
		.removing = priv->removing,
		.enabled = priv->enabled,
		.hardware_selected = priv->hardware_selected,
		.mac_present = !!priv->mac,
		.deadline = priv->to1_deadline,
		.armed_generation = priv->to1_generation,
		.current_generation = priv->session_generation,
		.expected_state = priv->to1_expected_state,
	};
	if (priv->mac)
		to1_state.current_state = FIELD_GET(
			EN7581_XGSPON_ACTIVATION_STATE,
			readl(priv->mac + EN7581_XGSPON_ACTIVATION_ST));
	decision = airoha_xgs_to1_decide(
		&to1_state, jiffies, EN7581_XGSPON_ACTIVATION_O4);
	if (decision == AIROHA_XGS_TO1_DISARM) {
		en7581_xgspon_cancel_to1_locked(priv);
		goto unlock;
	}
	if (decision == AIROHA_XGS_TO1_WAIT) {
		remaining = priv->to1_deadline - jiffies;
		mod_delayed_work(system_wq, &priv->to1_work, remaining);
		goto unlock;
	}

	generation = priv->to1_generation;
	priv->to1_deadline = 0;
	priv->to1_generation = 0;
	priv->to1_expected_state = 0;
	priv->to1_timeout_events++;

	/* TO1 is a session boundary. Keep the laser dark while removing every
	 * assignment derived from the timed-out Profile before rediscovery. */
	cleanup_ret = airoha_xgs_to1_cleanup_transaction(
		&en7581_xgspon_to1_cleanup_ops, priv, &cleanup_result);
	if (cleanup_result.disable_error)
		emergency_disable = true;
	if (cleanup_ret) {
		en7581_xgspon_activation_fault_locked(priv);
	} else if (priv->enabled && priv->hardware_selected &&
		   priv->hardware_state_valid &&
		   en7581_xgspon_phy_trusted_locked(priv)) {
		restart_discovery = true;
	}
	dev_warn_ratelimited(priv->dev,
		"XG/XGS-PON TO1 expired in O4 for session %llu; disable=%d session=%d restart=%u\n",
		generation, cleanup_result.disable_error,
		cleanup_result.session_error, restart_discovery);

unlock:
	mutex_unlock(&priv->lock);
	if (emergency_disable)
		airoha_en7572_emergency_disable(priv->bosa);
	if (restart_discovery && !READ_ONCE(priv->removing))
		mod_delayed_work(system_wq, &priv->activation_work, 0);
}

static bool en7581_xgspon_phy_live_ready(struct en7581_xgspon *priv,
					 u32 *rx_sync_value,
					 u32 *sfp_status_value,
					 u32 *phy_status_value)
{
	u32 rx_sync = readl(priv->phy_csr +
			    EN7581_XGSPON_PHY_RX_SYNC_STATUS);
	u32 sfp_status = readl(priv->phy_csr +
			      EN7581_XGSPON_PHY_SFP_STATUS);
	u32 phy_status = readl(priv->phy_csr + EN7581_XGSPON_PHY_STATUS);

	if (rx_sync_value)
		*rx_sync_value = rx_sync;
	if (sfp_status_value)
		*sfp_status_value = sfp_status;
	if (phy_status_value)
		*phy_status_value = phy_status;

	return !(sfp_status & EN7581_XGSPON_PHY_SFP_RX_LOS) &&
	       (rx_sync & EN7581_XGSPON_PHY_RX_SYNC) &&
	       (phy_status & EN7581_XGSPON_PHY_PHYA_READY);
}

static bool en7581_xgspon_phy_trusted_locked(struct en7581_xgspon *priv)
{
	return priv->hardware_selected && priv->phy_ready && !priv->phy_los &&
	       !priv->phy_lof && !priv->phy_recovering && !priv->phy_fault &&
	       en7581_xgspon_phy_live_ready(priv, NULL, NULL, NULL);
}

static void en7581_xgspon_phy_set_rx(struct en7581_xgspon *priv,
				     bool enabled)
{
	u32 value = readl(priv->phy_csr + EN7581_XGSPON_PHY_RX_SYNC_CTRL);

	if (enabled)
		value |= EN7581_XGSPON_PHY_RX_ENABLE;
	else
		value &= ~EN7581_XGSPON_PHY_RX_ENABLE;
	writel(value, priv->phy_csr + EN7581_XGSPON_PHY_RX_SYNC_CTRL);
}

static int en7581_xgspon_set_mac_reset(struct en7581_xgspon *priv,
				       bool asserted)
{
	u32 value = readl(priv->mac + EN7581_XGSPON_SW_RST);

	if (asserted)
		value &= ~EN7581_XGSPON_MAC_RESET_RELEASE;
	else
		value |= EN7581_XGSPON_MAC_RESET_RELEASE;
	writel(value, priv->mac + EN7581_XGSPON_SW_RST);
	value = readl(priv->mac + EN7581_XGSPON_SW_RST);

	return !!(value & EN7581_XGSPON_MAC_RESET_RELEASE) == !asserted ?
		0 : -EIO;
}

static int en7581_xgspon_set_path_stopped(struct en7581_xgspon *priv,
					  bool stopped)
{
	u32 value = readl(priv->mac + EN7581_XGSPON_MBI_MPI_STOP);

	if (stopped)
		value |= EN7581_XGSPON_PATH_STOP_MASK;
	else
		value &= ~EN7581_XGSPON_PATH_STOP_MASK;
	writel(value, priv->mac + EN7581_XGSPON_MBI_MPI_STOP);

	if (!stopped)
		return readl(priv->mac + EN7581_XGSPON_MBI_MPI_STOP) &
		       EN7581_XGSPON_PATH_STOP_MASK ? -EIO : 0;

	return readl_poll_timeout(priv->mac + EN7581_XGSPON_MBI_MPI_STOP,
		value, (value & EN7581_XGSPON_PATH_STOP_MASK) ==
			EN7581_XGSPON_PATH_STOP_MASK &&
		       (value & EN7581_XGSPON_PATH_STOP_DONE_MASK) ==
			EN7581_XGSPON_PATH_STOP_DONE_MASK,
		1, EN7581_XGSPON_MAC_STOP_TIMEOUT_US);
}

static int en7581_xgspon_hold_mac(struct en7581_xgspon *priv)
{
	int reset_ret;
	int ret;

	ret = en7581_xgspon_set_path_stopped(priv, true);
	reset_ret = en7581_xgspon_set_mac_reset(priv, true);

	return ret ? ret : reset_ret;
}

static int en7581_xgspon_prepare_mac(struct en7581_xgspon *priv)
{
	u32 value;
	int ret;

	/* Match the SDK release order while keeping all data paths stopped. */
	value = readl(priv->mac + EN7581_XGSPON_MBI_MPI_STOP) |
		EN7581_XGSPON_PATH_STOP_MASK;
	writel(value, priv->mac + EN7581_XGSPON_MBI_MPI_STOP);
	ret = en7581_xgspon_set_mac_reset(priv, false);
	if (!ret)
		ret = en7581_xgspon_set_path_stopped(priv, true);
	if (ret)
		en7581_xgspon_hold_mac(priv);

	return ret;
}

static bool en7581_xgspon_lods_session_ready_locked(
	struct en7581_xgspon *priv)
{
	return priv->enabled && priv->hardware_state_valid &&
		priv->tx_state == EN7581_XGSPON_TX_REGISTERED &&
		priv->tx_armed && priv->tx_authorized &&
		priv->registration_key_switch_verified &&
		priv->session_keys_set && priv->session_keys_programmed &&
		priv->onu_id_set && priv->omcc_xgem_programmed &&
		priv->equalization_delay_programmed &&
		!priv->ack_in_flight && !priv->key_report_in_flight &&
		!priv->data_key_pending &&
		FIELD_GET(EN7581_XGSPON_ACTIVATION_STATE,
			readl(priv->mac + EN7581_XGSPON_ACTIVATION_ST)) ==
			EN7581_XGSPON_ACTIVATION_O5;
}

static int en7581_xgspon_lods_release_hardware_locked(
	struct en7581_xgspon *priv)
{
	u32 value;
	int ret = 0;

	value = readl(priv->mac + EN7581_XGSPON_SW_RST) |
		EN7581_XGSPON_MAC_RESET_RELEASE;
	writel(value, priv->mac + EN7581_XGSPON_SW_RST);
	if (!(readl(priv->mac + EN7581_XGSPON_SW_RST) &
	      EN7581_XGSPON_MAC_RESET_RELEASE))
		ret = -EIO;

	value = readl(priv->mac + EN7581_XGSPON_MBI_MPI_STOP) &
		~EN7581_XGSPON_LODS_STOP_MASK;
	writel(value, priv->mac + EN7581_XGSPON_MBI_MPI_STOP);
	if (readl(priv->mac + EN7581_XGSPON_MBI_MPI_STOP) &
	    EN7581_XGSPON_LODS_STOP_MASK)
		ret = -EIO;

	writel(priv->lods_us_profile_valid,
	       priv->mac + EN7581_XGSPON_US_PROF_VLD);
	if (readl(priv->mac + EN7581_XGSPON_US_PROF_VLD) !=
	    priv->lods_us_profile_valid)
		ret = -EIO;

	return ret;
}

static int en7581_xgspon_lods_begin_locked(struct en7581_xgspon *priv)
{
	u32 value;
	int rollback_ret;
	int ret;

	if (!en7581_xgspon_lods_session_ready_locked(priv))
		return -EAGAIN;

	priv->lods_us_profile_valid =
		readl(priv->mac + EN7581_XGSPON_US_PROF_VLD);
	priv->lods_key_indices = readl(priv->mac + EN7581_XGSPON_CUR_KIDX) &
		(EN7581_XGSPON_CUR_PIK_IDX | EN7581_XGSPON_CUR_OIK_IDX);
	value = priv->lods_us_profile_valid &
		~EN7581_XGSPON_US_PROFILE_VALID_MASK;
	writel(value, priv->mac + EN7581_XGSPON_US_PROF_VLD);
	if (readl(priv->mac + EN7581_XGSPON_US_PROF_VLD) != value) {
		ret = -EIO;
		goto rollback;
	}

	value = readl(priv->mac + EN7581_XGSPON_MBI_MPI_STOP) |
		EN7581_XGSPON_LODS_STOP_MASK;
	writel(value, priv->mac + EN7581_XGSPON_MBI_MPI_STOP);
	ret = readl_poll_timeout(priv->mac + EN7581_XGSPON_MBI_MPI_STOP,
		value, (value & EN7581_XGSPON_LODS_STOP_DONE_MASK) ==
			EN7581_XGSPON_LODS_STOP_DONE_MASK,
		1, EN7581_XGSPON_MAC_STOP_TIMEOUT_US);
	if (ret)
		goto rollback;

	ret = en7581_xgspon_set_activation_state(
		priv, EN7581_XGSPON_ACTIVATION_O6);
	if (ret)
		goto rollback;
	value = readl(priv->mac + EN7581_XGSPON_SW_RST) &
		~EN7581_XGSPON_MAC_RESET_RELEASE;
	writel(value, priv->mac + EN7581_XGSPON_SW_RST);
	if (readl(priv->mac + EN7581_XGSPON_SW_RST) &
	    EN7581_XGSPON_MAC_RESET_RELEASE) {
		ret = -EIO;
		goto rollback;
	}

	priv->lods_session_preserved = true;
	priv->lods_deadline = jiffies +
		msecs_to_jiffies(EN7581_XGSPON_LODS_TIMEOUT_MS);
	priv->optical_tx_enabled = false;
	return 0;

rollback:
	rollback_ret = en7581_xgspon_lods_release_hardware_locked(priv);
	if (FIELD_GET(EN7581_XGSPON_ACTIVATION_STATE,
		      readl(priv->mac + EN7581_XGSPON_ACTIVATION_ST)) ==
	    EN7581_XGSPON_ACTIVATION_O6 &&
	    en7581_xgspon_set_activation_state(
		priv, EN7581_XGSPON_ACTIVATION_O1))
		rollback_ret = -EIO;
	if (rollback_ret)
		priv->hardware_state_valid = false;
	priv->lods_session_preserved = false;
	priv->lods_deadline = 0;
	return ret;
}

static int en7581_xgspon_lods_verify_session_locked(
	struct en7581_xgspon *priv)
{
	u32 data_control, data_valid, expected;
	u32 equalization_delay;
	u32 onu_id, xgem_status;
	bool tcont_valid;
	u16 alloc_id;
	unsigned int i;
	u8 slot;

	onu_id = readl(priv->mac + EN7581_XGSPON_ONU_ID);
	if ((onu_id & (EN7581_XGSPON_ONU_ID_VALUE |
		       EN7581_XGSPON_ONU_ID_VALID)) !=
	    (FIELD_PREP(EN7581_XGSPON_ONU_ID_VALUE, priv->onu_id) |
	     EN7581_XGSPON_ONU_ID_VALID))
		return -EIO;
	if (en7581_xgspon_read_xgem(priv, priv->onu_id, &xgem_status) ||
	    xgem_status != (EN7581_XGSPON_GEM_STATUS_VALID |
			    EN7581_XGSPON_GEM_STATUS_UNICAST))
		return -EIO;
	equalization_delay = priv->active_mode == AIROHA_XPON_MODE_XGSPON ?
		priv->equalization_delay << 2 : priv->equalization_delay;
	if (!en7581_xgspon_key_matches(priv, EN7581_XGSPON_PIK0,
					priv->session_keys.ploam) ||
	    !en7581_xgspon_key_matches(priv, EN7581_XGSPON_OIK0,
					priv->session_keys.omci) ||
	    !en7581_xgspon_key_matches(priv, EN7581_XGSPON_KEK0,
					priv->session_keys.kek) ||
	    !en7581_xgspon_pon_tag_matches(priv, priv->pon_tag) ||
	    readl(priv->mac + EN7581_XGSPON_EQD) != equalization_delay)
		return -EIO;

	for (i = 0; i < priv->service_tcont_count; i++) {
		const struct airoha_xgs_service_tcont *t =
			&priv->service_tconts[i];

		if (en7581_xgspon_read_tcont(priv, t->index, &tcont_valid,
					       &alloc_id) || !tcont_valid ||
		    alloc_id != t->alloc_id)
			return -EIO;
	}
	for (i = 0; i < priv->service_xgem_count; i++) {
		const struct airoha_xgs_service_xgem *x =
			&priv->service_xgems[i];

		expected = EN7581_XGSPON_GEM_STATUS_VALID;
		if (x->unicast)
			expected |= EN7581_XGSPON_GEM_STATUS_UNICAST;
		if (x->upstream_encrypted)
			expected |= EN7581_XGSPON_GEM_STATUS_US_ENCRYPT;
		if (en7581_xgspon_read_xgem(priv, x->xgem_id, &xgem_status) ||
		    xgem_status != expected)
			return -EIO;
	}

	if (!priv->data_key_confirmed)
		return 0;
	if (priv->data_key_index < 1 || priv->data_key_index > 2)
		return -EIO;
	slot = priv->data_key_index - 1;
	data_control = readl(priv->mac + EN7581_XGSPON_US_AES_KEY_CTRL);
	data_valid = readl(priv->mac + EN7581_XGSPON_DS_AES_KEY_VALID);
	if (!en7581_xgspon_key_matches(priv,
			slot ? EN7581_XGSPON_AES_UC_KEY1 :
			       EN7581_XGSPON_AES_UC_KEY0,
			priv->data_keys[slot]) ||
	    (data_control & (EN7581_XGSPON_US_AES_KEY_VALID |
			     EN7581_XGSPON_US_AES_KEY_INDEX)) !=
		(EN7581_XGSPON_US_AES_KEY_VALID |
		 (slot ? EN7581_XGSPON_US_AES_KEY_INDEX : 0)) ||
	    (data_valid & EN7581_XGSPON_DS_AES_UC_KEYS_VALID) != BIT(slot))
		return -EIO;

	return 0;
}

static int en7581_xgspon_lods_restore_locked(struct en7581_xgspon *priv)
{
	u32 resolved, value;
	int ret;

	if (!priv->lods_session_preserved ||
	    FIELD_GET(EN7581_XGSPON_ACTIVATION_STATE,
		      readl(priv->mac + EN7581_XGSPON_ACTIVATION_ST)) !=
		EN7581_XGSPON_ACTIVATION_O6)
		return -EINVAL;

	value = readl(priv->mac + EN7581_XGSPON_SW_RST) |
		EN7581_XGSPON_MAC_RESET_RELEASE;
	writel(value, priv->mac + EN7581_XGSPON_SW_RST);
	if (!(readl(priv->mac + EN7581_XGSPON_SW_RST) &
	      EN7581_XGSPON_MAC_RESET_RELEASE))
		return -EIO;

	value = readl(priv->mac + EN7581_XGSPON_SW_SET_KIDX);
	value &= ~(EN7581_XGSPON_SET_PIK_IDX | EN7581_XGSPON_SET_OIK_IDX);
	if (priv->lods_key_indices & EN7581_XGSPON_CUR_PIK_IDX)
		value |= EN7581_XGSPON_SET_PIK_IDX;
	if (priv->lods_key_indices & EN7581_XGSPON_CUR_OIK_IDX)
		value |= EN7581_XGSPON_SET_OIK_IDX;
	value |= EN7581_XGSPON_SET_PIK_EN | EN7581_XGSPON_SET_OIK_EN;
	writel(value, priv->mac + EN7581_XGSPON_SW_SET_KIDX);
	ret = readl_poll_timeout(priv->mac + EN7581_XGSPON_CUR_KIDX, value,
		(value & (EN7581_XGSPON_CUR_PIK_IDX |
			  EN7581_XGSPON_CUR_OIK_IDX)) == priv->lods_key_indices,
		1, EN7581_XGSPON_MAC_STOP_TIMEOUT_US);
	if (ret)
		return ret;

	value = readl(priv->mac + EN7581_XGSPON_MBI_MPI_STOP) &
		~EN7581_XGSPON_MPI_RX_STOP;
	writel(value, priv->mac + EN7581_XGSPON_MBI_MPI_STOP);
	if (readl(priv->mac + EN7581_XGSPON_MBI_MPI_STOP) &
	    EN7581_XGSPON_MPI_RX_STOP)
		return -EIO;

	ret = en7581_xgspon_program_equalization_delay(
		priv, priv->equalization_delay, true, false, &resolved);
	if (ret)
		return -EIO;
	if (resolved != priv->equalization_delay)
		return -EIO;
	ret = en7581_xgspon_lods_verify_session_locked(priv);
	if (ret)
		return ret;
	ret = en7581_xgspon_set_activation_state(
		priv, EN7581_XGSPON_ACTIVATION_O5);
	if (ret)
		return ret;

	value = readl(priv->mac + EN7581_XGSPON_DBG_RESYNC) |
		EN7581_XGSPON_SW_RESYNC_ENABLE | EN7581_XGSPON_SW_RESYNC_START;
	writel(value, priv->mac + EN7581_XGSPON_DBG_RESYNC);
	ret = readl_poll_timeout(priv->mac + EN7581_XGSPON_DBG_RESYNC, value,
		value & EN7581_XGSPON_TX_SYNC_READY, 1,
		EN7581_XGSPON_MAC_STOP_TIMEOUT_US);
	if (ret)
		return ret;

	ret = en7581_xgspon_lods_release_hardware_locked(priv);
	if (ret)
		return ret;
	priv->lods_session_preserved = false;
	priv->lods_deadline = 0;
	return 0;
}

static void en7581_xgspon_phy_invalidate_session_locked(
	struct en7581_xgspon *priv)
{
	bool session_present = priv->session_keys_set || priv->onu_id_set ||
			       priv->omcc_xgem_programmed ||
			       priv->registration_key_switch_verified;
	bool lods_reactivation = priv->mac &&
		FIELD_GET(EN7581_XGSPON_ACTIVATION_STATE,
			  readl(priv->mac + EN7581_XGSPON_ACTIVATION_ST)) ==
			EN7581_XGSPON_ACTIVATION_O6;
	int ret = 0;

	if (priv->lods_session_preserved &&
	    en7581_xgspon_lods_release_hardware_locked(priv))
		priv->hardware_state_valid = false;
	priv->lods_session_preserved = false;
	priv->lods_deadline = 0;
	priv->tx_authorized = false;
	priv->tx_armed = false;
	priv->registration_key_switch_verified = false;
	priv->optical_tx_enabled = false;
	if (session_present)
		ret = en7581_xgspon_clear_session_keys(priv);
	/* clear_session_keys() advances the generation after the O6-to-O1
	 * write. Carry that boundary event into the resulting generation. */
	if (lods_reactivation && priv->mac &&
	    FIELD_GET(EN7581_XGSPON_ACTIVATION_STATE,
		      readl(priv->mac + EN7581_XGSPON_ACTIVATION_ST)) ==
		EN7581_XGSPON_ACTIVATION_O1)
		priv->pm_lods.reactivations++;
	if (ret)
		dev_err(priv->dev,
			"failed to invalidate XGS-PON session after PHY loss\n");
}

static void en7581_xgspon_phy_schedule_recovery(
	struct en7581_xgspon *priv, unsigned long delay)
{
	if (priv->removing || priv->phy_fault)
		return;
	priv->phy_ready = false;
	priv->phy_recovering = true;
	if (!priv->lods_session_preserved)
		en7581_xgspon_phy_invalidate_session_locked(priv);
	mod_delayed_work(system_wq, &priv->phy_recovery_work, delay);
}

static void en7581_xgspon_phy_recovery_work(struct work_struct *work)
{
	struct en7581_xgspon *priv = container_of(
		to_delayed_work(work), struct en7581_xgspon, phy_recovery_work);
	enum airoha_pcs_xpon_mode pcs_mode;
	unsigned long delay = 0;
	bool retry = false;
	int ret;

	mutex_lock(&priv->lock);
	if (priv->removing || !priv->hardware_selected || !priv->phy_csr ||
	    !priv->pcs) {
		priv->phy_recovering = false;
		mutex_unlock(&priv->lock);
		return;
	}
	if (readl(priv->phy_csr + EN7581_XGSPON_PHY_SFP_STATUS) &
	    EN7581_XGSPON_PHY_SFP_RX_LOS) {
		if (priv->lods_session_preserved &&
		    time_before(jiffies, priv->lods_deadline)) {
			delay = priv->lods_deadline - jiffies;
			mutex_unlock(&priv->lock);
			mod_delayed_work(system_wq, &priv->phy_recovery_work,
					 delay);
			return;
		}
		if (priv->lods_session_preserved)
			en7581_xgspon_phy_invalidate_session_locked(priv);
		priv->phy_los = true;
		priv->phy_ready = false;
		priv->phy_recovering = false;
		en7581_xgspon_phy_set_rx(priv, false);
		mutex_unlock(&priv->lock);
		airoha_en7572_set_tx_enabled(priv->bosa, false);
		return;
	}
	if (priv->active_mode == AIROHA_XPON_MODE_XGPON)
		pcs_mode = AIROHA_PCS_XPON_MODE_XGPON;
	else if (priv->active_mode == AIROHA_XPON_MODE_XGSPON)
		pcs_mode = AIROHA_PCS_XPON_MODE_XGSPON;
	else {
		priv->phy_recovering = false;
		mutex_unlock(&priv->lock);
		return;
	}

	priv->phy_recovery_attempts++;
	priv->phy_ready = false;
	priv->phy_los = false;
	priv->phy_lof = false;
	if (!priv->lods_session_preserved)
		en7581_xgspon_phy_invalidate_session_locked(priv);
	en7581_xgspon_phy_set_rx(priv, false);
	writel(EN7581_XGSPON_PHY_RESET_HOLD,
	       priv->phy_csr + EN7581_XGSPON_PHY_RESET);
	mutex_unlock(&priv->lock);

	/* The BOSA must stay dark across every PMA/CDR recovery attempt. */
	ret = airoha_en7572_set_tx_enabled(priv->bosa, false);
	if (!ret)
		ret = airoha_pcs_xpon_quiesce(priv->pcs);
	if (!ret)
		ret = airoha_pcs_xpon_select_wan(priv->pcs, pcs_mode);
	if (!ret)
		ret = airoha_pcs_xpon_set_mode(priv->pcs, pcs_mode);
	if (!ret)
		ret = airoha_pcs_xpon_recover(priv->pcs);

	mutex_lock(&priv->lock);
	if (priv->removing) {
		priv->phy_recovering = false;
		mutex_unlock(&priv->lock);
		return;
	}
	if (ret) {
		priv->phy_recovery_failures++;
		if (priv->phy_recovery_attempts <
		    EN7581_XGSPON_PHY_MAX_RECOVERY_ATTEMPTS)
			retry = true;
		else {
			if (priv->lods_session_preserved)
				en7581_xgspon_phy_invalidate_session_locked(priv);
			priv->phy_fault = true;
			priv->phy_recovering = false;
		}
		mutex_unlock(&priv->lock);
		if (retry)
			mod_delayed_work(system_wq, &priv->phy_recovery_work,
					 msecs_to_jiffies(
						 EN7581_XGSPON_PHY_RETRY_DELAY_MS));
		return;
	}
	if (readl(priv->phy_csr + EN7581_XGSPON_PHY_SFP_STATUS) &
	    EN7581_XGSPON_PHY_SFP_RX_LOS) {
		if (priv->lods_session_preserved &&
		    time_before(jiffies, priv->lods_deadline)) {
			delay = priv->lods_deadline - jiffies;
			retry = true;
		} else if (priv->lods_session_preserved) {
			en7581_xgspon_phy_invalidate_session_locked(priv);
		}
		priv->phy_los = true;
		priv->phy_ready = false;
		priv->phy_recovering = retry;
		priv->phy_recovery_attempts = 0;
		en7581_xgspon_phy_set_rx(priv, false);
		mutex_unlock(&priv->lock);
		if (retry)
			mod_delayed_work(system_wq, &priv->phy_recovery_work,
					 delay);
		return;
	}

	writel(EN7581_XGSPON_PHY_RESET_RELEASE,
	       priv->phy_csr + EN7581_XGSPON_PHY_RESET);
	en7581_xgspon_phy_set_rx(priv, true);
	mutex_unlock(&priv->lock);
	msleep(EN7581_XGSPON_PHY_SETTLE_MS);

	mutex_lock(&priv->lock);
	if (!priv->removing && en7581_xgspon_phy_live_ready(priv, NULL, NULL,
						    NULL)) {
		ret = priv->lods_session_preserved ?
			en7581_xgspon_lods_restore_locked(priv) : 0;
		if (ret) {
			priv->phy_recovery_failures++;
			en7581_xgspon_phy_invalidate_session_locked(priv);
		}
		/* A failed fast restore may still fall back to a fully verified O1
		 * cleanup. Allow discovery to restart in that case; retain a hard
		 * fault only when cleanup cannot establish the hardware state. */
		priv->phy_ready = priv->hardware_state_valid;
		priv->phy_los = false;
		priv->phy_lof = false;
		priv->phy_recovering = false;
		priv->phy_fault = !priv->hardware_state_valid;
		priv->phy_recovery_attempts = 0;
		if (priv->phy_ready) {
			priv->phy_recoveries++;
			mod_delayed_work(system_wq, &priv->activation_work, 0);
		}
	} else if (!priv->removing &&
		   !(readl(priv->phy_csr + EN7581_XGSPON_PHY_SFP_STATUS) &
		     EN7581_XGSPON_PHY_SFP_RX_LOS) &&
		   priv->phy_recovery_attempts <
			EN7581_XGSPON_PHY_MAX_RECOVERY_ATTEMPTS) {
		retry = true;
	} else {
		priv->phy_recovering = false;
		if (!priv->removing && !priv->phy_los) {
			if (priv->lods_session_preserved)
				en7581_xgspon_phy_invalidate_session_locked(priv);
			priv->phy_fault = true;
			priv->phy_recovery_failures++;
		}
	}
	mutex_unlock(&priv->lock);

	if (retry)
		mod_delayed_work(system_wq, &priv->phy_recovery_work,
				 msecs_to_jiffies(
					 EN7581_XGSPON_PHY_RETRY_DELAY_MS));
}

static bool en7581_xgspon_write_verified(void __iomem *base, u32 offset,
					 u32 value)
{
	writel(value, base + offset);
	return readl(base + offset) == value;
}

static int en7581_xgspon_program_profile_locked(
	struct en7581_xgspon *priv, const struct airoha_xgs_profile *profile)
{
	u32 phy_offsets[] = {
		EN7581_XGSPON_PHY_PREAMBLE_BASE +
			profile->index * EN7581_XGSPON_PROFILE_REGISTER_STRIDE,
		EN7581_XGSPON_PHY_PREAMBLE_BASE +
			profile->index * EN7581_XGSPON_PROFILE_REGISTER_STRIDE + 4,
		EN7581_XGSPON_PHY_DELIMITER_BASE +
			profile->index * EN7581_XGSPON_PROFILE_REGISTER_STRIDE,
		EN7581_XGSPON_PHY_DELIMITER_BASE +
			profile->index * EN7581_XGSPON_PROFILE_REGISTER_STRIDE + 4,
		EN7581_XGSPON_PHY_TX_FEC_CTRL,
		EN7581_XGSPON_PHY_PSBU_INFO_BASE +
			profile->index * EN7581_XGSPON_PROFILE_INFO_STRIDE,
	};
	u32 old_phy[ARRAY_SIZE(phy_offsets)];
	u32 new_phy[ARRAY_SIZE(phy_offsets)];
	u32 valid, length, old_valid, old_length;
	u32 length_offset, length_shift, valid_shift;
	u16 psbu_length;
	bool rollback_failed = false;
	unsigned int i;

	if (profile->index > 3 || !priv->mac || !priv->phy_csr)
		return -EINVAL;

	length_offset = profile->index < 2 ?
		EN7581_XGSPON_US_PROF_PSB_LEN_0_1 :
		EN7581_XGSPON_US_PROF_PSB_LEN_2_3;
	length_shift = (profile->index & 1) * 16;
	valid_shift = profile->index * 8;
	psbu_length = profile->preamble_length *
		profile->preamble_repeat_count + profile->delimiter_length;

	old_valid = readl(priv->mac + EN7581_XGSPON_US_PROF_VLD);
	old_length = readl(priv->mac + length_offset);
	for (i = 0; i < ARRAY_SIZE(phy_offsets); i++)
		old_phy[i] = readl(priv->phy_csr + phy_offsets[i]);

	new_phy[0] = get_unaligned_be32(profile->preamble);
	new_phy[1] = get_unaligned_be32(profile->preamble + sizeof(u32));
	new_phy[2] = get_unaligned_be32(profile->delimiter);
	new_phy[3] = get_unaligned_be32(profile->delimiter + sizeof(u32));
	new_phy[4] = (old_phy[4] & ~BIT(valid_shift)) |
		(profile->fec ? BIT(valid_shift) : 0);
	new_phy[5] = profile->delimiter_length |
		(profile->preamble_length << 8) |
		(profile->preamble_repeat_count << 16);
	length = (old_length & ~(0xffffU << length_shift)) |
		((u32)psbu_length << length_shift);
	valid = old_valid & ~(0xf1U << valid_shift);
	valid |= BIT(valid_shift) | (profile->version << (valid_shift + 4));

	for (i = 0; i < ARRAY_SIZE(phy_offsets); i++)
		if (!en7581_xgspon_write_verified(priv->phy_csr, phy_offsets[i],
						  new_phy[i]))
			goto rollback;
	if (!en7581_xgspon_write_verified(priv->mac, length_offset, length) ||
	    !en7581_xgspon_write_verified(priv->mac,
					  EN7581_XGSPON_US_PROF_VLD, valid))
		goto rollback;

	return 0;

rollback:
	for (i = 0; i < ARRAY_SIZE(phy_offsets); i++)
		rollback_failed |= !en7581_xgspon_write_verified(
			priv->phy_csr, phy_offsets[i], old_phy[i]);
	rollback_failed |= !en7581_xgspon_write_verified(
		priv->mac, length_offset, old_length);
	rollback_failed |= !en7581_xgspon_write_verified(
		priv->mac, EN7581_XGSPON_US_PROF_VLD, old_valid);
	if (rollback_failed) {
		priv->hardware_state_valid = false;
		airoha_en7572_emergency_disable(priv->bosa);
	}
	return -EIO;
}

static bool en7581_xgspon_profile_uses_default_key(
	enum airoha_xpon_mode mode, u16 destination)
{
	if (destination == EN7581_XGPON_BROADCAST_ONU_ID)
		return true;

	return mode == AIROHA_XPON_MODE_XGSPON &&
	       (destination == EN7581_XGSPON_BROADCAST_ONU_ID ||
		destination == EN7581_XGSPON_NOKIA_BROADCAST_ONU_ID);
}

static bool en7581_xgspon_profile_requires_ack(
	enum airoha_xpon_mode mode, u16 destination, u32 activation_state)
{
	if (activation_state == EN7581_XGSPON_ACTIVATION_O2_3)
		return false;
	if (destination == EN7581_XGPON_BROADCAST_ONU_ID)
		return false;

	/* Match the SDK's XGS acknowledgement test. Its Nokia 0x7ff discovery
	 * address is accepted as a default-key destination, but only 0x3ff and
	 * 0x3fe suppress the operational ACK. */
	return mode != AIROHA_XPON_MODE_XGSPON ||
	       destination != EN7581_XGSPON_BROADCAST_ONU_ID;
}

/* The MAC FIFO owner is the only caller. Profile authentication, state gates,
 * PHY programming, initial key derivation and directed ACK reservation remain
 * under one mutex; userspace cannot assert that a Profile was authenticated. */
static int en7581_xgspon_accept_profile(
	struct en7581_xgspon *priv,
	const u32 words[AIROHA_XGS_PLOAM_FIFO_WORDS])
{
	struct airoha_xgs_security_keys default_keys;
	struct airoha_xgs_security_keys keys;
	struct airoha_xgs_profile profile;
	u8 default_registration_id[AIROHA_XGS_REGISTRATION_ID_SIZE];
	u8 default_msk[AIROHA_XGS_KEY_SIZE];
	u8 default_ploam[AIROHA_XGS_KEY_SIZE];
	const u8 *authentication_key;
	u32 activation_state, current_index;
	u16 destination;
	u8 ack_key_index = AIROHA_XGS_PLOAM_KEY_INDEX_1;
	bool xgspon, default_key, requires_ack;
	int ret;

	memset(&default_keys, 0, sizeof(default_keys));
	memset(&keys, 0, sizeof(keys));
	memset(default_registration_id, 0, sizeof(default_registration_id));
	memset(default_msk, 0, sizeof(default_msk));
	memset(default_ploam, 0, sizeof(default_ploam));
	memset(&profile, 0, sizeof(profile));
	mutex_lock(&priv->lock);
	if (!en7581_xgspon_phy_trusted_locked(priv)) {
		ret = -ENOLINK;
		goto out;
	}
	if (!priv->hardware_state_valid) {
		ret = -EIO;
		goto out;
	}
	if (!priv->serial_set || !priv->registration_msk_set) {
		ret = -ENODATA;
		goto out;
	}
	activation_state = FIELD_GET(
		EN7581_XGSPON_ACTIVATION_STATE,
		readl(priv->mac + EN7581_XGSPON_ACTIVATION_ST));
	if (activation_state != EN7581_XGSPON_ACTIVATION_O2_3 &&
	    activation_state != EN7581_XGSPON_ACTIVATION_O4 &&
	    activation_state != EN7581_XGSPON_ACTIVATION_O5) {
		ret = -EAGAIN;
		goto out;
	}
	xgspon = priv->active_mode == AIROHA_XPON_MODE_XGSPON;
	if (!xgspon && priv->active_mode != AIROHA_XPON_MODE_XGPON) {
		ret = -EPROTO;
		goto out;
	}
	destination = words[0] >> 16;
	default_key = en7581_xgspon_profile_uses_default_key(
		priv->active_mode, destination);
	if (activation_state == EN7581_XGSPON_ACTIVATION_O2_3 &&
	    !default_key) {
		ret = -EADDRNOTAVAIL;
		goto out;
	}
	if (activation_state != EN7581_XGSPON_ACTIVATION_O2_3) {
		if (!priv->onu_id_set ||
		    priv->rx_state != EN7581_XGSPON_RX_ONU_ID_ASSIGNED) {
			ret = -ENOKEY;
			goto out;
		}
		if (!default_key && destination != priv->onu_id) {
			ret = -EADDRNOTAVAIL;
			goto out;
		}
	}
	if (priv->session_keys_set && !priv->session_keys_programmed) {
		ret = -EIO;
		goto out;
	}
	if (!priv->session_keys_set &&
	    activation_state != EN7581_XGSPON_ACTIVATION_O2_3) {
		ret = -ENOKEY;
		goto out;
	}

	airoha_xgs_copy_default_ploam_key(default_ploam);
	if (default_key) {
		authentication_key = default_ploam;
	} else {
		current_index = readl(priv->mac + EN7581_XGSPON_CUR_KIDX);
		ack_key_index = current_index & EN7581_XGSPON_CUR_PIK_IDX ?
			AIROHA_XGS_PLOAM_KEY_INDEX_1 :
			AIROHA_XGS_PLOAM_KEY_INDEX_0;
		authentication_key = ack_key_index ==
			AIROHA_XGS_PLOAM_KEY_INDEX_1 ?
			default_ploam : priv->session_keys.ploam;
	}

	ret = airoha_xgs_authenticate_profile_words_mode_key(
		words, authentication_key, xgspon, &profile);
	if (ret)
		goto out;
	if (profile.destination != destination) {
		ret = -EPROTO;
		goto out;
	}
	if (priv->session_keys_set &&
	    crypto_memneq(profile.pon_tag, priv->pon_tag,
			  sizeof(profile.pon_tag))) {
		ret = -EKEYREJECTED;
		goto out;
	}
	if (priv->session_keys_set &&
	    !en7581_xgspon_pon_tag_matches(priv, priv->pon_tag)) {
		ret = -EIO;
		goto out;
	}
	requires_ack = en7581_xgspon_profile_requires_ack(
		priv->active_mode, destination, activation_state);
	if (requires_ack) {
		current_index = readl(priv->mac + EN7581_XGSPON_CUR_KIDX);
		ack_key_index = current_index & EN7581_XGSPON_CUR_PIK_IDX ?
			AIROHA_XGS_PLOAM_KEY_INDEX_1 :
			AIROHA_XGS_PLOAM_KEY_INDEX_0;
		ret = en7581_xgspon_ack_schedule_status_locked(
			priv, EN7581_XGSPON_PROFILE_MESSAGE_ID, profile.sequence,
			AIROHA_XGS_ACK_COMPLETION_OK, ack_key_index);
		if (ret)
			goto out;
	}
	ret = en7581_xgspon_program_profile_locked(priv, &profile);
	if (ret)
		goto out;
	if (priv->session_keys_set) {
		if (requires_ack)
			ret = en7581_xgspon_commit_ack_locked(
				priv, EN7581_XGSPON_PROFILE_MESSAGE_ID,
				profile.sequence, AIROHA_XGS_ACK_COMPLETION_OK,
				ack_key_index);
		goto out;
	}
	ret = airoha_xgs_derive_shared_keys(priv->registration_msk,
					    priv->serial, profile.pon_tag, &keys);
	if (ret)
		goto out;
	ret = airoha_xgs_derive_registration_msk(default_registration_id,
						 default_msk);
	if (ret)
		goto out;
	ret = airoha_xgs_derive_shared_keys(default_msk, priv->serial,
					    profile.pon_tag,
					    &default_keys);
	if (ret)
		goto out;
	ret = en7581_xgspon_program_session_keys(priv, &keys, &default_keys,
						 default_ploam, profile.pon_tag);
	if (ret)
		goto out;

	memcpy(priv->pon_tag, profile.pon_tag, sizeof(priv->pon_tag));
	memcpy(&priv->session_keys, &keys, sizeof(keys));
	priv->session_keys_set = true;
	en7581_xgspon_advance_session_locked(priv);
	priv->tx_state = EN7581_XGSPON_TX_SERIAL_READY;
	priv->rx_state = EN7581_XGSPON_RX_WAIT_ASSIGN_ONU_ID;
	/* A valid Profile is the first point at which the serial PLOAM can be
	 * assembled. Queue it through the same gated worker used after PHY
	 * recovery, rather than leaving the session stuck at serial-ready. */
	if (priv->enabled)
		mod_delayed_work(system_wq, &priv->activation_work, 0);
out:
	mutex_unlock(&priv->lock);
	memzero_explicit(default_registration_id,
			 sizeof(default_registration_id));
	memzero_explicit(default_msk, sizeof(default_msk));
	memzero_explicit(default_ploam, sizeof(default_ploam));
	memzero_explicit(&default_keys, sizeof(default_keys));
	memzero_explicit(&profile, sizeof(profile));
	memzero_explicit(&keys, sizeof(keys));
	return ret;
}

static int en7581_xgspon_accept_assign_onu_id(
	struct en7581_xgspon *priv,
	const u32 words[AIROHA_XGS_PLOAM_FIFO_WORDS])
{
	u8 default_ploam[AIROHA_XGS_KEY_SIZE];
	const u8 *active_key;
	u32 activation_state, current_index;
	u16 onu_id;
	u8 sequence;
	int rollback_ret;
	int ret;

	memset(default_ploam, 0, sizeof(default_ploam));
	mutex_lock(&priv->lock);
	if (!en7581_xgspon_phy_trusted_locked(priv)) {
		ret = -ENOLINK;
		goto out;
	}
	if (!priv->hardware_state_valid) {
		ret = -EIO;
		goto out;
	}
	if (!priv->session_keys_set || !priv->session_keys_programmed ||
	    priv->rx_state == EN7581_XGSPON_RX_WAIT_PROFILE) {
		ret = -ENOKEY;
		goto out;
	}
	activation_state = FIELD_GET(
		EN7581_XGSPON_ACTIVATION_STATE,
		readl(priv->mac + EN7581_XGSPON_ACTIVATION_ST));
	current_index = readl(priv->mac + EN7581_XGSPON_CUR_KIDX);
	if (current_index & EN7581_XGSPON_CUR_PIK_IDX) {
		airoha_xgs_copy_default_ploam_key(default_ploam);
		active_key = default_ploam;
	} else {
		active_key = priv->session_keys.ploam;
	}
	ret = airoha_xgs_authenticate_assign_onu_id_words(
		words, active_key, priv->serial, &onu_id, &sequence);
	if (ret)
		goto out;
	if (priv->onu_id_set) {
		if ((activation_state == EN7581_XGSPON_ACTIVATION_O4 ||
		     activation_state == EN7581_XGSPON_ACTIVATION_O5) &&
		    priv->onu_id == onu_id) {
			ret = -EALREADY;
			goto out;
		}
		ret = en7581_xgspon_clear_session_keys(priv);
		if (!ret)
			ret = -ESTALE;
		goto out;
	}
	if (activation_state != EN7581_XGSPON_ACTIVATION_O2_3 ||
	    priv->tx_state != EN7581_XGSPON_TX_WAIT_REGISTRATION) {
		ret = -EAGAIN;
		goto out;
	}
	ret = en7581_xgspon_program_onu_id(priv, onu_id, true);
	if (ret) {
		if (en7581_xgspon_rollback_onu_assignment(priv, onu_id))
			priv->hardware_state_valid = false;
		goto out;
	}
	ret = en7581_xgspon_program_omcc_xgem(priv, onu_id, true);
	if (ret) {
		if (en7581_xgspon_rollback_onu_assignment(priv, onu_id))
			priv->hardware_state_valid = false;
		goto out;
	}
	ret = en7581_xgspon_set_activation_state(
		priv, EN7581_XGSPON_ACTIVATION_O4);
	if (ret) {
		rollback_ret = en7581_xgspon_rollback_onu_assignment(priv, onu_id);
		if (en7581_xgspon_set_activation_state(
			priv, EN7581_XGSPON_ACTIVATION_O2_3) || rollback_ret)
			priv->hardware_state_valid = false;
		goto out;
	}
	priv->onu_id = onu_id;
	priv->onu_id_set = true;
	priv->omcc_xgem_programmed = true;
	priv->alloc_ids[0] = onu_id;
	priv->alloc_valid[0] = true;
	priv->assign_onu_id_sequence = sequence;
	priv->rx_state = EN7581_XGSPON_RX_ONU_ID_ASSIGNED;
	/* In SDK software-reply mode Assign ONU-ID is the trigger for the
	 * sequence-zero Registration message in O4. Request Registration is a
	 * separate O5 retransmission path and must not gate initial activation. */
	priv->request_registration_authenticated = false;
	priv->registration_response_pending = true;
	priv->request_registration_sequence = 0;
	en7581_xgspon_start_to1_locked(priv);
	if (priv->enabled)
		mod_delayed_work(system_wq, &priv->activation_work, 0);
out:
	mutex_unlock(&priv->lock);
	memzero_explicit(default_ploam, sizeof(default_ploam));
	return ret;
}

static int en7581_xgspon_accept_request_registration(
	struct en7581_xgspon *priv,
	const u32 words[AIROHA_XGS_PLOAM_FIFO_WORDS])
{
	u8 default_ploam[AIROHA_XGS_KEY_SIZE];
	const u8 *active_key;
	u32 activation_state, current_index;
	u8 sequence;
	int ret;

	memset(default_ploam, 0, sizeof(default_ploam));
	mutex_lock(&priv->lock);
	if (!en7581_xgspon_phy_trusted_locked(priv)) {
		ret = -ENOLINK;
		goto out;
	}
	if (!priv->hardware_state_valid) {
		ret = -EIO;
		goto out;
	}
	if (!priv->session_keys_set || !priv->session_keys_programmed ||
	    !priv->onu_id_set || !priv->ranging_time_authenticated ||
	    !priv->equalization_delay_programmed ||
	    priv->rx_state != EN7581_XGSPON_RX_ONU_ID_ASSIGNED) {
		ret = -ENOKEY;
		goto out;
	}
	activation_state = FIELD_GET(
		EN7581_XGSPON_ACTIVATION_STATE,
		readl(priv->mac + EN7581_XGSPON_ACTIVATION_ST));
	if (activation_state != EN7581_XGSPON_ACTIVATION_O5 ||
	    (priv->tx_state != EN7581_XGSPON_TX_WAIT_REGISTRATION &&
	     priv->tx_state != EN7581_XGSPON_TX_REGISTERED)) {
		ret = -EAGAIN;
		goto out;
	}
	current_index = readl(priv->mac + EN7581_XGSPON_CUR_KIDX);
	if (current_index & EN7581_XGSPON_CUR_PIK_IDX) {
		airoha_xgs_copy_default_ploam_key(default_ploam);
		active_key = default_ploam;
	} else {
		active_key = priv->session_keys.ploam;
	}
	ret = airoha_xgs_authenticate_request_registration_words(
		words, active_key, priv->onu_id, &sequence);
	if (!ret) {
		priv->request_registration_authenticated = true;
		priv->registration_response_pending = true;
		priv->request_registration_sequence = sequence;
		if (priv->enabled)
			mod_delayed_work(system_wq, &priv->activation_work, 0);
	}
out:
	mutex_unlock(&priv->lock);
	memzero_explicit(default_ploam, sizeof(default_ploam));
	return ret;
}

static int en7581_xgspon_accept_deactivate_onu_id(
	struct en7581_xgspon *priv,
	const u32 words[AIROHA_XGS_PLOAM_FIFO_WORDS])
{
	u8 default_ploam[AIROHA_XGS_KEY_SIZE];
	const u8 *active_key;
	u32 current_index;
	u8 sequence;
	bool accepted = false;
	int irq_ret;
	int state_ret;
	int ret;

	memset(default_ploam, 0, sizeof(default_ploam));
	mutex_lock(&priv->lock);
	if (!en7581_xgspon_phy_trusted_locked(priv)) {
		ret = -ENOLINK;
		goto out;
	}
	if (!priv->hardware_state_valid) {
		ret = -EIO;
		goto out;
	}
	if (!priv->session_keys_set || !priv->session_keys_programmed ||
	    !priv->onu_id_set ||
	    priv->rx_state == EN7581_XGSPON_RX_WAIT_PROFILE) {
		ret = -ENOKEY;
		goto out;
	}
	current_index = readl(priv->mac + EN7581_XGSPON_CUR_KIDX);
	if (current_index & EN7581_XGSPON_CUR_PIK_IDX) {
		airoha_xgs_copy_default_ploam_key(default_ploam);
		active_key = default_ploam;
	} else {
		active_key = priv->session_keys.ploam;
	}
	ret = airoha_xgs_authenticate_deactivate_onu_id_words(
		words, active_key, priv->onu_id, &sequence);
	if (ret)
		goto out;
	accepted = true;

	priv->enabled = false;
	priv->tx_armed = false;
	priv->tx_authorized = false;
	priv->optical_tx_enabled = false;
	priv->registration_key_switch_verified = false;
	irq_ret = en7581_xgspon_set_mac_irq_enable(
		priv, EN7581_XGSPON_INT_ENABLED);
	state_ret = en7581_xgspon_set_activation_state(
		priv, EN7581_XGSPON_ACTIVATION_O1);
	ret = en7581_xgspon_clear_session_keys(priv);
	priv->last_deactivate_sequence = sequence;
	priv->deactivate_events++;
	if (!ret)
		ret = state_ret;
	if (!ret)
		ret = irq_ret;
	if (ret)
		priv->hardware_state_valid = false;
out:
	mutex_unlock(&priv->lock);
	memzero_explicit(default_ploam, sizeof(default_ploam));
	if (accepted)
		en7581_xgspon_disable_tx(priv);
	return ret;
}

/* Disable Serial Number is authenticated with the active PLOAM key even
 * before an ONU-ID is assigned. Matching disable commands revoke every
 * session object and hold the BOSA dark until an explicit Allow command. */
static int en7581_xgspon_accept_disable_serial_number(
	struct en7581_xgspon *priv,
	const u32 words[AIROHA_XGS_PLOAM_FIFO_WORDS])
{
	enum airoha_xgs_disable_serial_action action;
	u8 default_ploam[AIROHA_XGS_KEY_SIZE];
	const u8 *active_key;
	u32 current_index;
	u8 sequence;
	bool disable_tx = false;
	bool discovery_only = false;
	int irq_ret, ret;

	memset(default_ploam, 0, sizeof(default_ploam));
	mutex_lock(&priv->lock);
	if (!en7581_xgspon_phy_trusted_locked(priv)) {
		ret = -ENOLINK;
		goto out;
	}
	if (!priv->hardware_state_valid || !priv->serial_set) {
		ret = -ENODATA;
		goto out;
	}
	airoha_xgs_copy_default_ploam_key(default_ploam);
	active_key = default_ploam;
	if (priv->session_keys_set && priv->session_keys_programmed) {
		current_index = readl(priv->mac + EN7581_XGSPON_CUR_KIDX);
		if (!(current_index & EN7581_XGSPON_CUR_PIK_IDX))
			active_key = priv->session_keys.ploam;
	}
	ret = airoha_xgs_authenticate_disable_serial_number_words(
		words, active_key, priv->serial, &action, &sequence);
	if (ret)
		goto out;

	switch (action) {
	case AIROHA_XGS_SERIAL_DISABLE:
		priv->disable_serial_events++;
		break;
	case AIROHA_XGS_SERIAL_DISABLE_DISCOVERY:
		priv->disable_discovery_events++;
		if (FIELD_GET(EN7581_XGSPON_ACTIVATION_STATE,
			      readl(priv->mac + EN7581_XGSPON_ACTIVATION_ST)) !=
		    EN7581_XGSPON_ACTIVATION_O2_3)
			goto out;
		discovery_only = true;
		break;
	case AIROHA_XGS_SERIAL_ALLOW:
		priv->allow_serial_events++;
		if (!priv->serial_disabled)
			goto out;
		priv->serial_disabled = false;
		ret = en7581_xgspon_clear_session_keys(priv);
		if (!ret)
			ret = en7581_xgspon_set_activation_state(
				priv, EN7581_XGSPON_ACTIVATION_O1);
		if (!ret && priv->enabled)
			mod_delayed_work(system_wq, &priv->activation_work, 0);
		goto out;
	}

	priv->serial_disabled = true;
	priv->tx_armed = false;
	priv->tx_authorized = false;
	priv->optical_tx_enabled = false;
	irq_ret = en7581_xgspon_set_mac_irq_enable(
		priv, EN7581_XGSPON_INT_ENABLED);
	ret = en7581_xgspon_clear_session_keys(priv);
	if (!ret)
		ret = en7581_xgspon_set_activation_state(
			priv, discovery_only ? EN7581_XGSPON_ACTIVATION_O2_3 :
			EN7581_XGSPON_ACTIVATION_O7);
	if (!ret)
		ret = irq_ret;
	if (ret)
		priv->hardware_state_valid = false;
	disable_tx = true;
out:
	mutex_unlock(&priv->lock);
	memzero_explicit(default_ploam, sizeof(default_ploam));
	if (disable_tx)
		en7581_xgspon_disable_tx(priv);
	return ret;
}

/* The recovered SDK only authenticates and accepts Sleep Allow; it does not
 * enter a sleep state or emit Sleep Request. Keep that conservative behaviour
 * until board-specific low-power sequencing is independently validated. */
static int en7581_xgspon_accept_sleep_allow(
	struct en7581_xgspon *priv,
	const u32 words[AIROHA_XGS_PLOAM_FIFO_WORDS])
{
	u8 default_ploam[AIROHA_XGS_KEY_SIZE];
	const u8 *active_key;
	u32 current_index;
	bool sleep_type;
	u8 sequence;
	int ret;

	memset(default_ploam, 0, sizeof(default_ploam));
	mutex_lock(&priv->lock);
	if (!en7581_xgspon_phy_trusted_locked(priv)) {
		ret = -ENOLINK;
		goto out;
	}
	if (!priv->session_keys_set || !priv->session_keys_programmed ||
	    !priv->onu_id_set) {
		ret = -ENOKEY;
		goto out;
	}
	current_index = readl(priv->mac + EN7581_XGSPON_CUR_KIDX);
	if (current_index & EN7581_XGSPON_CUR_PIK_IDX) {
		airoha_xgs_copy_default_ploam_key(default_ploam);
		active_key = default_ploam;
	} else {
		active_key = priv->session_keys.ploam;
	}
	ret = airoha_xgs_authenticate_sleep_allow_words(
		words, active_key, priv->onu_id, &sleep_type,
		&sequence);
	if (!ret) {
		priv->sleep_allow_events++;
		if (sleep_type)
			priv->sleep_allow_type_one_events++;
	}
out:
	mutex_unlock(&priv->lock);
	memzero_explicit(default_ploam, sizeof(default_ploam));
	return ret;
}

static int en7581_xgspon_accept_reboot_onu(
	struct en7581_xgspon *priv,
	const u32 words[AIROHA_XGS_PLOAM_FIFO_WORDS],
	struct airoha_xgs_reboot_request *request, u64 *generation)
{
	u8 default_ploam[AIROHA_XGS_KEY_SIZE];
	const u8 *active_key;
	u32 current_index;
	int ret;

	memset(default_ploam, 0, sizeof(default_ploam));
	memset(request, 0, sizeof(*request));
	*generation = 0;
	mutex_lock(&priv->lock);
	if (!en7581_xgspon_phy_trusted_locked(priv)) {
		ret = -ENOLINK;
		goto out;
	}
	if (!priv->session_keys_set || !priv->session_keys_programmed ||
	    !priv->onu_id_set) {
		ret = -ENOKEY;
		goto out;
	}
	current_index = readl(priv->mac + EN7581_XGSPON_CUR_KIDX);
	if (current_index & EN7581_XGSPON_CUR_PIK_IDX) {
		airoha_xgs_copy_default_ploam_key(default_ploam);
		active_key = default_ploam;
	} else {
		active_key = priv->session_keys.ploam;
	}
	ret = airoha_xgs_authenticate_reboot_onu_words(
		words, active_key, priv->onu_id, request);
	if (!ret) {
		priv->last_reboot_request = *request;
		priv->reboot_request_events++;
		priv->reboot_request_generation++;
		*generation = priv->reboot_request_generation;
	}
out:
	mutex_unlock(&priv->lock);
	memzero_explicit(default_ploam, sizeof(default_ploam));
	return ret;
}

static void en7581_xgspon_emit_reboot_request(
	struct en7581_xgspon *priv,
	const struct airoha_xgs_reboot_request *request, u64 generation)
{
	char event[] = "AIROHA_XPON_EVENT=reboot-request";
	char generation_env[48];
	char sequence_env[40];
	char depth_env[32];
	char image_env[32];
	char state_env[32];
	char flags_env[32];
	char *envp[] = {
		event, generation_env, sequence_env, depth_env, image_env,
		state_env, flags_env, NULL,
	};

	snprintf(generation_env, sizeof(generation_env),
		 "AIROHA_XPON_REBOOT_GENERATION=%llu",
		 (unsigned long long)generation);
	snprintf(sequence_env, sizeof(sequence_env),
		 "AIROHA_XPON_REBOOT_SEQUENCE=%u", request->sequence);
	snprintf(depth_env, sizeof(depth_env),
		 "AIROHA_XPON_REBOOT_DEPTH=%u", request->depth);
	snprintf(image_env, sizeof(image_env),
		 "AIROHA_XPON_REBOOT_IMAGE=%u", request->image);
	snprintf(state_env, sizeof(state_env),
		 "AIROHA_XPON_REBOOT_STATE=%u", request->state);
	snprintf(flags_env, sizeof(flags_env),
		 "AIROHA_XPON_REBOOT_FLAGS=%u", request->flags);
	if (kobject_uevent_env(&priv->dev->kobj, KOBJ_CHANGE, envp))
		dev_warn_ratelimited(priv->dev,
			"could not emit authenticated Reboot ONU event\n");
}

static int en7581_xgspon_accept_ranging_time(
	struct en7581_xgspon *priv,
	const u32 words[AIROHA_XGS_PLOAM_FIFO_WORDS])
{
	struct airoha_xgs_ranging_time ranging;
	u8 default_ploam[AIROHA_XGS_KEY_SIZE];
	const u8 *active_key;
	u32 activation_state, current_index, previous_eqd, resolved;
	bool acknowledge;
	int ack_ret, rollback_ret, state_ret;
	int ret;

	memset(&ranging, 0, sizeof(ranging));
	memset(default_ploam, 0, sizeof(default_ploam));
	mutex_lock(&priv->lock);
	if (!en7581_xgspon_phy_trusted_locked(priv)) {
		ret = -ENOLINK;
		goto out;
	}
	if (!priv->hardware_state_valid) {
		ret = -EIO;
		goto out;
	}
	if (!priv->session_keys_set || !priv->session_keys_programmed ||
	    !priv->onu_id_set ||
	    priv->rx_state != EN7581_XGSPON_RX_ONU_ID_ASSIGNED) {
		ret = -ENOKEY;
		goto out;
	}
	current_index = readl(priv->mac + EN7581_XGSPON_CUR_KIDX);
	if (current_index & EN7581_XGSPON_CUR_PIK_IDX) {
		airoha_xgs_copy_default_ploam_key(default_ploam);
		active_key = default_ploam;
	} else {
		active_key = priv->session_keys.ploam;
	}
	ret = airoha_xgs_authenticate_ranging_time_words(
		words, active_key, priv->onu_id, &ranging);
	if (ret)
		goto out;
	activation_state = FIELD_GET(
		EN7581_XGSPON_ACTIVATION_STATE,
		readl(priv->mac + EN7581_XGSPON_ACTIVATION_ST));
	if (activation_state == EN7581_XGSPON_ACTIVATION_O4) {
		if (!ranging.absolute || !ranging.directed) {
			ret = -EPROTO;
			goto out;
		}
		if (priv->tx_state != EN7581_XGSPON_TX_WAIT_REGISTRATION &&
		    priv->tx_state != EN7581_XGSPON_TX_REGISTRATION_QUEUED &&
		    priv->tx_state != EN7581_XGSPON_TX_REGISTERED) {
			ret = -EAGAIN;
			goto out;
		}
		acknowledge = true;
	} else if (activation_state == EN7581_XGSPON_ACTIVATION_O5) {
		if (!ranging.absolute &&
		    (!priv->ranging_time_authenticated ||
		     !priv->equalization_delay_programmed)) {
			ret = -ENODATA;
			goto out;
		}
		acknowledge = ranging.directed && ranging.equalization_delay;
	} else {
		ret = -EAGAIN;
		goto out;
	}
	previous_eqd = priv->equalization_delay;
	ret = en7581_xgspon_program_equalization_delay(
		priv, ranging.equalization_delay, ranging.absolute,
		ranging.negative, &resolved);
	if (ret)
		goto out;
	if (activation_state == EN7581_XGSPON_ACTIVATION_O4) {
		ret = en7581_xgspon_set_activation_state(
			priv, EN7581_XGSPON_ACTIVATION_O5);
		if (ret)
			goto rollback_ranging;
	}

	priv->last_ranging_time = ranging;
	priv->ranging_time_authenticated = true;
	priv->equalization_delay = resolved;
	priv->equalization_delay_programmed = true;
	en7581_xgspon_authorize_o5_locked(priv);
	if (acknowledge) {
		ack_ret = en7581_xgspon_schedule_ack_locked(
			priv, EN7581_XGSPON_RANGING_TIME_MESSAGE_ID,
			ranging.sequence, AIROHA_XGS_ACK_COMPLETION_OK);
		if (ack_ret) {
			if (activation_state == EN7581_XGSPON_ACTIVATION_O4) {
				ret = ack_ret;
				goto rollback_ranging;
			}
			dev_warn_ratelimited(priv->dev,
				"could not queue Ranging Time ACK: %d\n", ack_ret);
		}
	}
	if (activation_state == EN7581_XGSPON_ACTIVATION_O4)
		en7581_xgspon_cancel_to1_locked(priv);
	goto out;

rollback_ranging:
	priv->ranging_time_authenticated = false;
	priv->equalization_delay_programmed = false;
	priv->equalization_delay = previous_eqd;
	memset(&priv->last_ranging_time, 0, sizeof(priv->last_ranging_time));
	rollback_ret = en7581_xgspon_program_equalization_delay(
		priv, previous_eqd, true, false, &resolved);
	state_ret = en7581_xgspon_set_activation_state(
		priv, EN7581_XGSPON_ACTIVATION_O4);
	if (rollback_ret || state_ret) {
		priv->hardware_state_valid = false;
		en7581_xgspon_activation_fault_locked(priv);
	}
out:
	mutex_unlock(&priv->lock);
	memzero_explicit(default_ploam, sizeof(default_ploam));
	memzero_explicit(&ranging, sizeof(ranging));
	return ret;
}

static int en7581_xgspon_accept_assign_alloc_id(
	struct en7581_xgspon *priv,
	const u32 words[AIROHA_XGS_PLOAM_FIFO_WORDS])
{
	struct airoha_xgs_alloc_id_assignment assignment;
	u8 default_ploam[AIROHA_XGS_KEY_SIZE];
	const u8 *active_key;
	u8 tcont_index = EN7581_XGSPON_NO_TCONT;
	u32 current_index;
	int ack_ret;
	int ret;

	memset(&assignment, 0, sizeof(assignment));
	memset(default_ploam, 0, sizeof(default_ploam));
	mutex_lock(&priv->lock);
	if (!en7581_xgspon_phy_trusted_locked(priv)) {
		ret = -ENOLINK;
		goto out;
	}
	if (!priv->hardware_state_valid) {
		ret = -EIO;
		goto out;
	}
	if (!priv->session_keys_set || !priv->session_keys_programmed ||
	    !priv->onu_id_set || !priv->ranging_time_authenticated ||
	    !priv->equalization_delay_programmed ||
	    priv->rx_state != EN7581_XGSPON_RX_ONU_ID_ASSIGNED) {
		ret = -ENOKEY;
		goto out;
	}
	if (!en7581_xgspon_ack_tx_ready_locked(priv)) {
		ret = -EAGAIN;
		goto out;
	}
	current_index = readl(priv->mac + EN7581_XGSPON_CUR_KIDX);
	if (current_index & EN7581_XGSPON_CUR_PIK_IDX) {
		airoha_xgs_copy_default_ploam_key(default_ploam);
		active_key = default_ploam;
	} else {
		active_key = priv->session_keys.ploam;
	}
	ret = airoha_xgs_authenticate_assign_alloc_id_words(
		words, active_key, priv->onu_id, &assignment);
	if (ret)
		goto out;
	ret = en7581_xgspon_commit_alloc_id(priv, &assignment, &tcont_index);
	if (ret) {
		ack_ret = en7581_xgspon_schedule_ack_locked(
			priv, EN7581_XGSPON_ASSIGN_ALLOC_ID_MESSAGE_ID,
			assignment.sequence,
			AIROHA_XGS_ACK_COMPLETION_PROCESS_ERROR);
		if (ack_ret && ack_ret != -EAGAIN)
			dev_warn_ratelimited(priv->dev,
				"could not queue Assign Alloc-ID error ACK: %d\n",
				ack_ret);
		goto out;
	}

	priv->last_alloc_id_assignment = assignment;
	priv->assign_alloc_id_authenticated = true;
	priv->assign_alloc_id_committed = true;
	priv->last_tcont_index = tcont_index;
	ack_ret = en7581_xgspon_schedule_ack_locked(
		priv, EN7581_XGSPON_ASSIGN_ALLOC_ID_MESSAGE_ID,
		assignment.sequence, AIROHA_XGS_ACK_COMPLETION_OK);
	if (ack_ret && ack_ret != -EAGAIN)
		dev_warn_ratelimited(priv->dev,
			"could not queue Assign Alloc-ID ACK: %d\n", ack_ret);
out:
	mutex_unlock(&priv->lock);
	memzero_explicit(default_ploam, sizeof(default_ploam));
	memzero_explicit(&assignment, sizeof(assignment));
	return ret;
}

static int en7581_xgspon_accept_key_control(
	struct en7581_xgspon *priv,
	const u32 words[AIROHA_XGS_PLOAM_FIFO_WORDS])
{
	u8 fragment[AIROHA_XGS_KEY_REPORT_FRAGMENT_SIZE] = {};
	u8 sequence, control, key_index, slot;
	u32 interrupt_status, upstream_control;
	int ret, rollback_ret;

	mutex_lock(&priv->lock);
	if (!en7581_xgspon_ack_tx_ready_locked(priv) ||
	    !priv->tx_authorized ||
	    FIELD_GET(EN7581_XGSPON_ACTIVATION_STATE,
		readl(priv->mac + EN7581_XGSPON_ACTIVATION_ST)) !=
		EN7581_XGSPON_ACTIVATION_O5) {
		ret = -ENOLINK;
		goto out;
	}
	ret = airoha_xgs_parse_key_control_words(
		words, priv->session_keys.ploam, priv->onu_id,
		&sequence, &control, &key_index);
	if (ret)
		goto out;
	slot = key_index - 1;

	if (control == AIROHA_XGS_KEY_REPORT_GENERATE) {
		if (priv->data_key_pending) {
			if (priv->data_key_confirm_pending) {
				ret = -EBUSY;
				goto out;
			}
			if (priv->data_key_index != key_index) {
				ret = -EBUSY;
				goto out;
			}
			ret = airoha_xgs_aes_ecb_encrypt(
				priv->session_keys.kek, priv->data_keys[slot],
				fragment);
			if (!ret)
				ret = en7581_xgspon_schedule_key_report_locked(
					priv, sequence, control, key_index, fragment);
			goto out;
		}
		if (priv->data_key_confirmed &&
		    priv->data_key_index == key_index) {
			ret = -EPROTO;
			goto out;
		}
		ret = en7581_xgspon_snapshot_data_keys_locked(priv);
		if (ret)
			goto out;
		priv->data_key_exchange_generation++;
		get_random_bytes(priv->data_keys[slot], AIROHA_XGS_KEY_SIZE);
		ret = en7581_xgspon_program_data_key_locked(
			priv, slot, priv->data_keys[slot]);
		if (!ret)
			ret = airoha_xgs_aes_ecb_encrypt(
				priv->session_keys.kek, priv->data_keys[slot],
				fragment);
		if (!ret)
			ret = en7581_xgspon_schedule_key_report_locked(
				priv, sequence, control, key_index, fragment);
		if (!ret)
			ret = en7581_xgspon_set_ds_data_key_valid_locked(
				priv, slot, true);
		if (ret)
			goto rollback;
		priv->data_key_pending = true;
		priv->data_key_confirm_pending = false;
		priv->data_key_confirmed = false;
		priv->data_key_index = key_index;
		priv->data_key_sequence = sequence;
		priv->key_control_generate_events++;
		mod_delayed_work(system_wq, &priv->key_tk4_work,
			msecs_to_jiffies(EN7581_XGSPON_KEY_TK4_MS));
		mod_delayed_work(system_wq, &priv->key_tk5_work,
			msecs_to_jiffies(EN7581_XGSPON_KEY_TK5_MS));
		goto out;
	}

	if (priv->data_key_confirmed && priv->data_key_index == key_index) {
		ret = airoha_xgs_data_key_proof(
			priv->session_keys.kek, priv->data_keys[slot], fragment);
		if (!ret)
			ret = en7581_xgspon_schedule_key_report_locked(
				priv, sequence, control, key_index, fragment);
		goto out;
	}
	if (!priv->data_key_pending || priv->data_key_index != key_index) {
		ret = -EPROTO;
		goto out;
	}
	if (priv->data_key_confirm_pending) {
		ret = airoha_xgs_data_key_proof(
			priv->session_keys.kek, priv->data_keys[slot], fragment);
		if (!ret)
			ret = en7581_xgspon_schedule_key_report_locked(
				priv, sequence, control, key_index, fragment);
		goto out;
	}
	cancel_delayed_work(&priv->key_tk5_work);
	/* Clear a stale completion before requesting the active-slot switch. */
	writel(EN7581_XGSPON_INT_US_KEY_SWITCH_DONE,
	       priv->mac + EN7581_XGSPON_INT_STATUS);
	upstream_control = readl(priv->mac + EN7581_XGSPON_US_AES_KEY_CTRL);
	upstream_control &= ~EN7581_XGSPON_US_AES_KEY_INDEX;
	if (slot)
		upstream_control |= EN7581_XGSPON_US_AES_KEY_INDEX;
	writel(upstream_control, priv->mac + EN7581_XGSPON_US_AES_KEY_CTRL);
	writel(upstream_control | EN7581_XGSPON_US_AES_KEY_VALID,
	       priv->mac + EN7581_XGSPON_US_AES_KEY_CTRL);
	ret = readl_poll_timeout(priv->mac + EN7581_XGSPON_INT_STATUS,
		interrupt_status,
		interrupt_status & EN7581_XGSPON_INT_US_KEY_SWITCH_DONE,
		1, EN7581_XGSPON_KEY_SWITCH_TIMEOUT_US);
	if (ret)
		goto rollback;
	writel(EN7581_XGSPON_INT_US_KEY_SWITCH_DONE,
	       priv->mac + EN7581_XGSPON_INT_STATUS);
	if ((readl(priv->mac + EN7581_XGSPON_US_AES_KEY_CTRL) &
	     (EN7581_XGSPON_US_AES_KEY_INDEX |
	      EN7581_XGSPON_US_AES_KEY_VALID)) !=
	    ((slot ? EN7581_XGSPON_US_AES_KEY_INDEX : 0) |
	     EN7581_XGSPON_US_AES_KEY_VALID)) {
		ret = -EIO;
		goto rollback;
	}
	ret = en7581_xgspon_set_ds_data_key_valid_locked(priv, !slot, false);
	if (!ret)
		ret = en7581_xgspon_program_data_key_locked(
			priv, !slot, (const u8[AIROHA_XGS_KEY_SIZE]){});
	if (!ret)
		ret = airoha_xgs_data_key_proof(
			priv->session_keys.kek, priv->data_keys[slot], fragment);
	if (!ret)
		ret = en7581_xgspon_schedule_key_report_locked(
			priv, sequence, control, key_index, fragment);
	if (ret)
		goto rollback;
	priv->data_key_confirm_pending = true;
	priv->data_key_confirmed = false;
	priv->data_key_sequence = sequence;
	priv->key_control_confirm_events++;
	goto out;

rollback:
	rollback_ret = en7581_xgspon_rollback_data_key_exchange_locked(priv);
	if (rollback_ret) {
		priv->hardware_state_valid = false;
		ret = rollback_ret;
	}
out:
	mutex_unlock(&priv->lock);
	memzero_explicit(fragment, sizeof(fragment));
	return ret;
}

static void en7581_xgspon_key_tk4_work(struct work_struct *work)
{
	struct en7581_xgspon *priv = container_of(
		to_delayed_work(work), struct en7581_xgspon, key_tk4_work);
	bool disable_tx = false;

	mutex_lock(&priv->lock);
	if (priv->data_key_pending) {
		disable_tx = priv->key_report_in_flight &&
			priv->active_key_report.exchange_generation ==
				priv->data_key_exchange_generation;
		priv->key_exchange_timeout_events++;
		if (en7581_xgspon_rollback_data_key_exchange_locked(priv))
			priv->hardware_state_valid = false;
		if (disable_tx)
			en7581_xgspon_activation_fault_locked(priv);
	}
	mutex_unlock(&priv->lock);
	if (disable_tx)
		airoha_en7572_emergency_disable(priv->bosa);
}

static void en7581_xgspon_key_tk5_work(struct work_struct *work)
{
	struct en7581_xgspon *priv = container_of(
		to_delayed_work(work), struct en7581_xgspon, key_tk5_work);
	u8 fragment[AIROHA_XGS_KEY_REPORT_FRAGMENT_SIZE] = {};
	u8 slot;
	bool reschedule = false;

	mutex_lock(&priv->lock);
	if (!priv->data_key_pending || priv->data_key_index < 1 ||
	    priv->data_key_index > 2)
		goto out;
	slot = priv->data_key_index - 1;
	if (!airoha_xgs_aes_ecb_encrypt(priv->session_keys.kek,
		priv->data_keys[slot], fragment))
		en7581_xgspon_schedule_key_report_locked(
			priv, priv->data_key_sequence,
			AIROHA_XGS_KEY_REPORT_GENERATE,
			priv->data_key_index, fragment);
	reschedule = priv->data_key_pending &&
		     !priv->data_key_confirm_pending;
out:
	mutex_unlock(&priv->lock);
	if (reschedule && !READ_ONCE(priv->removing))
		mod_delayed_work(system_wq, &priv->key_tk5_work,
				 msecs_to_jiffies(EN7581_XGSPON_KEY_TK5_MS));
	memzero_explicit(fragment, sizeof(fragment));
}

/* The caller must hold priv->lock and establish the temporary activation
 * permission before one complete record may enter the hardware FIFO. */
static int en7581_xgspon_enqueue_upstream_ploam_copies_locked(
	struct en7581_xgspon *priv,
	const u8 frame[AIROHA_XGS_PLOAM_FRAME_SIZE], u8 key_index,
	unsigned int copies)
{
	u32 words[AIROHA_XGS_PLOAM_UPSTREAM_FIFO_WORDS];
	u32 fifo_status;
	unsigned int copy;
	int i, ret;

	memset(words, 0, sizeof(words));
	if (!copies || copies > U8_MAX / AIROHA_XGS_PLOAM_UPSTREAM_FIFO_WORDS) {
		ret = -EINVAL;
		goto out;
	}
	ret = airoha_xgs_encode_upstream_ploam_fifo(frame, key_index, words);
	if (ret)
		goto out;

	if (!priv->hardware_selected || !priv->mac) {
		ret = -ENODEV;
		goto out;
	}
	if (!priv->hardware_state_valid) {
		ret = -EIO;
		goto out;
	}
	if (!priv->tx_armed) {
		ret = -EACCES;
		goto out;
	}
	fifo_status = readl(priv->mac + EN7581_XGSPON_PLOAMU_FIFO_STS);
	if (fifo_status & EN7581_XGSPON_PLOAMU_OVERRUN) {
		ret = -EOVERFLOW;
		goto out;
	}
	if (FIELD_GET(EN7581_XGSPON_PLOAMU_AVAILABLE, fifo_status) <
	    copies * AIROHA_XGS_PLOAM_UPSTREAM_FIFO_WORDS) {
		ret = -EAGAIN;
		goto out;
	}

	/* Capacity for every complete record is established before the first
	 * word, so the SDK's duplicate initial Registration is all-or-nothing. */
	for (copy = 0; copy < copies; copy++)
		for (i = 0; i < AIROHA_XGS_PLOAM_UPSTREAM_FIFO_WORDS; i++)
			writel(words[i], priv->mac + EN7581_XGSPON_PLOAMU_WDATA);
	fifo_status = readl(priv->mac + EN7581_XGSPON_PLOAMU_FIFO_STS);
	ret = fifo_status & EN7581_XGSPON_PLOAMU_OVERRUN ? -EOVERFLOW : 0;
out:
	memzero_explicit(words, sizeof(words));
	return ret;
}

static int en7581_xgspon_enqueue_upstream_ploam_locked(
	struct en7581_xgspon *priv,
	const u8 frame[AIROHA_XGS_PLOAM_FRAME_SIZE], u8 key_index)
{
	return en7581_xgspon_enqueue_upstream_ploam_copies_locked(
		priv, frame, key_index, 1);
}

static void en7581_xgspon_activation_fault_locked(
	struct en7581_xgspon *priv)
{
	int ret;

	if (priv->data_key_pending &&
	    en7581_xgspon_rollback_data_key_exchange_locked(priv))
		priv->hardware_state_valid = false;
	priv->tx_armed = false;
	priv->tx_authorized = false;
	priv->registration_key_switch_verified = false;
	priv->optical_tx_enabled = false;
	priv->ack_head = 0;
	priv->ack_count = 0;
	priv->ack_in_flight = false;
	memset(priv->ack_queue, 0, sizeof(priv->ack_queue));
	memset(&priv->active_ack, 0, sizeof(priv->active_ack));
	priv->key_report_head = 0;
	priv->key_report_count = 0;
	priv->key_report_in_flight = false;
	memzero_explicit(priv->key_report_queue,
			 sizeof(priv->key_report_queue));
	memzero_explicit(&priv->active_key_report,
			 sizeof(priv->active_key_report));
	priv->tx_state = EN7581_XGSPON_TX_FAULT;
	if (priv->mac) {
		if (en7581_xgspon_set_mac_irq_enable(
				priv, EN7581_XGSPON_INT_ENABLED))
			priv->hardware_state_valid = false;
		ret = en7581_xgspon_set_ploam_reply_mode(priv, false);
		if (en7581_xgspon_set_activation_state(
				priv, EN7581_XGSPON_ACTIVATION_O1) || ret)
			priv->hardware_state_valid = false;
	}
}

static int en7581_xgspon_queue_serial_locked(struct en7581_xgspon *priv)
{
	u8 frame[AIROHA_XGS_PLOAM_FRAME_SIZE];
	u32 random_delay_register;
	u32 random_delay;
	int ret;

	if (priv->tx_state != EN7581_XGSPON_TX_SERIAL_READY ||
	    !en7581_xgspon_activation_ready_locked(priv))
		return -EAGAIN;
	random_delay_register = readl(priv->mac + EN7581_XGSPON_RDM_DLY);
	random_delay = en7581_xgspon_serial_random_delay(random_delay_register);
	{
		u32 maximum = FIELD_GET(EN7581_XGSPON_MAX_RANDOM_DELAY,
					 random_delay_register);

		maximum |= EN7581_XGSPON_RANDOM_DELAY_ENABLE_FIELD;
		random_delay_register &= ~EN7581_XGSPON_MAX_RANDOM_DELAY;
		random_delay_register |= FIELD_PREP(EN7581_XGSPON_MAX_RANDOM_DELAY,
						     maximum);
	}
	writel(random_delay_register, priv->mac + EN7581_XGSPON_RDM_DLY);
	ret = airoha_xgs_build_serial_number_ploam(
		priv->serial, random_delay, priv->active_mode, frame);
	if (!ret)
		ret = en7581_xgspon_enqueue_upstream_ploam_locked(priv, frame,
							 AIROHA_XGS_PLOAM_KEY_INDEX_1);
	if (!ret)
		priv->tx_state = EN7581_XGSPON_TX_SERIAL_QUEUED;
	memzero_explicit(frame, sizeof(frame));
	return ret;
}

static int en7581_xgspon_queue_registration_locked(
	struct en7581_xgspon *priv)
{
	u8 frame[AIROHA_XGS_PLOAM_FRAME_SIZE];
	u32 activation_state;
	bool initial;
	u8 sequence;
	int ret;

	initial = priv->tx_state == EN7581_XGSPON_TX_WAIT_REGISTRATION &&
		  !priv->request_registration_authenticated;
	if ((!initial &&
	     priv->tx_state != EN7581_XGSPON_TX_REGISTERED) ||
	    priv->ack_in_flight || priv->key_report_in_flight ||
	    !priv->registration_response_pending || !priv->onu_id_set ||
	    !en7581_xgspon_activation_ready_locked(priv))
		return -EAGAIN;
	activation_state = FIELD_GET(
		EN7581_XGSPON_ACTIVATION_STATE,
		readl(priv->mac + EN7581_XGSPON_ACTIVATION_ST));
	if (initial) {
		if (activation_state != EN7581_XGSPON_ACTIVATION_O4)
			return -EAGAIN;
		sequence = 0;
	} else {
		if (!priv->request_registration_authenticated ||
		    !priv->ranging_time_authenticated ||
		    !priv->equalization_delay_programmed ||
		    activation_state != EN7581_XGSPON_ACTIVATION_O5)
			return -EAGAIN;
		sequence = priv->request_registration_sequence;
	}
	ret = airoha_xgs_build_registration_ploam(
		priv->session_keys.ploam, priv->onu_id,
		sequence, priv->registration_id, frame);
	if (!ret) {
		if (initial)
			ret = en7581_xgspon_enqueue_upstream_ploam_copies_locked(
				priv, frame, AIROHA_XGS_PLOAM_KEY_INDEX_0, 2);
		else
			ret = en7581_xgspon_enqueue_upstream_ploam_locked(
				priv, frame, AIROHA_XGS_PLOAM_KEY_INDEX_0);
	}
	if (!ret) {
		priv->registration_response_pending = false;
		priv->tx_state = EN7581_XGSPON_TX_REGISTRATION_QUEUED;
	}
	memzero_explicit(frame, sizeof(frame));
	return ret;
}

static int en7581_xgspon_queue_ack_locked(struct en7581_xgspon *priv)
{
	struct en7581_xgspon_ack *ack;
	u8 frame[AIROHA_XGS_PLOAM_FRAME_SIZE];
	u8 default_ploam[AIROHA_XGS_KEY_SIZE];
	const u8 *key;
	int ret;

	memset(default_ploam, 0, sizeof(default_ploam));
	if (priv->ack_in_flight || !priv->ack_count)
		goto idle;

	ack = &priv->ack_queue[priv->ack_head];
	if (ack->source_message_id == EN7581_XGSPON_PROFILE_MESSAGE_ID)
		ack->key_index =
			readl(priv->mac + EN7581_XGSPON_CUR_KIDX) &
			EN7581_XGSPON_CUR_PIK_IDX ?
			AIROHA_XGS_PLOAM_KEY_INDEX_1 :
			AIROHA_XGS_PLOAM_KEY_INDEX_0;
	if (!en7581_xgspon_ack_key_ready_locked(
		priv, ack->source_message_id, ack->key_index)) {
		ret = -EAGAIN;
		goto out;
	}
	if (ack->key_index == AIROHA_XGS_PLOAM_KEY_INDEX_0) {
		key = priv->session_keys.ploam;
	} else if (ack->key_index == AIROHA_XGS_PLOAM_KEY_INDEX_1 &&
		   (ack->source_message_id ==
			EN7581_XGSPON_RANGING_TIME_MESSAGE_ID ||
		    ack->source_message_id ==
			EN7581_XGSPON_PROFILE_MESSAGE_ID)) {
		airoha_xgs_copy_default_ploam_key(default_ploam);
		key = default_ploam;
	} else {
		ret = -EPROTO;
		goto out;
	}
	if (ack->generation != priv->session_generation) {
		memset(ack, 0, sizeof(*ack));
		priv->ack_head = (priv->ack_head + 1) %
			EN7581_XGSPON_ACK_QUEUE_DEPTH;
		priv->ack_count--;
		ret = priv->ack_count ? -EAGAIN : 0;
		goto out;
	}
	ret = airoha_xgs_build_acknowledge_ploam(
		key, priv->onu_id, ack->sequence,
		ack->completion_code, frame);
	if (!ret)
		ret = en7581_xgspon_enqueue_upstream_ploam_locked(
			priv, frame, ack->key_index);
	if (!ret) {
		priv->active_ack = *ack;
		priv->ack_in_flight = true;
		memset(ack, 0, sizeof(*ack));
		priv->ack_head = (priv->ack_head + 1) %
			EN7581_XGSPON_ACK_QUEUE_DEPTH;
		priv->ack_count--;
	}
	out:
	memzero_explicit(frame, sizeof(frame));
	memzero_explicit(default_ploam, sizeof(default_ploam));
	return ret;

idle:
	ret = 0;
	goto out;
}

static int en7581_xgspon_queue_key_report_locked(
	struct en7581_xgspon *priv)
{
	struct en7581_xgspon_key_report *report;
	u8 frame[AIROHA_XGS_PLOAM_FRAME_SIZE];
	int ret;

	if (priv->key_report_in_flight || !priv->key_report_count)
		return 0;
	if (priv->ack_in_flight)
		return -EAGAIN;
	if (!en7581_xgspon_ack_tx_ready_locked(priv))
		return -EAGAIN;
	report = &priv->key_report_queue[priv->key_report_head];
	if (report->generation != priv->session_generation) {
		memzero_explicit(report, sizeof(*report));
		priv->key_report_head = (priv->key_report_head + 1) %
			EN7581_XGSPON_KEY_REPORT_QUEUE_DEPTH;
		priv->key_report_count--;
		return priv->key_report_count ? -EAGAIN : 0;
	}
	ret = airoha_xgs_build_key_report(
		priv->session_keys.ploam, priv->onu_id, report->sequence,
		report->report_type, report->key_index, report->fragment, frame);
	if (!ret)
		ret = en7581_xgspon_enqueue_upstream_ploam_locked(
			priv, frame, AIROHA_XGS_PLOAM_KEY_INDEX_0);
	if (!ret) {
		priv->active_key_report = *report;
		priv->key_report_in_flight = true;
		memzero_explicit(report, sizeof(*report));
		priv->key_report_head = (priv->key_report_head + 1) %
			EN7581_XGSPON_KEY_REPORT_QUEUE_DEPTH;
		priv->key_report_count--;
	}
	memzero_explicit(frame, sizeof(frame));
	return ret;
}

static void en7581_xgspon_activation_work(struct work_struct *work)
{
	struct en7581_xgspon *priv = container_of(
		to_delayed_work(work), struct en7581_xgspon, activation_work);
	bool disable_tx = false;
	bool retry = false;
	int ret = 0;

	mutex_lock(&priv->lock);
	if (priv->removing || !priv->enabled || !priv->hardware_selected) {
		priv->tx_armed = false;
		priv->tx_authorized = false;
		priv->optical_tx_enabled = false;
		mutex_unlock(&priv->lock);
		en7581_xgspon_disable_tx(priv);
		return;
	}
	/* O7 is an OLT-controlled quiet state. Do not burn CPU retrying the
	 * activation worker; an authenticated Allow message schedules recovery. */
	if (priv->serial_disabled) {
		priv->tx_armed = false;
		priv->tx_authorized = false;
		priv->optical_tx_enabled = false;
		mutex_unlock(&priv->lock);
		en7581_xgspon_disable_tx(priv);
		return;
	}
	if (!en7581_xgspon_activation_ready_locked(priv)) {
		retry = true;
		mutex_unlock(&priv->lock);
		goto out;
	}
	if (!priv->tx_armed) {
		/* Select software PLOAM replies and arm only the temporary
		 * Serial Number/Registration permission. Optical TX remains dark
		 * until a complete PLOAM record is in the FIFO. */
		ret = en7581_xgspon_set_ploam_reply_mode(priv, true);
		if (ret) {
			en7581_xgspon_activation_fault_locked(priv);
			disable_tx = true;
			mutex_unlock(&priv->lock);
			goto out;
		}
		ret = en7581_xgspon_set_activation_state(priv,
						 EN7581_XGSPON_ACTIVATION_O2_3);
		if (ret) {
			en7581_xgspon_activation_fault_locked(priv);
			disable_tx = true;
			mutex_unlock(&priv->lock);
			goto out;
		}
		priv->tx_armed = true;
		ret = en7581_xgspon_enable_upstream_irqs_locked(priv);
		if (ret) {
			en7581_xgspon_activation_fault_locked(priv);
			disable_tx = true;
			mutex_unlock(&priv->lock);
			goto out;
		}
	}
	if (priv->tx_state == EN7581_XGSPON_TX_SERIAL_READY)
		ret = en7581_xgspon_queue_serial_locked(priv);
	else if (priv->tx_state == EN7581_XGSPON_TX_WAIT_REGISTRATION) {
		if (priv->registration_response_pending &&
		    !priv->request_registration_authenticated)
			ret = en7581_xgspon_queue_registration_locked(priv);
		else {
			ret = en7581_xgspon_queue_ack_locked(priv);
			if (!ret && !priv->ack_in_flight &&
			    priv->request_registration_authenticated)
				ret = en7581_xgspon_queue_registration_locked(priv);
		}
	} else if (priv->tx_state == EN7581_XGSPON_TX_REGISTERED) {
		if (priv->registration_response_pending)
			ret = en7581_xgspon_queue_registration_locked(priv);
		else {
			ret = en7581_xgspon_queue_key_report_locked(priv);
			if (!ret && !priv->key_report_in_flight)
				ret = en7581_xgspon_queue_ack_locked(priv);
		}
	}
	if (ret == -EAGAIN) {
		retry = true;
	} else if (ret) {
		en7581_xgspon_activation_fault_locked(priv);
		disable_tx = true;
	} else if (priv->tx_state == EN7581_XGSPON_TX_SERIAL_QUEUED ||
		   priv->tx_state == EN7581_XGSPON_TX_REGISTRATION_QUEUED ||
		   priv->tx_state == EN7581_XGSPON_TX_REGISTERED) {
		/* The FIFO transaction is complete, so it is now safe to enable the
		 * laser. Final OMCC/data authorization still waits for Registration. */
		ret = airoha_en7572_set_tx_enabled(priv->bosa, true);
		if (ret) {
			en7581_xgspon_activation_fault_locked(priv);
			disable_tx = true;
		} else {
			priv->optical_tx_enabled = true;
		}
	}
	mutex_unlock(&priv->lock);

out:
	if (disable_tx)
		airoha_en7572_emergency_disable(priv->bosa);
	if (retry && !READ_ONCE(priv->removing))
		mod_delayed_work(system_wq, &priv->activation_work,
				 msecs_to_jiffies(EN7581_XGSPON_ACTIVATION_RETRY_MS));
}

static void en7581_xgspon_record_upstream_events(
	struct en7581_xgspon *priv, u32 status)
{
	u32 fifo_errors = 0;
	u32 tx_errors = 0;
	u32 current_index;
	enum en7581_xgspon_tx_state next;
	bool key_switch_verified;
	bool ack_was_in_flight, key_report_was_in_flight;
	bool emergency_disable = false;
	int transition_ret;

	if (status & EN7581_XGSPON_INT_FIFO_ERROR) {
		fifo_errors = readl(priv->mac + EN7581_XGSPON_FIFO_ERR_STS);
		writel(fifo_errors, priv->mac + EN7581_XGSPON_FIFO_ERR_STS);
	}
	if (status & EN7581_XGSPON_INT_TX_ERROR) {
		tx_errors = readl(priv->mac + EN7581_XGSPON_TX_ERR_STS) &
			    EN7581_XGSPON_TX_ERR_ALL;
		writel(tx_errors, priv->mac + EN7581_XGSPON_TX_ERR_STS);
	}
	current_index = readl(priv->mac + EN7581_XGSPON_CUR_KIDX);

	mutex_lock(&priv->lock);
	ack_was_in_flight = priv->ack_in_flight;
	key_report_was_in_flight = priv->key_report_in_flight;
	priv->last_upstream_irq_status =
		status & EN7581_XGSPON_INT_UPSTREAM_EVENTS;
	if (status & EN7581_XGSPON_INT_PLOAMU)
		priv->ploamu_send_events++;
	if (status & EN7581_XGSPON_INT_O23_SN_REQUEST)
		priv->serial_number_request_events++;
	if (status & EN7581_XGSPON_INT_O23_SN_SENT)
		priv->serial_number_send_events++;
	if (status & EN7581_XGSPON_INT_O4_RANGING_REQUEST)
		priv->ranging_request_events++;
	if (status & EN7581_XGSPON_INT_O4_REGISTRATION_SENT)
		priv->registration_send_events++;
	if (status & EN7581_XGSPON_INT_US_NO_MESSAGE)
		priv->no_message_events++;
	if (status & EN7581_XGSPON_INT_FIFO_ERROR) {
		priv->fifo_error_events++;
		priv->last_fifo_error_status = fifo_errors;
		if (fifo_errors & EN7581_XGSPON_FIFO_ERR_TX_PLOAMU_OVERRUN)
				priv->upstream_fifo_overrun_events++;
	}
	if (status & EN7581_XGSPON_INT_TX_ERROR) {
		priv->tx_error_events++;
		priv->last_tx_error_status = tx_errors;
	}
	if (!priv->tx_armed &&
	    (status & EN7581_XGSPON_INT_UPSTREAM_EVENTS))
		priv->unexpected_upstream_events++;
	transition_ret = en7581_xgspon_tx_transition(
		priv->tx_state, priv->tx_armed, status, fifo_errors,
		 tx_errors, current_index, &next, &key_switch_verified);
	priv->tx_state = next;
	if (!transition_ret &&
	    (status & EN7581_XGSPON_INT_O23_SN_SENT))
		priv->pm_serial_number_messages++;
	if (!transition_ret &&
	    (status & EN7581_XGSPON_INT_O4_REGISTRATION_SENT))
		priv->pm_registration_messages++;
	if (!transition_ret && !(status & EN7581_XGSPON_INT_US_NO_MESSAGE) &&
	    !fifo_errors && !tx_errors &&
	    (status & EN7581_XGSPON_INT_PLOAMU)) {
		if (status & (EN7581_XGSPON_INT_O23_SN_SENT |
			      EN7581_XGSPON_INT_O4_REGISTRATION_SENT)) {
			/* The type-specific completion owns this PLOAMu event. */
		} else if (priv->key_report_in_flight &&
		    priv->tx_state == EN7581_XGSPON_TX_REGISTERED) {
			if (priv->data_key_pending &&
			    priv->data_key_confirm_pending &&
			    priv->active_key_report.exchange_generation ==
				priv->data_key_exchange_generation &&
			    priv->active_key_report.report_type ==
				AIROHA_XGS_KEY_REPORT_CONFIRM &&
			    priv->active_key_report.key_index ==
				priv->data_key_index) {
				u8 old_slot = !(priv->data_key_index - 1);

				cancel_delayed_work(&priv->key_tk4_work);
				memzero_explicit(&priv->data_key_snapshot,
						 sizeof(priv->data_key_snapshot));
				priv->data_key_snapshot_valid = false;
				priv->data_key_pending = false;
				priv->data_key_confirm_pending = false;
				priv->data_key_confirmed = true;
				en7581_xgspon_drop_queued_key_reports_locked(
					priv, priv->data_key_exchange_generation);
				memzero_explicit(priv->data_keys[old_slot],
						 AIROHA_XGS_KEY_SIZE);
			}
			priv->key_report_in_flight = false;
			priv->key_report_send_events++;
			priv->pm_key_report_messages++;
			memzero_explicit(&priv->active_key_report,
					 sizeof(priv->active_key_report));
		} else if (priv->ack_in_flight &&
			   (priv->tx_state == EN7581_XGSPON_TX_WAIT_REGISTRATION ||
			    priv->tx_state == EN7581_XGSPON_TX_REGISTERED)) {
			priv->ack_in_flight = false;
			priv->acknowledge_send_events++;
			priv->pm_acknowledge_messages++;
			memset(&priv->active_ack, 0, sizeof(priv->active_ack));
		} else {
			priv->unexpected_upstream_events++;
		}
	}
	if (!transition_ret && ack_was_in_flight &&
	    (status & EN7581_XGSPON_INT_US_NO_MESSAGE))
		transition_ret = -EPROTO;
	if (!transition_ret && key_report_was_in_flight &&
	    (status & EN7581_XGSPON_INT_US_NO_MESSAGE))
		transition_ret = -EPROTO;
	if (key_switch_verified)
		priv->registration_key_switch_verified = true;
	if (!transition_ret)
		en7581_xgspon_authorize_o5_locked(priv);
	if (transition_ret) {
		en7581_xgspon_activation_fault_locked(priv);
		emergency_disable = true;
	}
	mutex_unlock(&priv->lock);

	if (emergency_disable)
		airoha_en7572_emergency_disable(priv->bosa);
	if (fifo_errors & EN7581_XGSPON_FIFO_ERR_TX_PLOAMU_OVERRUN)
		dev_err_ratelimited(priv->dev,
					    "upstream PLOAM FIFO overrun\n");
	if (tx_errors)
		dev_err_ratelimited(priv->dev,
					    "XGS-PON TX error status %#x; optical TX locked off\n",
					    tx_errors);
}

static void en7581_xgspon_record_downstream_errors(
	struct en7581_xgspon *priv, u32 status)
{
	u32 bwmap_errors = 0;
	u32 rx_errors = 0;

	if (status & EN7581_XGSPON_INT_RX_ERROR) {
		rx_errors = readl(priv->mac + EN7581_XGSPON_RX_ERR_STS) &
			    EN7581_XGSPON_RX_ERR_ALL;
		if (rx_errors)
			writel(rx_errors, priv->mac + EN7581_XGSPON_RX_ERR_STS);
	}
	if (status & EN7581_XGSPON_INT_BWMAP_CHECK_ERROR) {
		bwmap_errors = readl(priv->mac + EN7581_XGSPON_BWMAP_CHECK_STS) &
			       EN7581_XGSPON_BWMAP_CHECK_ERR_ALL;
		if (bwmap_errors)
			writel(bwmap_errors,
			       priv->mac + EN7581_XGSPON_BWMAP_CHECK_STS);
	}

	mutex_lock(&priv->lock);
	if (status & EN7581_XGSPON_INT_RX_ERROR) {
		priv->rx_error_events++;
		priv->last_rx_error_status = rx_errors;
		if (rx_errors & EN7581_XGSPON_RX_ERR_LOSS_OF_GEM_DELINEATION)
			priv->loss_of_gem_delineation_events++;
	}
	if (status & EN7581_XGSPON_INT_BWMAP_CHECK_ERROR) {
		priv->bwmap_check_error_events++;
		priv->last_bwmap_check_error_status = bwmap_errors;
	}
	mutex_unlock(&priv->lock);

	if (rx_errors)
		dev_warn_ratelimited(priv->dev,
				     "XG/XGS-PON RX error status %#x\n",
				     rx_errors);
	if (bwmap_errors)
		dev_warn_ratelimited(priv->dev,
				     "XG/XGS-PON BWmap check status %#x\n",
				     bwmap_errors);
}

static void en7581_xgspon_drain_ploam(struct en7581_xgspon *priv)
{
	struct airoha_xgs_reboot_request reboot_request;
	u32 words[AIROHA_XGS_PLOAM_FIFO_WORDS];
	u32 fifo_status;
	u64 reboot_generation;
	unsigned int messages = 0;
	u8 message_id;
	int i, ret;
	bool trusted_phy;

	while (messages++ < EN7581_XGSPON_MAX_IRQ_PLOAMS) {
		fifo_status = readl(priv->mac + EN7581_XGSPON_PLOAMD_FIFO_STS);
		if (fifo_status & EN7581_XGSPON_PLOAMD_OVERRUN)
			dev_warn_ratelimited(priv->dev,
					     "downstream PLOAM FIFO overrun\n");
		if (FIELD_GET(EN7581_XGSPON_PLOAMD_USED, fifo_status) <
		    AIROHA_XGS_PLOAM_FIFO_WORDS)
			break;

		for (i = 0; i < AIROHA_XGS_PLOAM_FIFO_WORDS; i++)
			words[i] = readl(priv->mac + EN7581_XGSPON_PLOAMD_RDATA);

		/* PHY loss may race a multi-message FIFO drain. Revalidate each
		 * record immediately before any authenticated state transition.
		 */
		mutex_lock(&priv->lock);
		trusted_phy = en7581_xgspon_phy_trusted_locked(priv);
		mutex_unlock(&priv->lock);
		if (!trusted_phy)
			continue;

		/* Byte 2 is the message ID in the FIFO's network-order word. */
		message_id = (words[0] >> 8) & 0xff;
		mutex_lock(&priv->lock);
		if (priv->serial_disabled &&
		    message_id != EN7581_XGSPON_DISABLE_SERIAL_NUMBER_MESSAGE_ID) {
			priv->unsupported_ploam_events++;
			mutex_unlock(&priv->lock);
			continue;
		}
		mutex_unlock(&priv->lock);
		/* Match the SDK's per-type parser-dispatch boundary. A record is
		 * counted once it reaches its recognized handler, independently of
		 * whether that handler subsequently rejects its state or MIC. */
		mutex_lock(&priv->lock);
		switch (message_id) {
		case EN7581_XGSPON_PROFILE_MESSAGE_ID:
			priv->profile_messages_received++;
			break;
		case EN7581_XGSPON_ASSIGN_ONU_ID_MESSAGE_ID:
			priv->assign_onu_id_messages_received++;
			break;
		case EN7581_XGSPON_RANGING_TIME_MESSAGE_ID:
			priv->ranging_time_messages_received++;
			break;
		case EN7581_XGSPON_DEACTIVATE_ONU_ID_MESSAGE_ID:
			priv->deactivate_onu_id_messages_received++;
			break;
		case EN7581_XGSPON_DISABLE_SERIAL_NUMBER_MESSAGE_ID:
			priv->disable_serial_number_messages_received++;
			break;
		case EN7581_XGSPON_REQUEST_REGISTRATION_MESSAGE_ID:
			priv->request_registration_messages_received++;
			break;
		case EN7581_XGSPON_ASSIGN_ALLOC_ID_MESSAGE_ID:
			priv->assign_alloc_id_messages_received++;
			break;
		case AIROHA_XGS_KEY_CONTROL_MESSAGE_ID:
			priv->key_control_messages_received++;
			break;
		case EN7581_XGSPON_SLEEP_ALLOW_MESSAGE_ID:
			priv->sleep_allow_messages_received++;
			break;
		case AIROHA_XGS_REBOOT_ONU_MESSAGE_ID:
			priv->reboot_onu_messages_received++;
			break;
		}
		mutex_unlock(&priv->lock);
		ret = -EOPNOTSUPP;
		switch (message_id) {
		case EN7581_XGSPON_PROFILE_MESSAGE_ID:
				ret = en7581_xgspon_accept_profile(priv, words);
				if (!ret) {
					dev_info(priv->dev,
						 "authenticated Profile; burst profile transaction committed\n");
				}
				else
					dev_warn_ratelimited(
						priv->dev,
						"rejected Profile: %d\n", ret);
				break;
			case EN7581_XGSPON_ASSIGN_ONU_ID_MESSAGE_ID:
				ret = en7581_xgspon_accept_assign_onu_id(priv, words);
				if (!ret) {
					dev_info(priv->dev,
						 "authenticated Assign ONU-ID; entered O4 and awaiting Ranging Time\n");
				}
				else if (ret != -EALREADY)
					dev_warn_ratelimited(
						priv->dev,
						"rejected Assign ONU-ID: %d\n", ret);
				break;
			case EN7581_XGSPON_RANGING_TIME_MESSAGE_ID:
				ret = en7581_xgspon_accept_ranging_time(priv, words);
					if (!ret) {
						dev_info(priv->dev,
							 "authenticated Ranging Time; EqD committed and O5 ACK scheduled when required\n");
					}
				else
					dev_warn_ratelimited(
						priv->dev,
						"rejected Ranging Time: %d\n", ret);
				break;
			case EN7581_XGSPON_DEACTIVATE_ONU_ID_MESSAGE_ID:
				ret = en7581_xgspon_accept_deactivate_onu_id(priv,
								       words);
				if (!ret) {
					dev_info(priv->dev,
						 "authenticated Deactivate ONU-ID; session and service state revoked\n");
				}
				else
					dev_warn_ratelimited(
						priv->dev,
						"rejected Deactivate ONU-ID: %d\n", ret);
				break;
			case EN7581_XGSPON_DISABLE_SERIAL_NUMBER_MESSAGE_ID:
				ret = en7581_xgspon_accept_disable_serial_number(priv,
									 words);
				if (!ret) {
				} else
					dev_warn_ratelimited(priv->dev,
						"rejected Disable Serial Number: %d\n", ret);
				break;
			case EN7581_XGSPON_REQUEST_REGISTRATION_MESSAGE_ID:
				ret = en7581_xgspon_accept_request_registration(priv,
									 words);
				if (!ret) {
					dev_info(priv->dev,
						 "authenticated O5 Request Registration; upstream response scheduled\n");
				}
				else
					dev_warn_ratelimited(
						priv->dev,
						"rejected Request Registration: %d\n", ret);
				break;
			case EN7581_XGSPON_ASSIGN_ALLOC_ID_MESSAGE_ID:
				ret = en7581_xgspon_accept_assign_alloc_id(priv,
									 words);
					if (!ret) {
						dev_info(priv->dev,
							 "authenticated Assign Alloc-ID; T-CONT transaction committed and O5 ACK scheduled\n");
					}
				else
					dev_warn_ratelimited(
						priv->dev,
						"rejected Assign Alloc-ID: %d\n", ret);
				break;
			case AIROHA_XGS_KEY_CONTROL_MESSAGE_ID:
				ret = en7581_xgspon_accept_key_control(priv, words);
				if (!ret) {
					dev_info(priv->dev,
						 "authenticated Key Control; data-key transaction advanced\n");
				}
				else
					dev_warn_ratelimited(
						priv->dev,
						"rejected Key Control: %d\n", ret);
				break;
			case EN7581_XGSPON_SLEEP_ALLOW_MESSAGE_ID:
				ret = en7581_xgspon_accept_sleep_allow(priv, words);
				if (!ret) {
				} else
					dev_warn_ratelimited(priv->dev,
						"rejected Sleep Allow: %d\n", ret);
				break;
			case AIROHA_XGS_REBOOT_ONU_MESSAGE_ID:
				ret = en7581_xgspon_accept_reboot_onu(
					priv, words, &reboot_request,
					&reboot_generation);
				if (!ret) {
					en7581_xgspon_emit_reboot_request(
						priv, &reboot_request,
						reboot_generation);
					dev_info(priv->dev,
						 "authenticated Reboot ONU request reported to userspace\n");
				} else {
					dev_warn_ratelimited(priv->dev,
						"rejected Reboot ONU: %d\n", ret);
				}
				memzero_explicit(&reboot_request,
						 sizeof(reboot_request));
				break;
			case EN7581_XGSPON_CALIBRATION_REQUEST_MESSAGE_ID:
			case EN7581_XGSPON_ADJUST_TX_WAVELENGTH_MESSAGE_ID:
			case EN7581_XGSPON_CHANGE_POWER_LEVEL_MESSAGE_ID:
			case EN7581_XGSPON_POWER_CONSUMPTION_INQUIRE_MESSAGE_ID:
			case EN7581_XGSPON_RATE_CONTROL_MESSAGE_ID:
				mutex_lock(&priv->lock);
				priv->recognized_noop_ploam_events++;
				mutex_unlock(&priv->lock);
				/* These SDK handlers intentionally only record the message. */
				ret = 0;
				dev_dbg_ratelimited(priv->dev,
					"recognized no-op downstream control message %#x\n",
					message_id);
				break;
			case EN7581_XGSPON_TUNING_CONTROL_MESSAGE_ID:
			case EN7581_XGSPON_SYSTEM_PROFILE_MESSAGE_ID:
			case EN7581_XGSPON_CHANNEL_PROFILE_MESSAGE_ID:
			case EN7581_XGSPON_PROTECTION_CONTROL_MESSAGE_ID:
				mutex_lock(&priv->lock);
				priv->ngpon2_control_ignored_events++;
				mutex_unlock(&priv->lock);
				/* The SDK also returns success without action outside NG-PON2. */
				ret = 0;
				dev_dbg_ratelimited(priv->dev,
					"ignored NG-PON2-only downstream control message %#x\n",
					message_id);
				break;
			default:
				mutex_lock(&priv->lock);
				priv->unsupported_ploam_events++;
				mutex_unlock(&priv->lock);
				dev_warn_ratelimited(priv->dev,
					"ignored unsupported downstream PLOAM message %#x\n",
					message_id);
				break;
		}
		if (!ret) {
			mutex_lock(&priv->lock);
			/* Profile establishes a session; Deactivate/Disable end one.
			 * Attribute those accepted transition messages to the resulting
			 * generation so the atomic snapshot never publishes a stale
			 * generation solely to retain the event. */
			en7581_xgspon_pm_record_accepted_locked(priv, message_id);
			mutex_unlock(&priv->lock);
		}
	}

	memzero_explicit(words, sizeof(words));
}

static irqreturn_t en7581_xgspon_irq(int irq, void *data)
{
	struct en7581_xgspon *priv = data;
	u32 enabled, status;

	if (READ_ONCE(priv->xpon_irqs_masked) ||
	    !en7581_xgspon_backend_active(priv))
		return IRQ_NONE;
	enabled = readl(priv->mac + EN7581_XGSPON_INT_ENABLE);
	status = readl(priv->mac + EN7581_XGSPON_INT_STATUS);
	if (!(status & enabled & EN7581_XGSPON_INT_SUPPORTED))
		return IRQ_NONE;

	return IRQ_WAKE_THREAD;
}

static irqreturn_t en7581_xgspon_irq_thread(int irq, void *data)
{
	struct en7581_xgspon *priv = data;
	u32 enabled, status;

	if (READ_ONCE(priv->xpon_irqs_masked) ||
	    !en7581_xgspon_backend_active(priv))
		return IRQ_HANDLED;
	enabled = readl(priv->mac + EN7581_XGSPON_INT_ENABLE);
	status = readl(priv->mac + EN7581_XGSPON_INT_STATUS) & enabled;
	if (!status)
		return IRQ_HANDLED;

	/* INT_STATUS is write-one-to-clear, matching the recovered SDK ISR. */
	writel(status, priv->mac + EN7581_XGSPON_INT_STATUS);
	if (status & EN7581_XGSPON_INT_DOWNSTREAM_ERRORS)
		en7581_xgspon_record_downstream_errors(priv, status);
	if (status & EN7581_XGSPON_INT_UPSTREAM_EVENTS)
		en7581_xgspon_record_upstream_events(priv, status);
	if (status & (EN7581_XGSPON_INT_PLOAMU |
		      EN7581_XGSPON_INT_O23_SN_REQUEST |
		      EN7581_XGSPON_INT_O23_SN_SENT |
		      EN7581_XGSPON_INT_O4_RANGING_REQUEST |
		      EN7581_XGSPON_INT_O4_REGISTRATION_SENT))
		mod_delayed_work(system_wq, &priv->activation_work, 0);
	if (status & EN7581_XGSPON_INT_PLOAMD)
		en7581_xgspon_drain_ploam(priv);

	return IRQ_HANDLED;
}

static irqreturn_t en7581_xgspon_phy_irq(int irq, void *data)
{
	struct en7581_xgspon *priv = data;
	u32 enabled, status;

	if (READ_ONCE(priv->xpon_irqs_masked) ||
	    !en7581_xgspon_backend_active(priv))
		return IRQ_NONE;
	enabled = readl(priv->phy_csr + EN7581_XGSPON_PHY_INT_ENABLE);
	status = readl(priv->phy_csr + EN7581_XGSPON_PHY_INT_STATUS);
	if (!(status & enabled & EN7581_XGSPON_PHY_RX_EVENTS))
		return IRQ_NONE;

	return IRQ_WAKE_THREAD;
}

static irqreturn_t en7581_xgspon_phy_irq_thread(int irq, void *data)
{
	struct en7581_xgspon *priv = data;
	u32 enabled, status;
	bool live_ready;
	bool quiesce = false;
	bool schedule_recovery = false;
	bool recovery_queued = false;
	bool enter_lods = false;
	unsigned long recovery_delay = msecs_to_jiffies(
		EN7581_XGSPON_PHY_RECOVERY_DELAY_MS);
	int ret = 0;

	if (READ_ONCE(priv->xpon_irqs_masked) ||
	    !en7581_xgspon_backend_active(priv))
		return IRQ_HANDLED;
	enabled = readl(priv->phy_csr + EN7581_XGSPON_PHY_INT_ENABLE);
	status = readl(priv->phy_csr + EN7581_XGSPON_PHY_INT_STATUS) &
		 enabled & EN7581_XGSPON_PHY_RX_EVENTS;
	if (!status)
		return IRQ_HANDLED;

	/* The recovered SDK reads INT_STA and writes back the same W1C value. */
	writel(status, priv->phy_csr + EN7581_XGSPON_PHY_INT_STATUS);

	mutex_lock(&priv->lock);
	priv->last_phy_irq_status = status;
	priv->phy_irq_events++;
	if (status & EN7581_XGSPON_PHY_INT_PHYA_READY)
		priv->phy_phya_ready_events++;
	if (status & EN7581_XGSPON_PHY_INT_RX_READY)
		priv->phy_rx_ready_events++;
	if (status & EN7581_XGSPON_PHY_INT_RX_LOF)
		priv->phy_lof_events++;
	if (status & EN7581_XGSPON_PHY_INT_RX_SYNC_OK)
		priv->phy_sync_events++;
	if (status & EN7581_XGSPON_PHY_INT_RX_LOS)
		priv->phy_los_events++;

	live_ready = en7581_xgspon_phy_live_ready(priv, NULL, NULL, NULL);
	if ((status & EN7581_XGSPON_PHY_INT_RX_LOS) ||
	    (readl(priv->phy_csr + EN7581_XGSPON_PHY_SFP_STATUS) &
	     EN7581_XGSPON_PHY_SFP_RX_LOS)) {
		priv->phy_los = true;
		priv->phy_lof = false;
		priv->phy_ready = false;
		priv->phy_recovering = false;
		priv->phy_recovery_attempts = 0;
		priv->phy_fault = false;
		enter_lods = FIELD_GET(EN7581_XGSPON_ACTIVATION_STATE,
			readl(priv->mac + EN7581_XGSPON_ACTIVATION_ST)) ==
			EN7581_XGSPON_ACTIVATION_O5;
		mod_delayed_work(system_wq, &priv->activation_work, 0);
		en7581_xgspon_phy_set_rx(priv, false);
		if (!enter_lods || en7581_xgspon_lods_begin_locked(priv))
			en7581_xgspon_phy_invalidate_session_locked(priv);
		else {
			schedule_recovery = true;
			recovery_delay = msecs_to_jiffies(
				EN7581_XGSPON_LODS_TIMEOUT_MS);
		}
		quiesce = true;
	} else if (status & EN7581_XGSPON_PHY_INT_RX_READY) {
		priv->phy_los = false;
		priv->phy_lof = false;
		schedule_recovery = true;
	} else if (status & EN7581_XGSPON_PHY_INT_RX_LOF) {
		priv->phy_los = false;
		priv->phy_lof = !live_ready;
		priv->phy_ready = live_ready;
		if (!live_ready) {
			enter_lods = FIELD_GET(EN7581_XGSPON_ACTIVATION_STATE,
				readl(priv->mac + EN7581_XGSPON_ACTIVATION_ST)) ==
				EN7581_XGSPON_ACTIVATION_O5;
			if (!enter_lods || en7581_xgspon_lods_begin_locked(priv))
				en7581_xgspon_phy_invalidate_session_locked(priv);
			schedule_recovery = true;
		}
	} else if (status & (EN7581_XGSPON_PHY_INT_RX_SYNC_OK |
			     EN7581_XGSPON_PHY_INT_PHYA_READY)) {
		priv->phy_los = false;
		priv->phy_lof = !live_ready;
		priv->phy_ready = live_ready && !priv->phy_recovering;
		if (!live_ready || priv->phy_recovering) {
			priv->phy_fake_sync_events++;
			if (!priv->lods_session_preserved)
				en7581_xgspon_phy_invalidate_session_locked(priv);
			if (!priv->phy_recovering)
				schedule_recovery = true;
		} else {
			priv->phy_recovering = false;
			priv->phy_fault = false;
			priv->phy_recovery_attempts = 0;
		}
	}
	mutex_unlock(&priv->lock);
	/* airoha_pcs_xpon_recover() is transactional: every recovery path,
	 * including LOF and fake-sync handling, must quiesce the PMA first.
	 * Queue the worker only after quiesce completes so its 2 ms delay cannot
	 * race this transition. */
	if (quiesce || schedule_recovery) {
		ret = airoha_en7572_set_tx_enabled(priv->bosa, false);
		if (!ret)
			ret = airoha_pcs_xpon_quiesce(priv->pcs);
		if (ret) {
			mutex_lock(&priv->lock);
			if (priv->lods_session_preserved)
				en7581_xgspon_phy_invalidate_session_locked(priv);
			else if (schedule_recovery)
				en7581_xgspon_phy_invalidate_session_locked(priv);
			priv->phy_fault = true;
			priv->phy_recovering = false;
			priv->phy_recovery_failures++;
			mutex_unlock(&priv->lock);
			dev_err_ratelimited(priv->dev,
				"failed to quiesce XG/XGS-PON PMA for recovery: %d\n",
				ret);
		}
	}
	if (schedule_recovery && !ret) {
		mutex_lock(&priv->lock);
		if (!priv->removing && !priv->phy_fault) {
			en7581_xgspon_phy_schedule_recovery(priv, recovery_delay);
			recovery_queued = true;
		}
		mutex_unlock(&priv->lock);
	}
	if (schedule_recovery && !ret && !recovery_queued)
		dev_dbg(priv->dev, "PHY recovery suppressed during teardown\n");

	return IRQ_HANDLED;
}

static int en7581_xgspon_parse_serial(const char *value, size_t count,
				      u8 *serial)
{
	size_t len = count;
	int i;

	while (len && (value[len - 1] == '\n' || value[len - 1] == '\r'))
		len--;
	if (len != 12)
		return -EINVAL;
	for (i = 0; i < 4; i++) {
		if (!isalnum(value[i]))
			return -EINVAL;
		serial[i] = value[i];
	}
	if (hex2bin(serial + 4, value + 4, 4))
		return -EINVAL;

	return 0;
}

static int en7581_xgspon_parse_registration_id(const char *value,
					       size_t count,
					       u8 *registration_id)
{
	size_t len = count;
	int i;

	while (len && (value[len - 1] == '\n' || value[len - 1] == '\r'))
		len--;
	memset(registration_id, 0, AIROHA_XGS_REGISTRATION_ID_SIZE);
	if (len == 4 + 2 * AIROHA_XGS_REGISTRATION_ID_SIZE &&
	    !memcmp(value, "hex:", 4))
		return hex2bin(registration_id, value + 4,
			       AIROHA_XGS_REGISTRATION_ID_SIZE) ? -EINVAL : 0;
	if (!len || len > AIROHA_XGS_REGISTRATION_ID_SIZE)
		return -EINVAL;
	for (i = 0; i < len; i++) {
		if (!isprint(value[i]))
			return -EINVAL;
		registration_id[i] = value[i];
	}

	return 0;
}

static ssize_t en7581_xgspon_omcc_read(struct file *file, char __user *buffer,
				      size_t count, loff_t *offset)
{
	struct en7581_xgspon *priv = file->private_data;
	struct en7581_xgspon_omcc_ready *ready;
	unsigned long flags;
	ssize_t ret;

	if (!count)
		return 0;
	if (mutex_lock_interruptible(&priv->omcc_read_lock))
		return -ERESTARTSYS;

	for (;;) {
		if (file->f_flags & O_NONBLOCK) {
			if (READ_ONCE(priv->removing)) {
				ret = -ENODEV;
				break;
			}
			if (!READ_ONCE(priv->omcc_ready_queue_depth)) {
				ret = -EAGAIN;
				break;
			}
		} else {
			ret = wait_event_interruptible(
				priv->omcc_rx_wait,
				READ_ONCE(priv->removing) ||
				READ_ONCE(priv->omcc_ready_queue_depth));
			if (ret)
				break;
		}

		mutex_lock(&priv->lock);
		if (priv->removing) {
			ret = -ENODEV;
			mutex_unlock(&priv->lock);
			break;
		}

		spin_lock_irqsave(&priv->omcc_rx_lock, flags);
		if (list_empty(&priv->omcc_ready_queue)) {
			spin_unlock_irqrestore(&priv->omcc_rx_lock, flags);
			mutex_unlock(&priv->lock);
			continue;
		}
		ready = list_first_entry(&priv->omcc_ready_queue,
					 struct en7581_xgspon_omcc_ready, list);
		if (ready->generation != priv->session_generation) {
			list_del(&ready->list);
			priv->omcc_ready_queue_depth--;
			spin_unlock_irqrestore(&priv->omcc_rx_lock, flags);
			mutex_unlock(&priv->lock);
			atomic64_inc(&priv->omcc_rx_dropped_session_reset);
			memzero_explicit(ready->record, ready->size);
			kfree(ready);
			continue;
		}
		if (count < ready->size) {
			ret = -EMSGSIZE;
			spin_unlock_irqrestore(&priv->omcc_rx_lock, flags);
			mutex_unlock(&priv->lock);
			break;
		}
		list_del(&ready->list);
		priv->omcc_ready_queue_depth--;
		spin_unlock_irqrestore(&priv->omcc_rx_lock, flags);

		if (copy_to_user(buffer, ready->record, ready->size)) {
			spin_lock_irqsave(&priv->omcc_rx_lock, flags);
			list_add(&ready->list, &priv->omcc_ready_queue);
			priv->omcc_ready_queue_depth++;
			spin_unlock_irqrestore(&priv->omcc_rx_lock, flags);
			atomic64_inc(&priv->omcc_rx_copy_faults);
			ret = -EFAULT;
			mutex_unlock(&priv->lock);
			break;
		}

		ret = ready->size;
		atomic64_inc(&priv->omcc_rx_delivered);
		mutex_unlock(&priv->lock);
		memzero_explicit(ready->record, ready->size);
		kfree(ready);
		break;
	}

	mutex_unlock(&priv->omcc_read_lock);
	return ret;
}

static ssize_t en7581_xgspon_omcc_write(struct file *file,
				       const char __user *buffer,
				       size_t count, loff_t *offset)
{
	struct en7581_xgspon *priv = file->private_data;
	struct airoha_xgs_omcc_record header;
	struct airoha_xgs_omcc_tx_metadata metadata;
	u8 *content = NULL;
	u8 *wire = NULL;
	u8 omci_key[AIROHA_XGS_KEY_SIZE];
	size_t content_len, wire_len = 0;
	unsigned long current_index;
	ssize_t ret;

	if (READ_ONCE(priv->removing))
		return -ENODEV;
	if (!buffer || count < sizeof(header) ||
	    count > sizeof(header) + AIROHA_XGS_OMCC_MAX_CONTENTS)
		return -EINVAL;
	if (copy_from_user(&header, buffer, sizeof(header)))
		return -EFAULT;
	if (be32_to_cpu(header.magic) != AIROHA_XGS_OMCC_MAGIC ||
	    header.abi_version != AIROHA_XGS_OMCC_ABI_VERSION ||
	    header.direction != AIROHA_XGS_OMCC_DIRECTION_TX ||
	    be16_to_cpu(header.flags) || header.reserved ||
	    header.instance_generation ||
	    header.session_generation ||
	    be16_to_cpu(header.length) < 4 ||
	    be16_to_cpu(header.length) > AIROHA_XGS_OMCC_MAX_CONTENTS ||
	    count != sizeof(header) + be16_to_cpu(header.length))
		return -EINVAL;

	content_len = be16_to_cpu(header.length);
	content = kmalloc(content_len, GFP_KERNEL);
	wire = kmalloc(AIROHA_XGS_OMCI_MAX_WIRE_SIZE, GFP_KERNEL);
	if (!content || !wire) {
		ret = -ENOMEM;
		goto out;
	}
	if (copy_from_user(content, buffer + sizeof(header), content_len)) {
		ret = -EFAULT;
		goto out;
	}

	memset(omci_key, 0, sizeof(omci_key));
	mutex_lock(&priv->lock);
	if (priv->removing) {
		ret = -ENODEV;
		goto unlock;
	}
	if (!priv->hardware_selected || !priv->hardware_state_valid ||
	    !priv->session_keys_set || !priv->session_keys_programmed ||
	    !priv->onu_id_set || !priv->omcc_xgem_programmed) {
		ret = -ENOKEY;
		goto unlock;
	}
	if (!en7581_xgspon_omcc_tx_ready_locked(priv)) {
		ret = -EACCES;
		goto unlock;
	}

	memcpy(omci_key, priv->session_keys.omci, sizeof(omci_key));
	current_index = readl(priv->mac + EN7581_XGSPON_CUR_KIDX);
	metadata.xgem_id = priv->onu_id;
	metadata.channel = 0;
	metadata.nboq = 0;
	metadata.mic_idx = !!(current_index & EN7581_XGSPON_CUR_OIK_IDX);
	ret = airoha_xgs_sign_upstream_omci(omci_key, content, content_len,
					    wire,
					    AIROHA_XGS_OMCC_MAX_CONTENTS,
					    &wire_len);
	if (!ret)
		ret = airoha_xgs_omcc_transmit(priv->ethernet_np, wire, wire_len,
						&metadata);
unlock:
	mutex_unlock(&priv->lock);
	memzero_explicit(omci_key, sizeof(omci_key));
	out:
	if (wire) {
		memzero_explicit(wire, AIROHA_XGS_OMCI_MAX_WIRE_SIZE);
		kfree(wire);
	}
	if (content) {
		memzero_explicit(content, content_len);
		kfree(content);
	}
	return ret ? ret : count;
}

static __poll_t en7581_xgspon_omcc_poll(struct file *file, poll_table *wait)
{
	struct en7581_xgspon *priv = file->private_data;
	__poll_t events = 0;
	unsigned long flags;
	bool tx_ready, tx_available = false;

	poll_wait(file, &priv->omcc_rx_wait, wait);
	spin_lock_irqsave(&priv->omcc_rx_lock, flags);
	if (priv->removing)
		events |= EPOLLERR | EPOLLHUP;
	else if (!list_empty(&priv->omcc_ready_queue))
		events |= EPOLLIN | EPOLLRDNORM;
	spin_unlock_irqrestore(&priv->omcc_rx_lock, flags);
	mutex_lock(&priv->lock);
	tx_ready = !priv->removing && en7581_xgspon_omcc_tx_ready_locked(priv);
	mutex_unlock(&priv->lock);
	if (tx_ready)
		tx_available = airoha_xgs_omcc_tx_available(priv->ethernet_np);
	if (tx_ready && tx_available)
		events |= EPOLLOUT | EPOLLWRNORM;

	return events;
}

static void en7581_xgspon_release_ref(struct kref *ref)
{
	struct en7581_xgspon *priv = container_of(
		ref, struct en7581_xgspon, refcount);

	en7581_xgspon_free_omcc_ready_queue(&priv->omcc_ready_queue);
	kfree(priv);
}

static void en7581_xgspon_put(void *data)
{
	struct en7581_xgspon *priv = data;

	kref_put(&priv->refcount, en7581_xgspon_release_ref);
}

static int en7581_xgspon_omcc_open(struct inode *inode, struct file *file)
{
	struct miscdevice *misc = file->private_data;
	struct en7581_xgspon *priv = container_of(
		misc, struct en7581_xgspon, omcc);
	int ret = 0;

	mutex_lock(&priv->lock);
	if (priv->removing)
		ret = -ENODEV;
	else
		kref_get(&priv->refcount);
	mutex_unlock(&priv->lock);
	if (ret)
		return ret;

	file->private_data = priv;
	ret = nonseekable_open(inode, file);
	if (ret)
		en7581_xgspon_put(priv);
	return ret;
}

static int en7581_xgspon_omcc_release(struct inode *inode, struct file *file)
{
	en7581_xgspon_put(file->private_data);
	return 0;
}

static long en7581_xgspon_omcc_ioctl(struct file *file, unsigned int command,
				    unsigned long argument)
{
	struct en7581_xgspon *priv = file->private_data;
	__u32 info = AIROHA_XGS_OMCC_ABI_VERSION;

	if (READ_ONCE(priv->removing))
		return -ENODEV;
	if (command != AIROHA_XGS_OMCC_GET_INFO)
		return -ENOTTY;
	mutex_lock(&priv->lock);
	if (priv->omcc_consumer_registered &&
	    en7581_xgspon_omcc_tx_ready_locked(priv) &&
	    airoha_xgs_omcc_tx_available(priv->ethernet_np))
		info |= AIROHA_XGS_OMCC_CAP_DS_MIC_VERIFIED |
			AIROHA_XGS_OMCC_CAP_US_MIC_SIGNED;
	mutex_unlock(&priv->lock);
	if (copy_to_user((void __user *)argument, &info, sizeof(info)))
		return -EFAULT;

	return 0;
}

static const struct file_operations en7581_xgspon_omcc_fops = {
	.owner = THIS_MODULE,
	.open = en7581_xgspon_omcc_open,
	.release = en7581_xgspon_omcc_release,
	.read = en7581_xgspon_omcc_read,
	.write = en7581_xgspon_omcc_write,
	.poll = en7581_xgspon_omcc_poll,
	.unlocked_ioctl = en7581_xgspon_omcc_ioctl,
	.compat_ioctl = compat_ptr_ioctl,
};

static ssize_t control_abi_version_show(struct device *dev,
					struct device_attribute *attribute,
					char *buf)
{
	return sysfs_emit(buf, "%u\n", AIROHA_XGS_OMCC_ABI_VERSION);
}
static DEVICE_ATTR_RO(control_abi_version);

static ssize_t pon_mode_show(struct device *dev,
			     struct device_attribute *attribute, char *buf)
{
	struct en7581_xgspon *priv = dev_get_drvdata(dev);
	const struct airoha_xpon_mode_descriptor *descriptor;

	mutex_lock(&priv->lock);
	descriptor = airoha_xpon_mode_descriptor(priv->active_mode);
	if (!en7581_xgspon_backend_active(priv))
		descriptor = NULL;
	mutex_unlock(&priv->lock);

	return sysfs_emit(buf, "%s\n", descriptor ? descriptor->name : "invalid");
}
static DEVICE_ATTR_RO(pon_mode);

static ssize_t service_reset_store(struct device *dev,
				   struct device_attribute *attribute,
				   const char *buf, size_t count)
{
	struct en7581_xgspon *priv = dev_get_drvdata(dev);

	if (!sysfs_streq(buf, "reset") && !sysfs_streq(buf, "1"))
		return -EINVAL;
	mutex_lock(&priv->lock);
	memset(priv->pending_tconts, 0, sizeof(priv->pending_tconts));
	memset(priv->pending_xgems, 0, sizeof(priv->pending_xgems));
	priv->pending_tcont_count = 0;
	priv->pending_xgem_count = 0;
	mutex_unlock(&priv->lock);

	return count;
}
static DEVICE_ATTR_WO(service_reset);

static ssize_t service_tcont_store(struct device *dev,
				   struct device_attribute *attribute,
				   const char *buf, size_t count)
{
	struct en7581_xgspon *priv = dev_get_drvdata(dev);
	struct airoha_xgs_service_tcont t = { .valid = 1 };
	unsigned int index, alloc, scheduler, weight, queue_weights[8];
	unsigned int cir, pir, cbs, pbs, i;
	char extra;
	int fields, ret = 0;

	fields = sscanf(buf,
		"%u %u %u %u %u %u %u %u %u %u %u %u %u %u %u %u %c",
		&index, &alloc, &scheduler, &weight,
		&queue_weights[0], &queue_weights[1], &queue_weights[2],
		&queue_weights[3], &queue_weights[4], &queue_weights[5],
		&queue_weights[6], &queue_weights[7], &cir, &pir, &cbs, &pbs,
		&extra);
	if (fields != 16 || index < AIROHA_XGS_SERVICE_FIRST_TCONT ||
	    index >= EN7581_XGSPON_MAX_TCONTS ||
	    alloc > AIROHA_XGS_SERVICE_MAX_ALLOC_ID || scheduler > 2 ||
	    weight > U8_MAX || (pir && cir > pir))
		return -ERANGE;
	for (i = 0; i < ARRAY_SIZE(queue_weights); i++)
		if (queue_weights[i] > U8_MAX)
			return -ERANGE;
	t.index = index;
	t.alloc_id = alloc;
	t.scheduler = scheduler;
	t.weight = weight;
	for (i = 0; i < ARRAY_SIZE(t.queue_weights); i++)
		t.queue_weights[i] = queue_weights[i];
	t.cir = cir;
	t.pir = pir;
	t.cbs = cbs;
	t.pbs = pbs;

	mutex_lock(&priv->lock);
	if (priv->pending_tcont_count >= AIROHA_XGS_SERVICE_MAX_TCONTS) {
		ret = -ENOSPC;
		goto unlock;
	}
	for (i = 0; i < priv->pending_tcont_count; i++)
		if (priv->pending_tconts[i].index == t.index ||
		    priv->pending_tconts[i].alloc_id == t.alloc_id) {
			ret = -EEXIST;
			goto unlock;
		}
	priv->pending_tconts[priv->pending_tcont_count++] = t;
unlock:
	mutex_unlock(&priv->lock);
	return ret ? ret : count;
}
static DEVICE_ATTR_WO(service_tcont);

static ssize_t service_xgem_store(struct device *dev,
				  struct device_attribute *attribute,
				  const char *buf, size_t count)
{
	struct en7581_xgspon *priv = dev_get_drvdata(dev);
	struct airoha_xgs_service_xgem x = { .valid = 1 };
	unsigned int xgem, tcont, direction, channel, nboq, mic, unicast;
	unsigned int encrypted, i;
	char extra;
	int fields, ret = 0;

	fields = sscanf(buf, "%u %u %u %u %u %u %u %u %c", &xgem, &tcont,
		&direction, &channel, &nboq, &mic, &unicast, &encrypted,
		&extra);
	if (fields != 8 || !xgem || xgem > AIROHA_XGS_SERVICE_MAX_XGEM_ID ||
	    tcont < AIROHA_XGS_SERVICE_FIRST_TCONT ||
	    tcont >= EN7581_XGSPON_MAX_TCONTS ||
	    direction < AIROHA_XGS_SERVICE_DIRECTION_DOWNSTREAM ||
	    direction > AIROHA_XGS_SERVICE_DIRECTION_BIDIRECTIONAL ||
	    channel > AIROHA_XGS_SERVICE_MAX_CHANNEL ||
	    nboq > AIROHA_XGS_SERVICE_MAX_NBOQ || mic > 1 || unicast > 1 ||
	    encrypted > 1)
		return -ERANGE;
	x.xgem_id = xgem;
	x.tcont_index = tcont;
	x.direction = direction;
	x.channel = channel;
	x.nboq = nboq;
	x.mic_idx = mic;
	x.unicast = unicast;
	x.upstream_encrypted = encrypted;

	mutex_lock(&priv->lock);
	if (priv->pending_xgem_count >= AIROHA_XGS_SERVICE_MAX_XGEMS) {
		ret = -ENOSPC;
		goto unlock;
	}
	for (i = 0; i < priv->pending_xgem_count; i++)
		if (priv->pending_xgems[i].xgem_id == x.xgem_id) {
			ret = -EEXIST;
			goto unlock;
		}
	priv->pending_xgems[priv->pending_xgem_count++] = x;
unlock:
	mutex_unlock(&priv->lock);
	return ret ? ret : count;
}
static DEVICE_ATTR_WO(service_xgem);

static ssize_t service_commit_store(struct device *dev,
				    struct device_attribute *attribute,
				    const char *buf, size_t count)
{
	struct en7581_xgspon *priv = dev_get_drvdata(dev);
	struct airoha_xgs_service_config config;
	int ret;

	if (!sysfs_streq(buf, "commit") && !sysfs_streq(buf, "1"))
		return -EINVAL;
	mutex_lock(&priv->lock);
	if ((priv->pending_tcont_count || priv->pending_xgem_count) &&
	    (!en7581_xgspon_activation_ready_locked(priv) ||
	     FIELD_GET(EN7581_XGSPON_ACTIVATION_STATE,
		       readl(priv->mac + EN7581_XGSPON_ACTIVATION_ST)) !=
		     EN7581_XGSPON_ACTIVATION_O5)) {
		ret = -ENOLINK;
		goto unlock;
	}
	config.version = AIROHA_XGS_SERVICE_ABI_VERSION;
	config.tcont_count = priv->pending_tcont_count;
	config.xgem_count = priv->pending_xgem_count;
	config.tconts = priv->pending_tconts;
	config.xgems = priv->pending_xgems;
	ret = en7581_xgspon_apply_service_locked(priv, &config);
unlock:
	mutex_unlock(&priv->lock);
	return ret ? ret : count;
}
static DEVICE_ATTR_WO(service_commit);

static ssize_t service_rollback_store(struct device *dev,
				      struct device_attribute *attribute,
				      const char *buf, size_t count)
{
	struct en7581_xgspon *priv = dev_get_drvdata(dev);
	struct airoha_xgs_service_config config;
	int ret;

	if (!sysfs_streq(buf, "rollback") && !sysfs_streq(buf, "1"))
		return -EINVAL;
	mutex_lock(&priv->lock);
	if (!priv->service_rollback_valid) {
		ret = -ENOENT;
		goto unlock;
	}
	config.version = AIROHA_XGS_SERVICE_ABI_VERSION;
	config.tcont_count = priv->rollback_tcont_count;
	config.xgem_count = priv->rollback_xgem_count;
	config.tconts = priv->rollback_tconts;
	config.xgems = priv->rollback_xgems;
	ret = en7581_xgspon_apply_service_locked(priv, &config);
	if (!ret)
		priv->service_rollback_valid = false;
unlock:
	mutex_unlock(&priv->lock);
	return ret ? ret : count;
}
static DEVICE_ATTR_WO(service_rollback);

static ssize_t service_state_show(struct device *dev,
				  struct device_attribute *attribute, char *buf)
{
	struct en7581_xgspon *priv = dev_get_drvdata(dev);
	ssize_t len;

	mutex_lock(&priv->lock);
	len = sysfs_emit(buf, "version=%u generation=%u tconts=%u xgems=%u\n",
		AIROHA_XGS_SERVICE_ABI_VERSION, priv->service_generation,
		priv->service_tcont_count, priv->service_xgem_count);
	mutex_unlock(&priv->lock);
	return len;
}
static DEVICE_ATTR_RO(service_state);

static ssize_t enabled_show(struct device *dev,
			    struct device_attribute *attribute, char *buf)
{
	struct en7581_xgspon *priv = dev_get_drvdata(dev);
	bool enabled;

	mutex_lock(&priv->lock);
	enabled = priv->enabled;
	mutex_unlock(&priv->lock);

	return sysfs_emit(buf, "%u\n", enabled);
}

static ssize_t enabled_store(struct device *dev,
			     struct device_attribute *attribute,
			     const char *buf, size_t count)
{
	struct en7581_xgspon *priv = dev_get_drvdata(dev);
	bool enabled;
	int irq_ret, ret;

	ret = kstrtobool(buf, &enabled);
	if (ret)
		return ret;

	mutex_lock(&priv->lock);
	if (!priv->hardware_selected) {
		ret = -EOPNOTSUPP;
	} else if (!enabled) {
		priv->enabled = false;
		priv->tx_armed = false;
		priv->tx_authorized = false;
		priv->optical_tx_enabled = false;
		priv->registration_key_switch_verified = false;
		ret = en7581_xgspon_clear_session_keys(priv);
		irq_ret = en7581_xgspon_set_mac_irq_enable(
			priv, EN7581_XGSPON_INT_ENABLED);
		if (!ret)
			ret = irq_ret;
	} else if (!priv->hardware_state_valid) {
		ret = -EIO;
	} else if (!priv->serial_set || !priv->registration_id_set) {
		ret = -ENODATA;
	} else if (!priv->bosa || !airoha_en7572_is_ready(priv->bosa)) {
		ret = -EAGAIN;
	} else {
		priv->enabled = true;
		priv->tx_armed = false;
		priv->tx_authorized = false;
		priv->registration_key_switch_verified = false;
		if (priv->tx_state == EN7581_XGSPON_TX_FAULT)
			priv->tx_state = priv->session_keys_set ?
				EN7581_XGSPON_TX_SERIAL_READY :
				EN7581_XGSPON_TX_BLOCKED;
		mod_delayed_work(system_wq, &priv->activation_work, 0);
		ret = 0;
	}
	mutex_unlock(&priv->lock);
	if (!enabled)
		en7581_xgspon_disable_tx(priv);

	return ret ? ret : count;
}
static DEVICE_ATTR_RW(enabled);

static ssize_t activation_ready_show(struct device *dev,
				     struct device_attribute *attribute,
				     char *buf)
{
	struct en7581_xgspon *priv = dev_get_drvdata(dev);
	bool ready;

	mutex_lock(&priv->lock);
	ready = en7581_xgspon_activation_ready_locked(priv);
	mutex_unlock(&priv->lock);

	return sysfs_emit(buf, "%u\n", ready);
}
static DEVICE_ATTR_RO(activation_ready);

static ssize_t ready_show(struct device *dev,
			  struct device_attribute *attribute, char *buf)
{
	struct en7581_xgspon *priv = dev_get_drvdata(dev);
	bool ready;

	mutex_lock(&priv->lock);
	ready = priv->omcc_consumer_registered &&
		en7581_xgspon_omcc_tx_ready_locked(priv) &&
		airoha_xgs_omcc_tx_available(priv->ethernet_np);
	mutex_unlock(&priv->lock);

	return sysfs_emit(buf, "%u\n", ready);
}
static DEVICE_ATTR_RO(ready);

static ssize_t state_show(struct device *dev,
			  struct device_attribute *attribute, char *buf)
{
	struct en7581_xgspon *priv = dev_get_drvdata(dev);
	const char *state;

	mutex_lock(&priv->lock);
	if (!priv->hardware_selected)
		state = "inactive-board-mode";
	else if (!priv->enabled)
		state = "disabled";
	else if (!priv->hardware_state_valid || priv->phy_fault)
		state = "fault";
	else if (!en7581_xgspon_activation_ready_locked(priv))
		state = "blocked";
	else
		state = en7581_xgspon_tx_state_name(priv->tx_state);
	mutex_unlock(&priv->lock);

	return sysfs_emit(buf, "%s\n", state);
}
static DEVICE_ATTR_RO(state);

static ssize_t activation_blockers_show(struct device *dev,
					struct device_attribute *attribute,
					char *buf)
{
	struct en7581_xgspon *priv = dev_get_drvdata(dev);
	ssize_t len = 0;
	bool first = true;

#define ADD_BLOCKER(_condition, _name) \
	do { \
		if (_condition) \
			len += sysfs_emit_at(buf, len, "%s%s", first ? "" : " ", \
					    (_name)); \
		if (_condition) \
			first = false; \
	} while (0)

	mutex_lock(&priv->lock);
	if (!priv->hardware_selected)
		ADD_BLOCKER(true, "board-mode");
	else {
		ADD_BLOCKER(!priv->hardware_state_valid, "hardware-state");
		ADD_BLOCKER(!priv->bosa || !airoha_en7572_is_ready(priv->bosa),
			    "calibration");
		ADD_BLOCKER(!priv->serial_set, "serial");
		ADD_BLOCKER(!priv->registration_id_set, "registration-id");
		ADD_BLOCKER(!en7581_xgspon_phy_trusted_locked(priv), "phy");
		ADD_BLOCKER(!en7581_xgspon_tx_sync_ready_locked(priv), "tx-sync");
	}
	mutex_unlock(&priv->lock);
	if (first)
		len += sysfs_emit_at(buf, len, "none");
	len += sysfs_emit_at(buf, len, "\n");
#undef ADD_BLOCKER
	return len;
}
static DEVICE_ATTR_RO(activation_blockers);

static ssize_t secure_omcc_info_show(struct device *dev,
				     struct device_attribute *attribute,
				     char *buf)
{
	return sysfs_emit(buf, "%u\n", AIROHA_XGS_OMCC_ABI_VERSION);
}
static DEVICE_ATTR_RO(secure_omcc_info);

static ssize_t omcc_rx_evidence_show(struct device *dev,
				     struct device_attribute *attribute,
				     char *buf)
{
	struct en7581_xgspon *priv = dev_get_drvdata(dev);
	unsigned long flags;
	u32 queue_depth, ready_depth;

	spin_lock_irqsave(&priv->omcc_rx_lock, flags);
	queue_depth = priv->omcc_rx_queue_depth;
	ready_depth = priv->omcc_ready_queue_depth;
	spin_unlock_irqrestore(&priv->omcc_rx_lock, flags);

	return sysfs_emit(buf,
		"version=1 complete=0 consumer_registered=%u hardware_ds_mic=%u hardware_us_mic=0 queue_depth=%u queue_limit=%u ready_depth=%u ready_limit=%u "
		"queued=%lld hw_verified=%lld sw_verified=%lld ready_queued=%lld delivered=%lld dropped_no_session=%lld dropped_wrong_xgem=%lld dropped_queue_full=%lld dropped_removing=%lld dropped_ready_full=%lld dropped_ready_alloc=%lld dropped_session_reset=%lld copy_faults=%lld dropped_invalid=%lld dropped_mic=%lld dropped_stale=%lld\n",
		priv->omcc_consumer_registered, priv->hardware_ds_omci_mic,
		queue_depth, EN7581_XGSPON_OMCC_RX_QUEUE_LIMIT,
		ready_depth, EN7581_XGSPON_OMCC_READY_QUEUE_LIMIT,
		(long long)atomic64_read(&priv->omcc_rx_queued),
		(long long)atomic64_read(&priv->omcc_rx_hw_verified),
		(long long)atomic64_read(&priv->omcc_rx_sw_verified),
		(long long)atomic64_read(&priv->omcc_rx_ready_queued),
		(long long)atomic64_read(&priv->omcc_rx_delivered),
		(long long)atomic64_read(&priv->omcc_rx_dropped_no_session),
		(long long)atomic64_read(&priv->omcc_rx_dropped_wrong_xgem),
		(long long)atomic64_read(&priv->omcc_rx_dropped_queue_full),
		(long long)atomic64_read(&priv->omcc_rx_dropped_removing),
		(long long)atomic64_read(&priv->omcc_rx_dropped_ready_full),
		(long long)atomic64_read(&priv->omcc_rx_dropped_ready_alloc),
		(long long)atomic64_read(&priv->omcc_rx_dropped_session_reset),
		(long long)atomic64_read(&priv->omcc_rx_copy_faults),
		(long long)atomic64_read(&priv->omcc_rx_dropped_invalid),
		(long long)atomic64_read(&priv->omcc_rx_dropped_mic),
		(long long)atomic64_read(&priv->omcc_rx_dropped_stale));
}
static DEVICE_ATTR_RO(omcc_rx_evidence);

static u64 en7581_xgspon_hec_errors(u32 value)
{
	return FIELD_GET(EN7581_XGSPON_HEC_1ERR, value) +
	       FIELD_GET(EN7581_XGSPON_HEC_2ERR, value) +
	       FIELD_GET(EN7581_XGSPON_HEC_3ERR, value);
}

/*
 * This is diagnostic evidence, not a complete G.988 PM backend. The SDK gets
 * the remaining fields from its TC/PLOAM software state and the OMCI process.
 */
static ssize_t xgs_counter_evidence_show(struct device *dev,
					 struct device_attribute *attribute,
					 char *buf)
{
	struct en7581_xgspon *priv = dev_get_drvdata(dev);
	u32 hlend, alloc, header, phy, mic, rx_omci, tx_omci;
	u32 unknown_profiles, transmitted_xgem_frames, fragment_xgem_frames;
	u32 xgem_hec_lost_words, xgem_key_errors;
	u32 transmitted_non_idle_bytes, received_non_idle_bytes;
	u32 downstream_ploam_messages, upstream_ploam_messages;
	u64 psbd_hec_errors;

	mutex_lock(&priv->lock);
	if (!priv->hardware_selected || !priv->mac) {
		mutex_unlock(&priv->lock);
		return -EOPNOTSUPP;
	}
	hlend = readl(priv->mac + EN7581_XGSPON_RX_HLEND_HEC_CNT);
	alloc = readl(priv->mac + EN7581_XGSPON_RX_ALLOC_HEC_CNT);
	header = readl(priv->mac + EN7581_XGSPON_RX_HDR_HEC_CNT);
	phy = readl(priv->mac + EN7581_XGSPON_RX_PHY_HEC_ERR_CNT);
	mic = readl(priv->mac + EN7581_XGSPON_RX_MIC_ERR_CNT);
	rx_omci = readl(priv->mac + EN7581_XGSPON_RX_OMCI_CNT);
	tx_omci = readl(priv->mac + EN7581_XGSPON_TX_OMCI_CNT);
	unknown_profiles = readl(priv->mac +
				 EN7581_XGSPON_INVLD_PROF_BST_GNT_CNT);
	transmitted_xgem_frames = readl(priv->mac + EN7581_XGSPON_TX_XGEM_CNT);
	fragment_xgem_frames = readl(priv->mac + EN7581_XGSPON_TX_NLF_XGEM_CNT);
	xgem_hec_lost_words = readl(priv->mac + EN7581_XGSPON_RX_LOST_WCNT);
	xgem_key_errors = readl(priv->mac + EN7581_XGSPON_RX_KEY_ERR_CNT);
	transmitted_non_idle_bytes = readl(priv->mac +
					  EN7581_XGSPON_TX_NON_IDLE_BCNT);
	received_non_idle_bytes = readl(priv->mac +
				       EN7581_XGSPON_RX_NON_IDLE_BCNT);
	downstream_ploam_messages = readl(priv->mac + EN7581_XGSPON_RX_PLOAMD_CNT);
	upstream_ploam_messages = readl(priv->mac + EN7581_XGSPON_TX_PLOAMU_CNT);
	psbd_hec_errors = en7581_xgspon_hec_errors(hlend) +
			  en7581_xgspon_hec_errors(alloc) +
			  en7581_xgspon_hec_errors(header) +
			  FIELD_GET(EN7581_XGSPON_PHY_SFC_HEC_ERR, phy) +
			  FIELD_GET(EN7581_XGSPON_PHY_PON_ID_HEC_ERR, phy);

	mutex_unlock(&priv->lock);

	/* Hardware counters wrap and are cleared only through the separate CNT_CLR. */
	return sysfs_emit(buf,
		"version=1 complete=0 semantics=raw-hardware-modulo "
		"tc_valid=%u downstream_valid=%u upstream_valid=%u "
		"psbd_hec_errors=%llu xgtc_hec_errors=%llu "
		"unknown_profiles=%u transmitted_xgem_frames=%u "
		"fragment_xgem_frames=%u xgem_hec_lost_words=%u "
		"xgem_key_errors=%u xgem_hec_errors=%llu "
		"transmitted_non_idle_bytes=%u received_non_idle_bytes=%u "
		"ploam_mic_errors=%u downstream_ploam_messages=%u "
		"omci_mic_errors=%u upstream_ploam_messages=%u "
		"rx_omci_mac_messages=%u rx_omci_fe_messages=%u "
		"tx_omci_mac_messages=%u tx_omci_fe_messages=%u\n",
		(u32)EN7581_XGSPON_TC_HW_VALID,
		(u32)EN7581_XGSPON_DS_HW_VALID,
		(u32)EN7581_XGSPON_US_HW_VALID, psbd_hec_errors,
		en7581_xgspon_hec_errors(hlend),
		unknown_profiles, transmitted_xgem_frames, fragment_xgem_frames,
		xgem_hec_lost_words, xgem_key_errors,
		en7581_xgspon_hec_errors(header),
		transmitted_non_idle_bytes, received_non_idle_bytes,
		(u32)FIELD_GET(EN7581_XGSPON_PLOAM_MIC_ERR, mic),
		downstream_ploam_messages,
		(u32)FIELD_GET(EN7581_XGSPON_OMCI_MIC_ERR, mic),
		upstream_ploam_messages,
		(u32)FIELD_GET(EN7581_XGSPON_OMCI_MAC_CNT, rx_omci),
		(u32)FIELD_GET(EN7581_XGSPON_OMCI_FE_CNT, rx_omci),
		(u32)FIELD_GET(EN7581_XGSPON_OMCI_MAC_CNT, tx_omci),
		(u32)FIELD_GET(EN7581_XGSPON_OMCI_FE_CNT, tx_omci));
}
static DEVICE_ATTR_RO(xgs_counter_evidence);

/*
 * These software counters use the same dispatch/completion boundary as the
 * recovered SDK's dsPloamCounter/usPloamCounter. They are driver-lifetime
 * totals, while session_generation identifies the current OMCC/key session.
 * They intentionally remain separate from the raw MAC counters and do not
 * claim the complete cross-layer, generation-aware G.988 PM snapshot.
 */
static ssize_t xgs_ploam_evidence_show(struct device *dev,
				       struct device_attribute *attribute,
				       char *buf)
{
	struct en7581_xgspon *priv = dev_get_drvdata(dev);
	u64 profile, assign_onu_id, ranging_time, deactivate, disable_serial;
	u64 request_registration, assign_alloc_id, key_control, sleep_allow;
	u64 serial, registration, key_report, acknowledge;
	u64 session_generation;

	mutex_lock(&priv->lock);
	profile = priv->profile_messages_received;
	assign_onu_id = priv->assign_onu_id_messages_received;
	ranging_time = priv->ranging_time_messages_received;
	deactivate = priv->deactivate_onu_id_messages_received;
	disable_serial = priv->disable_serial_number_messages_received;
	request_registration = priv->request_registration_messages_received;
	assign_alloc_id = priv->assign_alloc_id_messages_received;
	key_control = priv->key_control_messages_received;
	sleep_allow = priv->sleep_allow_messages_received;
	serial = priv->serial_number_send_events;
	registration = priv->registration_send_events;
	key_report = priv->key_report_send_events;
	acknowledge = priv->acknowledge_send_events;
	session_generation = priv->session_generation;
	mutex_unlock(&priv->lock);

	return sysfs_emit(buf,
		"version=3 complete=0 semantics=driver-lifecycle "
		"instance_generation=%llu session_generation=%llu "
		"profile_messages_received=%llu "
		"assign_onu_id_messages_received=%llu "
		"ranging_time_messages_received=%llu "
		"deactivate_onu_id_messages_received=%llu "
		"disable_serial_number_messages_received=%llu "
		"request_registration_messages_received=%llu "
		"assign_alloc_id_messages_received=%llu "
		"key_control_messages_received=%llu "
		"sleep_allow_messages_received=%llu "
		"serial_number_messages_completed=%llu "
		"registration_messages_completed=%llu "
		"key_report_messages_completed=%llu "
		"acknowledge_messages_completed=%llu "
		"sleep_request_messages_completed=0\n",
		priv->instance_generation, session_generation, profile, assign_onu_id,
		ranging_time, deactivate,
		disable_serial,
		request_registration, assign_alloc_id, key_control, sleep_allow,
		serial, registration, key_report, acknowledge);
}
static DEVICE_ATTR_RO(xgs_ploam_evidence);

static ssize_t reboot_request_show(struct device *dev,
				   struct device_attribute *attribute, char *buf)
{
	struct en7581_xgspon *priv = dev_get_drvdata(dev);
	struct airoha_xgs_reboot_request request;
	u64 generation;

	mutex_lock(&priv->lock);
	generation = priv->reboot_request_generation;
	request = priv->last_reboot_request;
	mutex_unlock(&priv->lock);
	if (!generation)
		return sysfs_emit(buf, "none\n");
	return sysfs_emit(buf,
		"generation=%llu sequence=%u depth=%u image=%u state=%u flags=%u\n",
		(unsigned long long)generation, request.sequence, request.depth,
		request.image, request.state, request.flags);
}
static DEVICE_ATTR_RO(reboot_request);

/*
 * Versioned foundation for the eventual cross-layer PM ABI. Hardware fields
 * are independently extended at a bounded cadence and rebased on every
 * kernel session. Daemon OMCI fields remain outside this kernel snapshot, and
 * the hardware extension assumptions remain board-unverified, so this endpoint
 * must not be used as XGSPONController yet. Sleep Request is a valid constant
 * zero because neither the recovered SDK nor this sole PLOAM owner has a sender.
 */
static ssize_t xgs_pm_snapshot_show(struct device *dev,
				    struct device_attribute *attribute,
				    char *buf)
{
	struct en7581_xgspon *priv = dev_get_drvdata(dev);
	u64 psbd_hec_errors, xgtc_hec_errors, xgem_hec_errors;
	u64 sequence, generation;
	ssize_t len;

	mutex_lock(&priv->lock);
	if (!priv->hardware_selected || !priv->mac) {
		mutex_unlock(&priv->lock);
		return -EOPNOTSUPP;
	}
	en7581_xgspon_pm_update_hardware_locked(priv);
	priv->pm_snapshot_sequence += 2;
	sequence = priv->pm_snapshot_sequence;
	generation = priv->session_generation;
	psbd_hec_errors =
		en7581_xgspon_pm_hardware_value(priv,
			EN7581_XGSPON_PM_HLEND_HEC_1) +
		en7581_xgspon_pm_hardware_value(priv,
			EN7581_XGSPON_PM_HLEND_HEC_2) +
		en7581_xgspon_pm_hardware_value(priv,
			EN7581_XGSPON_PM_HLEND_HEC_3) +
		en7581_xgspon_pm_hardware_value(priv,
			EN7581_XGSPON_PM_ALLOC_HEC_1) +
		en7581_xgspon_pm_hardware_value(priv,
			EN7581_XGSPON_PM_ALLOC_HEC_2) +
		en7581_xgspon_pm_hardware_value(priv,
			EN7581_XGSPON_PM_ALLOC_HEC_3) +
		en7581_xgspon_pm_hardware_value(priv,
			EN7581_XGSPON_PM_HEADER_HEC_1) +
		en7581_xgspon_pm_hardware_value(priv,
			EN7581_XGSPON_PM_HEADER_HEC_2) +
		en7581_xgspon_pm_hardware_value(priv,
			EN7581_XGSPON_PM_HEADER_HEC_3) +
		en7581_xgspon_pm_hardware_value(priv,
			EN7581_XGSPON_PM_PHY_SFC_HEC) +
		en7581_xgspon_pm_hardware_value(priv,
			EN7581_XGSPON_PM_PHY_PON_ID_HEC);
	xgtc_hec_errors =
		en7581_xgspon_pm_hardware_value(priv,
			EN7581_XGSPON_PM_HLEND_HEC_1) +
		en7581_xgspon_pm_hardware_value(priv,
			EN7581_XGSPON_PM_HLEND_HEC_2) +
		en7581_xgspon_pm_hardware_value(priv,
			EN7581_XGSPON_PM_HLEND_HEC_3);
	xgem_hec_errors =
		en7581_xgspon_pm_hardware_value(priv,
			EN7581_XGSPON_PM_HEADER_HEC_1) +
		en7581_xgspon_pm_hardware_value(priv,
			EN7581_XGSPON_PM_HEADER_HEC_2) +
		en7581_xgspon_pm_hardware_value(priv,
			EN7581_XGSPON_PM_HEADER_HEC_3);

	len = sysfs_emit(buf,
		"version=2 complete=0 semantics=kernel-instance-session-monotonic-partial "
		"instance_generation=%llu generation=%llu sequence_begin=%llu sequence_end=%llu "
		"sampling_interval_ms=%u sampling_assumption=single-wrap-no-reset-per-interval-unverified "
		"tc_valid=%u tc_required=%u downstream_valid=%u downstream_required=%u "
		"upstream_valid=%u upstream_required=%u "
		"psbd_hec_errors=%llu xgtc_hec_errors=%llu unknown_profiles=%llu "
		"transmitted_xgem_frames=%llu fragment_xgem_frames=%llu "
		"xgem_hec_lost_words=%llu xgem_key_errors=%llu xgem_hec_errors=%llu "
		"transmitted_non_idle_bytes=%llu received_non_idle_bytes=%llu "
		"lods_events=%llu lods_restored=%llu onu_reactivations_by_lods=%llu "
		"ploam_mic_errors=%llu downstream_ploam_messages=%llu "
		"profile_messages=%llu ranging_time_messages=%llu "
		"deactivate_onu_id_messages=%llu disable_serial_number_messages=%llu "
		"request_registration_messages=%llu assign_alloc_id_messages=%llu "
		"key_control_messages=%llu sleep_allow_messages=%llu "
		"baseline_omci_messages=0 extended_omci_messages=0 "
		"assign_onu_id_messages=%llu omci_mic_errors=%llu "
		"upstream_ploam_messages=%llu serial_number_messages=%llu "
		"registration_messages=%llu key_report_messages=%llu "
		"acknowledge_messages=%llu sleep_request_messages=0\n",
		priv->instance_generation, generation, sequence, sequence,
		EN7581_XGSPON_PM_SAMPLE_MS,
		(u32)EN7581_XGSPON_TC_PM_VALID,
		(u32)EN7581_XGSPON_TC_PM_REQUIRED,
		(u32)EN7581_XGSPON_DS_PM_VALID,
		(u32)EN7581_XGSPON_DS_PM_REQUIRED,
		(u32)EN7581_XGSPON_US_PM_VALID,
		(u32)EN7581_XGSPON_US_PM_REQUIRED,
		psbd_hec_errors, xgtc_hec_errors,
		en7581_xgspon_pm_hardware_value(priv,
			EN7581_XGSPON_PM_UNKNOWN_PROFILES),
		en7581_xgspon_pm_hardware_value(priv,
			EN7581_XGSPON_PM_TX_XGEM),
		en7581_xgspon_pm_hardware_value(priv,
			EN7581_XGSPON_PM_FRAGMENT_XGEM),
		en7581_xgspon_pm_hardware_value(priv,
			EN7581_XGSPON_PM_XGEM_LOST_WORDS),
		en7581_xgspon_pm_hardware_value(priv,
			EN7581_XGSPON_PM_XGEM_KEY_ERRORS),
		xgem_hec_errors,
		en7581_xgspon_pm_hardware_value(priv,
			EN7581_XGSPON_PM_TX_NON_IDLE_BYTES),
		en7581_xgspon_pm_hardware_value(priv,
			EN7581_XGSPON_PM_RX_NON_IDLE_BYTES),
		priv->pm_lods.events, priv->pm_lods.restored,
		priv->pm_lods.reactivations,
		en7581_xgspon_pm_hardware_value(priv,
			EN7581_XGSPON_PM_PLOAM_MIC_ERRORS),
		en7581_xgspon_pm_hardware_value(priv,
			EN7581_XGSPON_PM_RX_PLOAM),
		priv->pm_profile_messages, priv->pm_ranging_time_messages,
		priv->pm_deactivate_onu_id_messages,
		priv->pm_disable_serial_number_messages,
		priv->pm_request_registration_messages,
		priv->pm_assign_alloc_id_messages,
		priv->pm_key_control_messages, priv->pm_sleep_allow_messages,
		priv->pm_assign_onu_id_messages,
		en7581_xgspon_pm_hardware_value(priv,
			EN7581_XGSPON_PM_OMCI_MIC_ERRORS),
		en7581_xgspon_pm_hardware_value(priv,
			EN7581_XGSPON_PM_TX_PLOAM),
		priv->pm_serial_number_messages,
		priv->pm_registration_messages,
		priv->pm_key_report_messages,
		priv->pm_acknowledge_messages);
	mutex_unlock(&priv->lock);

	return len;
}
static DEVICE_ATTR_RO(xgs_pm_snapshot);

static ssize_t phy_evidence_show(struct device *dev,
				 struct device_attribute *attribute, char *buf)
{
	struct en7581_xgspon *priv = dev_get_drvdata(dev);
	u32 interrupt_status, interrupt_enable, rx_sync, sfp_status, phy_status;
	u32 last_irq, recovery_attempt;
	u32 tx_sync;
	u64 irq_events, los_events, lof_events, sync_events, rx_ready_events;
	u64 phya_ready_events, fake_sync_events, recoveries, recovery_failures;
	bool irq_owned, driver_ready, recovering, fault, lof, lods_preserved;

	mutex_lock(&priv->lock);
	if (!priv->hardware_selected || !priv->phy_csr || !priv->mac) {
		mutex_unlock(&priv->lock);
		return sysfs_emit(buf,
			"version=3 complete=0 hardware_selected=0 irq_owned=0 "
			"driver_ready=0 recovering=0 fault=0\n");
	}

	/* Observation only: do not clear status or alter the SDK interrupt mask. */
	interrupt_status = readl(priv->phy_csr +
				 EN7581_XGSPON_PHY_INT_STATUS);
	interrupt_enable = readl(priv->phy_csr +
				 EN7581_XGSPON_PHY_INT_ENABLE);
	rx_sync = readl(priv->phy_csr + EN7581_XGSPON_PHY_RX_SYNC_STATUS);
	sfp_status = readl(priv->phy_csr + EN7581_XGSPON_PHY_SFP_STATUS);
	phy_status = readl(priv->phy_csr + EN7581_XGSPON_PHY_STATUS);
	tx_sync = readl(priv->mac + EN7581_XGSPON_DBG_RESYNC);
	irq_owned = priv->phy_irq_requested;
	driver_ready = priv->phy_ready;
	recovering = priv->phy_recovering;
	fault = priv->phy_fault;
	lof = priv->phy_lof;
	lods_preserved = priv->lods_session_preserved;
	last_irq = priv->last_phy_irq_status;
	recovery_attempt = priv->phy_recovery_attempts;
	irq_events = priv->phy_irq_events;
	los_events = priv->phy_los_events;
	lof_events = priv->phy_lof_events;
	sync_events = priv->phy_sync_events;
	rx_ready_events = priv->phy_rx_ready_events;
	phya_ready_events = priv->phy_phya_ready_events;
	fake_sync_events = priv->phy_fake_sync_events;
	recoveries = priv->phy_recoveries;
	recovery_failures = priv->phy_recovery_failures;
	mutex_unlock(&priv->lock);

	return sysfs_emit(buf,
		"version=3 complete=0 hardware_selected=1 irq_owned=%u "
		"driver_ready=%u recovering=%u fault=%u "
		"lods_session_preserved=%u lods_timeout_ms=%u "
		"phya_ready=%u rx_sync=%u rx_sync_state=%u los=%u "
		"lof=%u lof_event_pending=%u rx_ready_event_pending=%u "
		"phya_ready_event_pending=%u tx_sync_ready=%u "
		"interrupt_status=%#x observed_interrupt_status=%#x interrupt_enable=%#x "
		"last_irq=%#x irq_events=%llu los_events=%llu lof_events=%llu "
		"sync_events=%llu rx_ready_events=%llu phya_ready_events=%llu "
		"fake_sync_events=%llu recovery_attempt=%u recoveries=%llu "
		"recovery_failures=%llu\n",
		irq_owned, driver_ready, recovering, fault, lods_preserved,
		EN7581_XGSPON_LODS_TIMEOUT_MS,
		!!(phy_status & EN7581_XGSPON_PHY_PHYA_READY),
		!!(rx_sync & EN7581_XGSPON_PHY_RX_SYNC),
		(u32)FIELD_GET(EN7581_XGSPON_PHY_RX_SYNC_STATE, rx_sync),
		!!(sfp_status & EN7581_XGSPON_PHY_SFP_RX_LOS),
		lof,
		!!(interrupt_status & EN7581_XGSPON_PHY_INT_RX_LOF),
		!!(interrupt_status & EN7581_XGSPON_PHY_INT_RX_READY),
		!!(interrupt_status & EN7581_XGSPON_PHY_INT_PHYA_READY),
		!!(tx_sync & EN7581_XGSPON_TX_SYNC_READY), interrupt_status,
		(u32)(interrupt_status & EN7581_XGSPON_PHY_RX_EVENTS),
		interrupt_enable, last_irq, irq_events, los_events,
		lof_events, sync_events, rx_ready_events, phya_ready_events,
		fake_sync_events, recovery_attempt, recoveries,
		recovery_failures);
}
static DEVICE_ATTR_RO(phy_evidence);

static ssize_t mac_errors_show(struct device *dev,
			       struct device_attribute *attribute, char *buf)
{
	struct en7581_xgspon *priv = dev_get_drvdata(dev);
	u32 bwmap_errors, interrupt_enable, rx_errors;
	ssize_t len;

	mutex_lock(&priv->lock);
	if (!priv->hardware_selected || !priv->mac) {
		mutex_unlock(&priv->lock);
		return sysfs_emit(buf,
			"version=1 hardware_selected=0 interrupt_enable=0 rx_error_events=0 bwmap_check_error_events=0\n");
	}
	interrupt_enable = readl(priv->mac + EN7581_XGSPON_INT_ENABLE);
	rx_errors = readl(priv->mac + EN7581_XGSPON_RX_ERR_STS) &
		    EN7581_XGSPON_RX_ERR_ALL;
	bwmap_errors = readl(priv->mac + EN7581_XGSPON_BWMAP_CHECK_STS) &
		       EN7581_XGSPON_BWMAP_CHECK_ERR_ALL;
	len = sysfs_emit(buf,
		"version=1 hardware_selected=1 interrupt_enable=%#x "
		"rx_summary_enabled=%u bwmap_summary_enabled=%u "
		"live_rx_error=%#x live_bwmap_check_error=%#x "
		"rx_error_events=%llu bwmap_check_error_events=%llu "
		"loss_of_gem_delineation_events=%llu "
		"last_rx_error=%#x last_bwmap_check_error=%#x\n",
		interrupt_enable,
		!!(interrupt_enable & EN7581_XGSPON_INT_RX_ERROR),
		!!(interrupt_enable & EN7581_XGSPON_INT_BWMAP_CHECK_ERROR),
		rx_errors, bwmap_errors, priv->rx_error_events,
		priv->bwmap_check_error_events,
		priv->loss_of_gem_delineation_events,
		priv->last_rx_error_status,
		priv->last_bwmap_check_error_status);
	mutex_unlock(&priv->lock);

	return len;
}
static DEVICE_ATTR_RO(mac_errors);

static ssize_t activation_evidence_show(struct device *dev,
					struct device_attribute *attribute,
					char *buf)
{
	struct en7581_xgspon *priv = dev_get_drvdata(dev);
	u32 activation, control, current_index, fifo_status, interrupt_enable;
	u32 upstream_aes_control, downstream_aes_valid;
	u32 equalization_delay_readback, random_delay_register, tx_error_status;
	u32 tcont_count = 0, tx_sync;
	int i;
	ssize_t len;

	mutex_lock(&priv->lock);
	if (!priv->hardware_selected || !priv->mac) {
		mutex_unlock(&priv->lock);
		return sysfs_emit(buf,
			"version=1 complete=0 hardware_selected=0 tx_authorized=0 optical_tx=disabled omcc_xgem_programmed=0 omcc_xgem_id=1023 ack_queue_depth=0 ack_in_flight=0\n");
	}
	activation = readl(priv->mac + EN7581_XGSPON_ACTIVATION_ST);
	control = readl(priv->mac + EN7581_XGSPON_O23_O4_PLOAMU_CTRL);
	current_index = readl(priv->mac + EN7581_XGSPON_CUR_KIDX);
	fifo_status = readl(priv->mac + EN7581_XGSPON_PLOAMU_FIFO_STS);
	interrupt_enable = readl(priv->mac + EN7581_XGSPON_INT_ENABLE);
	upstream_aes_control = readl(priv->mac +
				     EN7581_XGSPON_US_AES_KEY_CTRL);
	downstream_aes_valid = readl(priv->mac +
				    EN7581_XGSPON_DS_AES_KEY_VALID);
	random_delay_register = readl(priv->mac + EN7581_XGSPON_RDM_DLY);
	tx_error_status = readl(priv->mac + EN7581_XGSPON_TX_ERR_STS) &
				  EN7581_XGSPON_TX_ERR_ALL;
	tx_sync = readl(priv->mac + EN7581_XGSPON_DBG_RESYNC);
	equalization_delay_readback = readl(priv->mac + EN7581_XGSPON_EQD);
	for (i = 1; i < EN7581_XGSPON_MAX_TCONTS; i++)
		if (priv->alloc_valid[i])
			tcont_count++;
	len = sysfs_emit(buf,
		"version=1 complete=0 hardware_selected=1 tx_authorized=%u "
			"optical_tx=%s activation_state=%u tx_sync_ready=%u "
			"software_reply_mode=%u interrupt_enable=%#x upstream_interrupt_enable=%#x "
			"current_pik=%u current_oik=%u registration_key_switch_verified=%u "
			"tx_state=%s random_delay_enabled=%u serial_random_delay_bits=%u "
			"ploamu_available=%u ploamu_overrun=%u live_tx_error=%#x "
			"profile_authenticated=%u onu_id_assigned=%u "
			"omcc_xgem_programmed=%u omcc_xgem_id=%u "
			"request_registration_authenticated=%u request_registration_sequence=%u "
			"deactivate_events=%llu last_deactivate_sequence=%u serial_disabled=%u "
			"disable_serial_events=%llu allow_serial_events=%llu "
			"disable_discovery_events=%llu sleep_allow_events=%llu "
			"sleep_allow_type_one_events=%llu recognized_noop_ploam_events=%llu "
			"ngpon2_control_ignored_events=%llu "
			"unsupported_ploam_events=%llu "
			"data_key_pending=%u data_key_confirm_pending=%u data_key_confirmed=%u data_key_index=%u "
			"upstream_data_key_valid=%u upstream_data_key_index=%u downstream_data_key_valid=%#x "
				"ranging_time_authenticated=%u ranging_sequence=%u ranging_eqd=%u "
				"ranging_absolute=%u ranging_negative=%u "
				"eqd_programmed=%u eqd=%u eqd_readback=%u "
				"assign_alloc_id_authenticated=%u assign_alloc_id_committed=%u "
				"alloc_id=%u alloc_sequence=%u alloc_operation=%u "
			"last_tcont_index=%u business_tcont_count=%u "
			"ack_queue_depth=%u ack_in_flight=%u ack_source_message_id=%#x "
			"ack_sequence=%u ack_completion_code=%u "
			"ploamu_send_events=%llu serial_number_request_events=%llu "
			"serial_number_send_events=%llu ranging_request_events=%llu "
			"registration_send_events=%llu acknowledge_queued_events=%llu "
			"acknowledge_send_events=%llu acknowledge_coalesced_events=%llu "
			"acknowledge_dropped_events=%llu no_message_events=%llu "
			"key_report_queue_depth=%u key_report_in_flight=%u "
			"key_control_generate_events=%llu key_control_confirm_events=%llu "
			"key_report_queued_events=%llu key_report_send_events=%llu "
			"key_report_coalesced_events=%llu key_report_dropped_events=%llu "
			"key_exchange_timeout_events=%llu key_exchange_rollback_events=%llu "
			"fifo_error_events=%llu tx_error_events=%llu upstream_fifo_overrun_events=%llu "
			"unexpected_upstream_events=%llu last_upstream_irq=%#x "
			"last_fifo_error=%#x last_tx_error=%#x\n",
		priv->tx_authorized,
		priv->optical_tx_enabled ? "enabled" : "disabled",
		(u32)FIELD_GET(EN7581_XGSPON_ACTIVATION_STATE, activation),
		!!(tx_sync & EN7581_XGSPON_TX_SYNC_READY),
			!!(control & EN7581_XGSPON_O23_O4_PLOAMU_SOFTWARE),
			interrupt_enable,
			(u32)(interrupt_enable & EN7581_XGSPON_INT_UPSTREAM_EVENTS),
			!!(current_index & EN7581_XGSPON_CUR_PIK_IDX),
			!!(current_index & EN7581_XGSPON_CUR_OIK_IDX),
			priv->registration_key_switch_verified,
			en7581_xgspon_tx_state_name(priv->tx_state),
			!!(FIELD_GET(EN7581_XGSPON_MAX_RANDOM_DELAY,
				     random_delay_register) &
				   EN7581_XGSPON_RANDOM_DELAY_ENABLE_FIELD),
			en7581_xgspon_serial_random_delay(random_delay_register),
			(u32)FIELD_GET(EN7581_XGSPON_PLOAMU_AVAILABLE, fifo_status),
			!!(fifo_status & EN7581_XGSPON_PLOAMU_OVERRUN),
			tx_error_status,
		priv->session_keys_set, priv->onu_id_set,
			priv->omcc_xgem_programmed, priv->onu_id,
			priv->request_registration_authenticated,
			priv->request_registration_sequence,
			priv->deactivate_events,
			priv->last_deactivate_sequence,
			priv->serial_disabled,
			priv->disable_serial_events,
			priv->allow_serial_events,
			priv->disable_discovery_events,
			priv->sleep_allow_events,
			priv->sleep_allow_type_one_events,
			priv->recognized_noop_ploam_events,
			priv->ngpon2_control_ignored_events,
			priv->unsupported_ploam_events,
			priv->data_key_pending, priv->data_key_confirm_pending,
			priv->data_key_confirmed,
			priv->data_key_index,
			!!(upstream_aes_control & EN7581_XGSPON_US_AES_KEY_VALID),
			!!(upstream_aes_control & EN7581_XGSPON_US_AES_KEY_INDEX),
			(u32)(downstream_aes_valid &
			      EN7581_XGSPON_DS_AES_UC_KEYS_VALID),
			priv->ranging_time_authenticated,
			priv->last_ranging_time.sequence,
				priv->last_ranging_time.equalization_delay,
				priv->last_ranging_time.absolute,
				priv->last_ranging_time.negative,
				priv->equalization_delay_programmed,
				priv->equalization_delay,
				equalization_delay_readback,
				priv->assign_alloc_id_authenticated,
				priv->assign_alloc_id_committed,
				priv->last_alloc_id_assignment.alloc_id,
				priv->last_alloc_id_assignment.sequence,
				priv->last_alloc_id_assignment.operation,
				priv->last_tcont_index, tcont_count,
			priv->ack_count, priv->ack_in_flight,
			priv->ack_in_flight ? priv->active_ack.source_message_id : 0,
			priv->ack_in_flight ? priv->active_ack.sequence : 0,
			priv->ack_in_flight ? priv->active_ack.completion_code : 0,
			priv->ploamu_send_events,
			priv->serial_number_request_events,
			priv->serial_number_send_events,
			priv->ranging_request_events,
			priv->registration_send_events,
			priv->acknowledge_queued_events,
			priv->acknowledge_send_events,
			priv->acknowledge_coalesced_events,
			priv->acknowledge_dropped_events,
			priv->no_message_events,
			priv->key_report_count, priv->key_report_in_flight,
			priv->key_control_generate_events,
			priv->key_control_confirm_events,
			priv->key_report_queued_events,
			priv->key_report_send_events,
			priv->key_report_coalesced_events,
			priv->key_report_dropped_events,
			priv->key_exchange_timeout_events,
			priv->key_exchange_rollback_events,
			priv->fifo_error_events, priv->tx_error_events,
			priv->upstream_fifo_overrun_events,
			priv->unexpected_upstream_events, priv->last_upstream_irq_status,
			priv->last_fifo_error_status, priv->last_tx_error_status);
	mutex_unlock(&priv->lock);

	return len;
}
static DEVICE_ATTR_RO(activation_evidence);

static ssize_t to1_evidence_show(struct device *dev,
				 struct device_attribute *attribute, char *buf)
{
	struct en7581_xgspon *priv = dev_get_drvdata(dev);
	unsigned long remaining_ms = 0;
	u64 armed_generation, session_generation, timeout_events;
	u32 activation_state = 0, expected_state;
	bool armed;

	mutex_lock(&priv->lock);
	if (priv->hardware_selected && priv->mac)
		activation_state = FIELD_GET(
			EN7581_XGSPON_ACTIVATION_STATE,
			readl(priv->mac + EN7581_XGSPON_ACTIVATION_ST));
	armed_generation = priv->to1_generation;
	session_generation = priv->session_generation;
	expected_state = priv->to1_expected_state;
	timeout_events = priv->to1_timeout_events;
	armed = priv->to1_deadline &&
		priv->to1_generation == priv->session_generation &&
		priv->to1_expected_state == EN7581_XGSPON_ACTIVATION_O4;
	if (armed && time_before(jiffies, priv->to1_deadline))
		remaining_ms = jiffies_to_msecs(priv->to1_deadline - jiffies);
	mutex_unlock(&priv->lock);

	return sysfs_emit(buf,
		"version=1 sdk_timeout_ms=%u armed=%u expected_state=%u activation_state=%u armed_generation=%llu session_generation=%llu remaining_ms=%lu timeout_events=%llu\n",
		EN7581_XGSPON_TO1_MS, armed, expected_state, activation_state,
		armed_generation, session_generation, remaining_ms,
		timeout_events);
}
static DEVICE_ATTR_RO(to1_evidence);

static ssize_t serial_number_show(struct device *dev,
				  struct device_attribute *attribute, char *buf)
{
	struct en7581_xgspon *priv = dev_get_drvdata(dev);
	ssize_t len;

	mutex_lock(&priv->lock);
	if (!priv->serial_set)
		len = sysfs_emit(buf, "unset\n");
	else
		len = sysfs_emit(buf, "%c%c%c%c%02X%02X%02X%02X\n",
				 priv->serial[0], priv->serial[1], priv->serial[2],
				 priv->serial[3], priv->serial[4], priv->serial[5],
				 priv->serial[6], priv->serial[7]);
	mutex_unlock(&priv->lock);

	return len;
}

static ssize_t serial_number_store(struct device *dev,
				   struct device_attribute *attribute,
				   const char *buf, size_t count)
{
	struct en7581_xgspon *priv = dev_get_drvdata(dev);
	u8 serial[AIROHA_XGS_SERIAL_SIZE];
	u8 old_serial[AIROHA_XGS_SERIAL_SIZE] = {};
	int ret;

	ret = en7581_xgspon_parse_serial(buf, count, serial);
	if (ret)
		goto out;

	mutex_lock(&priv->lock);
	if (priv->serial_set)
		memcpy(old_serial, priv->serial, sizeof(old_serial));
	ret = en7581_xgspon_program_serial(priv, serial);
	if (ret) {
		en7581_xgspon_restore_serial(priv, old_serial);
		priv->hardware_state_valid = false;
		goto unlock;
	}
	ret = en7581_xgspon_clear_session_keys(priv);
	if (ret) {
		en7581_xgspon_restore_serial(priv, old_serial);
		goto unlock;
	}
	memcpy(priv->serial, serial, sizeof(serial));
	priv->serial_set = true;
	priv->hardware_state_valid = true;
unlock:
	mutex_unlock(&priv->lock);
out:
	memzero_explicit(old_serial, sizeof(old_serial));
	memzero_explicit(serial, sizeof(serial));

	return ret ? ret : count;
}
static DEVICE_ATTR_RW(serial_number);

static ssize_t registration_id_configured_show(struct device *dev,
						struct device_attribute *attribute,
						char *buf)
{
	struct en7581_xgspon *priv = dev_get_drvdata(dev);
	bool configured;

	mutex_lock(&priv->lock);
	configured = priv->registration_id_set;
	mutex_unlock(&priv->lock);

	return sysfs_emit(buf, "%u\n", configured);
}
static DEVICE_ATTR_RO(registration_id_configured);

static ssize_t key_derivation_state_show(struct device *dev,
					 struct device_attribute *attribute,
					 char *buf)
{
	struct en7581_xgspon *priv = dev_get_drvdata(dev);
	const char *state;

	mutex_lock(&priv->lock);
	if (!priv->hardware_state_valid)
		state = "hardware-state-uncertain-tx-disabled";
	else if (!priv->registration_msk_set)
		state = "registration-msk-unset";
	else if (priv->session_keys_set)
		state = !priv->session_keys_programmed ?
			"session-keys-ready-hardware-pending" :
			priv->tx_authorized ?
			"registered-secure-omcc-ready" :
			priv->onu_id_set ?
			"onu-id-assigned-registration-pending" :
			"session-keys-loaded-awaiting-onu-id-tx-disabled";
	else
		state = "registration-msk-ready-pon-tag-pending";
	mutex_unlock(&priv->lock);

	return sysfs_emit(buf, "%s\n", state);
}
static DEVICE_ATTR_RO(key_derivation_state);

static ssize_t mac_initialization_show(struct device *dev,
				       struct device_attribute *attribute,
				       char *buf)
{
	struct en7581_xgspon *priv = dev_get_drvdata(dev);
	const struct airoha_xgs_mac_init_setting *settings;
	u32 values[AIROHA_XGS_MAC_INIT_REGISTER_COUNT] = {};
	bool initialized, verified;
	unsigned int i;

	mutex_lock(&priv->lock);
	if (!priv->hardware_selected || !priv->mac) {
		mutex_unlock(&priv->lock);
		return sysfs_emit(buf,
			"version=1 complete=0 hardware_selected=0 initialized=0 verified=0\n");
	}
	initialized = priv->mac_initialized;
	settings = airoha_xgs_mac_init_profile(priv->active_mode);
	if (!settings) {
		mutex_unlock(&priv->lock);
		return sysfs_emit(buf,
			"version=1 complete=0 hardware_selected=1 initialized=0 verified=0\n");
	}
	verified = initialized;
	for (i = 0; i < AIROHA_XGS_MAC_INIT_REGISTER_COUNT; i++) {
		const struct airoha_xgs_mac_init_setting *setting =
			&settings[i];

		values[setting->reg] = readl(priv->mac +
			en7581_xgspon_mac_init_offsets[setting->reg]);
		/* These two startup-safe fields intentionally change as the ONU
		 * enters software-controlled activation and must not make the
		 * persistent configuration evidence false. */
		if (setting->reg == AIROHA_XGS_MAC_INIT_PLOAM_CONTROL ||
		    setting->reg == AIROHA_XGS_MAC_INIT_ACTIVATION)
			continue;
		if ((values[setting->reg] & setting->mask) != setting->value)
			verified = false;
	}
	mutex_unlock(&priv->lock);

	return sysfs_emit(buf,
		"version=1 complete=%u hardware_selected=1 initialized=%u verified=%u "
		"ploam_control=%#x activation=%#x response_time=%#x "
		"ds_fec=%#x debug_cap=%#x dying_gasp=%#x "
		"ploam_drop=%#x tx_resync=%#x idle_gem=%#x mib=%#x\n",
		verified, initialized, verified,
		values[AIROHA_XGS_MAC_INIT_PLOAM_CONTROL],
		values[AIROHA_XGS_MAC_INIT_ACTIVATION],
		values[AIROHA_XGS_MAC_INIT_RSP_TIME],
		values[AIROHA_XGS_MAC_INIT_DS_FEC],
		values[AIROHA_XGS_MAC_INIT_DEBUG_CAP],
		values[AIROHA_XGS_MAC_INIT_DYING_GASP],
		values[AIROHA_XGS_MAC_INIT_PLOAM_DROP],
		values[AIROHA_XGS_MAC_INIT_TX_RESYNC],
		values[AIROHA_XGS_MAC_INIT_IDLE_GEM],
		values[AIROHA_XGS_MAC_INIT_MIB]);
}
static DEVICE_ATTR_RO(mac_initialization);

static ssize_t registration_id_store(struct device *dev,
				     struct device_attribute *attribute,
				     const char *buf, size_t count)
{
	struct en7581_xgspon *priv = dev_get_drvdata(dev);
	u8 registration_id[AIROHA_XGS_REGISTRATION_ID_SIZE];
	u8 registration_msk[AIROHA_XGS_KEY_SIZE];
	u8 old_registration_id[AIROHA_XGS_REGISTRATION_ID_SIZE] = {};
	u8 zero_registration_id[AIROHA_XGS_REGISTRATION_ID_SIZE] = {};
	int ret;

	if (sysfs_streq(buf, "clear")) {
		mutex_lock(&priv->lock);
		if (priv->registration_id_set)
			memcpy(old_registration_id, priv->registration_id,
			       sizeof(old_registration_id));
		ret = en7581_xgspon_program_registration_id(
			priv, zero_registration_id);
		if (ret) {
			en7581_xgspon_restore_registration_id(
				priv, old_registration_id);
			priv->hardware_state_valid = false;
			goto unlock_clear;
		}
		ret = en7581_xgspon_clear_session_keys(priv);
		if (ret) {
			en7581_xgspon_restore_registration_id(
				priv, old_registration_id);
			goto unlock_clear;
		}
		memzero_explicit(priv->registration_id,
				 sizeof(priv->registration_id));
		memzero_explicit(priv->registration_msk,
				 sizeof(priv->registration_msk));
		priv->registration_id_set = false;
		priv->registration_msk_set = false;
		priv->hardware_state_valid = true;
	unlock_clear:
		mutex_unlock(&priv->lock);
		memzero_explicit(old_registration_id,
				 sizeof(old_registration_id));
		memzero_explicit(zero_registration_id,
				 sizeof(zero_registration_id));
		return ret ? ret : count;
	}

	ret = en7581_xgspon_parse_registration_id(buf, count,
						 registration_id);
	if (ret)
		goto out;
	ret = airoha_xgs_derive_registration_msk(registration_id,
						 registration_msk);
	if (ret)
		goto out;

	mutex_lock(&priv->lock);
	if (priv->registration_id_set)
		memcpy(old_registration_id, priv->registration_id,
		       sizeof(old_registration_id));
	ret = en7581_xgspon_program_registration_id(priv, registration_id);
	if (ret) {
		en7581_xgspon_restore_registration_id(priv,
					       old_registration_id);
		priv->hardware_state_valid = false;
		goto unlock;
	}
	ret = en7581_xgspon_clear_session_keys(priv);
	if (ret) {
		en7581_xgspon_restore_registration_id(priv,
					       old_registration_id);
		goto unlock;
	}
	memcpy(priv->registration_id, registration_id,
	       sizeof(registration_id));
	memcpy(priv->registration_msk, registration_msk,
	       sizeof(registration_msk));
	priv->registration_id_set = true;
	priv->registration_msk_set = true;
	priv->hardware_state_valid = true;
unlock:
	mutex_unlock(&priv->lock);

out:
	memzero_explicit(old_registration_id, sizeof(old_registration_id));
	memzero_explicit(zero_registration_id, sizeof(zero_registration_id));
	memzero_explicit(registration_id, sizeof(registration_id));
	memzero_explicit(registration_msk, sizeof(registration_msk));

	return ret ? ret : count;
}
static DEVICE_ATTR_WO(registration_id);

static struct attribute *en7581_xgspon_attrs[] = {
	&dev_attr_control_abi_version.attr,
	&dev_attr_pon_mode.attr,
	&dev_attr_service_reset.attr,
	&dev_attr_service_tcont.attr,
	&dev_attr_service_xgem.attr,
	&dev_attr_service_commit.attr,
	&dev_attr_service_rollback.attr,
	&dev_attr_service_state.attr,
	&dev_attr_enabled.attr,
	&dev_attr_activation_ready.attr,
	&dev_attr_ready.attr,
	&dev_attr_state.attr,
	&dev_attr_activation_blockers.attr,
	&dev_attr_secure_omcc_info.attr,
	&dev_attr_omcc_rx_evidence.attr,
	&dev_attr_xgs_counter_evidence.attr,
	&dev_attr_xgs_ploam_evidence.attr,
	&dev_attr_reboot_request.attr,
	&dev_attr_xgs_pm_snapshot.attr,
	&dev_attr_phy_evidence.attr,
	&dev_attr_mac_errors.attr,
	&dev_attr_activation_evidence.attr,
	&dev_attr_to1_evidence.attr,
	&dev_attr_mac_initialization.attr,
	&dev_attr_serial_number.attr,
	&dev_attr_registration_id_configured.attr,
	&dev_attr_key_derivation_state.attr,
	&dev_attr_registration_id.attr,
	NULL,
};
ATTRIBUTE_GROUPS(en7581_xgspon);

static int en7581_xgspon_xpon_block_traffic(void *context)
{
	struct en7581_xgspon *priv = context;

	cancel_delayed_work_sync(&priv->pm_work);
	cancel_delayed_work_sync(&priv->phy_recovery_work);
	cancel_delayed_work_sync(&priv->activation_work);
	cancel_delayed_work_sync(&priv->to1_work);
	cancel_delayed_work_sync(&priv->key_tk4_work);
	cancel_delayed_work_sync(&priv->key_tk5_work);
	mutex_lock(&priv->lock);
	priv->switch_resume_enabled = priv->enabled;
	priv->enabled = false;
	priv->tx_armed = false;
	priv->tx_authorized = false;
	priv->optical_tx_enabled = false;
	priv->active_mode = AIROHA_XPON_MODE_INVALID;
	mutex_unlock(&priv->lock);
	return 0;
}

static int en7581_xgspon_xpon_clear_session(void *context)
{
	struct en7581_xgspon *priv = context;
	int error = 0, ret;

	mutex_lock(&priv->lock);
	ret = en7581_xgspon_clear_service_locked(priv);
	if (ret)
		error = ret;
	ret = en7581_xgspon_clear_session_keys(priv);
	if (ret && !error)
		error = ret;
	ret = en7581_xgspon_reset_onu_id(priv);
	if (ret && !error)
		error = ret;
	ret = en7581_xgspon_clear_hardware_identity(priv);
	if (ret && !error)
		error = ret;
	en7581_xgspon_advance_session_locked(priv);
	en7581_xgspon_reset_activation_state(priv);
	mutex_unlock(&priv->lock);
	return error;
}

static int en7581_xgspon_xpon_mask_irqs(void *context)
{
	struct en7581_xgspon *priv = context;

	WRITE_ONCE(priv->xpon_irqs_masked, true);
	writel(0, priv->mac + EN7581_XGSPON_INT_ENABLE);
	writel(0, priv->phy_csr + EN7581_XGSPON_PHY_INT_ENABLE);
	if (readl(priv->mac + EN7581_XGSPON_INT_ENABLE) ||
	    readl(priv->phy_csr + EN7581_XGSPON_PHY_INT_ENABLE)) {
		writel(0, priv->mac + EN7581_XGSPON_INT_ENABLE);
		writel(0, priv->phy_csr + EN7581_XGSPON_PHY_INT_ENABLE);
		dev_err(priv->dev,
			"failed to verify XG/XGS-PON interrupt mask\n");
		return -EIO;
	}
	return 0;
}

static void en7581_xgspon_xpon_synchronize_irqs(void *context)
{
	struct en7581_xgspon *priv = context;

	synchronize_irq(priv->mac_irq);
	synchronize_irq(priv->phy_irq);
}

static int en7581_xgspon_xpon_stop_datapath(void *context)
{
	struct en7581_xgspon *priv = context;
	int error, ret;

	mutex_lock(&priv->lock);
	error = en7581_xgspon_clear_service_locked(priv);
	ret = en7581_xgspon_set_path_stopped(priv, true);
	if (ret) {
		priv->hardware_state_valid = false;
		if (!error)
			error = ret;
	}
	mutex_unlock(&priv->lock);
	return error;
}

static int en7581_xgspon_xpon_stop_mac(void *context)
{
	struct en7581_xgspon *priv = context;
	int ret;

	cancel_delayed_work_sync(&priv->pm_work);
	cancel_delayed_work_sync(&priv->phy_recovery_work);
	cancel_delayed_work_sync(&priv->activation_work);
	cancel_delayed_work_sync(&priv->to1_work);
	cancel_delayed_work_sync(&priv->key_tk4_work);
	cancel_delayed_work_sync(&priv->key_tk5_work);
	writel(0, priv->mac + EN7581_XGSPON_INT_ENABLE);
	mutex_lock(&priv->lock);
	ret = en7581_xgspon_hold_mac(priv);
	priv->hardware_selected = false;
	priv->hardware_state_valid = false;
	priv->mac_initialized = false;
	priv->phy_ready = false;
	priv->phy_recovering = false;
	priv->tx_armed = false;
	priv->tx_authorized = false;
	priv->optical_tx_enabled = false;
	mutex_unlock(&priv->lock);
	en7581_xgspon_detach_omcc(priv);
	return ret;
}

static int en7581_xgspon_scrub_hardware_locked(struct en7581_xgspon *priv)
{
	int error, ret;

	error = en7581_xgspon_clear_session_keys(priv);
	ret = en7581_xgspon_clear_hardware_identity(priv);
	if (!error)
		error = ret;
	if (error)
		priv->hardware_state_valid = false;

	return error;
}

static int en7581_xgspon_xpon_start_mac(void *context)
{
	struct en7581_xgspon *priv = context;
	enum airoha_xpon_mode target_mode;
	int cleanup_ret, hold_ret, ret;

	target_mode = airoha_en7572_get_mode(priv->bosa);
	if ((target_mode != AIROHA_XPON_MODE_XGPON &&
	     target_mode != AIROHA_XPON_MODE_XGSPON) ||
	    !airoha_en7572_is_ready(priv->bosa) ||
	    !airoha_en7572_tx_is_disabled(priv->bosa))
		return -EIO;
	ret = en7581_xgspon_attach_omcc(priv);
	if (ret)
		return ret;

	mutex_lock(&priv->lock);
	priv->hardware_selected = true;
	priv->active_mode = target_mode;
	writel(0, priv->mac + EN7581_XGSPON_INT_ENABLE);
	ret = en7581_xgspon_prepare_mac(priv);
	if (!ret)
		ret = en7581_xgspon_initialize_mac(priv);
	if (!ret)
		ret = en7581_xgspon_scrub_hardware_locked(priv);
	if (!ret && priv->serial_set)
		ret = en7581_xgspon_program_serial(priv, priv->serial);
	if (!ret && priv->registration_id_set)
		ret = en7581_xgspon_program_registration_id(
			priv, priv->registration_id);
	if (!ret)
		ret = en7581_xgspon_reset_onu_id(priv);
	if (!ret) {
		priv->mac_initialized = true;
		priv->hardware_ds_omci_mic = true;
		priv->hardware_state_valid = true;
		priv->phy_los = !!(readl(priv->phy_csr +
					 EN7581_XGSPON_PHY_SFP_STATUS) &
				    EN7581_XGSPON_PHY_SFP_RX_LOS);
		priv->phy_ready = !priv->phy_los &&
			en7581_xgspon_phy_live_ready(priv, NULL, NULL, NULL);
		priv->phy_fault = false;
	} else {
		/* A failed start may have partially restored identity or key state.
		 * Keep the laser dark and make a second, verified cleanup attempt. */
		cleanup_ret = en7581_xgspon_scrub_hardware_locked(priv);
		if (cleanup_ret)
			dev_err(priv->dev,
				"failed to scrub XG/XGS-PON hardware after start error: %d\n",
				cleanup_ret);
		hold_ret = en7581_xgspon_hold_mac(priv);
		if (hold_ret)
			dev_err(priv->dev,
				"failed to hold XG/XGS-PON MAC after start error: %d\n",
				hold_ret);
		priv->hardware_selected = false;
		priv->hardware_state_valid = false;
		priv->mac_initialized = false;
		priv->active_mode = AIROHA_XPON_MODE_INVALID;
	}
	mutex_unlock(&priv->lock);
	if (ret)
		en7581_xgspon_detach_omcc(priv);
	return ret;
}

static int en7581_xgspon_xpon_start_datapath(void *context)
{
	struct en7581_xgspon *priv = context;
	int hold_ret, ret;

	mutex_lock(&priv->lock);
	if (!priv->mac_initialized || !priv->hardware_selected ||
	    !priv->hardware_state_valid) {
		ret = -EIO;
		goto out;
	}
	ret = airoha_xgs_qdma_service_clear(priv->ethernet_np);
	if (ret)
		goto out;
	ret = en7581_xgspon_set_path_stopped(priv, false);
	if (!ret)
		goto out;

	hold_ret = en7581_xgspon_hold_mac(priv);
	if (hold_ret)
		dev_err(priv->dev,
			"failed to hold XG/XGS-PON MAC after datapath error: %d\n",
			hold_ret);
	priv->hardware_state_valid = false;
out:
	mutex_unlock(&priv->lock);
	return ret;
}

static int en7581_xgspon_xpon_unmask_irqs(void *context)
{
	struct en7581_xgspon *priv = context;
	int ret;

	if (!priv->hardware_selected || !priv->hardware_state_valid ||
	    airoha_en7572_fault_locked(priv->bosa))
		return -EIO;
	writel(EN7581_XGSPON_RX_ERR_ALL,
	       priv->mac + EN7581_XGSPON_RX_ERR_STS);
	writel(EN7581_XGSPON_BWMAP_CHECK_ERR_ALL,
	       priv->mac + EN7581_XGSPON_BWMAP_CHECK_STS);
	writel(U32_MAX, priv->mac + EN7581_XGSPON_INT_STATUS);
	writel(U32_MAX, priv->phy_csr + EN7581_XGSPON_PHY_INT_STATUS);
	ret = en7581_xgspon_set_mac_irq_enable(
		priv, EN7581_XGSPON_INT_ENABLED);
	if (ret)
		return ret;
	writel(EN7581_XGSPON_PHY_RX_EVENTS,
	       priv->phy_csr + EN7581_XGSPON_PHY_INT_ENABLE);
	if (readl(priv->mac + EN7581_XGSPON_INT_ENABLE) !=
			EN7581_XGSPON_INT_ENABLED ||
	    readl(priv->phy_csr + EN7581_XGSPON_PHY_INT_ENABLE) !=
			EN7581_XGSPON_PHY_RX_EVENTS) {
		writel(0, priv->mac + EN7581_XGSPON_INT_ENABLE);
		writel(0, priv->phy_csr + EN7581_XGSPON_PHY_INT_ENABLE);
		WRITE_ONCE(priv->xpon_irqs_masked, true);
		dev_err(priv->dev,
			"failed to verify XG/XGS-PON interrupt enable\n");
		return -EIO;
	}
	WRITE_ONCE(priv->xpon_irqs_masked, false);
	return 0;
}

static int en7581_xgspon_xpon_mode_committed(void *context)
{
	struct en7581_xgspon *priv = context;

	if (!en7581_xgspon_backend_active(priv))
		return 0;
	mod_delayed_work(system_wq, &priv->pm_work, 0);
	mutex_lock(&priv->lock);
	if (!priv->phy_los && !priv->phy_ready)
		en7581_xgspon_phy_schedule_recovery(
			priv, msecs_to_jiffies(
				EN7581_XGSPON_PHY_RECOVERY_DELAY_MS));
	if (priv->switch_resume_enabled && priv->serial_set &&
	    priv->registration_id_set) {
		priv->enabled = true;
		mod_delayed_work(system_wq, &priv->activation_work, 0);
	}
	mutex_unlock(&priv->lock);
	return 0;
}

static const struct airoha_xpon_backend_ops en7581_xgspon_xpon_ops = {
	.block_traffic = en7581_xgspon_xpon_block_traffic,
	.clear_session = en7581_xgspon_xpon_clear_session,
	.mask_irqs = en7581_xgspon_xpon_mask_irqs,
	.synchronize_irqs = en7581_xgspon_xpon_synchronize_irqs,
	.stop_datapath = en7581_xgspon_xpon_stop_datapath,
	.stop_mac = en7581_xgspon_xpon_stop_mac,
	.start_mac = en7581_xgspon_xpon_start_mac,
	.start_datapath = en7581_xgspon_xpon_start_datapath,
	.unmask_irqs = en7581_xgspon_xpon_unmask_irqs,
	.mode_committed = en7581_xgspon_xpon_mode_committed,
};

static void en7581_xgspon_xpon_unregister(void *data)
{
	airoha_xpon_backend_unregister(data);
}

static void en7581_xgspon_misc_deregister(void *data)
{
	struct en7581_xgspon *priv = data;

	if (!priv->omcc_registered)
		return;
	misc_deregister(&priv->omcc);
	priv->omcc_registered = false;
}

static int en7581_xgspon_probe(struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;
	struct en7581_xgspon *priv;
	struct device_node *bosa_np;
	struct device_node *controller_np;
	struct resource *resource;
	enum airoha_xpon_mode initial_mode;
	bool hardware_selected;
	int ret;

	if (!en7581_xgspon_register_layout_selftest())
		return dev_err_probe(dev, -EINVAL,
					     "XGS-PON key register layout self-test failed\n");
	if (!en7581_xgspon_tx_state_selftest())
		return dev_err_probe(dev, -EINVAL,
				     "XGS-PON TX state self-test failed\n");
	if (!en7581_xgspon_pm_extender_selftest())
		return dev_err_probe(dev, -EINVAL,
				     "XGS-PON PM extender self-test failed\n");
	ret = en7581_xgspon_board_mode(dev, &initial_mode, &hardware_selected);
	if (ret)
		return ret;

	priv = kzalloc(sizeof(*priv), GFP_KERNEL);
	if (!priv)
		return -ENOMEM;
	kref_init(&priv->refcount);
	priv->dev = dev;
	priv->hardware_selected = false;
	priv->hardware_state_valid = false;
	priv->active_mode = AIROHA_XPON_MODE_INVALID;
	do {
		priv->instance_generation = get_random_u64();
	} while (!priv->instance_generation);
	en7581_xgspon_reset_activation_state(priv);
	en7581_xgspon_pm_init(priv);
	mutex_init(&priv->lock);
	mutex_init(&priv->omcc_read_lock);
	spin_lock_init(&priv->omcc_rx_lock);
	INIT_LIST_HEAD(&priv->omcc_rx_queue);
	INIT_LIST_HEAD(&priv->omcc_ready_queue);
	init_waitqueue_head(&priv->omcc_rx_wait);
	INIT_WORK(&priv->omcc_rx_work, en7581_xgspon_omcc_rx_work);
	INIT_DELAYED_WORK(&priv->phy_recovery_work,
			  en7581_xgspon_phy_recovery_work);
	INIT_DELAYED_WORK(&priv->activation_work,
			  en7581_xgspon_activation_work);
	INIT_DELAYED_WORK(&priv->to1_work, en7581_xgspon_to1_work);
	INIT_DELAYED_WORK(&priv->key_tk4_work,
			  en7581_xgspon_key_tk4_work);
	INIT_DELAYED_WORK(&priv->key_tk5_work,
			  en7581_xgspon_key_tk5_work);
	INIT_DELAYED_WORK(&priv->pm_work, en7581_xgspon_pm_work);
	platform_set_drvdata(pdev, priv);
	ret = devm_add_action_or_reset(dev, en7581_xgspon_put, priv);
	if (ret)
		return ret;

	resource = platform_get_resource_byname(pdev, IORESOURCE_MEM, "mac");
	if (!resource)
		return dev_err_probe(dev, -EINVAL,
				     "missing XGS-PON MAC resource\n");
	if (resource_size(resource) != EN7581_XGSPON_MAC_SIZE)
		return dev_err_probe(dev, -EINVAL,
				     "unexpected XGS-PON MAC resource size %#llx\n",
				     (unsigned long long)resource_size(resource));
	priv->mac = devm_ioremap_resource(dev, resource);
	if (IS_ERR(priv->mac))
		return PTR_ERR(priv->mac);
	resource = platform_get_resource_byname(pdev, IORESOURCE_MEM, "phy-csr");
	if (!resource)
		return dev_err_probe(dev, -EINVAL,
				     "missing XGS-PON PHY CSR resource\n");
	if (resource_size(resource) != EN7581_XGSPON_PHY_CSR_SIZE)
		return dev_err_probe(dev, -EINVAL,
				     "unexpected XGS-PON PHY CSR resource size %#llx\n",
				     (unsigned long long)resource_size(resource));
	/* GPON and XGS-PON expose disjoint registers in this shared CSR window. */
	priv->phy_csr = devm_ioremap(dev, resource->start,
				     resource_size(resource));
	if (!priv->phy_csr)
		return -ENOMEM;

	priv->ethernet_np = of_parse_phandle(dev->of_node, "ethernet", 0);
	if (!priv->ethernet_np)
		return dev_err_probe(dev, -EINVAL, "missing ethernet phandle\n");
	ret = devm_add_action_or_reset(dev, en7581_xgspon_of_node_put,
				       priv->ethernet_np);
	if (ret)
		return ret;
	priv->pcs = fwnode_pcs_get(dev_fwnode(dev), 0);
	if (IS_ERR(priv->pcs))
		return dev_err_probe(dev, PTR_ERR(priv->pcs),
				     "XGS-PON PCS is not ready\n");
	bosa_np = of_parse_phandle(dev->of_node, "bosa-controller", 0);
	if (!bosa_np)
		return dev_err_probe(dev, -EINVAL,
				     "missing bosa-controller phandle\n");
	priv->bosa = airoha_en7572_get(bosa_np);
	of_node_put(bosa_np);
	if (IS_ERR(priv->bosa))
		return dev_err_probe(dev, PTR_ERR(priv->bosa),
				     "BOSA controller is not ready\n");
	ret = devm_add_action_or_reset(dev, en7581_xgspon_bosa_put, priv->bosa);
	if (ret)
		return ret;
	if (!airoha_en7572_is_ready(priv->bosa))
		return dev_err_probe(dev, -EPROBE_DEFER,
				     "XGS-PON BOSA calibration is not ready\n");
	if (hardware_selected &&
	    airoha_en7572_get_mode(priv->bosa) != initial_mode)
		return dev_err_probe(dev, -EINVAL,
				     "BOSA mode does not match 10G PON MAC\n");
	ret = airoha_en7572_set_tx_enabled(priv->bosa, false);
	if (ret)
		return dev_err_probe(dev, ret,
				     "failed to keep optical TX disabled\n");

	ret = devm_add_action_or_reset(dev, en7581_xgspon_detach_omcc, priv);
	if (ret)
		return ret;

	priv->mac_irq = platform_get_irq_byname(pdev, "mac");
	if (priv->mac_irq < 0)
		return priv->mac_irq;
	priv->phy_irq = platform_get_irq_byname(pdev, "phy");
	if (priv->phy_irq < 0)
		return priv->phy_irq;
	writel(0, priv->mac + EN7581_XGSPON_INT_ENABLE);
	writel(U32_MAX, priv->mac + EN7581_XGSPON_INT_STATUS);
	writel(0, priv->phy_csr + EN7581_XGSPON_PHY_INT_ENABLE);
	writel(U32_MAX, priv->phy_csr + EN7581_XGSPON_PHY_INT_STATUS);
	ret = devm_request_threaded_irq(dev, priv->mac_irq,
					en7581_xgspon_irq,
					en7581_xgspon_irq_thread,
					IRQF_ONESHOT | IRQF_SHARED,
					"airoha-xgspon-mac", priv);
	if (ret)
		return dev_err_probe(dev, ret,
				     "failed to request XGS-PON MAC IRQ\n");
	priv->mac_irq_requested = true;
	ret = devm_request_threaded_irq(dev, priv->phy_irq,
					en7581_xgspon_phy_irq,
					en7581_xgspon_phy_irq_thread,
					IRQF_ONESHOT | IRQF_SHARED,
					"airoha-xgspon-phy", priv);
	if (ret)
		return dev_err_probe(dev, ret,
				     "failed to request XGS-PON PHY IRQ\n");
	priv->phy_irq_requested = true;
	priv->xpon_irqs_masked = true;

	controller_np = of_parse_phandle(dev->of_node,
					 "airoha,xpon-controller", 0);
	if (!controller_np)
		return dev_err_probe(dev, -EINVAL,
				     "missing airoha,xpon-controller phandle\n");
	priv->xgpon_backend = airoha_xpon_backend_register(
		dev, controller_np, AIROHA_XPON_MODE_XGPON,
		&en7581_xgspon_xpon_ops, priv);
	if (IS_ERR(priv->xgpon_backend)) {
		of_node_put(controller_np);
		return dev_err_probe(dev, PTR_ERR(priv->xgpon_backend),
				     "XPON runtime owner is not ready\n");
	}
	ret = devm_add_action_or_reset(dev, en7581_xgspon_xpon_unregister,
				       priv->xgpon_backend);
	if (ret) {
		of_node_put(controller_np);
		return ret;
	}
	priv->xgspon_backend = airoha_xpon_backend_register(
		dev, controller_np, AIROHA_XPON_MODE_XGSPON,
		&en7581_xgspon_xpon_ops, priv);
	of_node_put(controller_np);
	if (IS_ERR(priv->xgspon_backend))
		return dev_err_probe(dev, PTR_ERR(priv->xgspon_backend),
				     "XPON runtime owner rejected XGS-PON backend\n");
	ret = devm_add_action_or_reset(dev, en7581_xgspon_xpon_unregister,
				       priv->xgspon_backend);
	if (ret)
		return ret;

	priv->omcc.minor = MISC_DYNAMIC_MINOR;
	priv->omcc.name = "airoha-xgs-omcc";
	priv->omcc.fops = &en7581_xgspon_omcc_fops;
	priv->omcc.parent = dev;
	ret = misc_register(&priv->omcc);
	if (ret)
		return dev_err_probe(dev, ret,
				     "failed to register secure OMCC endpoint\n");
	priv->omcc_registered = true;
	ret = devm_add_action_or_reset(dev, en7581_xgspon_misc_deregister,
				       priv);
	if (ret)
		return ret;

	ret = airoha_xpon_backend_ready(priv->xgpon_backend);
	if (!ret)
		ret = airoha_xpon_backend_ready(priv->xgspon_backend);
	if (ret)
		return dev_err_probe(dev, ret == -ENODEV ? -EPROBE_DEFER : ret,
				     "failed to claim selected XG/XGS-PON mode\n");

	if (en7581_xgspon_backend_active(priv)) {
		dev_warn(dev,
			 "%s backend active; upstream OMCC and optical TX remain gated\n",
			 airoha_xpon_mode_descriptor(priv->active_mode)->name);
	} else {
		dev_info(dev,
			 "XG-PON/XGS-PON backends ready for runtime activation\n");
	}
	return 0;
}

static void en7581_xgspon_remove(struct platform_device *pdev)
{
	struct en7581_xgspon *priv = platform_get_drvdata(pdev);

	if (priv->mac_irq_requested) {
		writel(0, priv->mac + EN7581_XGSPON_INT_ENABLE);
		writel(U32_MAX, priv->mac + EN7581_XGSPON_INT_STATUS);
	}
	if (priv->phy_irq_requested) {
		writel(0, priv->phy_csr + EN7581_XGSPON_PHY_INT_ENABLE);
		writel(U32_MAX, priv->phy_csr + EN7581_XGSPON_PHY_INT_STATUS);
	}
	mutex_lock(&priv->lock);
	spin_lock_irq(&priv->omcc_rx_lock);
	priv->removing = true;
	spin_unlock_irq(&priv->omcc_rx_lock);
	en7581_xgspon_advance_session_locked(priv);
	mutex_unlock(&priv->lock);
	cancel_delayed_work_sync(&priv->pm_work);
	cancel_delayed_work_sync(&priv->phy_recovery_work);
	cancel_delayed_work_sync(&priv->activation_work);
	cancel_delayed_work_sync(&priv->to1_work);
	cancel_delayed_work_sync(&priv->key_tk4_work);
	cancel_delayed_work_sync(&priv->key_tk5_work);
	en7581_xgspon_misc_deregister(priv);
	en7581_xgspon_detach_omcc(priv);
	mutex_lock(&priv->lock);
	if (priv->hardware_selected && en7581_xgspon_clear_session_keys(priv))
		dev_err(priv->dev,
			"failed to verify XGS-PON session cleanup during remove\n");
	if (priv->hardware_selected &&
	    en7581_xgspon_clear_hardware_identity(priv))
		dev_err(priv->dev,
			"failed to clear XGS-PON identity during remove\n");
	memzero_explicit(priv->serial, sizeof(priv->serial));
	memzero_explicit(priv->registration_id, sizeof(priv->registration_id));
	memzero_explicit(priv->registration_msk,
			 sizeof(priv->registration_msk));
	mutex_unlock(&priv->lock);
}

static void en7581_xgspon_shutdown(struct platform_device *pdev)
{
	struct en7581_xgspon *priv = platform_get_drvdata(pdev);

	mutex_lock(&priv->lock);
	spin_lock_irq(&priv->omcc_rx_lock);
	priv->removing = true;
	spin_unlock_irq(&priv->omcc_rx_lock);
	priv->enabled = false;
	priv->tx_armed = false;
	priv->tx_authorized = false;
	priv->optical_tx_enabled = false;
	en7581_xgspon_advance_session_locked(priv);
	mutex_unlock(&priv->lock);

	WRITE_ONCE(priv->xpon_irqs_masked, true);
	if (priv->mac_irq_requested) {
		writel(0, priv->mac + EN7581_XGSPON_INT_ENABLE);
		writel(U32_MAX, priv->mac + EN7581_XGSPON_INT_STATUS);
		synchronize_irq(priv->mac_irq);
	}
	if (priv->phy_irq_requested) {
		writel(0, priv->phy_csr + EN7581_XGSPON_PHY_INT_ENABLE);
		writel(U32_MAX, priv->phy_csr + EN7581_XGSPON_PHY_INT_STATUS);
		synchronize_irq(priv->phy_irq);
	}
	cancel_delayed_work_sync(&priv->pm_work);
	cancel_delayed_work_sync(&priv->phy_recovery_work);
	cancel_delayed_work_sync(&priv->activation_work);
	cancel_delayed_work_sync(&priv->to1_work);
	cancel_delayed_work_sync(&priv->key_tk4_work);
	cancel_delayed_work_sync(&priv->key_tk5_work);
	cancel_work_sync(&priv->omcc_rx_work);
	airoha_en7572_emergency_disable(priv->bosa);
}

static const struct of_device_id en7581_xgspon_of_match[] = {
	{ .compatible = "airoha,en7581-xgspon" },
	{ }
};
MODULE_DEVICE_TABLE(of, en7581_xgspon_of_match);

static struct platform_driver en7581_xgspon_driver = {
	.probe = en7581_xgspon_probe,
	.remove = en7581_xgspon_remove,
	.shutdown = en7581_xgspon_shutdown,
	.driver = {
		.name = "airoha-xgspon",
		.of_match_table = en7581_xgspon_of_match,
		.dev_groups = en7581_xgspon_groups,
	},
};
module_platform_driver(en7581_xgspon_driver);

MODULE_AUTHOR("OpenWrt contributors");
MODULE_DESCRIPTION("Airoha EN7581 fail-closed XGS-PON control endpoint");
MODULE_LICENSE("GPL");
