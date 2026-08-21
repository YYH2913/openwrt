/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef _AIROHA_EPON_SECURITY_H_
#define _AIROHA_EPON_SECURITY_H_

#ifdef __KERNEL__
#include <linux/types.h>
#else
#include <stdbool.h>
#include <stdint.h>
typedef uint8_t u8;
typedef uint32_t u32;
#endif

#define AIROHA_EPON_KEY_COMMAND_WRITE	(1U << 31)
#define AIROHA_EPON_KEY_COMMAND_DONE	(1U << 16)

static inline u32 airoha_epon_key_command(bool write, u8 llid, u8 key_index,
					  u8 word)
{
	return (write ? AIROHA_EPON_KEY_COMMAND_WRITE : 0) |
	       ((u32)(llid & 0x3f) << 8) |
	       ((u32)(key_index & 0x1) << 4) | (word & 0x3);
}

static inline u32 airoha_epon_ctc_key_word(const u8 *key, u8 word)
{
	const u8 *part = key + word * 3;

	return (u32)part[0] << 16 | (u32)part[1] << 8 | part[2];
}

static inline u32 airoha_epon_dpoe_key_word(const u8 *key, u8 word)
{
	const u8 *part = key + word * 4;

	return (u32)part[0] << 24 | (u32)part[1] << 16 |
	       (u32)part[2] << 8 | part[3];
}

#endif
