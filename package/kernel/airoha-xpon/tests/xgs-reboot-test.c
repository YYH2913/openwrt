// SPDX-License-Identifier: GPL-2.0-only
#include <assert.h>
#include <stdio.h>
#include <string.h>

#include "../src/airoha-xgs-reboot.h"

int main(void)
{
	struct airoha_xgs_reboot_request request;
	u8 content[AIROHA_XGS_REBOOT_CONTENT_SIZE] = {
		[2] = AIROHA_XGS_REBOOT_ONU_MESSAGE_ID,
		[3] = 0x5a,
		[4] = 'A', [5] = 'I', [6] = 'R', [7] = 'O',
		[8] = 'H', [9] = 'A', [10] = '0', [11] = '1',
		[12] = 3,
		[13] = 1,
		[14] = 1,
		[15] = 2,
	};

	memset(&request, 0, sizeof(request));
	assert(airoha_xgs_parse_reboot_content(content, &request));
	assert(request.sequence == 0x5a);
	assert(!memcmp(request.serial, "AIROHA01", 8));
	assert(request.depth == 3 && request.image == 1);
	assert(request.state == 1 && request.flags == 2);

	content[12] = 4;
	assert(!airoha_xgs_parse_reboot_content(content, &request));
	content[12] = 3;
	content[15] = 3;
	assert(!airoha_xgs_parse_reboot_content(content, &request));
	content[15] = 0x80;
	assert(!airoha_xgs_parse_reboot_content(content, &request));
	content[15] = 2;
	content[2] = 0x1c;
	assert(!airoha_xgs_parse_reboot_content(content, &request));

	puts("XG-PON/XGS-PON Reboot ONU field validation passed");
	return 0;
}
