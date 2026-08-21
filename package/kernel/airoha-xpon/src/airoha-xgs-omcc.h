/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef _AIROHA_XGS_OMCC_H_
#define _AIROHA_XGS_OMCC_H_

#include <linux/ioctl.h>
#include <linux/types.h>

#define AIROHA_XGS_OMCC_ABI_VERSION	3
/* The QDMA wire limit includes the XGS-PON four-byte OMCI MIC. */
#define AIROHA_XGS_OMCC_MAX_CONTENTS	1976

#define AIROHA_XGS_OMCC_CAP_DS_MIC_VERIFIED	(1U << 8)
#define AIROHA_XGS_OMCC_CAP_US_MIC_SIGNED	(1U << 9)

#define AIROHA_XGS_OMCC_GET_INFO	_IOR('X', 0, __u32)

#define AIROHA_XGS_OMCC_MAGIC		0x584f4d43
#define AIROHA_XGS_OMCC_DIRECTION_RX	1
#define AIROHA_XGS_OMCC_DIRECTION_TX	2
#define AIROHA_XGS_OMCC_FLAG_MIC_VERIFIED	(1U << 0)
#define AIROHA_XGS_OMCC_FLAG_TRAILER_STRIPPED	(1U << 1)

struct airoha_xgs_omcc_record {
	__be32 magic;
	__u8 abi_version;
	__u8 direction;
	__be16 flags;
	__be16 length;
	__be16 reserved;
	__be64 instance_generation;
	__be64 session_generation;
	__u8 contents[];
} __packed;

#endif
