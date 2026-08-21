/* SPDX-License-Identifier: GPL-2.0-only */
#include <assert.h>
#include <errno.h>
#include <stdint.h>
#include <string.h>

#include "../src/airoha-epon-ieee.h"

static void test_sdk_standard_attributes(void)
{
	const uint8_t request[] = {
		0x07, 0x00, 0x25,
		0x07, 0x00, 0x4f,
		0x07, 0x00, 0x52,
		0x07, 0x00, 0x53,
		0x07, 0x01, 0x39,
		0x07, 0x01, 0x3a,
		0x07, 0x12, 0x34,
		0x00,
	};
	const uint8_t technology[] = {
		0x00, 0x00, 0x00, 0x06, 0x00, 0x00, 0x00, 0x0e,
		0x00, 0x00, 0x00, 0x8e, 0x00, 0x00, 0x00, 0x20,
		0x00, 0x00, 0x01, 0x42, 0x00, 0x00, 0x00, 0x28,
		0x00, 0x00, 0x01, 0x92,
	};
	struct airoha_ieee_variable_state state = {
		.fec_available = true,
		.fec_enabled = true,
	};
	struct airoha_ieee_variable_result result;
	uint8_t response[128] = { 0 };
	size_t offset = 0;
	int length;

	length = airoha_ieee_build_variable_response(request, sizeof(request),
		&state, response, sizeof(response), &result);
	assert(length == 4 + 4 + 4 + 4 + 4 + sizeof(technology) +
		       4 + sizeof(technology) + 8 + 8 + 4 + 1);
	assert(result.descriptors == 7 && result.supported == 6);
	assert(!memcmp(response + offset,
		(uint8_t[]){ 0x07, 0x00, 0x25, 4, 0, 0, 0, 0 }, 8));
	offset += 8;
	assert(!memcmp(response + offset,
		(uint8_t[]){ 0x07, 0x00, 0x4f, 4, 0, 0, 0, 0 }, 8));
	offset += 8;
	assert(!memcmp(response + offset,
		(uint8_t[]){ 0x07, 0x00, 0x52, sizeof(technology) }, 4));
	assert(!memcmp(response + offset + 4, technology, sizeof(technology)));
	offset += 4 + sizeof(technology);
	assert(!memcmp(response + offset,
		(uint8_t[]){ 0x07, 0x00, 0x53, sizeof(technology) }, 4));
	assert(!memcmp(response + offset + 4, technology, sizeof(technology)));
	offset += 4 + sizeof(technology);
	assert(!memcmp(response + offset,
		(uint8_t[]){ 0x07, 0x01, 0x39, 4, 0, 0, 0,
			AIROHA_IEEE_FEC_SUPPORTED }, 8));
	offset += 8;
	assert(!memcmp(response + offset,
		(uint8_t[]){ 0x07, 0x01, 0x3a, 4, 0, 0, 0,
			AIROHA_IEEE_FEC_ENABLED }, 8));
	offset += 8;
	assert(!memcmp(response + offset,
		(uint8_t[]){ 0x07, 0x12, 0x34,
			AIROHA_IEEE_VARIABLE_OPERATION_UNSUPPORTED, 0 }, 5));
}

static void test_fec_state_and_failures(void)
{
	const uint8_t request[] = {
		0x07, 0x01, 0x39, 0x07, 0x01, 0x3a, 0x00,
	};
	struct airoha_ieee_variable_state state = {
		.fec_available = true,
		.fec_enabled = false,
	};
	struct airoha_ieee_variable_result result;
	uint8_t response[32] = { 0 };
	int length;

	length = airoha_ieee_build_variable_response(request, sizeof(request),
		&state, response, sizeof(response), &result);
	assert(length == 17);
	assert(response[7] == AIROHA_IEEE_FEC_SUPPORTED);
	assert(response[15] == AIROHA_IEEE_FEC_DISABLED);
	assert(response[16] == 0);

	state.fec_available = false;
	length = airoha_ieee_build_variable_response(request, sizeof(request),
		&state, response, sizeof(response), &result);
	assert(length == 9);
	assert(response[3] == AIROHA_IEEE_VARIABLE_OPERATION_UNSUPPORTED);
	assert(response[7] == AIROHA_IEEE_VARIABLE_OPERATION_UNSUPPORTED);
	assert(response[8] == 0);
	assert(airoha_ieee_build_variable_response(request, sizeof(request) - 2,
		&state, response, sizeof(response), &result) == -EINVAL);
	assert(airoha_ieee_build_variable_response(request, sizeof(request),
		&state, response, 3, &result) == -ENOSPC);
	assert(airoha_ieee_standard_value(0x07, AIROHA_IEEE_FEC_MODE, &state,
		response, sizeof(response)) == -EOPNOTSUPP);
}

int main(void)
{
	test_sdk_standard_attributes();
	test_fec_state_and_failures();
	return 0;
}
