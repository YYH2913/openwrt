/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef _AIROHA_EPON_LOOPBACK_H_
#define _AIROHA_EPON_LOOPBACK_H_

#include <errno.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define AIROHA_EPON_LOOPBACK_ENABLE	0x01
#define AIROHA_EPON_LOOPBACK_DISABLE	0x02

/* IEEE 802.3ah OAM Configuration: active, loopback, events, variables. */
#define AIROHA_EPON_OAM_CONFIGURATION	0x1d

/* Parser=loopback (1), MUX=discard (1), matching the stock state byte. */
#define AIROHA_EPON_OAM_LOOPBACK_STATE	0x05

static inline int airoha_epon_parse_loopback_control(const uint8_t *payload,
						      size_t length,
						      bool *enabled)
{
	if (!payload || !length || !enabled)
		return -EINVAL;
	if (payload[0] == AIROHA_EPON_LOOPBACK_ENABLE) {
		*enabled = true;
		return 0;
	}
	if (payload[0] == AIROHA_EPON_LOOPBACK_DISABLE) {
		*enabled = false;
		return 0;
	}
	return -EOPNOTSUPP;
}

#endif
