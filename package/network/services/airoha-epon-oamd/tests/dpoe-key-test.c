/* SPDX-License-Identifier: GPL-2.0-only */
#include <assert.h>
#include <errno.h>
#include <stdint.h>
#include <string.h>

#include "../src/airoha-epon-dpoe.h"

int main(void)
{
	const uint8_t onu_mac[6] = { 0x02, 0xaa, 0xbb, 0xcc, 0xdd, 0xee };
	const uint8_t first[16] = {
		0x00, 0x11, 0x22, 0x33, 0x44, 0x55, 0x66, 0x77,
		0x88, 0x99, 0xaa, 0xbb, 0xcc, 0xdd, 0xee, 0xff,
	};
	const uint8_t replacement[16] = { 0xff };
	const uint8_t prefix[] = {
		0x01, 0x80, 0xc2, 0x00, 0x00, 0x02,
		0x02, 0xaa, 0xbb, 0xcc, 0xdd, 0xee,
		0x88, 0x09, 0x03, 0x00, 0x50, 0xfe,
		0x00, 0x10, 0x00, 0x08, 0x01, 0x10,
	};
	struct airoha_dpoe_key_exchange exchange = { 0 };
	struct airoha_dpoe_rekey_timer timer = { 0 };
	uint8_t frame[AIROHA_DPOE_KEY_FRAME_LENGTH] = { 0 };
	const uint8_t sia_set[] = {
		0x00, 0x10, 0x00, AIROHA_DPOE_SIA_SET_REQUEST,
		0xd7, 0x04, 0x01, 0x02, 0x00, 0x0a,
		0xd7, 0x04, 0x02, 0x01, 0x01,
		0xd7, 0x06, 0x05, 0x02, 0x01, 0x00,
		0xd7, 0x12, 0x34, 0x01, 0xff,
		0x00, 0x00, 0x00,
	};
	const uint8_t sia_response_expected[] = {
		0x00, 0x10, 0x00, AIROHA_DPOE_SIA_SET_RESPONSE,
		0xd7, 0x04, 0x01, AIROHA_DPOE_SIA_STATUS_SUCCESS,
		0xd7, 0x04, 0x02, AIROHA_DPOE_SIA_STATUS_SUCCESS,
		0xd7, 0x06, 0x05, AIROHA_DPOE_SIA_STATUS_SUCCESS,
		0xd7, 0x12, 0x34, AIROHA_DPOE_SIA_STATUS_UNSUPPORTED,
	};
	struct airoha_dpoe_sia_set_result sia_result = { 0 };
	struct airoha_dpoe_fec_set fec = { 0 };
	uint8_t sia_response[sizeof(sia_response_expected)] = { 0 };
	const uint8_t sia_get[] = {
		0x00, 0x10, 0x00, AIROHA_DPOE_SIA_GET_REQUEST,
		0x07, 0x01, 0x42,
		0xd7, 0x00, 0x02,
		0xd7, 0x00, 0x08,
		0xd7, 0x00, 0x09,
		0xd7, 0x00, 0x13,
		0xd7, 0x02, 0x1d,
		0xd7, 0x02, 0x1e,
		0xd7, 0x02, 0x1f,
		0xd7, 0x02, 0x20,
		0xd7, 0x02, 0x21,
		0xd7, 0x12, 0x34,
		0x00, 0x00, 0x00,
	};
	const uint8_t sia_get_expected_10g_1g[] = {
		0x00, 0x10, 0x00, AIROHA_DPOE_SIA_GET_RESPONSE,
		0x07, 0x01, 0x42, 0x08, 1, 2, 3, 4, 5, 6, 7, 8,
		0xd7, 0x00, 0x02, 0x03, 'r', '3', '1',
		0xd7, 0x00, 0x08, 0x02, 0x00, 0x04,
		0xd7, 0x00, 0x09, 0x02, 0x00, 0x04,
		0xd7, 0x00, 0x13, 0x06, '1', '0', 'G', '/', '1', 'G',
		0xd7, 0x02, 0x1d, 0x02, 0x12, 0x34,
		0xd7, 0x02, 0x1e, 0x02, 0x23, 0x45,
		0xd7, 0x02, 0x1f, 0x02, 0x34, 0x56,
		0xd7, 0x02, 0x20, 0x02, 0x45, 0x67,
		0xd7, 0x02, 0x21, 0x02, 0x56, 0x78,
		0xd7, 0x12, 0x34, AIROHA_DPOE_SIA_STATUS_UNSUPPORTED_GET,
	};
	const uint8_t line_rate_get[] = {
		0x00, 0x10, 0x00, AIROHA_DPOE_SIA_GET_REQUEST,
		0xd7, 0x00, 0x13,
	};
	const uint8_t line_rate_expected_10g_10g[] = {
		0x00, 0x10, 0x00, AIROHA_DPOE_SIA_GET_RESPONSE,
		0xd7, 0x00, 0x13, 0x07, '1', '0', 'G', '/', '1', '0', 'G',
	};
	const uint8_t diagnostic_get[] = {
		0x00, 0x10, 0x00, AIROHA_DPOE_SIA_GET_REQUEST,
		0xd7, 0x02, 0x1d,
	};
	const uint8_t diagnostic_error_expected[] = {
		0x00, 0x10, 0x00, AIROHA_DPOE_SIA_GET_RESPONSE,
		0xd7, 0x02, 0x1d, AIROHA_DPOE_SIA_STATUS_READ_ERROR,
	};
	const uint8_t inventory_get[] = {
		0x00, 0x10, 0x00, AIROHA_DPOE_SIA_GET_REQUEST,
		0xd7, 0x00, 0x06,
		0xd7, 0x00, 0x07,
		0xd7, 0x00, 0x08,
	};
	const uint8_t inventory_get_expected[86] = {
		[0] = 0x00, [1] = 0x10, [2] = 0x00,
		[3] = AIROHA_DPOE_SIA_GET_RESPONSE,
		[4] = 0xd7, [5] = 0x00, [6] = 0x06,
		[7] = AIROHA_DPOE_SIA_LLID_COUNT_LENGTH,
		[72] = 0xd7, [73] = 0x00, [74] = 0x07,
		[75] = AIROHA_DPOE_SIA_PON_PORT_COUNT_LENGTH,
		[76] = 0x00, [77] = 0x01, [78] = 0x00, [79] = 0x01,
		[80] = 0xd7, [81] = 0x00, [82] = 0x08,
		[83] = AIROHA_DPOE_SIA_UNI_PORT_COUNT_LENGTH,
		[84] = 0x00, [85] = 0x04,
	};
	const uint8_t truncated_get[] = {
		0x00, 0x10, 0x00, AIROHA_DPOE_SIA_GET_REQUEST, 0xd7, 0x00,
	};
	const uint8_t illegal_get_tail[] = {
		0x00, 0x10, 0x00, AIROHA_DPOE_SIA_GET_REQUEST,
		0x00, 0x00, 0x00, 0xd7, 0x00, 0x02,
	};
	const uint8_t padded_get[] = {
		0x00, 0x10, 0x00, AIROHA_DPOE_SIA_GET_REQUEST,
		0xd7, 0x00, 0x13,
		0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
	};
	const uint8_t truncated_set[] = {
		0x00, 0x10, 0x00, AIROHA_DPOE_SIA_SET_REQUEST,
		0xd7, 0x04, 0x01, 0x02, 0x00,
	};
	struct airoha_dpoe_inventory inventory = {
		.onu_id = { 1, 2, 3, 4, 5, 6, 7, 8 },
		.firmware_version = { .value = { 'r', '3', '1' }, .length = 3 },
		.line_rate_mode = {
			.value = { '1', '0', 'G', '/', '1', 'G' }, .length = 6,
		},
		.pon_port_count = { 1, 1 },
		.uni_port_count = 4,
		.packet_buffer_kib = 4,
		.uni_port_type = 1,
	};
	struct airoha_dpoe_optical_diagnostics diagnostics = {
		.available = true,
		.temperature = 0x1234,
		.supply_voltage = 0x2345,
		.laser_bias_current = 0x3456,
		.transmit_power = 0x4567,
		.receive_power = 0x5678,
	};
	struct airoha_dpoe_sia_get_result get_result = { 0 };
	uint8_t get_response[sizeof(sia_get_expected_10g_1g)] = { 0 };
	uint8_t inventory_response[sizeof(inventory_get_expected)] = { 0 };
	uint8_t inventory_value[AIROHA_DPOE_SIA_LLID_COUNT_LENGTH] = { 0 };
	int sia_length;

	assert(!airoha_dpoe_exchange_begin(&exchange, 0, first));
	assert(exchange.pending && !exchange.installed);
	assert(exchange.key_index == 1);
	assert(!memcmp(exchange.key, first, sizeof(first)));
	/* A duplicate event must retain the key already programmed for retry. */
	assert(!airoha_dpoe_exchange_begin(&exchange, 0, replacement));
	assert(!memcmp(exchange.key, first, sizeof(first)));
	assert(airoha_dpoe_build_key_frame(onu_mac, exchange.key_index,
		exchange.key, frame, sizeof(frame)) == (int)sizeof(frame));
	assert(!memcmp(frame, prefix, sizeof(prefix)));
	assert(!memcmp(frame + sizeof(prefix), first, sizeof(first)));
	assert(frame[40] == 0 && frame[41] == 0);
	assert(airoha_dpoe_build_key_frame(onu_mac, 2, first, frame,
		sizeof(frame)) == -EINVAL);
	assert(airoha_dpoe_build_key_frame(onu_mac, 0, first, frame,
		sizeof(frame) - 1) == -ENOSPC);

	airoha_dpoe_exchange_clear(&exchange);
	assert(!exchange.pending && !exchange.installed);
	assert(exchange.key_index == 0);
	assert(!memcmp(exchange.key, (uint8_t[16]){ 0 }, sizeof(exchange.key)));

	assert(airoha_dpoe_rekey_schedule(&timer, 100));
	assert(timer.armed && timer.deadline_ms == 10100);
	/* Stock AddTimer() does not restart an already-active one-shot timer. */
	assert(!airoha_dpoe_rekey_schedule(&timer, 5000));
	assert(timer.deadline_ms == 10100);
	assert(!airoha_dpoe_rekey_consume(&timer, 10099));
	assert(timer.armed);
	assert(airoha_dpoe_rekey_consume(&timer, 10100));
	assert(!timer.armed && timer.deadline_ms == 0);
	assert(!airoha_dpoe_rekey_consume(&timer, 10100));
	assert(airoha_dpoe_rekey_schedule(&timer, 20000));
	assert(timer.deadline_ms == 30000);
	airoha_dpoe_rekey_cancel(&timer);
	assert(!timer.armed && timer.deadline_ms == 0);

	assert(!airoha_dpoe_parse_extended_fec(sia_set, sizeof(sia_set), &fec));
	assert(fec.present && fec.valid && fec.rx_enabled && !fec.tx_enabled);
	fec.applied = true;
	sia_length = airoha_dpoe_build_sia_set_response(sia_set,
		sizeof(sia_set), sia_response, sizeof(sia_response), &fec, &sia_result);
	assert(sia_length == (int)sizeof(sia_response_expected));
	assert(!memcmp(sia_response, sia_response_expected,
		sizeof(sia_response_expected)));
	assert(sia_result.descriptors == 4 && sia_result.supported == 3);
	assert(sia_result.arm_rekey && sia_result.encryption_mode_seen);
	assert(airoha_dpoe_build_sia_set_response(sia_set,
		sizeof(sia_set), sia_response, sizeof(sia_response) - 1,
		&fec, &sia_result) == -ENOSPC);
	assert(airoha_dpoe_build_sia_set_response(truncated_set,
		sizeof(truncated_set), sia_response, sizeof(sia_response),
		&fec, &sia_result) == -EINVAL);

	sia_length = airoha_dpoe_build_sia_get_response(sia_get,
		sizeof(sia_get), get_response, sizeof(get_response), &inventory,
		&diagnostics, &get_result);
	assert(sia_length == (int)sizeof(sia_get_expected_10g_1g));
	assert(!memcmp(get_response, sia_get_expected_10g_1g,
		sizeof(sia_get_expected_10g_1g)));
	assert(get_result.descriptors == 11 && get_result.supported == 10);
	assert(get_result.read_errors == 0);
	assert(airoha_dpoe_build_sia_get_response(sia_get, sizeof(sia_get),
		get_response, sizeof(get_response) - 1, &inventory, &diagnostics,
		&get_result) == -ENOSPC);

	sia_length = airoha_dpoe_build_sia_get_response(inventory_get,
		sizeof(inventory_get), inventory_response,
		sizeof(inventory_response), &inventory, &diagnostics, &get_result);
	assert(sia_length == (int)sizeof(inventory_get_expected));
	assert(!memcmp(inventory_response, inventory_get_expected,
		sizeof(inventory_get_expected)));
	assert(get_result.descriptors == 3 && get_result.supported == 3);
	assert(get_result.read_errors == 0);
	assert(airoha_dpoe_build_sia_get_response(inventory_get,
		sizeof(inventory_get), inventory_response,
		sizeof(inventory_response) - 1, &inventory, &diagnostics,
		&get_result) == -ENOSPC);
	assert(airoha_dpoe_get_sia_value(AIROHA_DPOE_SIA_ATTRIBUTE_BRANCH,
		AIROHA_DPOE_SIA_LLID_COUNT, &inventory, &diagnostics,
		inventory_value, AIROHA_DPOE_SIA_LLID_COUNT_LENGTH - 1) == -ENOSPC);
	assert(airoha_dpoe_get_sia_value(AIROHA_DPOE_SIA_ATTRIBUTE_BRANCH,
		AIROHA_DPOE_SIA_PON_PORT_COUNT, &inventory, &diagnostics,
		inventory_value, AIROHA_DPOE_SIA_PON_PORT_COUNT_LENGTH - 1) ==
		-ENOSPC);
	assert(airoha_dpoe_get_sia_value(AIROHA_DPOE_SIA_ATTRIBUTE_BRANCH,
		AIROHA_DPOE_SIA_UNI_PORT_COUNT, &inventory, &diagnostics,
		inventory_value, AIROHA_DPOE_SIA_UNI_PORT_COUNT_LENGTH - 1) ==
		-ENOSPC);

	memcpy(inventory.line_rate_mode.value, "10G/10G", 7);
	inventory.line_rate_mode.length = 7;
	sia_length = airoha_dpoe_build_sia_get_response(line_rate_get,
		sizeof(line_rate_get), get_response, sizeof(get_response), &inventory,
		&diagnostics, &get_result);
	assert(sia_length == (int)sizeof(line_rate_expected_10g_10g));
	assert(!memcmp(get_response, line_rate_expected_10g_10g,
		sizeof(line_rate_expected_10g_10g)));
	sia_length = airoha_dpoe_build_sia_get_response(padded_get,
		sizeof(padded_get), get_response, sizeof(get_response), &inventory,
		&diagnostics, &get_result);
	assert(sia_length == (int)sizeof(line_rate_expected_10g_10g));
	assert(!memcmp(get_response, line_rate_expected_10g_10g,
		sizeof(line_rate_expected_10g_10g)));

	diagnostics.available = false;
	sia_length = airoha_dpoe_build_sia_get_response(diagnostic_get,
		sizeof(diagnostic_get), get_response, sizeof(get_response), &inventory,
		&diagnostics, &get_result);
	assert(sia_length == (int)sizeof(diagnostic_error_expected));
	assert(!memcmp(get_response, diagnostic_error_expected,
		sizeof(diagnostic_error_expected)));
	assert(get_result.descriptors == 1 && get_result.supported == 0);
	assert(get_result.read_errors == 1);
	assert(airoha_dpoe_build_sia_get_response(truncated_get,
		sizeof(truncated_get), get_response, sizeof(get_response), &inventory,
		&diagnostics, &get_result) == -EINVAL);
	assert(airoha_dpoe_build_sia_get_response(illegal_get_tail,
		sizeof(illegal_get_tail), get_response, sizeof(get_response), &inventory,
		&diagnostics, &get_result) == -EINVAL);

	assert(airoha_dpoe_decode_extended_fec((uint8_t[]){ 0, 1 }, 2,
		&fec.rx_enabled, &fec.tx_enabled) == 0);
	assert(!fec.rx_enabled && fec.tx_enabled);
	assert(airoha_dpoe_decode_extended_fec((uint8_t[]){ 0, 2 }, 2,
		&fec.rx_enabled, &fec.tx_enabled) == -EINVAL);
	assert(airoha_dpoe_decode_extended_fec((uint8_t[]){ 0 }, 1,
		&fec.rx_enabled, &fec.tx_enabled) == -EINVAL);
	return 0;
}
