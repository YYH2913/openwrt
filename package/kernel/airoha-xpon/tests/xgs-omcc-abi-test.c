/* SPDX-License-Identifier: GPL-2.0-only */
#include <stddef.h>
#include <stdio.h>

#ifndef __packed
#define __packed __attribute__((__packed__))
#endif

#include "../src/airoha-xgs-omcc.h"

int main(void)
{
	printf("%u %u %u %u %#lx %#x %u %u %u %u %zu %u %zu %zu\n",
	       AIROHA_XGS_OMCC_ABI_VERSION,
	       AIROHA_XGS_OMCC_MAX_CONTENTS,
	       AIROHA_XGS_OMCC_CAP_DS_MIC_VERIFIED,
	       AIROHA_XGS_OMCC_CAP_US_MIC_SIGNED,
	       (unsigned long)AIROHA_XGS_OMCC_GET_INFO,
	       AIROHA_XGS_OMCC_MAGIC,
	       AIROHA_XGS_OMCC_DIRECTION_RX,
	       AIROHA_XGS_OMCC_DIRECTION_TX,
	       AIROHA_XGS_OMCC_FLAG_MIC_VERIFIED,
	       AIROHA_XGS_OMCC_FLAG_TRAILER_STRIPPED,
	       offsetof(struct airoha_xgs_omcc_record, contents),
	       (unsigned int)sizeof(struct airoha_xgs_omcc_record),
	       offsetof(struct airoha_xgs_omcc_record, instance_generation),
	       offsetof(struct airoha_xgs_omcc_record, session_generation));
	return 0;
}
