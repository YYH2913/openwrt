/* SPDX-License-Identifier: GPL-2.0-only */
#define _GNU_SOURCE

#include <assert.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <sys/stat.h>
#include <string.h>
#include <unistd.h>

#include "../src/airoha-epon-ctc.h"

static void test_authentication(void)
{
	uint8_t loid[AIROHA_CTC_LOID_LENGTH] = "ONU-1234";
	uint8_t password[AIROHA_CTC_PASSWORD_LENGTH] = "secret";
	uint8_t response[AIROHA_CTC_AUTH_CREDENTIAL_RESPONSE_LENGTH] = { 0 };
	int length;

	length = airoha_ctc_build_auth_response(
		AIROHA_CTC_AUTH_LOID_PASSWORD, loid, password, response,
		sizeof(response));
	assert(length == AIROHA_CTC_AUTH_CREDENTIAL_RESPONSE_LENGTH);
	assert(response[0] == AIROHA_CTC_AUTH_RESPONSE);
	assert(response[1] == 0 && response[2] == 0x25);
	assert(response[3] == AIROHA_CTC_AUTH_LOID_PASSWORD);
	assert(!memcmp(response + 4, loid, sizeof(loid)));
	assert(!memcmp(response + 4 + sizeof(loid), password, sizeof(password)));

	memset(response, 0, sizeof(response));
	length = airoha_ctc_build_auth_response(7, loid, password, response,
						 sizeof(response));
	assert(length == 5);
	assert(response[0] == AIROHA_CTC_AUTH_RESPONSE);
	assert(response[1] == 0 && response[2] == 2);
	assert(response[3] == AIROHA_CTC_AUTH_NAK);
	assert(response[4] == AIROHA_CTC_AUTH_LOID_PASSWORD);
}

static void test_churning(void)
{
	const uint8_t key[AIROHA_CTC_CHURNING_KEY_LENGTH] = {
		0x10, 0x11, 0x12, 0x20, 0x21, 0x22, 0x30, 0x31, 0x32,
	};
	uint8_t response[AIROHA_CTC_CHURNING_RESPONSE_LENGTH] = { 0 };
	int length;

	length = airoha_ctc_build_churning_response(1, key, response,
						    sizeof(response));
	assert(length == AIROHA_CTC_CHURNING_RESPONSE_LENGTH);
	assert(response[0] == AIROHA_CTC_CHURNING_RESPONSE);
	assert(response[1] == 1);
	assert(!memcmp(response + 2, key, sizeof(key)));
	assert(airoha_ctc_build_churning_response(2, key, response,
						  sizeof(response)) == -EINVAL);
}

static void test_organization_frames(void)
{
	const uint8_t onu_mac[6] = { 0x02, 0xaa, 0xbb, 0xcc, 0xdd, 0xee };
	const uint8_t ctc_oui[3] = { 0x11, 0x11, 0x11 };
	const uint8_t authentication[] = {
		AIROHA_CTC_AUTH_RESPONSE, 0x00, 0x02,
		AIROHA_CTC_AUTH_NAK, AIROHA_CTC_AUTH_LOID_PASSWORD,
	};
	const uint8_t churning[] = {
		AIROHA_CTC_CHURNING_RESPONSE, 0x01,
		0x10, 0x11, 0x12, 0x20, 0x21, 0x22, 0x30, 0x31, 0x32,
	};
	const uint8_t expected_prefix[] = {
		0x01, 0x80, 0xc2, 0x00, 0x00, 0x02,
		0x02, 0xaa, 0xbb, 0xcc, 0xdd, 0xee,
		0x88, 0x09, 0x03, 0x12, 0x34, 0xfe,
		0x11, 0x11, 0x11,
	};
	uint8_t frame[128] = { 0 };
	int length;

	length = airoha_ctc_build_organization_frame(onu_mac, ctc_oui,
		0x1234, AIROHA_CTC_OPCODE_AUTHENTICATION, authentication,
		sizeof(authentication), frame, sizeof(frame));
	assert(length == (int)(sizeof(expected_prefix) + 1 +
			       sizeof(authentication)));
	assert(!memcmp(frame, expected_prefix, sizeof(expected_prefix)));
	assert(frame[sizeof(expected_prefix)] ==
	       AIROHA_CTC_OPCODE_AUTHENTICATION);
	assert(!memcmp(frame + sizeof(expected_prefix) + 1, authentication,
		       sizeof(authentication)));

	memset(frame, 0, sizeof(frame));
	length = airoha_ctc_build_organization_frame(onu_mac, ctc_oui,
		0x0040, AIROHA_CTC_OPCODE_CHURNING, churning,
		sizeof(churning), frame, sizeof(frame));
	assert(length == AIROHA_CTC_OAM_HEADER_LENGTH +
		AIROHA_CTC_ORGANIZATION_HEADER_LENGTH + (int)sizeof(churning));
	assert(frame[15] == 0x00 && frame[16] == 0x40);
	assert(frame[17] == 0xfe);
	assert(frame[21] == AIROHA_CTC_OPCODE_CHURNING);
	assert(!memcmp(frame + 22, churning, sizeof(churning)));
	assert(airoha_ctc_build_organization_frame(onu_mac, ctc_oui, 0,
		AIROHA_CTC_OPCODE_CHURNING, churning, sizeof(churning), frame,
		AIROHA_CTC_OAM_HEADER_LENGTH) == -ENOSPC);
}

static void test_dba(void)
{
	const uint8_t bitmap[AIROHA_CTC_DBA_THRESHOLD_SET_COUNT] = {
		0x05, 0x02, 0x00,
	};
	const uint16_t threshold[AIROHA_CTC_DBA_THRESHOLD_SET_COUNT]
				[AIROHA_CTC_DBA_QUEUE_COUNT] = {
		{ 0x0102, 0, 0x0304, 0, 0, 0, 0, 0 },
		{ 0, 0x0506, 0, 0, 0, 0, 0, 0 },
	};
	const uint8_t expected_get[] = {
		AIROHA_CTC_DBA_GET_RESPONSE, 3,
		0x05, 0x01, 0x02, 0x03, 0x04,
		0x02, 0x05, 0x06,
		0x00,
	};
	uint8_t set_request[2 + 2 * AIROHA_CTC_DBA_SET_BYTES_PER_SET] = {
		AIROHA_CTC_DBA_SET_REQUEST, 3,
	};
	uint8_t parsed_count = 0;
	uint8_t parsed_bitmap[AIROHA_CTC_DBA_THRESHOLD_SET_COUNT];
	uint16_t parsed_threshold[AIROHA_CTC_DBA_THRESHOLD_SET_COUNT]
				 [AIROHA_CTC_DBA_QUEUE_COUNT];
	uint8_t response[AIROHA_CTC_DBA_RESPONSE_MAX] = { 0 };
	size_t offset = 2;
	unsigned int set, queue;
	int length;

	length = airoha_ctc_build_dba_get_response(3, bitmap, threshold,
		response, sizeof(response));
	assert(length == (int)sizeof(expected_get));
	assert(!memcmp(response, expected_get, sizeof(expected_get)));
	assert(airoha_ctc_build_dba_get_response(0, bitmap, threshold,
		response, sizeof(response)) == -EINVAL);
	assert(airoha_ctc_build_dba_get_response(3, bitmap, threshold,
		response, 4) == -ENOSPC);

	for (set = 0; set < 2; set++) {
		set_request[offset++] = bitmap[set];
		for (queue = 0; queue < AIROHA_CTC_DBA_QUEUE_COUNT; queue++) {
			set_request[offset++] = threshold[set][queue] >> 8;
			set_request[offset++] = threshold[set][queue] & 0xff;
		}
	}
	assert(airoha_ctc_parse_dba_set(set_request, sizeof(set_request),
		&parsed_count, parsed_bitmap, parsed_threshold) == 0);
	assert(parsed_count == 3);
	assert(!memcmp(parsed_bitmap, bitmap, sizeof(parsed_bitmap)));
	assert(!memcmp(parsed_threshold, threshold, sizeof(parsed_threshold)));
	assert(airoha_ctc_parse_dba_set(set_request,
		sizeof(set_request) - 1, &parsed_count, parsed_bitmap,
		parsed_threshold) == -EINVAL);
	set_request[1] = 1;
	assert(airoha_ctc_parse_dba_set(set_request, sizeof(set_request),
		&parsed_count, parsed_bitmap, parsed_threshold) == -EINVAL);
	assert(airoha_ctc_build_dba_set_response(true, response,
		sizeof(response)) == 2);
	assert(response[0] == AIROHA_CTC_DBA_SET_RESPONSE);
	assert(response[1] == AIROHA_CTC_DBA_ACK);
	assert(airoha_ctc_build_dba_set_response(false, response,
		sizeof(response)) == 2);
	assert(response[1] == AIROHA_CTC_DBA_NACK);
}

static void test_extended_variables(void)
{
	const uint8_t request[] = {
		0xc7, 0x00, 0x01,
		0xc7, 0x00, 0x02,
		0xc7, 0x00, 0x03,
		AIROHA_CTC_STANDARD_ATTRIBUTE_BRANCH, 0x01, 0x3a,
		0xc7, 0x12, 0x34,
		0x00, 0x00, 0x00,
	};
	const uint8_t set_request[] = {
		AIROHA_CTC_STANDARD_ATTRIBUTE_BRANCH, 0x01, 0x3a, 0x04,
		0x00, 0x00, 0x00, AIROHA_CTC_STANDARD_FEC_ENABLED,
		0xc7, 0x00, 0x11, 0x01, 0x01,
		0xc9, 0x00, 0x01, 0x00,
		0x00, 0x00, 0x00,
	};
	const uint8_t set_expected[] = {
		AIROHA_CTC_STANDARD_ATTRIBUTE_BRANCH, 0x01, 0x3a,
		AIROHA_CTC_VARIABLE_STATUS_SUCCESS,
		0xc7, 0x00, 0x11, AIROHA_CTC_VARIABLE_STATUS_UNSUPPORTED,
		0xc9, 0x00, 0x01, AIROHA_CTC_VARIABLE_STATUS_UNSUPPORTED,
	};
	struct airoha_ctc_identity identity = {
		.vendor_id = { 'A', 'I', 'R', 'O' },
		.model = { 'X', 'G', '2', '0' },
		.onu_mac = { 0x02, 0xaa, 0xbb, 0xcc, 0xdd, 0xee },
		.hardware_version = { 'R', '1', ' ', ' ', ' ', ' ', ' ', ' ' },
		.software_version = {
			'O', 'p', 'e', 'n', 'W', 'r', 't', ' ',
			' ', ' ', ' ', ' ', ' ', ' ', ' ', ' ',
		},
		.chipset_id = { 'A', 'N', '7', '5', '8', '1', ' ', ' ' },
		.firmware_version = { 'r', '1', '5' },
		.firmware_version_length = 3,
	};
	struct airoha_ctc_variable_result result = { 0 };
	struct airoha_ctc_fec_mode fec_mode = {
		.available = true,
		.enabled = true,
	};
	struct airoha_ctc_fec_set fec_set = { 0 };
	uint8_t response[128] = { 0 };
	uint8_t expected_serial[AIROHA_CTC_ONU_SERIAL_NUMBER_LENGTH];
	size_t offset = 0;
	int length;

	memcpy(expected_serial + offset, identity.vendor_id,
	       sizeof(identity.vendor_id));
	offset += sizeof(identity.vendor_id);
	memcpy(expected_serial + offset, identity.model, sizeof(identity.model));
	offset += sizeof(identity.model);
	memcpy(expected_serial + offset, identity.onu_mac,
	       sizeof(identity.onu_mac));
	offset += sizeof(identity.onu_mac);
	memcpy(expected_serial + offset, identity.hardware_version,
	       sizeof(identity.hardware_version));
	offset += sizeof(identity.hardware_version);
	memcpy(expected_serial + offset, identity.software_version,
	       sizeof(identity.software_version));
	assert(offset + sizeof(identity.software_version) ==
	       sizeof(expected_serial));

	length = airoha_ctc_build_variable_get_response(request,
		sizeof(request), &identity, NULL, &fec_mode, NULL, NULL, response,
		sizeof(response), &result);
	assert(length == 4 + AIROHA_CTC_ONU_SERIAL_NUMBER_LENGTH + 4 + 3 +
	       4 + AIROHA_CTC_CHIPSET_ID_LENGTH + 8 + 4);
	assert(result.descriptors == 5 && result.supported == 4);
	assert(!memcmp(response, (uint8_t[]){ 0xc7, 0x00, 0x01,
		AIROHA_CTC_ONU_SERIAL_NUMBER_LENGTH }, 4));
	assert(!memcmp(response + 4, expected_serial, sizeof(expected_serial)));
	offset = 4 + sizeof(expected_serial);
	assert(!memcmp(response + offset,
		(uint8_t[]){ 0xc7, 0x00, 0x02, 3, 'r', '1', '5' }, 7));
	offset += 7;
	assert(!memcmp(response + offset,
		(uint8_t[]){ 0xc7, 0x00, 0x03,
		AIROHA_CTC_CHIPSET_ID_LENGTH }, 4));
	assert(!memcmp(response + offset + 4, identity.chipset_id,
	       sizeof(identity.chipset_id)));
	offset += 4 + sizeof(identity.chipset_id);
	assert(!memcmp(response + offset,
		(uint8_t[]){ AIROHA_CTC_STANDARD_ATTRIBUTE_BRANCH, 0x01, 0x3a,
		AIROHA_CTC_STANDARD_FEC_MODE_LENGTH, 0x00, 0x00, 0x00,
		AIROHA_CTC_STANDARD_FEC_ENABLED }, 8));
	offset += 8;
	assert(!memcmp(response + offset,
		(uint8_t[]){ 0xc7, 0x12, 0x34,
		AIROHA_CTC_VARIABLE_STATUS_UNSUPPORTED }, 4));
	assert(airoha_ctc_build_variable_get_response(request,
		sizeof(request) - 1, &identity, NULL, &fec_mode, NULL, NULL, response,
		sizeof(response), &result) == -EINVAL);
	assert(airoha_ctc_build_variable_get_response(request,
		sizeof(request), &identity, NULL, &fec_mode, NULL, NULL, response, 8,
		&result) == -ENOSPC);

	assert(airoha_ctc_parse_fec_set(set_request, sizeof(set_request),
		&fec_set) == 0);
	assert(fec_set.present && fec_set.valid && fec_set.enabled);
	fec_set.applied = true;
	length = airoha_ctc_build_variable_set_response(set_request,
		sizeof(set_request), response, sizeof(response), &fec_set, NULL, NULL,
		&result);
	assert(length == (int)sizeof(set_expected));
	assert(!memcmp(response, set_expected, sizeof(set_expected)));
	assert(result.descriptors == 3 && result.supported == 1);
	assert(airoha_ctc_build_variable_set_response(set_request,
		sizeof(set_request) - 4, response, sizeof(response), &fec_set, NULL,
		NULL,
		&result) == -EINVAL);

	memcpy(response, set_request, sizeof(set_request));
	response[7] = 7;
	assert(airoha_ctc_parse_fec_set(response, sizeof(set_request),
		&fec_set) == 0);
	assert(fec_set.present && !fec_set.valid);
	assert(airoha_ctc_decode_standard_fec(
		(uint8_t[]){ 0, 0, 0, AIROHA_CTC_STANDARD_FEC_DISABLED }, 4,
		&fec_mode.enabled) == 0);
	assert(!fec_mode.enabled);
}

static void test_onu_capabilities(void)
{
	const uint8_t request[] = {
		AIROHA_CTC_EXTENDED_ATTRIBUTE_BRANCH, 0x00, 0x04,
		AIROHA_CTC_EXTENDED_ATTRIBUTE_BRANCH, 0x00, 0x07,
		AIROHA_CTC_EXTENDED_ATTRIBUTE_BRANCH, 0x00, 0x0c,
		AIROHA_CTC_EXTENDED_ATTRIBUTE_BRANCH, 0x00, 0x0d,
		0x00, 0x00, 0x00,
	};
	const uint8_t expected_1[AIROHA_CTC_ONU_CAPABILITIES_1_LENGTH] = {
		0x07, 0x01, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
		0x00, 0x01, 0x03, 0x00, 0x00, 0x00, 0x00, 0x00,
		0x00, 0x00, 0x07, 0x01, 0x00, 0x08, 0x08, 0x08,
		0x08, 0x00,
	};
	const uint8_t expected_2[AIROHA_CTC_ONU_CAPABILITIES_2_LENGTH] = {
		0x00, 0x00, 0x00, 0x00, 0x20, 0x00, 0x01, 0x00,
		0x05,
		0x00, 0x00, 0x00, 0x00, 0x00, 0x01,
		0x00, 0x00, 0x00, 0x01, 0x00, 0x03,
		0x00, 0x00, 0x00, 0x02, 0x00, 0x01,
		0x00, 0x00, 0x00, 0x06, 0x00, 0x01,
		0x00, 0x00, 0x00, 0x07, 0x00, 0x01,
		0x00,
	};
	const uint8_t expected_3[AIROHA_CTC_ONU_CAPABILITIES_3_LENGTH] = {
		0x01, 0x01, 0x01,
	};
	struct airoha_ctc_identity identity = { 0 };
	struct airoha_ctc_variable_result result = { 0 };
	uint8_t response[128] = { 0 };
	size_t offset = 0;
	int length;

	length = airoha_ctc_build_variable_get_response(request,
		sizeof(request), &identity, NULL, NULL, NULL, NULL, response,
		sizeof(response), &result);
	assert(length == 4 + (int)sizeof(expected_1) +
		4 + (int)sizeof(expected_2) + 4 + (int)sizeof(expected_3) + 4);
	assert(result.descriptors == 4 && result.supported == 3);

	assert(!memcmp(response + offset,
		(uint8_t[]){ 0xc7, 0x00, 0x04, sizeof(expected_1) }, 4));
	offset += 4;
	assert(!memcmp(response + offset, expected_1, sizeof(expected_1)));
	offset += sizeof(expected_1);
	assert(!memcmp(response + offset,
		(uint8_t[]){ 0xc7, 0x00, 0x07, sizeof(expected_2) }, 4));
	offset += 4;
	assert(!memcmp(response + offset, expected_2, sizeof(expected_2)));
	offset += sizeof(expected_2);
	assert(!memcmp(response + offset,
		(uint8_t[]){ 0xc7, 0x00, 0x0c, sizeof(expected_3) }, 4));
	offset += 4;
	assert(!memcmp(response + offset, expected_3, sizeof(expected_3)));
	offset += sizeof(expected_3);
	assert(!memcmp(response + offset,
		(uint8_t[]){ 0xc7, 0x00, 0x0d,
			AIROHA_CTC_VARIABLE_STATUS_UNSUPPORTED }, 4));

	assert(airoha_ctc_build_variable_get_response(request, sizeof(request),
		&identity, NULL, NULL, NULL, NULL, response, (size_t)length - 1,
		&result) == -ENOSPC);
}

static void test_optical_diagnostics(void)
{
	const uint8_t request[] = {
		AIROHA_CTC_EXTENDED_ATTRIBUTE_BRANCH, 0x00, 0x05,
		0x00, 0x00, 0x00,
	};
	const uint8_t expected[] = {
		0xc7, 0x00, 0x05,
		AIROHA_CTC_OPTICAL_TRANSCEIVER_DIAGNOSIS_LENGTH,
		0x64, 0x00, 0x80, 0xe8, 0x09, 0xc4, 0x27, 0x10, 0x00, 0x0a,
	};
	struct airoha_ctc_optical_diagnostics diagnostics = {
		.available = true,
		.temperature = 0x6400,
		.supply_voltage = 33000,
		.laser_bias_current = 2500,
		.transmit_power = 10000,
		.receive_power = 10,
	};
	struct airoha_ctc_identity identity = { 0 };
	struct airoha_ctc_variable_result result = { 0 };
	uint8_t response[32] = { 0 };
	int length;

	length = airoha_ctc_build_variable_get_response(request,
		sizeof(request), &identity, &diagnostics, NULL, NULL, NULL, response,
		sizeof(response), &result);
	assert(length == (int)sizeof(expected));
	assert(!memcmp(response, expected, sizeof(expected)));
	assert(result.descriptors == 1 && result.supported == 1);

	diagnostics.available = false;
	length = airoha_ctc_build_variable_get_response(request,
		sizeof(request), &identity, &diagnostics, NULL, NULL, NULL, response,
		sizeof(response), &result);
	assert(length == 4);
	assert(!memcmp(response,
		(uint8_t[]){ 0xc7, 0x00, 0x05,
			AIROHA_CTC_VARIABLE_STATUS_UNSUPPORTED }, 4));
	assert(result.descriptors == 1 && result.supported == 0);
}

static void test_object_scoped_eth_link_state(void)
{
	const uint8_t request[] = {
		AIROHA_CTC_EXTENDED_ATTRIBUTE_BRANCH, 0x00, 0x0b,
		AIROHA_CTC_OBJECT_INSTANCE_BRANCH_V1, 0x00, 0x00, 0x01, 0x01,
		AIROHA_CTC_EXTENDED_ATTRIBUTE_BRANCH, 0x00, 0x0b,
		AIROHA_CTC_OBJECT_INSTANCE_BRANCH_V2, 0x00, 0x01, 0x04,
			0x01, 0x00, 0x00, 0x02,
		AIROHA_CTC_EXTENDED_ATTRIBUTE_BRANCH, 0x00, 0x0b,
		AIROHA_CTC_OBJECT_INSTANCE_BRANCH_V1, 0x00, 0x00, 0x01, 0x05,
		AIROHA_CTC_EXTENDED_ATTRIBUTE_BRANCH, 0x00, 0x0b,
		AIROHA_CTC_OBJECT_INSTANCE_BRANCH_V1, 0x00, 0x00, 0x01, 0xff,
		AIROHA_CTC_EXTENDED_ATTRIBUTE_BRANCH, 0x00, 0x0b,
		AIROHA_CTC_OBJECT_INSTANCE_BRANCH_V1, 0x00, 0x00, 0x01, 0x03,
		AIROHA_CTC_EXTENDED_ATTRIBUTE_BRANCH, 0x00, 0x0b,
		0x00, 0x00, 0x00,
	};
	const uint8_t expected[] = {
		0xc7, 0x00, 0x0b, AIROHA_CTC_VARIABLE_STATUS_UNSUPPORTED,
		0x36, 0x00, 0x00, 0x01, 0x01,
		0xc7, 0x00, 0x0b, 0x01, 0x01,
		0x37, 0x00, 0x01, 0x04, 0x01, 0x00, 0x00, 0x02,
		0xc7, 0x00, 0x0b, 0x01, 0x00,
		0x36, 0x00, 0x00, 0x01, 0x05,
		0xc7, 0x00, 0x0b, AIROHA_CTC_VARIABLE_STATUS_UNSUPPORTED,
		0x36, 0x00, 0x00, 0x01, 0xff,
		0xc7, 0x00, 0x0b, AIROHA_CTC_VARIABLE_STATUS_UNSUPPORTED,
		0x36, 0x00, 0x00, 0x01, 0x03,
		0xc7, 0x00, 0x0b, 0x01, 0x01,
	};
	struct airoha_ctc_identity identity = { 0 };
	struct airoha_ctc_uni_links links = {
		.available = { true, true, true, false },
		.link_up = { true, false, true, false },
	};
	struct airoha_ctc_variable_result result = { 0 };
	uint8_t response[128] = { 0 };
	int length;

	length = airoha_ctc_build_variable_get_response(request,
		sizeof(request), &identity, NULL, NULL, &links, NULL, response,
		sizeof(response), &result);
	assert(length == (int)sizeof(expected));
	assert(!memcmp(response, expected, sizeof(expected)));
	assert(result.descriptors == 6 && result.supported == 3);
}

static void test_object_boundaries_and_set_context(void)
{
	const uint8_t malformed_v1[] = {
		AIROHA_CTC_OBJECT_INSTANCE_BRANCH_V1, 0x00, 0x00, 0x01,
	};
	const uint8_t bad_width[] = {
		AIROHA_CTC_OBJECT_INSTANCE_BRANCH_V2, 0x00, 0x01, 0x01, 0x01,
	};
	const uint8_t set_request[] = {
		AIROHA_CTC_OBJECT_INSTANCE_BRANCH_V1_ALT,
			0x00, 0x00, 0x01, 0x01,
		AIROHA_CTC_STANDARD_ATTRIBUTE_BRANCH, 0x01, 0x3a, 0x04,
			0x00, 0x00, 0x00, AIROHA_CTC_STANDARD_FEC_ENABLED,
		0x00, 0x00, 0x00,
	};
	const uint8_t expected[] = {
		AIROHA_CTC_OBJECT_INSTANCE_BRANCH_V1_ALT,
			0x00, 0x00, 0x01, 0x01,
		AIROHA_CTC_STANDARD_ATTRIBUTE_BRANCH, 0x01, 0x3a,
			AIROHA_CTC_VARIABLE_STATUS_SUCCESS,
	};
	struct airoha_ctc_identity identity = { 0 };
	struct airoha_ctc_variable_result result = { 0 };
	struct airoha_ctc_fec_set fec = { 0 };
	uint8_t response[32] = { 0 };
	int length;

	assert(airoha_ctc_build_variable_get_response(malformed_v1,
		sizeof(malformed_v1), &identity, NULL, NULL, NULL, NULL, response,
		sizeof(response), &result) == -EINVAL);
	assert(airoha_ctc_build_variable_get_response(bad_width,
		sizeof(bad_width), &identity, NULL, NULL, NULL, NULL, response,
		sizeof(response), &result) == -EINVAL);
	assert(airoha_ctc_build_variable_get_response(set_request, 5,
		&identity, NULL, NULL, NULL, NULL, response, 4,
		&result) == -ENOSPC);

	assert(airoha_ctc_parse_fec_set(set_request, sizeof(set_request),
		&fec) == 0);
	assert(fec.present && fec.valid && fec.enabled);
	fec.applied = true;
	length = airoha_ctc_build_variable_set_response(set_request,
		sizeof(set_request), response, sizeof(response), &fec, NULL, NULL,
		&result);
	assert(length == (int)sizeof(expected));
	assert(!memcmp(response, expected, sizeof(expected)));
	assert(result.descriptors == 1 && result.supported == 1);
}

static void test_holdover_config(void)
{
	const uint8_t get_request[] = {
		AIROHA_CTC_EXTENDED_ATTRIBUTE_BRANCH, 0x00, 0x06,
		0x00, 0x00, 0x00,
	};
	const uint8_t get_expected[] = {
		AIROHA_CTC_EXTENDED_ATTRIBUTE_BRANCH, 0x00, 0x06,
		AIROHA_CTC_HOLDOVER_CONFIG_LENGTH, 0x01, 0x01, 0xf4,
	};
	uint8_t set_request[] = {
		AIROHA_CTC_EXTENDED_ATTRIBUTE_BRANCH, 0x00, 0x06,
		AIROHA_CTC_HOLDOVER_CONFIG_LENGTH, 0x01, 0x01, 0xf4,
		0x00, 0x00, 0x00,
	};
	const uint8_t set_expected[] = {
		AIROHA_CTC_EXTENDED_ATTRIBUTE_BRANCH, 0x00, 0x06,
		AIROHA_CTC_VARIABLE_STATUS_SUCCESS,
	};
	struct airoha_ctc_holdover_config config = {
		.available = true,
		.enabled = true,
		.time_ms = 500,
	};
	struct airoha_ctc_holdover_set set = { 0 };
	struct airoha_ctc_identity identity = { 0 };
	struct airoha_ctc_variable_result result = { 0 };
	uint8_t response[32] = { 0 };
	bool enabled;
	uint16_t time_ms;
	int length;

	length = airoha_ctc_build_variable_get_response(get_request,
		sizeof(get_request), &identity, NULL, NULL, NULL, &config,
		response, sizeof(response), &result);
	assert(length == (int)sizeof(get_expected));
	assert(!memcmp(response, get_expected, sizeof(get_expected)));
	assert(result.descriptors == 1 && result.supported == 1);

	config.available = false;
	length = airoha_ctc_build_variable_get_response(get_request,
		sizeof(get_request), &identity, NULL, NULL, NULL, &config,
		response, sizeof(response), &result);
	assert(length == 4 && response[3] ==
	       AIROHA_CTC_VARIABLE_STATUS_UNSUPPORTED);

	assert(airoha_ctc_parse_holdover_set(set_request,
		sizeof(set_request), &set) == 0);
	assert(set.present && set.valid && set.enabled && set.time_ms == 500);
	set.applied = true;
	length = airoha_ctc_build_variable_set_response(set_request,
		sizeof(set_request), response, sizeof(response), NULL, &set, NULL,
		&result);
	assert(length == (int)sizeof(set_expected));
	assert(!memcmp(response, set_expected, sizeof(set_expected)));
	assert(result.descriptors == 1 && result.supported == 1);

	assert(airoha_ctc_decode_holdover(
		(uint8_t[]){ 0, 0, AIROHA_CTC_HOLDOVER_TIME_MIN_MS }, 3,
		&enabled, &time_ms) == 0);
	assert(!enabled && time_ms == AIROHA_CTC_HOLDOVER_TIME_MIN_MS);
	assert(airoha_ctc_decode_holdover(
		(uint8_t[]){ 1, 0x03, 0xe8 }, 3, &enabled, &time_ms) == 0);
	assert(enabled && time_ms == AIROHA_CTC_HOLDOVER_TIME_MAX_MS);
	assert(airoha_ctc_decode_holdover(
		(uint8_t[]){ 2, 0x01, 0xf4 }, 3,
		&enabled, &time_ms) == -EINVAL);
	assert(airoha_ctc_decode_holdover(
		(uint8_t[]){ 1, 0x00, 0x31 }, 3,
		&enabled, &time_ms) == -ERANGE);
	set_request[5] = 0x00;
	set_request[6] = 0x31;
	assert(airoha_ctc_parse_holdover_set(set_request,
		sizeof(set_request), &set) == 0);
	assert(set.present && !set.valid);
}

static void test_ctc_uni_management_wire_format(void)
{
	const uint8_t request[] = {
		AIROHA_CTC_OBJECT_INSTANCE_BRANCH_V1,
			0x00, AIROHA_CTC_OBJECT_PORT, 0x01, 0x01,
		AIROHA_CTC_EXTENDED_ATTRIBUTE_BRANCH, 0x00,
			AIROHA_CTC_ETHERNET_PAUSE, 0x01, 0x01,
		AIROHA_CTC_EXTENDED_ATTRIBUTE_BRANCH, 0x00,
			AIROHA_CTC_UPSTREAM_POLICING, 0x0a,
			0x01, 0x00, 0x03, 0xe8, 0x00, 0x07, 0xd0,
			0x00, 0x0b, 0xb8,
		AIROHA_CTC_OBJECT_INSTANCE_BRANCH_V2, 0x00,
			AIROHA_CTC_OBJECT_PORT, 0x04, 0x01, 0x00, 0xff, 0xff,
		AIROHA_CTC_EXTENDED_ATTRIBUTE_BRANCH, 0x00,
			AIROHA_CTC_DOWNSTREAM_RATE_LIMITING, 0x07,
			0x01, 0x00, 0x13, 0x88, 0x00, 0x27, 0x10,
		AIROHA_CTC_EXTENDED_ATTRIBUTE_BRANCH, 0x00,
			AIROHA_CTC_PORT_MAC_AGING_TIME, 0x04,
			0x00, 0x00, 0x01, 0x2c,
		0x00, 0x00, 0x00,
	};
	const uint8_t expected[] = {
		AIROHA_CTC_OBJECT_INSTANCE_BRANCH_V1,
			0x00, AIROHA_CTC_OBJECT_PORT, 0x01, 0x01,
		AIROHA_CTC_EXTENDED_ATTRIBUTE_BRANCH, 0x00,
			AIROHA_CTC_ETHERNET_PAUSE,
			AIROHA_CTC_VARIABLE_STATUS_SUCCESS,
		AIROHA_CTC_EXTENDED_ATTRIBUTE_BRANCH, 0x00,
			AIROHA_CTC_UPSTREAM_POLICING,
			AIROHA_CTC_VARIABLE_STATUS_SUCCESS,
		AIROHA_CTC_OBJECT_INSTANCE_BRANCH_V2, 0x00,
			AIROHA_CTC_OBJECT_PORT, 0x04, 0x01, 0x00, 0xff, 0xff,
		AIROHA_CTC_EXTENDED_ATTRIBUTE_BRANCH, 0x00,
			AIROHA_CTC_DOWNSTREAM_RATE_LIMITING,
			AIROHA_CTC_VARIABLE_STATUS_SUCCESS,
		AIROHA_CTC_EXTENDED_ATTRIBUTE_BRANCH, 0x00,
			AIROHA_CTC_PORT_MAC_AGING_TIME,
			AIROHA_CTC_VARIABLE_STATUS_SUCCESS,
	};
	const uint8_t conflicting[] = {
		AIROHA_CTC_OBJECT_INSTANCE_BRANCH_V1,
			0x00, AIROHA_CTC_OBJECT_PORT, 0x01, 0x02,
		AIROHA_CTC_EXTENDED_ATTRIBUTE_BRANCH, 0x00,
			AIROHA_CTC_ETHERNET_PAUSE, 0x01, 0x01,
		AIROHA_CTC_EXTENDED_ATTRIBUTE_BRANCH, 0x00,
			AIROHA_CTC_ETHERNET_PAUSE, 0x01, 0x00,
		0x00, 0x00, 0x00,
	};
	const uint8_t unscoped[] = {
		AIROHA_CTC_EXTENDED_ATTRIBUTE_BRANCH, 0x00,
			AIROHA_CTC_ETHERNET_PAUSE, 0x01, 0x01,
		0x00, 0x00, 0x00,
	};
	struct airoha_ctc_management_set management;
	struct airoha_ctc_variable_result result;
	uint8_t response[128] = { 0 };
	unsigned int uni;
	int length;

	assert(airoha_ctc_parse_management_set(request, sizeof(request),
		&management) == 0);
	assert(management.uni[0].pause_present &&
	       management.uni[0].pause_valid &&
	       management.uni[0].pause_enabled);
	assert(management.uni[0].upstream_present &&
	       management.uni[0].upstream_valid &&
	       management.uni[0].upstream_enabled);
	assert(management.uni[0].upstream_cir_kbps == 1000);
	assert(management.uni[0].upstream_cbs_bytes == 2000);
	assert(management.uni[0].upstream_ebs_bytes == 3000);
	for (uni = 0; uni < AIROHA_CTC_ETHERNET_UNI_COUNT; uni++) {
		assert(management.uni[uni].downstream_present);
		assert(management.uni[uni].downstream_valid);
		assert(management.uni[uni].downstream_enabled);
		assert(management.uni[uni].downstream_cir_kbps == 5000);
		assert(management.uni[uni].downstream_pir_kbps == 10000);
		management.uni[uni].downstream_applied = true;
	}
	assert(management.aging_present && management.aging_valid &&
	       management.aging_time_seconds == 300);
	management.uni[0].pause_applied = true;
	management.uni[0].upstream_applied = true;
	management.aging_applied = true;
	length = airoha_ctc_build_variable_set_response(request,
		sizeof(request), response, sizeof(response), NULL, NULL,
		&management, &result);
	assert(length == (int)sizeof(expected));
	assert(!memcmp(response, expected, sizeof(expected)));
	assert(result.descriptors == 4 && result.supported == 4);

	management.uni[2].downstream_applied = false;
	length = airoha_ctc_build_variable_set_response(request,
		sizeof(request), response, sizeof(response), NULL, NULL,
		&management, &result);
	assert(length == (int)sizeof(expected));
	assert(response[sizeof(expected) - 5] ==
	       AIROHA_CTC_VARIABLE_STATUS_UNSUPPORTED);
	assert(result.supported == 3);

	assert(airoha_ctc_parse_management_set(conflicting,
		sizeof(conflicting), &management) == 0);
	assert(management.uni[1].pause_present &&
	       !management.uni[1].pause_valid);
	assert(airoha_ctc_parse_management_set(unscoped, sizeof(unscoped),
		&management) == 0);
	for (uni = 0; uni < AIROHA_CTC_ETHERNET_UNI_COUNT; uni++)
		assert(management.uni[uni].pause_present &&
		       !management.uni[uni].pause_valid);
	assert(airoha_ctc_decode_downstream_rate(
		(uint8_t[]){ 1, 0, 0x27, 0x10, 0, 0x13, 0x88 }, 7,
		&management.uni[0].downstream_enabled,
		&management.uni[0].downstream_cir_kbps,
		&management.uni[0].downstream_pir_kbps) == -ERANGE);
	assert(airoha_ctc_decode_upstream_policing(
		(uint8_t[]){ 0 }, 1, &management.uni[0].upstream_enabled,
		&management.uni[0].upstream_cir_kbps,
		&management.uni[0].upstream_cbs_bytes,
		&management.uni[0].upstream_ebs_bytes) == 0);
}

static void test_ctc_classification_wire_format(void)
{
	static const uint8_t value_lengths[AIROHA_CTC_CLASSIFICATION_MATCH_MAX] = {
		6, 6, 1, 2, 2, 4, 4, 1, 1, 1, 2, 2, 1, 3, 16, 16, 16, 16, 1,
	};
	const uint8_t all_fields[] = {
		AIROHA_CTC_CLASSIFICATION_ADD, 1,
		1, 225, 7, AIROHA_CTC_CLASSIFICATION_PRIORITY_UNCHANGED, 19,
		0, 0x00, 0x11, 0x22, 0x33, 0x44, 0x55,
			AIROHA_CTC_CLASSIFICATION_OP_EQUAL,
		1, 0x66, 0x77, 0x88, 0x99, 0xaa, 0xbb,
			AIROHA_CTC_CLASSIFICATION_OP_NOT_EQUAL,
		2, 0, 0, 0, 0, 0, 7, AIROHA_CTC_CLASSIFICATION_OP_LESS_OR_EQUAL,
		3, 0, 0, 0, 0, 0x0f, 0xff,
			AIROHA_CTC_CLASSIFICATION_OP_GREATER_OR_EQUAL,
		4, 0, 0, 0, 0, 0x86, 0xdd, AIROHA_CTC_CLASSIFICATION_OP_EQUAL,
		5, 0, 0, 192, 0, 2, 1, AIROHA_CTC_CLASSIFICATION_OP_EQUAL,
		6, 0, 0, 198, 51, 100, 2, AIROHA_CTC_CLASSIFICATION_OP_EQUAL,
		7, 0, 0, 0, 0, 0, 255, AIROHA_CTC_CLASSIFICATION_OP_EXISTS,
		8, 0, 0, 0, 0, 0, 63, AIROHA_CTC_CLASSIFICATION_OP_EQUAL,
		9, 0, 0, 0, 0, 0, 63, AIROHA_CTC_CLASSIFICATION_OP_EQUAL,
		10, 0, 0, 0, 0, 0, 0, AIROHA_CTC_CLASSIFICATION_OP_ALWAYS,
		11, 0, 0, 0, 0, 0xff, 0xff,
			AIROHA_CTC_CLASSIFICATION_OP_EQUAL,
		12, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 6,
			AIROHA_CTC_CLASSIFICATION_OP_EQUAL,
		13, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
			0x0f, 0xff, 0xff, AIROHA_CTC_CLASSIFICATION_OP_EQUAL,
		14, 0x20, 0x01, 0x0d, 0xb8, 0, 0, 0, 0,
			0, 0, 0, 0, 0, 0, 0, 1, AIROHA_CTC_CLASSIFICATION_OP_EQUAL,
		15, 0x20, 0x01, 0x0d, 0xb8, 0, 0, 0, 0,
			0, 0, 0, 0, 0, 0, 0, 2, AIROHA_CTC_CLASSIFICATION_OP_NOT_EQUAL,
		16, 0x20, 0x01, 0x0d, 0xb8, 0, 0, 0, 0,
			0, 0, 0, 0, 0, 0, 0, 120,
			AIROHA_CTC_CLASSIFICATION_OP_LESS_OR_EQUAL,
		17, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
			AIROHA_CTC_CLASSIFICATION_OP_GREATER_OR_EQUAL,
		18, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 59,
			AIROHA_CTC_CLASSIFICATION_OP_EQUAL,
	};
	const uint8_t bad_pbit[] = {
		AIROHA_CTC_CLASSIFICATION_ADD, 1, 1, 11, 0, 0xff, 1,
		2, 0, 0, 0, 0, 0, 8, AIROHA_CTC_CLASSIFICATION_OP_EQUAL,
	};
	const uint8_t bad_dscp[] = {
		AIROHA_CTC_CLASSIFICATION_ADD, 1, 1, 11, 0, 0xff, 1,
		8, 0, 0, 0, 0, 0, 64, AIROHA_CTC_CLASSIFICATION_OP_EQUAL,
	};
	const uint8_t bad_ip_version[] = {
		AIROHA_CTC_CLASSIFICATION_ADD, 1, 1, 21, 0, 0xff, 1,
		12, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 5,
			AIROHA_CTC_CLASSIFICATION_OP_EQUAL,
	};
	const uint8_t bad_prefix[] = {
		AIROHA_CTC_CLASSIFICATION_ADD, 1, 1, 21, 0, 0xff, 1,
		16, 0x20, 0x01, 0x0d, 0xb8, 0, 0, 0, 0,
			0, 0, 0, 0, 0, 0, 0, 121,
			AIROHA_CTC_CLASSIFICATION_OP_EQUAL,
	};
	const uint8_t clear[] = { AIROHA_CTC_CLASSIFICATION_CLEAR };
	struct airoha_ctc_uni_policy decoded;
	struct airoha_ctc_management_set management;
	struct airoha_ctc_variable_result result;
	uint8_t malformed[sizeof(all_fields)];
	uint8_t request[8 + 4 + sizeof(all_fields) + 3] = { 0 };
	uint8_t response[32] = { 0 };
	size_t offset;
	unsigned int field, uni;
	int length;

	assert(sizeof(all_fields) == 229);
	assert(airoha_ctc_decode_classification(all_fields, sizeof(all_fields),
		&decoded) == 0);
	assert(decoded.classification_present && decoded.classification_valid &&
	       decoded.classification_action == AIROHA_CTC_CLASSIFICATION_ADD &&
	       decoded.classification_rule_count == 1);
	assert(decoded.classification_rule[0].precedence == 1 &&
	       decoded.classification_rule[0].queue == 7 &&
	       decoded.classification_rule[0].priority == 0xff &&
	       decoded.classification_rule[0].match_count == 19);
	for (field = 0; field < AIROHA_CTC_CLASSIFICATION_MATCH_MAX; field++) {
		assert(decoded.classification_rule[0].match[field].field == field);
		assert(decoded.classification_rule[0].match[field].value_length ==
		       value_lengths[field]);
	}
	assert(decoded.classification_rule[0].match[2].value[0] == 7);
	assert(decoded.classification_rule[0].match[3].value[0] == 0x0f &&
	       decoded.classification_rule[0].match[3].value[1] == 0xff);
	assert(decoded.classification_rule[0].match[13].value[0] == 0x0f &&
	       decoded.classification_rule[0].match[13].value[2] == 0xff);
	assert(decoded.classification_rule[0].match[16].value[15] == 120);
	assert(decoded.classification_rule[0].match[17].value[15] == 0);

	memcpy(malformed, all_fields, sizeof(malformed));
	malformed[3]--;
	assert(airoha_ctc_decode_classification(malformed, sizeof(malformed),
		&decoded) == -EINVAL);
	memcpy(malformed, all_fields, sizeof(malformed));
	malformed[4] = 8;
	assert(airoha_ctc_decode_classification(malformed, sizeof(malformed),
		&decoded) == -ERANGE);
	memcpy(malformed, all_fields, sizeof(malformed));
	malformed[5] = 8;
	assert(airoha_ctc_decode_classification(malformed, sizeof(malformed),
		&decoded) == -ERANGE);
	memcpy(malformed, all_fields, sizeof(malformed));
	malformed[14] = 8;
	assert(airoha_ctc_decode_classification(malformed, sizeof(malformed),
		&decoded) == -ERANGE);
	assert(airoha_ctc_decode_classification(all_fields,
		sizeof(all_fields) - 1, &decoded) == -EINVAL);
	assert(airoha_ctc_decode_classification(bad_pbit, sizeof(bad_pbit),
		&decoded) == -ERANGE);
	assert(airoha_ctc_decode_classification(bad_dscp, sizeof(bad_dscp),
		&decoded) == -ERANGE);
	assert(airoha_ctc_decode_classification(bad_ip_version,
		sizeof(bad_ip_version), &decoded) == -ERANGE);
	assert(airoha_ctc_decode_classification(bad_prefix, sizeof(bad_prefix),
		&decoded) == -ERANGE);
	assert(airoha_ctc_decode_classification(clear, sizeof(clear), &decoded) == 0);
	assert(airoha_ctc_decode_classification(
		(uint8_t[]){ AIROHA_CTC_CLASSIFICATION_CLEAR, 0 }, 2,
		&decoded) == -EINVAL);

	request[0] = AIROHA_CTC_OBJECT_INSTANCE_BRANCH_V1;
	request[1] = 0;
	request[2] = AIROHA_CTC_OBJECT_PORT;
	request[3] = 1;
	request[4] = 2;
	request[5] = AIROHA_CTC_EXTENDED_ATTRIBUTE_BRANCH;
	request[6] = 0;
	request[7] = AIROHA_CTC_CLASSIFICATION_MARKING;
	request[8] = sizeof(all_fields);
	memcpy(request + 9, all_fields, sizeof(all_fields));
	assert(airoha_ctc_parse_management_set(request, 9 + sizeof(all_fields) + 3,
		&management) == 0);
	assert(!management.uni[0].classification_present);
	assert(management.uni[1].classification_present &&
	       management.uni[1].classification_valid);
	management.uni[1].classification_applied = true;
	length = airoha_ctc_build_variable_set_response(request,
		9 + sizeof(all_fields) + 3, response, sizeof(response), NULL, NULL,
		&management, &result);
	assert(length == 9);
	assert(!memcmp(response, request, 5));
	assert(!memcmp(response + 5, request + 5, 3));
	assert(response[8] == AIROHA_CTC_VARIABLE_STATUS_SUCCESS);
	assert(result.descriptors == 1 && result.supported == 1);
	management.uni[1].classification_applied = false;
	length = airoha_ctc_build_variable_set_response(request,
		9 + sizeof(all_fields) + 3, response, sizeof(response), NULL, NULL,
		&management, &result);
	assert(length == 9 && response[8] ==
	       AIROHA_CTC_VARIABLE_STATUS_UNSUPPORTED && result.supported == 0);

	memset(request, 0, sizeof(request));
	request[0] = AIROHA_CTC_OBJECT_INSTANCE_BRANCH_V2;
	request[1] = 0;
	request[2] = AIROHA_CTC_OBJECT_PORT;
	request[3] = 4;
	request[4] = 1;
	request[5] = 0;
	request[6] = 0xff;
	request[7] = 0xff;
	request[8] = AIROHA_CTC_EXTENDED_ATTRIBUTE_BRANCH;
	request[9] = 0;
	request[10] = AIROHA_CTC_CLASSIFICATION_MARKING;
	request[11] = sizeof(all_fields);
	memcpy(request + 12, all_fields, sizeof(all_fields));
	offset = 12 + sizeof(all_fields);
	assert(airoha_ctc_parse_management_set(request, offset + 3,
		&management) == 0);
	for (uni = 0; uni < AIROHA_CTC_ETHERNET_UNI_COUNT; uni++)
		assert(management.uni[uni].classification_present &&
		       management.uni[uni].classification_valid);

	assert(airoha_ctc_parse_management_set(request + 8,
		4 + sizeof(all_fields) + 3, &management) == 0);
	for (uni = 0; uni < AIROHA_CTC_ETHERNET_UNI_COUNT; uni++)
		assert(management.uni[uni].classification_present &&
		       !management.uni[uni].classification_valid);
}

static void test_ctc_vlan_wire_format(void)
{
	const uint8_t transparent[] = { AIROHA_CTC_VLAN_MODE_TRANSPARENT };
	const uint8_t tag[] = {
		AIROHA_CTC_VLAN_MODE_TAG, 0x81, 0x00, 0x00, 0x64,
	};
	const uint8_t translation[] = {
		AIROHA_CTC_VLAN_MODE_TRANSLATION, 0x81, 0x00, 0x00, 0x64,
		0x81, 0x00, 0x00, 0x0a, 0x81, 0x00, 0x00, 0xc8,
		0x88, 0xa8, 0x00, 0x14, 0x81, 0x00, 0x01, 0x2c,
	};
	const uint8_t n_to_one[] = {
		AIROHA_CTC_VLAN_MODE_N_TO_ONE, 0x81, 0x00, 0x00, 0x64,
		0x00, 0x01,
		0x00, 0x02, 0x81, 0x00, 0x01, 0x90,
		0x81, 0x00, 0x00, 0x0a, 0x81, 0x00, 0x00, 0x14,
	};
	const uint8_t trunk[] = {
		AIROHA_CTC_VLAN_MODE_TRUNK, 0x81, 0x00, 0x00, 0x64,
		0x81, 0x00, 0x00, 0x64, 0x88, 0xa8, 0x00, 0xc8,
	};
	const uint8_t request[] = {
		AIROHA_CTC_OBJECT_INSTANCE_BRANCH_V2,
			0x00, AIROHA_CTC_OBJECT_PORT, 0x04, 0x01, 0x00, 0x00, 0x02,
		AIROHA_CTC_EXTENDED_ATTRIBUTE_BRANCH, 0x00, AIROHA_CTC_VLAN,
			sizeof(trunk),
		AIROHA_CTC_VLAN_MODE_TRUNK, 0x81, 0x00, 0x00, 0x64,
		0x81, 0x00, 0x00, 0x64, 0x88, 0xa8, 0x00, 0xc8,
		0x00, 0x00, 0x00,
	};
	const uint8_t expected[] = {
		AIROHA_CTC_OBJECT_INSTANCE_BRANCH_V2,
			0x00, AIROHA_CTC_OBJECT_PORT, 0x04, 0x01, 0x00, 0x00, 0x02,
		AIROHA_CTC_EXTENDED_ATTRIBUTE_BRANCH, 0x00, AIROHA_CTC_VLAN,
			AIROHA_CTC_VARIABLE_STATUS_SUCCESS,
	};
	const uint8_t duplicate_translation[] = {
		AIROHA_CTC_VLAN_MODE_TRANSLATION, 0x81, 0x00, 0x00, 0x64,
		0x81, 0x00, 0x00, 0x0a, 0x81, 0x00, 0x00, 0xc8,
		0x81, 0x00, 0x00, 0x14, 0x81, 0x00, 0x00, 0xc8,
	};
	const uint8_t bad_n_to_one[] = {
		AIROHA_CTC_VLAN_MODE_N_TO_ONE, 0x81, 0x00, 0x00, 0x64,
		0x00, 0x01, 0x00, 0x01, 0x81, 0x00, 0x01, 0x90,
		0x88, 0xa8, 0x00, 0x0a,
	};
	struct airoha_ctc_uni_policy policy;
	struct airoha_ctc_management_set management;
	struct airoha_ctc_variable_result result;
	uint8_t response[64] = { 0 };
	int length;

	assert(airoha_ctc_decode_vlan(transparent, sizeof(transparent),
		&policy) == 0);
	assert(policy.vlan_mode == AIROHA_CTC_VLAN_MODE_TRANSPARENT &&
	       policy.vlan_rule_count == 0);
	assert(airoha_ctc_decode_vlan(tag, sizeof(tag), &policy) == 0);
	assert(policy.vlan_default_tag == UINT32_C(0x81000064));
	assert(airoha_ctc_decode_vlan(translation, sizeof(translation),
		&policy) == 0);
	assert(policy.vlan_rule_count == 2 &&
	       policy.vlan_old_tag[1] == UINT32_C(0x88a80014) &&
	       policy.vlan_new_tag[1] == UINT32_C(0x8100012c));
	assert(airoha_ctc_decode_vlan(n_to_one, sizeof(n_to_one), &policy) == 0);
	assert(policy.vlan_rule_count == 2 &&
	       policy.vlan_new_tag[0] == policy.vlan_new_tag[1]);
	assert(airoha_ctc_decode_vlan(trunk, sizeof(trunk), &policy) == 0);
	assert(policy.vlan_rule_count == 2 &&
	       policy.vlan_old_tag[1] == policy.vlan_new_tag[1]);

	assert(airoha_ctc_parse_management_set(request, sizeof(request),
		&management) == 0);
	assert(management.uni[1].vlan_present &&
	       management.uni[1].vlan_valid &&
	       management.uni[1].vlan_mode == AIROHA_CTC_VLAN_MODE_TRUNK);
	management.uni[1].vlan_applied = true;
	length = airoha_ctc_build_variable_set_response(request,
		sizeof(request), response, sizeof(response), NULL, NULL,
		&management, &result);
	assert(length == (int)sizeof(expected));
	assert(!memcmp(response, expected, sizeof(expected)));
	assert(result.descriptors == 1 && result.supported == 1);

	assert(airoha_ctc_decode_vlan(tag, sizeof(tag) - 1, &policy) == -EINVAL);
	assert(airoha_ctc_decode_vlan(duplicate_translation,
		sizeof(duplicate_translation), &policy) == -EINVAL);
	assert(airoha_ctc_decode_vlan(bad_n_to_one, sizeof(bad_n_to_one),
		&policy) == -EINVAL);
}

static void write_carrier(const char *root, const char *netdev,
			  const char *value)
{
	char directory[512], path[512];
	FILE *file;

	assert(snprintf(directory, sizeof(directory), "%s/%s", root, netdev) > 0);
	assert(mkdir(directory, 0700) == 0);
	assert(snprintf(path, sizeof(path), "%s/carrier", directory) > 0);
	file = fopen(path, "w");
	assert(file);
	assert(fputs(value, file) >= 0);
	assert(fclose(file) == 0);
}

static void test_ctc_phy_action_wire_format(void)
{
	const uint8_t request[] = {
		AIROHA_CTC_OBJECT_INSTANCE_BRANCH_V1,
			0x00, AIROHA_CTC_OBJECT_PORT, 0x01, 0x01,
		AIROHA_CTC_STANDARD_ACTION_BRANCH, 0x00,
			AIROHA_CTC_PHY_ADMIN_CONTROL, 0x04,
			0x00, 0x00, 0x00, AIROHA_CTC_ADMIN_ENABLED,
		AIROHA_CTC_STANDARD_ACTION_BRANCH, 0x00,
			AIROHA_CTC_AUTONEG_ADMIN_CONTROL, 0x04,
			0x00, 0x00, 0x00, AIROHA_CTC_ADMIN_DISABLED,
		AIROHA_CTC_STANDARD_ACTION_BRANCH, 0x00,
			AIROHA_CTC_AUTONEG_RESTART, 0x00,
		0x00, 0x00, 0x00,
	};
	const uint8_t expected[] = {
		AIROHA_CTC_OBJECT_INSTANCE_BRANCH_V1,
			0x00, AIROHA_CTC_OBJECT_PORT, 0x01, 0x01,
		AIROHA_CTC_STANDARD_ACTION_BRANCH, 0x00,
			AIROHA_CTC_PHY_ADMIN_CONTROL,
			AIROHA_CTC_VARIABLE_STATUS_SUCCESS,
		AIROHA_CTC_STANDARD_ACTION_BRANCH, 0x00,
			AIROHA_CTC_AUTONEG_ADMIN_CONTROL,
			AIROHA_CTC_VARIABLE_STATUS_SUCCESS,
		AIROHA_CTC_STANDARD_ACTION_BRANCH, 0x00,
			AIROHA_CTC_AUTONEG_RESTART,
			AIROHA_CTC_VARIABLE_STATUS_SUCCESS,
	};
	const uint8_t conflicting[] = {
		AIROHA_CTC_OBJECT_INSTANCE_BRANCH_V1,
			0x00, AIROHA_CTC_OBJECT_PORT, 0x01, 0x02,
		AIROHA_CTC_STANDARD_ACTION_BRANCH, 0x00,
			AIROHA_CTC_PHY_ADMIN_CONTROL, 0x04, 0, 0, 0, 1,
		AIROHA_CTC_STANDARD_ACTION_BRANCH, 0x00,
			AIROHA_CTC_PHY_ADMIN_CONTROL, 0x04, 0, 0, 0, 2,
		0x00, 0x00, 0x00,
	};
	const uint8_t bad_restart[] = {
		AIROHA_CTC_OBJECT_INSTANCE_BRANCH_V1,
			0x00, AIROHA_CTC_OBJECT_PORT, 0x01, 0x03,
		AIROHA_CTC_STANDARD_ACTION_BRANCH, 0x00,
			AIROHA_CTC_AUTONEG_RESTART, 0x01, 0,
		0x00, 0x00, 0x00,
	};
	struct airoha_ctc_management_set management;
	struct airoha_ctc_variable_result result;
	uint8_t response[64] = { 0 };
	int length;

	assert(airoha_ctc_parse_management_set(request, sizeof(request),
		&management) == 0);
	assert(management.uni[0].phy_admin_present &&
	       management.uni[0].phy_admin_valid &&
	       management.uni[0].phy_admin_enabled);
	assert(management.uni[0].autoneg_present &&
	       management.uni[0].autoneg_valid &&
	       !management.uni[0].autoneg_enabled);
	assert(management.uni[0].autoneg_restart_present &&
	       management.uni[0].autoneg_restart_valid);
	management.uni[0].phy_admin_applied = true;
	management.uni[0].autoneg_applied = true;
	management.uni[0].autoneg_restart_applied = true;
	length = airoha_ctc_build_variable_set_response(request,
		sizeof(request), response, sizeof(response), NULL, NULL,
		&management, &result);
	assert(length == (int)sizeof(expected));
	assert(!memcmp(response, expected, sizeof(expected)));
	assert(result.descriptors == 3 && result.supported == 3);

	assert(airoha_ctc_parse_management_set(conflicting,
		sizeof(conflicting), &management) == 0);
	assert(management.uni[1].phy_admin_present &&
	       !management.uni[1].phy_admin_valid);
	assert(airoha_ctc_parse_management_set(bad_restart,
		sizeof(bad_restart), &management) == 0);
	assert(management.uni[2].autoneg_restart_present &&
	       !management.uni[2].autoneg_restart_valid);
	assert(airoha_ctc_decode_admin_control(
		(uint8_t[]){ 0, 0, 0, 3 }, 4,
		&management.uni[0].phy_admin_enabled) == -EINVAL);
}

static void remove_carrier(const char *root, const char *netdev)
{
	char directory[512], path[512];

	assert(snprintf(directory, sizeof(directory), "%s/%s", root, netdev) > 0);
	assert(snprintf(path, sizeof(path), "%s/carrier", directory) > 0);
	assert(unlink(path) == 0);
	assert(rmdir(directory) == 0);
}

static void test_uni_link_sysfs_backend(void)
{
	char root[] = "/tmp/airoha-ctc-links-XXXXXX";
	const char *const netdevs[AIROHA_CTC_ETHERNET_UNI_COUNT] = {
		"lan1", "lan2", "lan3", "lan4",
	};
	struct airoha_ctc_uni_links links;
	char path[512];
	FILE *file;

	assert(mkdtemp(root));
	write_carrier(root, "lan1", "1\n");
	write_carrier(root, "lan2", "0\n");
	write_carrier(root, "lan3", "2\n");
	airoha_ctc_read_uni_links(root, netdevs, &links);
	assert(links.available[0] && links.link_up[0]);
	assert(links.available[1] && !links.link_up[1]);
	assert(!links.available[2] && !links.link_up[2]);
	assert(!links.available[3] && !links.link_up[3]);

	assert(snprintf(path, sizeof(path), "%s/lan1/carrier", root) > 0);
	file = fopen(path, "w");
	assert(file);
	assert(fputs("0\n", file) >= 0);
	assert(fclose(file) == 0);
	airoha_ctc_read_uni_links(root, netdevs, &links);
	assert(links.available[0] && !links.link_up[0]);

	remove_carrier(root, "lan1");
	remove_carrier(root, "lan2");
	remove_carrier(root, "lan3");
	assert(rmdir(root) == 0);
}

int main(void)
{
	test_authentication();
	test_churning();
	test_organization_frames();
	test_dba();
	test_extended_variables();
	test_onu_capabilities();
	test_optical_diagnostics();
	test_object_scoped_eth_link_state();
	test_object_boundaries_and_set_context();
	test_holdover_config();
	test_ctc_uni_management_wire_format();
	test_ctc_classification_wire_format();
	test_ctc_vlan_wire_format();
	test_ctc_phy_action_wire_format();
	test_uni_link_sysfs_backend();
	return 0;
}
