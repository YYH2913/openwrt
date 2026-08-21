/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef _AIROHA_XGS_REBOOT_H_
#define _AIROHA_XGS_REBOOT_H_

#ifdef __KERNEL__
#include <linux/types.h>
#else
#include <stdbool.h>
#include <stdint.h>
#define u8 uint8_t
#define u16 uint16_t
#endif

#define AIROHA_XGS_REBOOT_ONU_MESSAGE_ID	0x1d
#define AIROHA_XGS_REBOOT_CONTENT_SIZE		40
#define AIROHA_XGS_REBOOT_SERIAL_SIZE		8

struct airoha_xgs_reboot_request {
	u8 serial[AIROHA_XGS_REBOOT_SERIAL_SIZE];
	u8 sequence;
	u8 depth;
	u8 image;
	u8 state;
	u8 flags;
};

static inline bool airoha_xgs_parse_reboot_content(
	const u8 content[AIROHA_XGS_REBOOT_CONTENT_SIZE],
	struct airoha_xgs_reboot_request *request)
{
	u8 flags;

	if (!content || !request ||
	    content[2] != AIROHA_XGS_REBOOT_ONU_MESSAGE_ID)
		return false;
	flags = content[15];
	if (content[12] > 3 || content[13] > 1 || content[14] > 1 ||
	    (flags & 0xfc) || (flags & 0x03) > 2)
		return false;
	request->sequence = content[3];
	for (flags = 0; flags < AIROHA_XGS_REBOOT_SERIAL_SIZE; flags++)
		request->serial[flags] = content[4 + flags];
	request->depth = content[12];
	request->image = content[13];
	request->state = content[14];
	request->flags = content[15] & 0x03;
	return true;
}

#endif
