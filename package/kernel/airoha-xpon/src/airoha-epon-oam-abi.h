/* SPDX-License-Identifier: GPL-2.0-only WITH Linux-syscall-note */
#ifndef _AIROHA_EPON_OAM_ABI_H_
#define _AIROHA_EPON_OAM_ABI_H_

#include <linux/ioctl.h>
#include <linux/types.h>

#define AIROHA_EPON_OAM_ABI_VERSION	1
#define AIROHA_EPON_OAM_KEY_EVENTS_VERSION 1
#define AIROHA_EPON_OAM_IOC_MAGIC	0xe7

enum airoha_epon_oam_key_suite {
	AIROHA_EPON_OAM_KEY_CTC_TRIPLE_CHURNING = 1,
	AIROHA_EPON_OAM_KEY_DPOE_AES_128 = 2,
};

#define AIROHA_EPON_OAM_KEY_F_OLT_MAC	0x01

struct airoha_epon_oam_key {
	__u8 version;
	__u8 suite;
	__u8 llid_index;
	__u8 key_index;
	__u8 key_length;
	__u8 flags;
	__u8 reserved[2];
	__u8 key[16];
	__u8 olt_mac[6];
	__u8 reserved2[2];
};

#define AIROHA_EPON_DBA_QUEUE_SET_COUNT	4
#define AIROHA_EPON_DBA_THRESHOLD_SET_COUNT 3
#define AIROHA_EPON_DBA_QUEUE_COUNT	8

struct airoha_epon_oam_dba {
	__u8 version;
	__u8 llid_index;
	__u8 queue_set_count;
	__u8 reserved;
	__u8 report_bitmap[AIROHA_EPON_DBA_THRESHOLD_SET_COUNT];
	__u8 reserved2;
	__u16 threshold[AIROHA_EPON_DBA_THRESHOLD_SET_COUNT]
			 [AIROHA_EPON_DBA_QUEUE_COUNT];
};

struct airoha_epon_oam_key_events {
	__u8 version;
	__u8 reserved[3];
	__u32 downstream_llids;
	__u32 upstream_llids;
	__u32 reserved2[2];
};

struct airoha_epon_oam_fec {
	__u8 version;
	__u8 llid_index;
	__u8 tx_enabled;
	__u8 rx_enabled;
	__u32 reserved2;
};

struct airoha_epon_oam_loopback {
	__u8 version;
	__u8 llid_index;
	__u8 enabled;
	__u8 reserved;
	__u32 reserved2;
};

struct airoha_epon_oam_holdover {
	__u8 version;
	__u8 enabled;
	__u8 active;
	__u8 reserved;
	__u16 time_ms;
	__u16 reserved2;
};

struct airoha_epon_oam_session {
	__u8 version;
	__u8 llid_index;
	__u8 reserved[6];
};

#define AIROHA_EPON_OAM_IOC_SET_KEY \
	_IOW(AIROHA_EPON_OAM_IOC_MAGIC, 0x01, struct airoha_epon_oam_key)
#define AIROHA_EPON_OAM_IOC_GET_DBA \
	_IOWR(AIROHA_EPON_OAM_IOC_MAGIC, 0x02, struct airoha_epon_oam_dba)
#define AIROHA_EPON_OAM_IOC_SET_DBA \
	_IOW(AIROHA_EPON_OAM_IOC_MAGIC, 0x03, struct airoha_epon_oam_dba)
#define AIROHA_EPON_OAM_IOC_GET_KEY_EVENTS \
	_IOWR(AIROHA_EPON_OAM_IOC_MAGIC, 0x04, \
	      struct airoha_epon_oam_key_events)
#define AIROHA_EPON_OAM_IOC_GET_FEC \
	_IOWR(AIROHA_EPON_OAM_IOC_MAGIC, 0x05, struct airoha_epon_oam_fec)
#define AIROHA_EPON_OAM_IOC_SET_FEC \
	_IOW(AIROHA_EPON_OAM_IOC_MAGIC, 0x06, struct airoha_epon_oam_fec)
#define AIROHA_EPON_OAM_IOC_GET_LOOPBACK \
	_IOWR(AIROHA_EPON_OAM_IOC_MAGIC, 0x07, \
	      struct airoha_epon_oam_loopback)
#define AIROHA_EPON_OAM_IOC_SET_LOOPBACK \
	_IOW(AIROHA_EPON_OAM_IOC_MAGIC, 0x08, \
	     struct airoha_epon_oam_loopback)
#define AIROHA_EPON_OAM_IOC_GET_HOLDOVER \
	_IOWR(AIROHA_EPON_OAM_IOC_MAGIC, 0x09, \
	      struct airoha_epon_oam_holdover)
#define AIROHA_EPON_OAM_IOC_SET_HOLDOVER \
	_IOW(AIROHA_EPON_OAM_IOC_MAGIC, 0x0a, \
	     struct airoha_epon_oam_holdover)
#define AIROHA_EPON_OAM_IOC_CLEAR_SESSION \
	_IOW(AIROHA_EPON_OAM_IOC_MAGIC, 0x0b, \
	     struct airoha_epon_oam_session)

#endif
