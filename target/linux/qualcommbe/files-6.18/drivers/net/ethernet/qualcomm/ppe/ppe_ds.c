// SPDX-License-Identifier: GPL-2.0-only
/* Qualcomm IPQ9574 PPE direct-switch support. */

#include <linux/bitfield.h>
#include <linux/debugfs.h>
#include <linux/dma-mapping.h>
#include <linux/err.h>
#include <linux/iopoll.h>
#include <linux/interrupt.h>
#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/mutex.h>
#include <linux/netdevice.h>
#include <linux/of.h>
#include <linux/of_irq.h>
#include <linux/regmap.h>
#include <linux/seq_file.h>
#include <linux/slab.h>
#include <linux/soc/qcom/ppe-ds.h>

#include "edma.h"
#include "ppe.h"
#include "ppe_api.h"
#include "ppe_ds.h"
#include "ppe_regs.h"
#include "ppe_vp.h"

#define PPE_DS_NODES			3
#define PPE_DS_QUEUES_PER_NODE		8
#define PPE_DS_QUEUE_BASE		32
#define PPE_DS_RING_BASE		1
#define PPE_DS_RX_HEADROOM		130
#define PPE_DS_FC_XOFF			32
#define PPE_DS_FC_XON			64
#define PPE_DS_DEFAULT_BUDGET		256
#define PPE_DS_QID2RID(q)		(0xb9000 + 4 * (q))
#define PPE_DS_RXFILL_INT		BIT(0)
#define PPE_DS_RING_DISABLE		BIT(0)
#define PPE_DS_POLL_US			10
#define PPE_DS_POLL_TIMEOUT_US		100000
#define PPE_DS_PPE2TCL_TIMER_US		3
#define PPE_DS_DMA_HIGH_MASK		GENMASK(7, 0)
#define PPE_DS_RXDESC_WR_PH		BIT(10)

enum ppe_ds_irq {
	PPE_DS_IRQ_TXCMPL,
	PPE_DS_IRQ_PPE2TCL,
	PPE_DS_IRQ_RXFILL,
	PPE_DS_IRQ_COUNT,
};

enum ppe_ds_state {
	PPE_DS_FREE,
	PPE_DS_ALLOCATED,
	PPE_DS_REGISTERED,
	PPE_DS_STARTED,
};

struct ppe_ds_stats {
	atomic64_t ppe2tcl_irqs;
	atomic64_t ppe2tcl_updates;
	atomic64_t refill_irqs;
	atomic64_t refill_buffers;
	atomic64_t refill_short;
	atomic64_t reo2ppe_irqs;
	atomic64_t reo2ppe_buffers;
	atomic64_t vp_refs;
	atomic64_t vp_attach;
	atomic64_t vp_detach;
	atomic64_t vp_fail;
	atomic64_t starts;
	atomic64_t stops;
};

struct ppe_ds {
	struct ppe_device *ppe_dev;
	struct mutex lock;
	struct qcom_ppe_ds_node *nodes[PPE_DS_NODES];
	int irqs[PPE_DS_NODES][PPE_DS_IRQ_COUNT];
	struct dentry *debugfs;
	bool stopping;
};

struct qcom_ppe_ds_node {
	struct ppe_ds *ds;
	struct device *client;
	const struct qcom_ppe_ds_ops *ops;
	struct mutex lock;
	enum ppe_ds_state state;
	int id;
	u32 queue_start;
	u32 queue_count;

	struct qcom_ppe_ds_reg reg;
	struct edma_rxfill_ring rxfill;
	struct edma_rxdesc_ring ppe2tcl;
	struct edma_txdesc_ring reo2ppe;
	struct edma_txcmpl_ring txcmpl;
	struct napi_struct rxfill_napi;
	struct qcom_ppe_ds_rxfill *rxfill_bufs;
	struct qcom_ppe_ds_txcmpl *txcmpl_bufs;
	struct net_device *napi_dev;
	int rxfill_irq;
	int ppe2tcl_irq;
	int txcmpl_irq;
	char rxfill_irq_name[32];
	char ppe2tcl_irq_name[32];
	char txcmpl_irq_name[32];
	bool rxfill_irq_requested;
	bool ppe2tcl_irq_requested;
	bool txcmpl_irq_requested;
	bool napi_added;
	bool napi_enabled;
	struct ppe_ds_stats stats;
	u8 priv[] __aligned(NETDEV_ALIGN);
};

static DEFINE_MUTEX(ppe_ds_global_lock);
static struct ppe_ds *ppe_ds_global;

static inline struct regmap *ppe_ds_regmap(struct qcom_ppe_ds_node *node)
{
	return node->ds->ppe_dev->regmap;
}

static inline u32 ppe_ds_reg(u32 reg)
{
	return EDMA_BASE_OFFSET + reg;
}

static bool ppe_ds_ring_valid(u32 count)
{
	return count >= 64 && count <= U16_MAX && is_power_of_2(count);
}

static void ppe_ds_rxfill_write(struct qcom_ppe_ds_node *node, u32 count)
{
	struct edma_rxfill_ring *ring = &node->rxfill;
	u32 i, prod = ring->prod_idx;

	for (i = 0; i < count; i++) {
		struct qcom_ppe_ds_rxfill *buf = &node->rxfill_bufs[i];
		struct edma_rxfill_desc *desc = EDMA_RXFILL_DESC(ring, prod);

		desc->word0 = lower_32_bits(buf->dma);
		desc->word1 = (node->reg.buffer_size - node->reg.headroom) <<
				 EDMA_RXFILL_BUF_SIZE_SHIFT;
		desc->word2 = lower_32_bits(buf->cookie);
		desc->word3 = upper_32_bits(buf->cookie);
		EDMA_RXFILL_ENDIAN_SET(desc);
		prod = (prod + 1) & (ring->count - 1);
	}

	if (!count)
		return;

	dma_wmb();
	regmap_write(ppe_ds_regmap(node),
		     ppe_ds_reg(EDMA_REG_RXFILL_PROD_IDX(ring->ring_id)), prod);
	ring->prod_idx = prod;
}

static u32 ppe_ds_refill(struct qcom_ppe_ds_node *node, u32 request)
{
	u32 filled;

	if (!request)
		return 0;

	filled = node->ops->ppe2tcl_refill(node, request,
					       node->reg.buffer_size,
					       node->reg.headroom,
					       node->rxfill_bufs);
	if (WARN_ON_ONCE(filled > request))
		filled = request;

	ppe_ds_rxfill_write(node, filled);
	atomic64_add(filled, &node->stats.refill_buffers);
	if (filled != request)
		atomic64_inc(&node->stats.refill_short);

	return filled;
}

static int ppe_ds_ppe2tcl_poll(struct napi_struct *napi, int budget)
{
	struct edma_rxdesc_ring *ring =
		container_of(napi, struct edma_rxdesc_ring, napi);
	struct qcom_ppe_ds_node *node =
		container_of(ring, struct qcom_ppe_ds_node, ppe2tcl);
	u32 prod, status;

	regmap_read(ppe_ds_regmap(node),
		    ppe_ds_reg(EDMA_REG_RXDESC_PROD_IDX(ring->ring_id)), &prod);
	prod &= EDMA_RXDESC_PROD_IDX_MASK;
	node->ops->ppe2tcl_produce(node, prod);
	atomic64_inc(&node->stats.ppe2tcl_updates);

	/* INT_STAT is clear-on-read. Keep NAPI scheduled if the ring advanced
	 * after the producer-index snapshot.
	 */
	regmap_read(ppe_ds_regmap(node),
		    ppe_ds_reg(EDMA_REG_RXDESC_INT_STAT(ring->ring_id)), &status);
	if (status & EDMA_RXDESC_RING_INT_STATUS_MASK)
		return budget;

	if (napi_complete_done(napi, 0) &&
	    READ_ONCE(node->state) == PPE_DS_STARTED)
		regmap_write(ppe_ds_regmap(node),
			     ppe_ds_reg(EDMA_REG_RXDESC_INT_MASK(ring->ring_id)),
			     EDMA_RXDESC_INT_MASK_PKT_INT);

	return 0;
}

static int ppe_ds_rxfill_poll(struct napi_struct *napi, int budget)
{
	struct qcom_ppe_ds_node *node =
		container_of(napi, struct qcom_ppe_ds_node, rxfill_napi);
	struct edma_rxfill_ring *ring = &node->rxfill;
	u32 cons, done = 0, request, status;

	do {
		regmap_read(ppe_ds_regmap(node),
			    ppe_ds_reg(EDMA_REG_RXFILL_CONS_IDX(ring->ring_id)),
			    &cons);
		cons &= EDMA_RXFILL_CONS_IDX_MASK;
		request = (cons - ring->prod_idx + ring->count - 1) &
			  (ring->count - 1);
		request = min_t(u32, request, budget - done);
		ppe_ds_refill(node, request);
		done += request;
		if (done >= budget)
			return done;

		/* Refill again when the consumer advanced after our snapshot. */
		regmap_read(ppe_ds_regmap(node),
			    ppe_ds_reg(EDMA_REG_RXFILL_INT_STAT(ring->ring_id)),
			    &status);
		if ((status & PPE_DS_RXFILL_INT) && !request)
			return budget;
	} while (status & PPE_DS_RXFILL_INT);

	if (napi_complete_done(napi, done) &&
	    READ_ONCE(node->state) == PPE_DS_STARTED)
		regmap_write(ppe_ds_regmap(node),
			     ppe_ds_reg(EDMA_REG_RXFILL_INT_MASK(ring->ring_id)),
			     PPE_DS_RXFILL_INT);

	return done;
}

static u32 ppe_ds_txcmpl_reap(struct qcom_ppe_ds_node *node, u32 budget)
{
	struct edma_txcmpl_ring *ring = &node->txcmpl;
	u32 cons = ring->cons_idx, prod, count, i;

	regmap_read(ppe_ds_regmap(node),
		    ppe_ds_reg(EDMA_REG_TXCMPL_PROD_IDX(ring->id)), &prod);
	prod &= EDMA_TXCMPL_PROD_IDX_MASK;
	count = min_t(u32, EDMA_DESC_AVAIL_COUNT(prod, cons, ring->count),
		      budget);
	if (count)
		dma_rmb();

	for (i = 0; i < count; i++) {
		struct edma_txcmpl_desc *desc = EDMA_TXCMPL_DESC(ring, cons);

		node->txcmpl_bufs[i].cookie = EDMA_TXCMPL_OPAQUE_GET(desc);
		cons = (cons + 1) & (ring->count - 1);
	}

	if (!count)
		return 0;

	ring->cons_idx = cons;
	regmap_write(ppe_ds_regmap(node),
		     ppe_ds_reg(EDMA_REG_TXCMPL_CONS_IDX(ring->id)), cons);
	node->ops->reo2ppe_complete(node, node->txcmpl_bufs, count);
	atomic64_add(count, &node->stats.reo2ppe_buffers);

	return count;
}

static int ppe_ds_txcmpl_poll(struct napi_struct *napi, int budget)
{
	struct edma_txcmpl_ring *ring =
		container_of(napi, struct edma_txcmpl_ring, napi);
	struct qcom_ppe_ds_node *node =
		container_of(ring, struct qcom_ppe_ds_node, txcmpl);
	u32 done = 0, status;

	do {
		done += ppe_ds_txcmpl_reap(node, budget - done);
		if (done >= budget)
			return done;

		/* TX_INT_STAT is clear-on-read. Reap again when an event raced
		 * with the producer-index snapshot before unmasking the IRQ.
		 */
		regmap_read(ppe_ds_regmap(node),
			    ppe_ds_reg(EDMA_REG_TX_INT_STAT(ring->id)), &status);
	} while (status & EDMA_TXCMPL_RING_INT_STATUS_MASK);

	if (napi_complete_done(napi, done) &&
	    READ_ONCE(node->state) == PPE_DS_STARTED)
		regmap_write(ppe_ds_regmap(node),
			     ppe_ds_reg(EDMA_REG_TX_INT_MASK(ring->id)),
			     EDMA_TX_INT_MASK_PKT_INT);

	return done;
}

static irqreturn_t ppe_ds_ppe2tcl_irq(int irq, void *data)
{
	struct qcom_ppe_ds_node *node = data;

	regmap_write(ppe_ds_regmap(node),
		     ppe_ds_reg(EDMA_REG_RXDESC_INT_MASK(node->ppe2tcl.ring_id)), 0);
	atomic64_inc(&node->stats.ppe2tcl_irqs);
	napi_schedule_irqoff(&node->ppe2tcl.napi);

	return IRQ_HANDLED;
}

static irqreturn_t ppe_ds_rxfill_irq(int irq, void *data)
{
	struct qcom_ppe_ds_node *node = data;

	regmap_write(ppe_ds_regmap(node),
		     ppe_ds_reg(EDMA_REG_RXFILL_INT_MASK(node->rxfill.ring_id)), 0);
	atomic64_inc(&node->stats.refill_irqs);
	napi_schedule_irqoff(&node->rxfill_napi);

	return IRQ_HANDLED;
}

static irqreturn_t ppe_ds_txcmpl_irq(int irq, void *data)
{
	struct qcom_ppe_ds_node *node = data;

	regmap_write(ppe_ds_regmap(node),
		     ppe_ds_reg(EDMA_REG_TX_INT_MASK(node->txcmpl.id)), 0);
	atomic64_inc(&node->stats.reo2ppe_irqs);
	napi_schedule_irqoff(&node->txcmpl.napi);

	return IRQ_HANDLED;
}

static int ppe_ds_irqs_request(struct qcom_ppe_ds_node *node)
{
	int ret;

	snprintf(node->txcmpl_irq_name, sizeof(node->txcmpl_irq_name),
		 "ppe-ds%d-txcmpl", node->id);
	ret = request_irq(node->txcmpl_irq, ppe_ds_txcmpl_irq, IRQF_SHARED,
			  node->txcmpl_irq_name, node);
	if (ret)
		return ret;
	node->txcmpl_irq_requested = true;

	snprintf(node->ppe2tcl_irq_name, sizeof(node->ppe2tcl_irq_name),
		 "ppe-ds%d-ppe2tcl", node->id);
	ret = request_irq(node->ppe2tcl_irq, ppe_ds_ppe2tcl_irq, IRQF_SHARED,
			  node->ppe2tcl_irq_name, node);
	if (ret)
		goto err_ppe2tcl;
	node->ppe2tcl_irq_requested = true;

	snprintf(node->rxfill_irq_name, sizeof(node->rxfill_irq_name),
		 "ppe-ds%d-rxfill", node->id);
	ret = request_irq(node->rxfill_irq, ppe_ds_rxfill_irq, IRQF_SHARED,
			  node->rxfill_irq_name, node);
	if (ret)
		goto err_rxfill;
	node->rxfill_irq_requested = true;

	return 0;

err_rxfill:
	free_irq(node->ppe2tcl_irq, node);
	node->ppe2tcl_irq_requested = false;
err_ppe2tcl:
	free_irq(node->txcmpl_irq, node);
	node->txcmpl_irq_requested = false;
	return ret;
}

static void ppe_ds_irqs_free(struct qcom_ppe_ds_node *node)
{
	if (node->rxfill_irq_requested) {
		free_irq(node->rxfill_irq, node);
		node->rxfill_irq_requested = false;
	}
	if (node->ppe2tcl_irq_requested) {
		free_irq(node->ppe2tcl_irq, node);
		node->ppe2tcl_irq_requested = false;
	}
	if (node->txcmpl_irq_requested) {
		free_irq(node->txcmpl_irq, node);
		node->txcmpl_irq_requested = false;
	}
}

static void ppe_ds_napi_add(struct qcom_ppe_ds_node *node)
{
	u32 rxfill_budget = node->reg.rxfill_budget ?: PPE_DS_DEFAULT_BUDGET;
	u32 txcmpl_budget = node->reg.txcmpl_budget ?: PPE_DS_DEFAULT_BUDGET;

	netif_napi_add_weight(node->napi_dev, &node->ppe2tcl.napi,
			      ppe_ds_ppe2tcl_poll, 1);
	netif_napi_add_weight(node->napi_dev, &node->rxfill_napi,
			      ppe_ds_rxfill_poll, rxfill_budget);
	netif_napi_add_weight(node->napi_dev, &node->txcmpl.napi,
			      ppe_ds_txcmpl_poll, txcmpl_budget);
	node->napi_added = true;
}

static void ppe_ds_napi_enable(struct qcom_ppe_ds_node *node)
{
	if (node->napi_enabled)
		return;
	napi_enable(&node->ppe2tcl.napi);
	napi_enable(&node->rxfill_napi);
	napi_enable(&node->txcmpl.napi);
	node->napi_enabled = true;
}

static void ppe_ds_napi_disable(struct qcom_ppe_ds_node *node)
{
	if (!node->napi_enabled)
		return;
	napi_disable(&node->ppe2tcl.napi);
	napi_disable(&node->rxfill_napi);
	napi_disable(&node->txcmpl.napi);
	node->napi_enabled = false;
}

static void ppe_ds_napi_del(struct qcom_ppe_ds_node *node)
{
	if (!node->napi_added)
		return;
	netif_napi_del(&node->ppe2tcl.napi);
	netif_napi_del(&node->rxfill_napi);
	netif_napi_del(&node->txcmpl.napi);
	node->napi_added = false;
}

static int ppe_ds_map_rings(struct qcom_ppe_ds_node *node)
{
	struct regmap *regmap = ppe_ds_regmap(node);
	u32 ring = node->id + PPE_DS_RING_BASE;
	u32 reg, mask, val, queue;
	int queues[PPE_DS_QUEUES_PER_NODE];
	int i, ret;

	reg = ppe_ds_reg(EDMA_REG_RXDESC2FILL_MAP_0_ADDR);
	mask = EDMA_RXDESC2FILL_MAP_RXDESC_MASK << (ring * 3);
	val = node->rxfill.ring_id << (ring * 3);
	ret = regmap_update_bits(regmap, reg, mask, val);
	if (ret)
		return ret;

	reg = ppe_ds_reg(EDMA_REG_TXDESC2CMPL_MAP_0_ADDR);
	mask = EDMA_TXDESC2CMPL_MAP_TXDESC_MASK << (ring * 5);
	val = node->txcmpl.id << (ring * 5);
	ret = regmap_update_bits(regmap, reg, mask, val);
	if (ret)
		return ret;

	for (i = 0; i < node->queue_count; i++) {
		queue = node->queue_start + i;
		reg = ppe_ds_reg(PPE_DS_QID2RID(queue / 4));
		mask = GENMASK((queue % 4) * 8 + 7, (queue % 4) * 8);
		ret = regmap_update_bits(regmap, reg, mask,
					 ring << ((queue % 4) * 8));
		if (ret)
			return ret;
		queues[i] = queue;
	}

	return ppe_edma_ring_to_queues_config(node->ds->ppe_dev, ring,
					       node->queue_count, queues);
}

static void ppe_ds_unmap_rings(struct qcom_ppe_ds_node *node)
{
	struct regmap *regmap = ppe_ds_regmap(node);
	u32 ring = node->id + PPE_DS_RING_BASE;
	u32 reg, mask, queue;
	u32 empty[10] = {};
	int i;

	reg = ppe_ds_reg(EDMA_REG_RXDESC2FILL_MAP_0_ADDR);
	mask = EDMA_RXDESC2FILL_MAP_RXDESC_MASK << (ring * 3);
	regmap_update_bits(regmap, reg, mask, 0);
	reg = ppe_ds_reg(EDMA_REG_TXDESC2CMPL_MAP_0_ADDR);
	mask = EDMA_TXDESC2CMPL_MAP_TXDESC_MASK << (ring * 5);
	regmap_update_bits(regmap, reg, mask, 0);

	for (i = 0; i < node->queue_count; i++) {
		queue = node->queue_start + i;
		reg = ppe_ds_reg(PPE_DS_QID2RID(queue / 4));
		mask = GENMASK((queue % 4) * 8 + 7, (queue % 4) * 8);
		regmap_update_bits(regmap, reg, mask, 0);
	}
	ppe_ring_queue_map_set(node->ds->ppe_dev, ring, empty);
}

static int ppe_ds_configure(struct qcom_ppe_ds_node *node)
{
	struct regmap *regmap = ppe_ds_regmap(node);
	u32 data;
	int ret;

	ret = ppe_ds_map_rings(node);
	if (ret)
		return ret;

	regmap_write(regmap, ppe_ds_reg(EDMA_REG_RXFILL_BA(node->rxfill.ring_id)),
		     lower_32_bits(node->rxfill.dma));
	regmap_write(regmap,
		     ppe_ds_reg(EDMA_REG_RXFILL_BA_HIGH(node->rxfill.ring_id)),
		     upper_32_bits(node->rxfill.dma) & PPE_DS_DMA_HIGH_MASK);
	regmap_write(regmap,
		     ppe_ds_reg(EDMA_REG_RXFILL_RING_SIZE(node->rxfill.ring_id)),
		     node->rxfill.count & EDMA_RXFILL_RING_SIZE_MASK);
	regmap_write(regmap,
		     ppe_ds_reg(EDMA_REG_RXFILL_FC_THRE(node->rxfill.ring_id)),
		     FIELD_PREP(GENMASK(10, 0), PPE_DS_FC_XOFF) |
		     FIELD_PREP(GENMASK(22, 12), PPE_DS_FC_XON));

	regmap_write(regmap,
		     ppe_ds_reg(EDMA_REG_RXDESC_BA(node->ppe2tcl.ring_id)),
		     lower_32_bits(node->ppe2tcl.pdma));
	regmap_write(regmap,
		     ppe_ds_reg(EDMA_REG_RXDESC_PREHEADER_BA(node->ppe2tcl.ring_id)),
		     lower_32_bits(node->ppe2tcl.sdma));
	regmap_write(regmap,
		     ppe_ds_reg(EDMA_REG_RXDESC_BA_HIGH(node->ppe2tcl.ring_id)),
		     upper_32_bits(node->ppe2tcl.pdma) & PPE_DS_DMA_HIGH_MASK);
	regmap_write(regmap,
		     ppe_ds_reg(EDMA_REG_RXDESC_PREHEADER_BA_HIGH(node->ppe2tcl.ring_id)),
		     upper_32_bits(node->ppe2tcl.sdma) & PPE_DS_DMA_HIGH_MASK);
	regmap_write(regmap,
		     ppe_ds_reg(EDMA_REG_RXDESC_RING_SIZE(node->ppe2tcl.ring_id)),
		     node->ppe2tcl.count & EDMA_RXDESC_RING_SIZE_MASK);
	regmap_clear_bits(regmap,
			  ppe_ds_reg(EDMA_REG_RXDESC_CTRL(node->ppe2tcl.ring_id)),
			  PPE_DS_RXDESC_WR_PH);
	regmap_write(regmap,
		     ppe_ds_reg(EDMA_REG_RXDESC_FC_THRE(node->ppe2tcl.ring_id)),
		     FIELD_PREP(GENMASK(10, 0), PPE_DS_FC_XOFF) |
		     FIELD_PREP(GENMASK(22, 12), PPE_DS_FC_XON));
	data = EDMA_MICROSEC_TO_TIMER_UNIT(PPE_DS_PPE2TCL_TIMER_US,
					   node->ds->ppe_dev->clk_rate / MHZ);
	regmap_write(regmap,
		     ppe_ds_reg(EDMA_REG_RX_MOD_TIMER(node->ppe2tcl.ring_id)), data);
	regmap_write(regmap,
		     ppe_ds_reg(EDMA_REG_RX_INT_CTRL(node->ppe2tcl.ring_id)),
		     EDMA_RX_NE_INT_EN);

	regmap_write(regmap, ppe_ds_reg(EDMA_REG_TXDESC_BA(node->reo2ppe.id)),
		     lower_32_bits(node->reo2ppe.pdma));
	regmap_write(regmap, ppe_ds_reg(EDMA_REG_TXDESC_BA2(node->reo2ppe.id)),
		     lower_32_bits(node->reo2ppe.sdma));
	regmap_write(regmap, ppe_ds_reg(EDMA_REG_TXDESC_BA_HIGH(node->reo2ppe.id)),
		     upper_32_bits(node->reo2ppe.pdma) & PPE_DS_DMA_HIGH_MASK);
	regmap_write(regmap, ppe_ds_reg(EDMA_REG_TXDESC_BA2_HIGH(node->reo2ppe.id)),
		     upper_32_bits(node->reo2ppe.sdma) & PPE_DS_DMA_HIGH_MASK);
	regmap_write(regmap,
		     ppe_ds_reg(EDMA_REG_TXDESC_RING_SIZE(node->reo2ppe.id)),
		     node->reo2ppe.count & EDMA_TXDESC_RING_SIZE_MASK);
	regmap_write(regmap, ppe_ds_reg(EDMA_REG_TXDESC_PROD_IDX(node->reo2ppe.id)),
		     0);

	regmap_write(regmap, ppe_ds_reg(EDMA_REG_TXCMPL_BA(node->txcmpl.id)),
		     lower_32_bits(node->txcmpl.dma));
	regmap_write(regmap, ppe_ds_reg(EDMA_REG_TXCMPL_BA_HIGH(node->txcmpl.id)),
		     upper_32_bits(node->txcmpl.dma) & PPE_DS_DMA_HIGH_MASK);
	regmap_write(regmap,
		     ppe_ds_reg(EDMA_REG_TXCMPL_RING_SIZE(node->txcmpl.id)),
		     node->txcmpl.count & EDMA_TXDESC_RING_SIZE_MASK);
	regmap_write(regmap, ppe_ds_reg(EDMA_REG_TXCMPL_CTRL(node->txcmpl.id)),
		     EDMA_TXCMPL_RETMODE_OPAQUE);
	data = EDMA_MICROSEC_TO_TIMER_UNIT(250,
					   node->ds->ppe_dev->clk_rate / MHZ);
	regmap_write(regmap, ppe_ds_reg(EDMA_REG_TX_MOD_TIMER(node->txcmpl.id)),
		     data);
	regmap_write(regmap, ppe_ds_reg(EDMA_REG_TX_INT_CTRL(node->txcmpl.id)),
		     EDMA_TX_NE_INT_EN);
	regmap_read(regmap, ppe_ds_reg(EDMA_REG_TXCMPL_PROD_IDX(node->txcmpl.id)),
		    &data);
	node->txcmpl.cons_idx = data & EDMA_TXCMPL_PROD_IDX_MASK;
	regmap_write(regmap,
		     ppe_ds_reg(EDMA_REG_TXCMPL_CONS_IDX(node->txcmpl.id)),
		     node->txcmpl.cons_idx);

	regmap_read(regmap,
		    ppe_ds_reg(EDMA_REG_RXFILL_CONS_IDX(node->rxfill.ring_id)),
		    &data);
	node->rxfill.prod_idx = data & EDMA_RXFILL_CONS_IDX_MASK;
	regmap_write(regmap,
		     ppe_ds_reg(EDMA_REG_RXFILL_PROD_IDX(node->rxfill.ring_id)),
		     node->rxfill.prod_idx);

	return 0;
}

static int ppe_ds_resources_alloc(struct qcom_ppe_ds_node *node)
{
	struct device *dev = node->ds->ppe_dev->dev;
	size_t size;

	size = sizeof(*node->rxfill.desc) * node->rxfill.count;
	node->rxfill.desc = dma_alloc_coherent(dev, size, &node->rxfill.dma,
					       GFP_KERNEL | __GFP_ZERO);
	if (!node->rxfill.desc)
		return -ENOMEM;

	size = sizeof(*node->ppe2tcl.sdesc) * node->ppe2tcl.count;
	node->ppe2tcl.sdesc = dma_alloc_coherent(dev, size,
						 &node->ppe2tcl.sdma,
						 GFP_KERNEL | __GFP_ZERO);
	if (!node->ppe2tcl.sdesc)
		return -ENOMEM;

	size = sizeof(*node->reo2ppe.sdesc) * node->reo2ppe.count;
	node->reo2ppe.sdesc = dma_alloc_coherent(dev, size,
						 &node->reo2ppe.sdma,
						 GFP_KERNEL | __GFP_ZERO);
	if (!node->reo2ppe.sdesc)
		return -ENOMEM;

	size = sizeof(*node->txcmpl.desc) * node->txcmpl.count;
	node->txcmpl.desc = dma_alloc_coherent(dev, size, &node->txcmpl.dma,
					       GFP_KERNEL | __GFP_ZERO);
	if (!node->txcmpl.desc)
		return -ENOMEM;

	node->rxfill_bufs = kcalloc(node->rxfill.count,
				    sizeof(*node->rxfill_bufs), GFP_KERNEL);
	node->txcmpl_bufs = kcalloc(node->txcmpl.count,
				    sizeof(*node->txcmpl_bufs), GFP_KERNEL);
	if (!node->rxfill_bufs || !node->txcmpl_bufs)
		return -ENOMEM;

	node->napi_dev = alloc_netdev_dummy(0);
	if (!node->napi_dev)
		return -ENOMEM;

	return 0;
}

static void ppe_ds_resources_free(struct qcom_ppe_ds_node *node)
{
	struct device *dev = node->ds->ppe_dev->dev;

	if (node->napi_dev) {
		free_netdev(node->napi_dev);
		node->napi_dev = NULL;
	}
	kfree(node->txcmpl_bufs);
	node->txcmpl_bufs = NULL;
	kfree(node->rxfill_bufs);
	node->rxfill_bufs = NULL;

	if (node->txcmpl.desc)
		dma_free_coherent(dev,
				  sizeof(*node->txcmpl.desc) * node->txcmpl.count,
				  node->txcmpl.desc, node->txcmpl.dma);
	node->txcmpl.desc = NULL;
	if (node->reo2ppe.sdesc)
		dma_free_coherent(dev,
				  sizeof(*node->reo2ppe.sdesc) * node->reo2ppe.count,
				  node->reo2ppe.sdesc, node->reo2ppe.sdma);
	node->reo2ppe.sdesc = NULL;
	if (node->ppe2tcl.sdesc)
		dma_free_coherent(dev,
				  sizeof(*node->ppe2tcl.sdesc) * node->ppe2tcl.count,
				  node->ppe2tcl.sdesc, node->ppe2tcl.sdma);
	node->ppe2tcl.sdesc = NULL;
	if (node->rxfill.desc)
		dma_free_coherent(dev,
				  sizeof(*node->rxfill.desc) * node->rxfill.count,
				  node->rxfill.desc, node->rxfill.dma);
	node->rxfill.desc = NULL;
}

static void ppe_ds_release_pending_fill(struct qcom_ppe_ds_node *node)
{
	struct edma_rxfill_ring *ring = &node->rxfill;
	u32 cons;

	if (!node->ops->ppe2tcl_release)
		return;

	regmap_read(ppe_ds_regmap(node),
		    ppe_ds_reg(EDMA_REG_RXFILL_CONS_IDX(ring->ring_id)), &cons);
	cons &= EDMA_RXFILL_CONS_IDX_MASK;
	while (cons != ring->prod_idx) {
		struct edma_rxfill_desc *desc = EDMA_RXFILL_DESC(ring, cons);
		u64 cookie = EDMA_RXFILL_OPAQUE_GET(desc);

		node->ops->ppe2tcl_release(node, cookie);
		cons = (cons + 1) & (ring->count - 1);
	}
	ring->prod_idx = cons;
	regmap_write(ppe_ds_regmap(node),
		     ppe_ds_reg(EDMA_REG_RXFILL_PROD_IDX(ring->ring_id)), cons);
}

struct qcom_ppe_ds_node *
qcom_ppe_ds_node_alloc_id(struct device *client,
			  const struct qcom_ppe_ds_ops *ops, int requested_id,
			  size_t priv_size)
{
	struct qcom_ppe_ds_node *node;
	struct ppe_ds *ds;
	int id;

	if (!client || !ops || !ops->ppe2tcl_produce ||
	    !ops->ppe2tcl_refill || !ops->reo2ppe_complete)
		return ERR_PTR(-EINVAL);

	mutex_lock(&ppe_ds_global_lock);
	ds = ppe_ds_global;
	if (!ds || ds->stopping) {
		mutex_unlock(&ppe_ds_global_lock);
		return ERR_PTR(-EPROBE_DEFER);
	}

	if (requested_id < -1 || requested_id >= PPE_DS_NODES) {
		mutex_unlock(&ppe_ds_global_lock);
		return ERR_PTR(-EINVAL);
	}

	mutex_lock(&ds->lock);
	if (requested_id >= 0) {
		id = requested_id;
		if (ds->nodes[id])
			id = PPE_DS_NODES;
	} else {
		for (id = 0; id < PPE_DS_NODES; id++)
			if (!ds->nodes[id])
				break;
	}
	if (id == PPE_DS_NODES) {
		mutex_unlock(&ds->lock);
		mutex_unlock(&ppe_ds_global_lock);
		return ERR_PTR(-ENOSPC);
	}

	node = kzalloc(struct_size(node, priv, priv_size), GFP_KERNEL);
	if (!node) {
		mutex_unlock(&ds->lock);
		mutex_unlock(&ppe_ds_global_lock);
		return ERR_PTR(-ENOMEM);
	}

	node->ds = ds;
	node->client = get_device(client);
	node->ops = ops;
	node->id = id;
	node->queue_start = PPE_DS_QUEUE_BASE + id * PPE_DS_QUEUES_PER_NODE;
	node->queue_count = PPE_DS_QUEUES_PER_NODE;
	node->txcmpl_irq = ds->irqs[id][PPE_DS_IRQ_TXCMPL];
	node->ppe2tcl_irq = ds->irqs[id][PPE_DS_IRQ_PPE2TCL];
	node->rxfill_irq = ds->irqs[id][PPE_DS_IRQ_RXFILL];
	node->state = PPE_DS_ALLOCATED;
	mutex_init(&node->lock);
	ds->nodes[id] = node;
	mutex_unlock(&ds->lock);
	mutex_unlock(&ppe_ds_global_lock);

	dev_info(client, "allocated PPE direct-switch node %d, queues %u-%u\n",
		 id, node->queue_start,
		 node->queue_start + node->queue_count - 1);
	return node;
}
EXPORT_SYMBOL_GPL(qcom_ppe_ds_node_alloc_id);

struct qcom_ppe_ds_node *
qcom_ppe_ds_node_alloc(struct device *client,
		       const struct qcom_ppe_ds_ops *ops, size_t priv_size)
{
	return qcom_ppe_ds_node_alloc_id(client, ops, -1, priv_size);
}
EXPORT_SYMBOL_GPL(qcom_ppe_ds_node_alloc);

void *qcom_ppe_ds_priv(struct qcom_ppe_ds_node *node)
{
	return node ? node->priv : NULL;
}
EXPORT_SYMBOL_GPL(qcom_ppe_ds_priv);

int qcom_ppe_ds_node_id(struct qcom_ppe_ds_node *node)
{
	return node ? node->id : -EINVAL;
}
EXPORT_SYMBOL_GPL(qcom_ppe_ds_node_id);

int qcom_ppe_ds_queue_start(struct qcom_ppe_ds_node *node)
{
	return node ? node->queue_start : -EINVAL;
}
EXPORT_SYMBOL_GPL(qcom_ppe_ds_queue_start);

int qcom_ppe_ds_register(struct qcom_ppe_ds_node *node,
			 const struct qcom_ppe_ds_reg *reg)
{
	u32 ring;
	int ret;

	if (!node || !reg || !ppe_ds_ring_valid(reg->ppe2tcl_count) ||
	    !ppe_ds_ring_valid(reg->reo2ppe_count) ||
	    !ppe_ds_ring_valid(reg->rxfill_count) ||
	    !ppe_ds_ring_valid(reg->txcmpl_count) ||
	    !reg->buffer_size || reg->headroom >= reg->buffer_size ||
	    (upper_32_bits(reg->ppe2tcl_dma) & ~PPE_DS_DMA_HIGH_MASK) ||
	    (upper_32_bits(reg->reo2ppe_dma) & ~PPE_DS_DMA_HIGH_MASK))
		return -EINVAL;

	mutex_lock(&node->lock);
	if (node->state != PPE_DS_ALLOCATED) {
		ret = -EBUSY;
		goto out_unlock;
	}

	ring = node->id + PPE_DS_RING_BASE;
	node->reg = *reg;
	if (!node->reg.headroom)
		node->reg.headroom = PPE_DS_RX_HEADROOM;
	if (node->reg.headroom >= node->reg.buffer_size) {
		ret = -EINVAL;
		goto out_unlock;
	}
	node->rxfill.ring_id = ring;
	node->rxfill.count = reg->rxfill_count;
	node->rxfill.alloc_size = reg->buffer_size;
	node->ppe2tcl.ring_id = ring;
	node->ppe2tcl.count = reg->ppe2tcl_count;
	node->ppe2tcl.pdma = reg->ppe2tcl_dma;
	node->reo2ppe.id = ring;
	node->reo2ppe.count = reg->reo2ppe_count;
	node->reo2ppe.pdma = reg->reo2ppe_dma;
	node->txcmpl.id = ring;
	node->txcmpl.count = reg->txcmpl_count;

	ret = ppe_ds_resources_alloc(node);
	if (ret)
		goto err_resources;
	ppe_ds_napi_add(node);
	ret = ppe_ds_irqs_request(node);
	if (ret)
		goto err_irqs;
	ret = ppe_ds_configure(node);
	if (ret)
		goto err_configure;

	node->state = PPE_DS_REGISTERED;
	dev_info(node->client,
		 "registered PPE direct-switch node %d on EDMA rings %u\n",
		 node->id, ring);
	mutex_unlock(&node->lock);
	return 0;

err_configure:
	ppe_ds_unmap_rings(node);
	ppe_ds_irqs_free(node);
err_irqs:
	ppe_ds_napi_del(node);
err_resources:
	ppe_ds_resources_free(node);
out_unlock:
	mutex_unlock(&node->lock);
	return ret;
}
EXPORT_SYMBOL_GPL(qcom_ppe_ds_register);

int qcom_ppe_ds_start(struct qcom_ppe_ds_node *node)
{
	struct regmap *regmap;
	u32 data, filled;
	int ret = 0;

	if (!node)
		return -EINVAL;

	mutex_lock(&node->lock);
	if (node->state == PPE_DS_STARTED)
		goto out;
	if (node->state != PPE_DS_REGISTERED) {
		ret = -EINVAL;
		goto out;
	}

	regmap = ppe_ds_regmap(node);
	filled = ppe_ds_refill(node, node->rxfill.count - 1);
	if (filled != node->rxfill.count - 1) {
		dev_err(node->client,
			"PPE direct-switch node %d initial refill short: %u/%u\n",
			node->id, filled, node->rxfill.count - 1);
		ppe_ds_release_pending_fill(node);
		ret = -ENOMEM;
		goto out;
	}
	ppe_ds_napi_enable(node);

	regmap_update_bits(regmap,
			   ppe_ds_reg(EDMA_REG_RXDESC_DISABLE(node->ppe2tcl.ring_id)),
			   PPE_DS_RING_DISABLE, 0);
	regmap_update_bits(regmap,
			   ppe_ds_reg(EDMA_REG_RXFILL_DISABLE(node->rxfill.ring_id)),
			   PPE_DS_RING_DISABLE, 0);
	regmap_update_bits(regmap,
			   ppe_ds_reg(EDMA_REG_RXDESC_CTRL(node->ppe2tcl.ring_id)),
			   EDMA_RXDESC_RX_EN, EDMA_RXDESC_RX_EN);
	regmap_update_bits(regmap,
			   ppe_ds_reg(EDMA_REG_RXFILL_RING_EN(node->rxfill.ring_id)),
			   EDMA_RXFILL_RING_EN, EDMA_RXFILL_RING_EN);
	regmap_update_bits(regmap,
			   ppe_ds_reg(EDMA_REG_TXDESC_CTRL(node->reo2ppe.id)),
			   EDMA_TXDESC_CTRL_TXEN_MASK,
			   EDMA_TXDESC_CTRL_TXEN_MASK);

	data = node->reg.rxfill_low_threshold ?: 32;
	regmap_write(regmap,
		     ppe_ds_reg(EDMA_REG_RXFILL_UGT_THRE(node->rxfill.ring_id)),
		     data & U16_MAX);
	WRITE_ONCE(node->state, PPE_DS_STARTED);
	regmap_write(regmap,
		     ppe_ds_reg(EDMA_REG_RXFILL_INT_MASK(node->rxfill.ring_id)),
		     PPE_DS_RXFILL_INT);
	regmap_write(regmap,
		     ppe_ds_reg(EDMA_REG_RXDESC_INT_MASK(node->ppe2tcl.ring_id)),
		     EDMA_RXDESC_INT_MASK_PKT_INT);
	regmap_write(regmap,
		     ppe_ds_reg(EDMA_REG_TX_INT_MASK(node->txcmpl.id)),
		     EDMA_TX_INT_MASK_PKT_INT);
	atomic64_inc(&node->stats.starts);
out:
	mutex_unlock(&node->lock);
	return ret;
}
EXPORT_SYMBOL_GPL(qcom_ppe_ds_start);

void qcom_ppe_ds_stop(struct qcom_ppe_ds_node *node)
{
	struct regmap *regmap;
	int ret;
	u32 val;

	if (!node)
		return;

	mutex_lock(&node->lock);
	if (node->state != PPE_DS_STARTED)
		goto out;

	WRITE_ONCE(node->state, PPE_DS_REGISTERED);
	if (node->ops->quiesce)
		node->ops->quiesce(node);

	regmap = ppe_ds_regmap(node);
	regmap_write(regmap,
		     ppe_ds_reg(EDMA_REG_RXFILL_INT_MASK(node->rxfill.ring_id)), 0);
	regmap_write(regmap,
		     ppe_ds_reg(EDMA_REG_RXDESC_INT_MASK(node->ppe2tcl.ring_id)), 0);
	regmap_write(regmap,
		     ppe_ds_reg(EDMA_REG_TX_INT_MASK(node->txcmpl.id)), 0);
	synchronize_irq(node->rxfill_irq);
	synchronize_irq(node->ppe2tcl_irq);
	synchronize_irq(node->txcmpl_irq);
	ppe_ds_napi_disable(node);

	regmap_clear_bits(regmap,
			  ppe_ds_reg(EDMA_REG_TXDESC_CTRL(node->reo2ppe.id)),
			  EDMA_TXDESC_CTRL_TXEN_MASK);
	regmap_clear_bits(regmap,
			  ppe_ds_reg(EDMA_REG_RXDESC_CTRL(node->ppe2tcl.ring_id)),
			  EDMA_RXDESC_RX_EN);
	regmap_set_bits(regmap,
			ppe_ds_reg(EDMA_REG_RXDESC_DISABLE(node->ppe2tcl.ring_id)),
			PPE_DS_RING_DISABLE);
	ret = regmap_read_poll_timeout(regmap,
			 ppe_ds_reg(EDMA_REG_RXDESC_DISABLE_DONE(node->ppe2tcl.ring_id)),
			 val, val & PPE_DS_RING_DISABLE,
			 PPE_DS_POLL_US, PPE_DS_POLL_TIMEOUT_US);
	if (ret)
		dev_warn(node->client, "PPE2TCL ring %u did not stop: %d\n",
			 node->ppe2tcl.ring_id, ret);
	regmap_clear_bits(regmap,
			  ppe_ds_reg(EDMA_REG_RXFILL_RING_EN(node->rxfill.ring_id)),
			  EDMA_RXFILL_RING_EN);
	regmap_set_bits(regmap,
			ppe_ds_reg(EDMA_REG_RXFILL_DISABLE(node->rxfill.ring_id)),
			PPE_DS_RING_DISABLE);
	ret = regmap_read_poll_timeout(regmap,
			 ppe_ds_reg(EDMA_REG_RXFILL_DISABLE_DONE(node->rxfill.ring_id)),
			 val, val & PPE_DS_RING_DISABLE,
			 PPE_DS_POLL_US, PPE_DS_POLL_TIMEOUT_US);
	if (ret)
		dev_warn(node->client, "RXFILL ring %u did not stop: %d\n",
			 node->rxfill.ring_id, ret);

	ppe_ds_txcmpl_reap(node, node->txcmpl.count - 1);
	ppe_ds_release_pending_fill(node);
	atomic64_inc(&node->stats.stops);
out:
	mutex_unlock(&node->lock);
}
EXPORT_SYMBOL_GPL(qcom_ppe_ds_stop);

void qcom_ppe_ds_node_free(struct qcom_ppe_ds_node *node)
{
	struct ppe_ds *ds;

	if (!node)
		return;
	qcom_ppe_ds_stop(node);

	mutex_lock(&node->lock);
	if (node->state == PPE_DS_REGISTERED) {
		ppe_ds_unmap_rings(node);
		ppe_ds_irqs_free(node);
		ppe_ds_napi_del(node);
		ppe_ds_resources_free(node);
		node->state = PPE_DS_ALLOCATED;
	}
	mutex_unlock(&node->lock);

	ds = node->ds;
	mutex_lock(&ds->lock);
	if (WARN_ON(ds->nodes[node->id] != node)) {
		mutex_unlock(&ds->lock);
		return;
	}
	ds->nodes[node->id] = NULL;
	mutex_unlock(&ds->lock);

	dev_info(node->client, "freed PPE direct-switch node %d\n", node->id);
	put_device(node->client);
	mutex_destroy(&node->lock);
	kfree(node);
}
EXPORT_SYMBOL_GPL(qcom_ppe_ds_node_free);

void qcom_ppe_ds_ppe2tcl_consume(struct qcom_ppe_ds_node *node, u16 cons)
{
	if (!node || READ_ONCE(node->state) != PPE_DS_STARTED)
		return;
	regmap_write(ppe_ds_regmap(node),
		     ppe_ds_reg(EDMA_REG_RXDESC_CONS_IDX(node->ppe2tcl.ring_id)),
		     cons);
}
EXPORT_SYMBOL_GPL(qcom_ppe_ds_ppe2tcl_consume);

u16 qcom_ppe_ds_ppe2tcl_prod(struct qcom_ppe_ds_node *node)
{
	u32 prod = 0;

	if (node)
		regmap_read(ppe_ds_regmap(node),
			    ppe_ds_reg(EDMA_REG_RXDESC_PROD_IDX(node->ppe2tcl.ring_id)),
			    &prod);
	return prod & EDMA_RXDESC_PROD_IDX_MASK;
}
EXPORT_SYMBOL_GPL(qcom_ppe_ds_ppe2tcl_prod);

void qcom_ppe_ds_reo2ppe_produce(struct qcom_ppe_ds_node *node, u16 prod)
{
	if (!node || READ_ONCE(node->state) != PPE_DS_STARTED)
		return;
	dma_wmb();
	regmap_write(ppe_ds_regmap(node),
		     ppe_ds_reg(EDMA_REG_TXDESC_PROD_IDX(node->reo2ppe.id)), prod);
}
EXPORT_SYMBOL_GPL(qcom_ppe_ds_reo2ppe_produce);

u16 qcom_ppe_ds_reo2ppe_cons(struct qcom_ppe_ds_node *node)
{
	u32 cons = 0;

	if (node)
		regmap_read(ppe_ds_regmap(node),
			    ppe_ds_reg(EDMA_REG_TXDESC_CONS_IDX(node->reo2ppe.id)),
			    &cons);
	return cons & EDMA_TXDESC_CONS_IDX_MASK;
}
EXPORT_SYMBOL_GPL(qcom_ppe_ds_reo2ppe_cons);

int qcom_ppe_ds_vp_alloc(struct qcom_ppe_ds_node *node,
			 struct net_device *dev)
{
	int vp;

	if (!node || !dev || READ_ONCE(node->state) != PPE_DS_STARTED)
		return -EINVAL;

	vp = ppe_vp_ds_attach(node->ds->ppe_dev, dev, node->queue_start,
			      node->id);
	if (vp < 0) {
		atomic64_inc(&node->stats.vp_fail);
		return vp;
	}

	atomic64_inc(&node->stats.vp_refs);
	atomic64_inc(&node->stats.vp_attach);
	return vp;
}
EXPORT_SYMBOL_GPL(qcom_ppe_ds_vp_alloc);

void qcom_ppe_ds_vp_free(struct qcom_ppe_ds_node *node, int vp)
{
	if (!node)
		return;

	if (!ppe_vp_ds_detach(node->ds->ppe_dev, vp, node->id)) {
		atomic64_inc(&node->stats.vp_fail);
		return;
	}
	atomic64_dec(&node->stats.vp_refs);
	atomic64_inc(&node->stats.vp_detach);
}
EXPORT_SYMBOL_GPL(qcom_ppe_ds_vp_free);

static int ppe_ds_debugfs_show(struct seq_file *s, void *unused)
{
	struct ppe_ds *ds = s->private;
	int i;

	mutex_lock(&ds->lock);
	for (i = 0; i < PPE_DS_NODES; i++) {
		struct qcom_ppe_ds_node *node = ds->nodes[i];
		u32 p2t_prod, p2t_cons, r2p_prod, r2p_cons;

		if (!node) {
			seq_printf(s, "node%d state=free ring=%d queues=%d-%d\n",
				   i, i + PPE_DS_RING_BASE,
				   PPE_DS_QUEUE_BASE + i * PPE_DS_QUEUES_PER_NODE,
				   PPE_DS_QUEUE_BASE + (i + 1) * PPE_DS_QUEUES_PER_NODE - 1);
			continue;
		}
		regmap_read(ppe_ds_regmap(node),
			    ppe_ds_reg(EDMA_REG_RXDESC_PROD_IDX(node->ppe2tcl.ring_id)),
			    &p2t_prod);
		regmap_read(ppe_ds_regmap(node),
			    ppe_ds_reg(EDMA_REG_RXDESC_CONS_IDX(node->ppe2tcl.ring_id)),
			    &p2t_cons);
		regmap_read(ppe_ds_regmap(node),
			    ppe_ds_reg(EDMA_REG_TXDESC_PROD_IDX(node->reo2ppe.id)),
			    &r2p_prod);
		regmap_read(ppe_ds_regmap(node),
			    ppe_ds_reg(EDMA_REG_TXDESC_CONS_IDX(node->reo2ppe.id)),
			    &r2p_cons);
		seq_printf(s,
			   "node%d state=%u ring=%d queues=%u-%u "
			   "ppe2tcl=%u/%u reo2ppe=%u/%u vp=%lld "
			   "attach=%lld detach=%lld vp_fail=%lld "
			   "ppe2tcl_irq=%lld ppe2tcl_update=%lld "
			   "refill_irq=%lld refill=%lld refill_short=%lld "
			   "reo2ppe_irq=%lld reo2ppe_done=%lld "
			   "start=%lld stop=%lld\n",
			   i, node->state, i + PPE_DS_RING_BASE,
			   node->queue_start,
			   node->queue_start + node->queue_count - 1,
			   p2t_prod & EDMA_RXDESC_PROD_IDX_MASK,
			   p2t_cons & EDMA_RXDESC_CONS_IDX_MASK,
			   r2p_prod & EDMA_TXDESC_PROD_IDX_MASK,
			   r2p_cons & EDMA_TXDESC_CONS_IDX_MASK,
			   atomic64_read(&node->stats.vp_refs),
			   atomic64_read(&node->stats.vp_attach),
			   atomic64_read(&node->stats.vp_detach),
			   atomic64_read(&node->stats.vp_fail),
			   atomic64_read(&node->stats.ppe2tcl_irqs),
			   atomic64_read(&node->stats.ppe2tcl_updates),
			   atomic64_read(&node->stats.refill_irqs),
			   atomic64_read(&node->stats.refill_buffers),
			   atomic64_read(&node->stats.refill_short),
			   atomic64_read(&node->stats.reo2ppe_irqs),
			   atomic64_read(&node->stats.reo2ppe_buffers),
			   atomic64_read(&node->stats.starts),
			   atomic64_read(&node->stats.stops));
	}
	mutex_unlock(&ds->lock);
	return 0;
}

DEFINE_SHOW_ATTRIBUTE(ppe_ds_debugfs);

int ppe_ds_setup(struct ppe_device *ppe_dev)
{
	struct device_node *edma_np;
	struct ppe_ds *ds;
	char name[20];
	int i, irq;

	if (!ppe_dev->variant->has_vports)
		return 0;

	ds = devm_kzalloc(ppe_dev->dev, sizeof(*ds), GFP_KERNEL);
	if (!ds)
		return -ENOMEM;
	ds->ppe_dev = ppe_dev;
	mutex_init(&ds->lock);

	edma_np = of_get_child_by_name(ppe_dev->dev->of_node, "ethernet-dma");
	if (!edma_np)
		return -ENODEV;
	for (i = 0; i < PPE_DS_NODES; i++) {
		snprintf(name, sizeof(name), "txcmpl_%d", i + PPE_DS_RING_BASE);
		irq = of_irq_get_byname(edma_np, name);
		if (irq < 0)
			goto err_irq;
		ds->irqs[i][PPE_DS_IRQ_TXCMPL] = irq;
		snprintf(name, sizeof(name), "rxdesc_%d", i + PPE_DS_RING_BASE);
		irq = of_irq_get_byname(edma_np, name);
		if (irq < 0)
			goto err_irq;
		ds->irqs[i][PPE_DS_IRQ_PPE2TCL] = irq;
		snprintf(name, sizeof(name), "rxfill_%d", i + PPE_DS_RING_BASE);
		irq = of_irq_get_byname(edma_np, name);
		if (irq < 0)
			goto err_irq;
		ds->irqs[i][PPE_DS_IRQ_RXFILL] = irq;
	}
	of_node_put(edma_np);

	ds->debugfs = debugfs_create_file("direct_switch", 0444,
					  ppe_dev->debugfs_root, ds,
					  &ppe_ds_debugfs_fops);
	mutex_lock(&ppe_ds_global_lock);
	if (ppe_ds_global) {
		mutex_unlock(&ppe_ds_global_lock);
		debugfs_remove(ds->debugfs);
		mutex_destroy(&ds->lock);
		return -EBUSY;
	}
	ppe_ds_global = ds;
	mutex_unlock(&ppe_ds_global_lock);

	dev_info(ppe_dev->dev,
		 "PPE direct-switch core ready: 3 nodes, EDMA rings 1-3\n");
	return 0;

err_irq:
	of_node_put(edma_np);
	mutex_destroy(&ds->lock);
	return irq;
}

void ppe_ds_teardown(struct ppe_device *ppe_dev)
{
	struct ppe_ds *ds;
	int i;

	mutex_lock(&ppe_ds_global_lock);
	ds = ppe_ds_global;
	if (!ds || ds->ppe_dev != ppe_dev) {
		mutex_unlock(&ppe_ds_global_lock);
		return;
	}
	ds->stopping = true;
	ppe_ds_global = NULL;
	mutex_unlock(&ppe_ds_global_lock);

	mutex_lock(&ds->lock);
	for (i = 0; i < PPE_DS_NODES; i++)
		if (WARN_ON(ds->nodes[i]))
			dev_err(ppe_dev->dev,
				"direct-switch node %d still registered during teardown\n",
				i);
	mutex_unlock(&ds->lock);
	debugfs_remove(ds->debugfs);
	mutex_destroy(&ds->lock);
}
