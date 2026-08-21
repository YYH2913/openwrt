/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef _AIROHA_EPON_IEEE_H_
#define _AIROHA_EPON_IEEE_H_

#include <errno.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#define AIROHA_IEEE_ATTRIBUTE_BRANCH 0x07
#define AIROHA_IEEE_PHY_ADMIN_STATE 0x0025
#define AIROHA_IEEE_AUTONEG_ADMIN_STATE 0x004f
#define AIROHA_IEEE_AUTONEG_LOCAL_TECHNOLOGY 0x0052
#define AIROHA_IEEE_AUTONEG_ADVERTISED_TECHNOLOGY 0x0053
#define AIROHA_IEEE_FEC_ABILITY 0x0139
#define AIROHA_IEEE_FEC_MODE 0x013a

#define AIROHA_IEEE_FEC_SUPPORTED 1
#define AIROHA_IEEE_FEC_ENABLED 2
#define AIROHA_IEEE_FEC_DISABLED 3
#define AIROHA_IEEE_VARIABLE_OPERATION_UNSUPPORTED 0x83

struct airoha_ieee_variable_state {
	bool fec_available;
	bool fec_enabled;
};

struct airoha_ieee_variable_result {
	unsigned int descriptors;
	unsigned int supported;
};

static inline void airoha_ieee_put_u32(uint8_t *value, uint32_t number)
{
	value[0] = number >> 24;
	value[1] = number >> 16;
	value[2] = number >> 8;
	value[3] = number;
}

static inline int airoha_ieee_standard_value(uint8_t branch, uint16_t leaf,
		const struct airoha_ieee_variable_state *state, uint8_t *value,
		size_t capacity)
{
	static const uint32_t technology_abilities[] = {
		0x00000006, 0x0000000e, 0x0000008e, 0x00000020,
		0x00000142, 0x00000028, 0x00000192,
	};
	size_t index;

	if (!state || !value)
		return -EINVAL;
	if (branch != AIROHA_IEEE_ATTRIBUTE_BRANCH)
		return -EOPNOTSUPP;
	switch (leaf) {
	case AIROHA_IEEE_PHY_ADMIN_STATE:
	case AIROHA_IEEE_AUTONEG_ADMIN_STATE:
		if (capacity < sizeof(uint32_t))
			return -ENOSPC;
		/* These two reserved status values match the SDK OAM callbacks. */
		airoha_ieee_put_u32(value, 0);
		return sizeof(uint32_t);
	case AIROHA_IEEE_AUTONEG_LOCAL_TECHNOLOGY:
	case AIROHA_IEEE_AUTONEG_ADVERTISED_TECHNOLOGY:
		if (capacity < sizeof(technology_abilities))
			return -ENOSPC;
		for (index = 0; index < sizeof(technology_abilities) /
					     sizeof(technology_abilities[0]); index++)
			airoha_ieee_put_u32(value + index * sizeof(uint32_t),
					      technology_abilities[index]);
		return sizeof(technology_abilities);
	case AIROHA_IEEE_FEC_ABILITY:
		if (!state->fec_available)
			return -EOPNOTSUPP;
		if (capacity < sizeof(uint32_t))
			return -ENOSPC;
		airoha_ieee_put_u32(value, AIROHA_IEEE_FEC_SUPPORTED);
		return sizeof(uint32_t);
	case AIROHA_IEEE_FEC_MODE:
		if (!state->fec_available)
			return -EOPNOTSUPP;
		if (capacity < sizeof(uint32_t))
			return -ENOSPC;
		airoha_ieee_put_u32(value, state->fec_enabled ?
				      AIROHA_IEEE_FEC_ENABLED :
				      AIROHA_IEEE_FEC_DISABLED);
		return sizeof(uint32_t);
	default:
		return -EOPNOTSUPP;
	}
}

static inline int airoha_ieee_build_variable_response(
		const uint8_t *request, size_t request_length,
		const struct airoha_ieee_variable_state *state, uint8_t *response,
		size_t capacity, struct airoha_ieee_variable_result *result)
{
	size_t input = 0, output = 0;

	if (!request || !state || !response || !result)
		return -EINVAL;
	memset(result, 0, sizeof(*result));
	while (input < request_length) {
		uint16_t leaf;
		int value_length;

		if (!request[input]) {
			if (capacity == output)
				return -ENOSPC;
			response[output++] = 0;
			break;
		}
		if (request_length - input < 3)
			return -EINVAL;
		if (capacity - output < 4)
			return -ENOSPC;
		leaf = (uint16_t)request[input + 1] << 8 | request[input + 2];
		memcpy(response + output, request + input, 3);
		value_length = airoha_ieee_standard_value(request[input], leaf,
			state, response + output + 4, capacity - output - 4);
		if (value_length == -EOPNOTSUPP) {
			response[output + 3] =
				AIROHA_IEEE_VARIABLE_OPERATION_UNSUPPORTED;
			value_length = 0;
		} else if (value_length < 0) {
			return value_length;
		} else {
			response[output + 3] = value_length;
			result->supported++;
		}
		result->descriptors++;
		input += 3;
		output += 4 + (size_t)value_length;
	}
	return (int)output;
}

#endif
