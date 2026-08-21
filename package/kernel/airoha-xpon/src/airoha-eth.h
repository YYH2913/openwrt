/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef _AIROHA_ETH_H_
#define _AIROHA_ETH_H_

#include <linux/skbuff.h>
#include <linux/types.h>

#include "airoha-xgs-service.h"

struct device_node;

#define AIROHA_GPON_QOS_CHANNELS 16
#define AIROHA_GPON_QOS_QUEUES 8

struct airoha_gpon_qos_config {
	u8 channel;
	u8 scheduler;
	u16 weights[AIROHA_GPON_QOS_QUEUES];
	u32 cir;
	u32 pir;
	u32 cbs;
	u32 pbs;
};

struct airoha_xgs_omcc_metadata {
	u16 xgem_id;
	bool no_mic;
};

struct airoha_xgs_omcc_ops {
	/* Called from NAPI and consumes skb in all cases. */
	void (*receive)(void *context, struct sk_buff *skb,
			const struct airoha_xgs_omcc_metadata *metadata);
};

/* XGS-PON OMCC TX metadata. The Ethernet driver owns the skb/ring. */
struct airoha_xgs_omcc_tx_metadata {
	u16 xgem_id;
	u8 channel;
	u8 nboq;
	u8 mic_idx;
};

struct airoha_epon_oam_metadata {
	u8 llid_index;
	u16 llid;
	bool no_mic;
};

struct airoha_epon_oam_ops {
	/* Called from NAPI and consumes skb in all cases. */
	void (*receive)(void *context, struct sk_buff *skb,
			const struct airoha_epon_oam_metadata *metadata);
};

struct airoha_epon_oam_tx_metadata {
	u8 llid_index;
};

int airoha_gpon_set_gem(struct device_node *np, u16 gem, u8 channel,
			bool valid);
int airoha_gpon_set_gems(struct device_node *np, const u16 *gems,
			 const u8 *channels, const u8 *directions,
			 unsigned int count);
int airoha_gpon_set_qos(struct device_node *np,
			const struct airoha_gpon_qos_config *cfg,
			unsigned int count);
int airoha_gpon_get_qos(struct device_node *np,
			struct airoha_gpon_qos_config *cfg,
			unsigned int *count);
int airoha_gpon_set_omcc(struct device_node *np, u16 gem, bool valid);
int airoha_xgs_omcc_register(struct device_node *np,
			     const struct airoha_xgs_omcc_ops *ops,
			     void *context);
void airoha_xgs_omcc_unregister(struct device_node *np, void *context);
int airoha_xgs_omcc_transmit(struct device_node *np, const u8 *data,
			     size_t len,
			     const struct airoha_xgs_omcc_tx_metadata *metadata);
bool airoha_xgs_omcc_tx_available(struct device_node *np);
int airoha_xgs_qdma_service_apply(struct device_node *np,
				  const struct airoha_xgs_service_config *config);
int airoha_xgs_qdma_service_clear(struct device_node *np);
int airoha_epon_qdma_llids_apply(struct device_node *np, u32 llid_mask,
				 u32 report_fec_mask);
int airoha_epon_qdma_llids_clear(struct device_node *np);
int airoha_xpon_qdma_channels_retire(struct device_node *np,
				      u32 channel_mask);
int airoha_epon_loopback_set(struct device_node *np, u8 llid_index,
			     bool enabled);
int airoha_epon_oam_register(struct device_node *np,
			     const struct airoha_epon_oam_ops *ops,
			     void *context);
void airoha_epon_oam_unregister(struct device_node *np, void *context);
int airoha_epon_oam_transmit(
	struct device_node *np, const u8 *data, size_t len,
	const struct airoha_epon_oam_tx_metadata *metadata);
bool airoha_epon_oam_tx_available(struct device_node *np);

#endif
