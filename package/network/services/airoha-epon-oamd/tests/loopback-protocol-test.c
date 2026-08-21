/* SPDX-License-Identifier: GPL-2.0-only */
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <sys/ioctl.h>

#include "airoha-epon-loopback.h"
#include "airoha-epon-oam-abi.h"

#define CHECK(condition) do { \
	if (!(condition)) { \
		fprintf(stderr, "check failed at line %d: %s\n", __LINE__, \
			#condition); \
		return 1; \
	} \
} while (0)

int main(void)
{
	const uint8_t enable_with_padding[] = {
		AIROHA_EPON_LOOPBACK_ENABLE, 0, 0, 0,
	};
	const uint8_t disable[] = { AIROHA_EPON_LOOPBACK_DISABLE };
	const uint8_t invalid[] = { 0x03 };
	bool enabled = false;

	CHECK(sizeof(struct airoha_epon_oam_loopback) == 8);
	CHECK(sizeof(struct airoha_epon_oam_session) == 8);
	CHECK(_IOC_NR(AIROHA_EPON_OAM_IOC_GET_LOOPBACK) == 0x07);
	CHECK(_IOC_NR(AIROHA_EPON_OAM_IOC_SET_LOOPBACK) == 0x08);
	CHECK(_IOC_SIZE(AIROHA_EPON_OAM_IOC_GET_LOOPBACK) == 8);
	CHECK(_IOC_SIZE(AIROHA_EPON_OAM_IOC_SET_LOOPBACK) == 8);
	CHECK(_IOC_NR(AIROHA_EPON_OAM_IOC_CLEAR_SESSION) == 0x0b);
	CHECK(_IOC_SIZE(AIROHA_EPON_OAM_IOC_CLEAR_SESSION) == 8);
	CHECK(AIROHA_EPON_OAM_CONFIGURATION == 0x1d);
	CHECK(AIROHA_EPON_OAM_LOOPBACK_STATE == 0x05);

	CHECK(!airoha_epon_parse_loopback_control(enable_with_padding,
		sizeof(enable_with_padding), &enabled));
	CHECK(enabled);
	CHECK(!airoha_epon_parse_loopback_control(disable, sizeof(disable),
		&enabled));
	CHECK(!enabled);
	CHECK(airoha_epon_parse_loopback_control(invalid, sizeof(invalid),
		&enabled) < 0);
	CHECK(airoha_epon_parse_loopback_control(NULL, 0, &enabled) < 0);

	puts("IEEE 802.3ah remote-loopback vectors passed");
	return 0;
}
