/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef _AIROHA_XGS_SERVICE_H_
#define _AIROHA_XGS_SERVICE_H_

#include <linux/types.h>

/*
 * XGS-PON service provisioning is intentionally separate from the legacy
 * GPON data_gems/tconts ABI.  The hardware uses 16-bit XGEM Port-IDs and
 * 14-bit Alloc-IDs, while the Ethernet/QDMA side also needs the selected
 * channel and NBOQ for every upstream XGEM.
 */
#define AIROHA_XGS_SERVICE_ABI_VERSION 1
#define AIROHA_XGS_SERVICE_MAX_TCONTS 32
#define AIROHA_XGS_SERVICE_MAX_XGEMS 256
#define AIROHA_XGS_SERVICE_MAX_XGEM_ID 0xfffe
#define AIROHA_XGS_SERVICE_MAX_ALLOC_ID 0x3fff
#define AIROHA_XGS_SERVICE_MAX_CHANNEL 31
#define AIROHA_XGS_SERVICE_MAX_NBOQ 31

#define AIROHA_XGS_SERVICE_DIRECTION_DOWNSTREAM 1
#define AIROHA_XGS_SERVICE_DIRECTION_UPSTREAM 2
#define AIROHA_XGS_SERVICE_DIRECTION_BIDIRECTIONAL 3

/* Logical service records are translated to the PLOAM-assigned hardware
 * T-CONT slot by the XGS-PON MAC driver.  Slot zero is reserved for the
 * ONU-ID shadow and is never accepted here. */
#define AIROHA_XGS_SERVICE_FIRST_TCONT 1

struct airoha_xgs_service_tcont {
	__u8 index;
	__u8 valid;
	__u8 scheduler;
	__u8 weight;
	__u16 alloc_id;
	__u8 queue_weights[8];
	__u32 cir;
	__u32 pir;
	__u32 cbs;
	__u32 pbs;
};

struct airoha_xgs_service_xgem {
	__u16 xgem_id;
	__u8 tcont_index;
	__u8 direction;
	__u8 channel;
	__u8 nboq;
	__u8 mic_idx;
	__u8 valid;
	__u8 unicast;
	__u8 upstream_encrypted;
};

/* Internal kernel-module API. Pointers are valid only for the call. */
struct airoha_xgs_service_config {
	__u16 version;
	__u16 tcont_count;
	__u16 xgem_count;
	const struct airoha_xgs_service_tcont *tconts;
	const struct airoha_xgs_service_xgem *xgems;
};

/* Text sysfs transaction format used by airoha-omcid.  The kernel ABI is
 * still the typed structure above; these limits keep the text parser bounded
 * while allowing the full XGS table sizes. */
#define AIROHA_XGS_SERVICE_TEXT_MAX 65536

#endif
