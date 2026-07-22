/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef __LINUX_SOC_QCOM_PPE_DS_H
#define __LINUX_SOC_QCOM_PPE_DS_H

#include <linux/dma-mapping.h>
#include <linux/types.h>

struct device;
struct net_device;
struct qcom_ppe_ds_node;

/**
 * struct qcom_ppe_ds_rxfill - buffer supplied to an EDMA PPE2TCL fill ring
 * @dma: DMA address of the WLAN-owned buffer
 * @cookie: opaque value returned when the buffer is completed
 */
struct qcom_ppe_ds_rxfill {
	dma_addr_t dma;
	u64 cookie;
};

/**
 * struct qcom_ppe_ds_txcmpl - completed REO2PPE buffer
 * @cookie: opaque value copied from the WLAN REO2PPE descriptor
 */
struct qcom_ppe_ds_txcmpl {
	u64 cookie;
};

/**
 * struct qcom_ppe_ds_ops - WLAN callbacks used by the PPE direct-switch core
 * @ppe2tcl_produce: publish the latest PPE2TCL producer index to WLAN
 * @ppe2tcl_refill: supply WLAN buffers to the EDMA fill ring
 * @ppe2tcl_release: release one unused WLAN transmit buffer
 * @reo2ppe_complete: release WLAN RX buffers consumed by PPE
 * @ring_reset: reset WLAN ring pointers to the EDMA-selected empty positions
 * @quiesce: stop WLAN from accessing the direct-switch rings
 */
struct qcom_ppe_ds_ops {
	void (*ppe2tcl_produce)(struct qcom_ppe_ds_node *node, u16 prod);
	u32 (*ppe2tcl_refill)(struct qcom_ppe_ds_node *node, u32 count,
			       u32 size, u32 headroom,
			       struct qcom_ppe_ds_rxfill *buffers);
	void (*ppe2tcl_release)(struct qcom_ppe_ds_node *node, u64 cookie);
	void (*reo2ppe_complete)(struct qcom_ppe_ds_node *node,
				 struct qcom_ppe_ds_txcmpl *buffers,
				 u32 count);
	int (*ring_reset)(struct qcom_ppe_ds_node *node, u16 ppe2tcl,
			  u16 reo2ppe);
	void (*quiesce)(struct qcom_ppe_ds_node *node);
};

/**
 * struct qcom_ppe_ds_reg - WLAN rings registered with EDMA
 * @ppe2tcl_dma: DMA base of the WLAN PPE2TCL destination ring
 * @reo2ppe_dma: DMA base of the WLAN REO2PPE source ring
 * @ppe2tcl_count: number of PPE2TCL descriptors
 * @reo2ppe_count: number of REO2PPE descriptors
 * @rxfill_count: number of EDMA fill descriptors
 * @txcmpl_count: number of EDMA completion descriptors
 * @buffer_size: size of each WLAN transmit buffer
 * @headroom: headroom reserved in each WLAN transmit buffer
 * @rxfill_low_threshold: refill interrupt threshold
 * @rxfill_budget: refill NAPI budget
 * @txcmpl_budget: completion NAPI budget
 */
struct qcom_ppe_ds_reg {
	dma_addr_t ppe2tcl_dma;
	dma_addr_t reo2ppe_dma;
	u32 ppe2tcl_count;
	u32 reo2ppe_count;
	u32 rxfill_count;
	u32 txcmpl_count;
	u32 buffer_size;
	u32 headroom;
	u32 rxfill_low_threshold;
	u32 rxfill_budget;
	u32 txcmpl_budget;
};

struct qcom_ppe_ds_node *
qcom_ppe_ds_node_alloc(struct device *client,
		       const struct qcom_ppe_ds_ops *ops, size_t priv_size);
struct qcom_ppe_ds_node *
qcom_ppe_ds_node_alloc_id(struct device *client,
			  const struct qcom_ppe_ds_ops *ops, int id,
			  size_t priv_size);
void *qcom_ppe_ds_priv(struct qcom_ppe_ds_node *node);
int qcom_ppe_ds_node_id(struct qcom_ppe_ds_node *node);
int qcom_ppe_ds_queue_start(struct qcom_ppe_ds_node *node);

int qcom_ppe_ds_register(struct qcom_ppe_ds_node *node,
			 const struct qcom_ppe_ds_reg *reg);
int qcom_ppe_ds_start(struct qcom_ppe_ds_node *node);
void qcom_ppe_ds_stop(struct qcom_ppe_ds_node *node);
void qcom_ppe_ds_node_free(struct qcom_ppe_ds_node *node);

void qcom_ppe_ds_ppe2tcl_consume(struct qcom_ppe_ds_node *node, u16 cons);
u16 qcom_ppe_ds_ppe2tcl_prod(struct qcom_ppe_ds_node *node);
void qcom_ppe_ds_reo2ppe_produce(struct qcom_ppe_ds_node *node, u16 prod);
u16 qcom_ppe_ds_reo2ppe_cons(struct qcom_ppe_ds_node *node);

int qcom_ppe_ds_vp_alloc(struct qcom_ppe_ds_node *node,
			 struct net_device *dev);
void qcom_ppe_ds_vp_free(struct qcom_ppe_ds_node *node, int vp);

#endif
