// SPDX-License-Identifier: GPL-2.0-only
#include <assert.h>
#include <stdbool.h>
#include <stdio.h>

#include "../src/airoha-epon-holdover.h"

int main(void)
{
	struct airoha_epon_holdover_state state = {
		.time_ms = AIROHA_EPON_HOLDOVER_TIME_DEFAULT_MS,
	};

	assert(airoha_epon_holdover_config_valid(false, 50));
	assert(airoha_epon_holdover_config_valid(true, 1000));
	assert(!airoha_epon_holdover_config_valid(true, 49));
	assert(!airoha_epon_holdover_config_valid(true, 1001));

	assert(airoha_epon_holdover_loss(&state) ==
	       AIROHA_EPON_HOLDOVER_CLEAR);
	state.enabled = true;
	assert(airoha_epon_holdover_loss(&state) ==
	       AIROHA_EPON_HOLDOVER_START);
	assert(state.active);
	assert(airoha_epon_holdover_loss(&state) ==
	       AIROHA_EPON_HOLDOVER_PRESERVE);
	assert(airoha_epon_holdover_ready(&state) ==
	       AIROHA_EPON_HOLDOVER_RESTORE);
	assert(!state.active);
	assert(airoha_epon_holdover_ready(&state) ==
	       AIROHA_EPON_HOLDOVER_NONE);

	assert(airoha_epon_holdover_loss(&state) ==
	       AIROHA_EPON_HOLDOVER_START);
	assert(airoha_epon_holdover_timeout(&state, false) ==
	       AIROHA_EPON_HOLDOVER_CLEAR);
	assert(!state.active);
	assert(airoha_epon_holdover_loss(&state) ==
	       AIROHA_EPON_HOLDOVER_START);
	assert(airoha_epon_holdover_timeout(&state, true) ==
	       AIROHA_EPON_HOLDOVER_RESTORE);
	assert(!state.active);

	assert(airoha_epon_holdover_loss(&state) ==
	       AIROHA_EPON_HOLDOVER_START);
	assert(airoha_epon_holdover_cancel(&state));
	assert(!state.active);
	assert(!airoha_epon_holdover_cancel(&state));
	puts("EPON SDK Type-B holdover transitions passed");
	return 0;
}
