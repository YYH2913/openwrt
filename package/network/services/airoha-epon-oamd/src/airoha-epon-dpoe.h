/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef _AIROHA_EPON_DPOE_H_
#define _AIROHA_EPON_DPOE_H_

#include <errno.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#define AIROHA_DPOE_AES_KEY_LENGTH	16
#define AIROHA_DPOE_KEY_EXCHANGE_OPCODE	0x08
#define AIROHA_DPOE_KEY_FRAME_LENGTH	42
#define AIROHA_DPOE_SIA_GET_REQUEST	0x01
#define AIROHA_DPOE_SIA_GET_RESPONSE	0x02
#define AIROHA_DPOE_SIA_SET_REQUEST	0x04
#define AIROHA_DPOE_SIA_SET_RESPONSE	0x05
#define AIROHA_DPOE_SIA_HEADER_LENGTH	4
#define AIROHA_DPOE_SIA_STANDARD_BRANCH	0x07
#define AIROHA_DPOE_SIA_ATTRIBUTE_BRANCH 0xd7
#define AIROHA_DPOE_SIA_ONU_ID		0x0142
#define AIROHA_DPOE_SIA_FIRMWARE_VERSION 0x0002
#define AIROHA_DPOE_SIA_CHIPSET		0x0003
#define AIROHA_DPOE_SIA_MANUFACTURE_DATE 0x0004
#define AIROHA_DPOE_SIA_MANUFACTURER	0x0005
#define AIROHA_DPOE_SIA_LLID_COUNT	0x0006
#define AIROHA_DPOE_SIA_PON_PORT_COUNT	0x0007
#define AIROHA_DPOE_SIA_UNI_PORT_COUNT	0x0008
#define AIROHA_DPOE_SIA_PACKET_BUFFER	0x0009
#define AIROHA_DPOE_SIA_ORGANIZATION_NAME 0x000d
#define AIROHA_DPOE_SIA_UNI_PORT_TYPE	0x000f
#define AIROHA_DPOE_SIA_VENDOR_NAME	0x0010
#define AIROHA_DPOE_SIA_MODEL_NUMBER	0x0011
#define AIROHA_DPOE_SIA_HARDWARE_VERSION 0x0012
#define AIROHA_DPOE_SIA_LINE_RATE_MODE	0x0013
#define AIROHA_DPOE_SIA_SOFTWARE_BUNDLE	0x0014
#define AIROHA_DPOE_SIA_FIRMWARE_FILENAME 0x0015
#define AIROHA_DPOE_SIA_OPTICAL_TEMPERATURE 0x021d
#define AIROHA_DPOE_SIA_OPTICAL_VCC	0x021e
#define AIROHA_DPOE_SIA_OPTICAL_BIAS	0x021f
#define AIROHA_DPOE_SIA_OPTICAL_TX_POWER 0x0220
#define AIROHA_DPOE_SIA_OPTICAL_RX_POWER 0x0221
#define AIROHA_DPOE_SIA_ENCRYPTION_KEY_EXPIRATION 0x0401
#define AIROHA_DPOE_SIA_ENCRYPTION_MODE	0x0402
#define AIROHA_DPOE_SIA_EXTENDED_FEC_MODE 0x0605
#define AIROHA_DPOE_SIA_EXTENDED_FEC_LENGTH 2
#define AIROHA_DPOE_SIA_STATUS_SUCCESS	0x80
#define AIROHA_DPOE_SIA_STATUS_UNSUPPORTED_GET 0xa1
#define AIROHA_DPOE_SIA_STATUS_READ_ERROR 0xa3
#define AIROHA_DPOE_SIA_STATUS_UNSUPPORTED 0x86
#define AIROHA_DPOE_SIA_TEXT_MAX	64
#define AIROHA_DPOE_SIA_LLID_COUNT_LENGTH 64
#define AIROHA_DPOE_SIA_PON_PORT_COUNT_LENGTH 4
#define AIROHA_DPOE_SIA_UNI_PORT_COUNT_LENGTH 2
#define AIROHA_DPOE_REKEY_DELAY_MS	10000

struct airoha_dpoe_text {
	uint8_t value[AIROHA_DPOE_SIA_TEXT_MAX];
	uint8_t length;
};

struct airoha_dpoe_inventory {
	uint8_t onu_id[8];
	struct airoha_dpoe_text firmware_version;
	struct airoha_dpoe_text chipset;
	struct airoha_dpoe_text manufacture_date;
	struct airoha_dpoe_text manufacturer;
	struct airoha_dpoe_text organization_name;
	struct airoha_dpoe_text vendor_name;
	struct airoha_dpoe_text model_number;
	struct airoha_dpoe_text hardware_version;
	struct airoha_dpoe_text line_rate_mode;
	struct airoha_dpoe_text software_bundle;
	struct airoha_dpoe_text firmware_filename;
	/* Legacy stock ABI exposes an opaque 64-byte LLID runtime image. */
	uint8_t llid_count[AIROHA_DPOE_SIA_LLID_COUNT_LENGTH];
	uint16_t pon_port_count[2];
	uint16_t uni_port_count;
	uint16_t packet_buffer_kib;
	uint8_t uni_port_type;
};

struct airoha_dpoe_optical_diagnostics {
	bool available;
	uint16_t temperature;
	uint16_t supply_voltage;
	uint16_t laser_bias_current;
	uint16_t transmit_power;
	uint16_t receive_power;
};

struct airoha_dpoe_sia_get_result {
	unsigned int descriptors;
	unsigned int supported;
	unsigned int read_errors;
};

struct airoha_dpoe_sia_set_result {
	unsigned int descriptors;
	unsigned int supported;
	bool arm_rekey;
	bool encryption_mode_seen;
};

struct airoha_dpoe_fec_set {
	bool present;
	bool valid;
	bool applied;
	bool rx_enabled;
	bool tx_enabled;
};

struct airoha_dpoe_key_exchange {
	uint8_t key[AIROHA_DPOE_AES_KEY_LENGTH];
	uint8_t key_index;
	bool pending;
	bool installed;
};

struct airoha_dpoe_rekey_timer {
	uint64_t deadline_ms;
	bool armed;
};

static inline bool airoha_dpoe_zero_padding(const uint8_t *data, size_t length)
{
	while (length--)
		if (*data++)
			return false;
	return true;
}

static inline void airoha_dpoe_rekey_cancel(
		struct airoha_dpoe_rekey_timer *timer)
{
	if (!timer)
		return;
	timer->deadline_ms = 0;
	timer->armed = false;
}

static inline bool airoha_dpoe_rekey_schedule(
		struct airoha_dpoe_rekey_timer *timer, uint64_t now_ms)
{
	if (!timer || timer->armed)
		return false;
	timer->deadline_ms = now_ms + AIROHA_DPOE_REKEY_DELAY_MS;
	timer->armed = true;
	return true;
}

static inline bool airoha_dpoe_rekey_consume(
		struct airoha_dpoe_rekey_timer *timer, uint64_t now_ms)
{
	if (!timer || !timer->armed || now_ms < timer->deadline_ms)
		return false;
	airoha_dpoe_rekey_cancel(timer);
	return true;
}

static inline void airoha_dpoe_exchange_clear(
		struct airoha_dpoe_key_exchange *exchange)
{
	volatile uint8_t *byte;
	size_t length;

	if (!exchange)
		return;
	byte = (volatile uint8_t *)exchange;
	length = sizeof(*exchange);
	while (length--)
		*byte++ = 0;
}

static inline int airoha_dpoe_exchange_begin(
		struct airoha_dpoe_key_exchange *exchange,
		uint8_t active_index,
		const uint8_t key[AIROHA_DPOE_AES_KEY_LENGTH])
{
	if (!exchange || !key || active_index > 1)
		return -EINVAL;
	if (exchange->pending)
		return 0;
	memcpy(exchange->key, key, sizeof(exchange->key));
	exchange->key_index = active_index ^ 1;
	exchange->pending = true;
	exchange->installed = false;
	return 0;
}

static inline int airoha_dpoe_build_key_frame(
		const uint8_t onu_mac[6], uint8_t key_index,
		const uint8_t key[AIROHA_DPOE_AES_KEY_LENGTH],
		uint8_t *frame, size_t capacity)
{
	static const uint8_t destination[6] = {
		0x01, 0x80, 0xc2, 0x00, 0x00, 0x02,
	};
	static const uint8_t oui[3] = { 0x00, 0x10, 0x00 };

	if (!onu_mac || !key || !frame || key_index > 1)
		return -EINVAL;
	if (capacity < AIROHA_DPOE_KEY_FRAME_LENGTH)
		return -ENOSPC;
	memset(frame, 0, AIROHA_DPOE_KEY_FRAME_LENGTH);
	memcpy(frame, destination, sizeof(destination));
	memcpy(frame + 6, onu_mac, 6);
	frame[12] = 0x88;
	frame[13] = 0x09;
	frame[14] = 0x03;
	frame[15] = 0x00;
	frame[16] = 0x50;
	frame[17] = 0xfe;
	memcpy(frame + 18, oui, sizeof(oui));
	frame[21] = AIROHA_DPOE_KEY_EXCHANGE_OPCODE;
	frame[22] = key_index;
	frame[23] = AIROHA_DPOE_AES_KEY_LENGTH;
	memcpy(frame + 24, key, AIROHA_DPOE_AES_KEY_LENGTH);
	return AIROHA_DPOE_KEY_FRAME_LENGTH;
}

static inline bool airoha_dpoe_is_extended_fec(uint8_t branch, uint16_t leaf)
{
	return branch == AIROHA_DPOE_SIA_ATTRIBUTE_BRANCH &&
	       leaf == AIROHA_DPOE_SIA_EXTENDED_FEC_MODE;
}

static inline int airoha_dpoe_decode_extended_fec(const uint8_t *value,
		size_t width, bool *rx_enabled, bool *tx_enabled)
{
	if (!value || !rx_enabled || !tx_enabled ||
	    width != AIROHA_DPOE_SIA_EXTENDED_FEC_LENGTH ||
	    value[0] > 1 || value[1] > 1)
		return -EINVAL;
	*rx_enabled = value[0];
	*tx_enabled = value[1];
	return 0;
}

static inline int airoha_dpoe_parse_extended_fec(
		const uint8_t *request, size_t request_length,
		struct airoha_dpoe_fec_set *fec)
{
	static const uint8_t oui[3] = { 0x00, 0x10, 0x00 };
	size_t input = AIROHA_DPOE_SIA_HEADER_LENGTH;

	if (!request || !fec)
		return -EINVAL;
	memset(fec, 0, sizeof(*fec));
	fec->valid = true;
	if (request_length < AIROHA_DPOE_SIA_HEADER_LENGTH ||
	    memcmp(request, oui, sizeof(oui)) ||
	    request[3] != AIROHA_DPOE_SIA_SET_REQUEST)
		return -EINVAL;

	while (input < request_length) {
		uint16_t leaf;
		uint8_t width;
		bool rx_enabled, tx_enabled;

		if (request_length - input >= 3 && !request[input] &&
		    !request[input + 1] && !request[input + 2]) {
			if (!airoha_dpoe_zero_padding(request + input,
					request_length - input))
				return -EINVAL;
			break;
		}
		if (request_length - input < 4)
			return -EINVAL;
		width = request[input + 3];
		if ((size_t)width > request_length - input - 4)
			return -EINVAL;
		leaf = (uint16_t)request[input + 1] << 8 | request[input + 2];
		if (airoha_dpoe_is_extended_fec(request[input], leaf)) {
			if (airoha_dpoe_decode_extended_fec(request + input + 4,
					width, &rx_enabled, &tx_enabled)) {
				fec->valid = false;
			} else if (fec->present &&
				   (fec->rx_enabled != rx_enabled ||
				    fec->tx_enabled != tx_enabled)) {
				fec->valid = false;
			} else {
				fec->rx_enabled = rx_enabled;
				fec->tx_enabled = tx_enabled;
			}
			fec->present = true;
		}
		input += 4 + width;
	}
	return 0;
}

static inline int airoha_dpoe_copy_sia_text(
		const struct airoha_dpoe_text *text, uint8_t *value,
		size_t capacity)
{
	if (!text || !text->length || text->length > sizeof(text->value))
		return -EOPNOTSUPP;
	if (capacity < text->length)
		return -ENOSPC;
	memcpy(value, text->value, text->length);
	return text->length;
}

static inline int airoha_dpoe_get_sia_value(uint8_t branch, uint16_t leaf,
		const struct airoha_dpoe_inventory *inventory,
		const struct airoha_dpoe_optical_diagnostics *diagnostics,
		uint8_t *value, size_t capacity)
{
	const struct airoha_dpoe_text *text = NULL;
	uint16_t optical_value = 0;

	if (!inventory || !value)
		return -EINVAL;
	if (branch == AIROHA_DPOE_SIA_STANDARD_BRANCH &&
	    leaf == AIROHA_DPOE_SIA_ONU_ID) {
		if (capacity < sizeof(inventory->onu_id))
			return -ENOSPC;
		memcpy(value, inventory->onu_id, sizeof(inventory->onu_id));
		return sizeof(inventory->onu_id);
	}
	if (branch != AIROHA_DPOE_SIA_ATTRIBUTE_BRANCH)
		return -EOPNOTSUPP;

	switch (leaf) {
	case AIROHA_DPOE_SIA_FIRMWARE_VERSION:
		text = &inventory->firmware_version;
		break;
	case AIROHA_DPOE_SIA_CHIPSET:
		text = &inventory->chipset;
		break;
	case AIROHA_DPOE_SIA_MANUFACTURE_DATE:
		text = &inventory->manufacture_date;
		break;
	case AIROHA_DPOE_SIA_MANUFACTURER:
		text = &inventory->manufacturer;
		break;
	case AIROHA_DPOE_SIA_ORGANIZATION_NAME:
		text = &inventory->organization_name;
		break;
	case AIROHA_DPOE_SIA_VENDOR_NAME:
		text = &inventory->vendor_name;
		break;
	case AIROHA_DPOE_SIA_MODEL_NUMBER:
		text = &inventory->model_number;
		break;
	case AIROHA_DPOE_SIA_HARDWARE_VERSION:
		text = &inventory->hardware_version;
		break;
	case AIROHA_DPOE_SIA_LINE_RATE_MODE:
		text = &inventory->line_rate_mode;
		break;
	case AIROHA_DPOE_SIA_SOFTWARE_BUNDLE:
		text = &inventory->software_bundle;
		break;
	case AIROHA_DPOE_SIA_FIRMWARE_FILENAME:
		text = &inventory->firmware_filename;
		break;
	case AIROHA_DPOE_SIA_LLID_COUNT:
		if (capacity < sizeof(inventory->llid_count))
			return -ENOSPC;
		memcpy(value, inventory->llid_count,
		       sizeof(inventory->llid_count));
		return sizeof(inventory->llid_count);
	case AIROHA_DPOE_SIA_PON_PORT_COUNT:
		if (capacity < AIROHA_DPOE_SIA_PON_PORT_COUNT_LENGTH)
			return -ENOSPC;
		value[0] = inventory->pon_port_count[0] >> 8;
		value[1] = inventory->pon_port_count[0];
		value[2] = inventory->pon_port_count[1] >> 8;
		value[3] = inventory->pon_port_count[1];
		return AIROHA_DPOE_SIA_PON_PORT_COUNT_LENGTH;
	case AIROHA_DPOE_SIA_UNI_PORT_COUNT:
		if (capacity < AIROHA_DPOE_SIA_UNI_PORT_COUNT_LENGTH)
			return -ENOSPC;
		value[0] = inventory->uni_port_count >> 8;
		value[1] = inventory->uni_port_count;
		return AIROHA_DPOE_SIA_UNI_PORT_COUNT_LENGTH;
	case AIROHA_DPOE_SIA_PACKET_BUFFER:
		if (capacity < 2)
			return -ENOSPC;
		value[0] = inventory->packet_buffer_kib >> 8;
		value[1] = inventory->packet_buffer_kib;
		return 2;
	case AIROHA_DPOE_SIA_UNI_PORT_TYPE:
		if (capacity < 1)
			return -ENOSPC;
		value[0] = inventory->uni_port_type;
		return 1;
	case AIROHA_DPOE_SIA_OPTICAL_TEMPERATURE:
		if (!diagnostics || !diagnostics->available)
			return -EIO;
		optical_value = diagnostics->temperature;
		break;
	case AIROHA_DPOE_SIA_OPTICAL_VCC:
		if (!diagnostics || !diagnostics->available)
			return -EIO;
		optical_value = diagnostics->supply_voltage;
		break;
	case AIROHA_DPOE_SIA_OPTICAL_BIAS:
		if (!diagnostics || !diagnostics->available)
			return -EIO;
		optical_value = diagnostics->laser_bias_current;
		break;
	case AIROHA_DPOE_SIA_OPTICAL_TX_POWER:
		if (!diagnostics || !diagnostics->available)
			return -EIO;
		optical_value = diagnostics->transmit_power;
		break;
	case AIROHA_DPOE_SIA_OPTICAL_RX_POWER:
		if (!diagnostics || !diagnostics->available)
			return -EIO;
		optical_value = diagnostics->receive_power;
		break;
	default:
		return -EOPNOTSUPP;
	}

	if (text)
		return airoha_dpoe_copy_sia_text(text, value, capacity);
	if (capacity < 2)
		return -ENOSPC;
	value[0] = optical_value >> 8;
	value[1] = optical_value;
	return 2;
}

/*
 * Stock SIA Get requests contain OUI/opcode and three-byte descriptors.
 * Responses retain each descriptor and append width/value, or one status byte.
 */
static inline int airoha_dpoe_build_sia_get_response(
		const uint8_t *request, size_t request_length,
		uint8_t *response, size_t capacity,
		const struct airoha_dpoe_inventory *inventory,
		const struct airoha_dpoe_optical_diagnostics *diagnostics,
		struct airoha_dpoe_sia_get_result *result)
{
	static const uint8_t oui[3] = { 0x00, 0x10, 0x00 };
	size_t input = AIROHA_DPOE_SIA_HEADER_LENGTH;
	size_t output = AIROHA_DPOE_SIA_HEADER_LENGTH;

	if (!request || !response || !inventory || !result)
		return -EINVAL;
	memset(result, 0, sizeof(*result));
	if (request_length < AIROHA_DPOE_SIA_HEADER_LENGTH ||
	    memcmp(request, oui, sizeof(oui)) ||
	    request[3] != AIROHA_DPOE_SIA_GET_REQUEST)
		return -EINVAL;
	if (capacity < AIROHA_DPOE_SIA_HEADER_LENGTH)
		return -ENOSPC;
	memcpy(response, request, AIROHA_DPOE_SIA_HEADER_LENGTH);
	response[3] = AIROHA_DPOE_SIA_GET_RESPONSE;

	while (input < request_length) {
		uint8_t value[AIROHA_DPOE_SIA_TEXT_MAX];
		uint16_t leaf;
		int value_length;

		if (request_length - input < 3)
			return -EINVAL;
		if (!request[input] && !request[input + 1] &&
		    !request[input + 2]) {
			if (!airoha_dpoe_zero_padding(request + input,
					request_length - input))
				return -EINVAL;
			break;
		}
		leaf = (uint16_t)request[input + 1] << 8 |
			request[input + 2];
		value_length = airoha_dpoe_get_sia_value(request[input], leaf,
			inventory, diagnostics, value, sizeof(value));
		if (value_length >= 0) {
			if (capacity - output < (size_t)value_length + 4)
				return -ENOSPC;
			memcpy(response + output, request + input, 3);
			response[output + 3] = value_length;
			memcpy(response + output + 4, value, value_length);
			output += 4 + value_length;
			result->supported++;
		} else {
			uint8_t status;

			if (value_length == -ENOSPC)
				return -ENOSPC;
			if (capacity - output < 4)
				return -ENOSPC;
			status = value_length == -EIO ?
				AIROHA_DPOE_SIA_STATUS_READ_ERROR :
				AIROHA_DPOE_SIA_STATUS_UNSUPPORTED_GET;
			memcpy(response + output, request + input, 3);
			response[output + 3] = status;
			output += 4;
			if (status == AIROHA_DPOE_SIA_STATUS_READ_ERROR)
				result->read_errors++;
		}
		result->descriptors++;
		input += 3;
	}
	return output;
}

/*
 * Stock SIA uses a four-byte organization header followed by Set
 * descriptors: branch, big-endian leaf, width and value.  Set responses keep
 * the three-byte descriptor identity and replace width/value with status.
 */
static inline int airoha_dpoe_build_sia_set_response(
		const uint8_t *request, size_t request_length,
		uint8_t *response, size_t capacity,
		const struct airoha_dpoe_fec_set *fec,
		struct airoha_dpoe_sia_set_result *result)
{
	static const uint8_t oui[3] = { 0x00, 0x10, 0x00 };
	size_t input = AIROHA_DPOE_SIA_HEADER_LENGTH;
	size_t output = AIROHA_DPOE_SIA_HEADER_LENGTH;

	if (!request || !response || !result)
		return -EINVAL;
	memset(result, 0, sizeof(*result));
	if (request_length < AIROHA_DPOE_SIA_HEADER_LENGTH ||
	    memcmp(request, oui, sizeof(oui)) ||
	    request[3] != AIROHA_DPOE_SIA_SET_REQUEST)
		return -EINVAL;
	if (capacity < AIROHA_DPOE_SIA_HEADER_LENGTH)
		return -ENOSPC;

	memcpy(response, request, AIROHA_DPOE_SIA_HEADER_LENGTH);
	response[3] = AIROHA_DPOE_SIA_SET_RESPONSE;
	while (input < request_length) {
		uint16_t leaf;
		uint8_t status = AIROHA_DPOE_SIA_STATUS_UNSUPPORTED;
		uint8_t width;

		if (request_length - input >= 3 && !request[input] &&
		    !request[input + 1] && !request[input + 2]) {
			if (!airoha_dpoe_zero_padding(request + input,
					request_length - input))
				return -EINVAL;
			break;
		}
		if (request_length - input < 4)
			return -EINVAL;
		width = request[input + 3];
		if ((size_t)width > request_length - input - 4)
			return -EINVAL;
		if (capacity - output < 4)
			return -ENOSPC;

		leaf = (uint16_t)request[input + 1] << 8 |
			request[input + 2];
		if (request[input] == AIROHA_DPOE_SIA_ATTRIBUTE_BRANCH) {
			switch (leaf) {
			case AIROHA_DPOE_SIA_ENCRYPTION_KEY_EXPIRATION:
				status = AIROHA_DPOE_SIA_STATUS_SUCCESS;
				result->arm_rekey = true;
				result->supported++;
				break;
			case AIROHA_DPOE_SIA_ENCRYPTION_MODE:
				status = AIROHA_DPOE_SIA_STATUS_SUCCESS;
				result->encryption_mode_seen = true;
				result->supported++;
				break;
			case AIROHA_DPOE_SIA_EXTENDED_FEC_MODE: {
				bool rx_enabled, tx_enabled;

				if (fec && fec->present && fec->valid && fec->applied &&
				    !airoha_dpoe_decode_extended_fec(
					    request + input + 4, width,
					    &rx_enabled, &tx_enabled) &&
				    rx_enabled == fec->rx_enabled &&
				    tx_enabled == fec->tx_enabled) {
					status = AIROHA_DPOE_SIA_STATUS_SUCCESS;
					result->supported++;
				}
				break;
			}
			default:
				break;
			}
		}
		memcpy(response + output, request + input, 3);
		response[output + 3] = status;
		result->descriptors++;
		input += 4 + width;
		output += 4;
	}
	return (int)output;
}

#endif
