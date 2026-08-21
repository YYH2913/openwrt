/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef _AIROHA_EPON_CTC_H_
#define _AIROHA_EPON_CTC_H_

#include <errno.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "airoha-epon-ieee.h"

#define AIROHA_CTC_OPCODE_AUTHENTICATION 0x05
#define AIROHA_CTC_OPCODE_VARIABLE_REQUEST 0x01
#define AIROHA_CTC_OPCODE_VARIABLE_RESPONSE 0x02
#define AIROHA_CTC_OPCODE_SET_REQUEST 0x03
#define AIROHA_CTC_OPCODE_SET_RESPONSE 0x04
#define AIROHA_CTC_OPCODE_CHURNING 0x09
#define AIROHA_CTC_OPCODE_DBA 0x0a
#define AIROHA_CTC_AUTH_REQUEST 0x01
#define AIROHA_CTC_AUTH_RESPONSE 0x02
#define AIROHA_CTC_AUTH_SUCCESS 0x03
#define AIROHA_CTC_AUTH_FAILURE 0x04
#define AIROHA_CTC_AUTH_LOID_PASSWORD 0x01
#define AIROHA_CTC_AUTH_NAK 0x02
#define AIROHA_CTC_LOID_LENGTH 24
#define AIROHA_CTC_PASSWORD_LENGTH 12
#define AIROHA_CTC_AUTH_CREDENTIAL_RESPONSE_LENGTH 40
#define AIROHA_CTC_CHURNING_KEY_LENGTH 9
#define AIROHA_CTC_CHURNING_REQUEST 0x00
#define AIROHA_CTC_CHURNING_RESPONSE 0x01
#define AIROHA_CTC_CHURNING_RESPONSE_LENGTH 11
#define AIROHA_CTC_OAM_HEADER_LENGTH 18
#define AIROHA_CTC_ORGANIZATION_HEADER_LENGTH 4
#define AIROHA_CTC_DBA_GET_REQUEST 0x00
#define AIROHA_CTC_DBA_GET_RESPONSE 0x01
#define AIROHA_CTC_DBA_SET_REQUEST 0x02
#define AIROHA_CTC_DBA_SET_RESPONSE 0x03
#define AIROHA_CTC_DBA_ACK 0x01
#define AIROHA_CTC_DBA_NACK 0x00
#define AIROHA_CTC_DBA_QUEUE_SET_COUNT 4
#define AIROHA_CTC_DBA_THRESHOLD_SET_COUNT 3
#define AIROHA_CTC_DBA_QUEUE_COUNT 8
#define AIROHA_CTC_DBA_SET_BYTES_PER_SET 17
#define AIROHA_CTC_DBA_RESPONSE_MAX 54
#define AIROHA_CTC_EXTENDED_ATTRIBUTE_BRANCH 0xc7
#define AIROHA_CTC_STANDARD_ACTION_BRANCH 0x09
/* Stock CTC V1/V2 request parser consumes 0x36/0x37; accept D6/D7 peers too. */
#define AIROHA_CTC_OBJECT_INSTANCE_BRANCH_V1 0x36
#define AIROHA_CTC_OBJECT_INSTANCE_BRANCH_V2 0x37
#define AIROHA_CTC_OBJECT_INSTANCE_BRANCH_V1_ALT 0xd6
#define AIROHA_CTC_OBJECT_INSTANCE_BRANCH_V2_ALT 0xd7
#define AIROHA_CTC_OBJECT_PORT 0x0001
#define AIROHA_CTC_PORT_ETHER 0x01
#define AIROHA_CTC_PORT_ALL 0xff
#define AIROHA_CTC_PORT_ETHER_ALL 0xffff
#define AIROHA_CTC_STANDARD_ATTRIBUTE_BRANCH AIROHA_IEEE_ATTRIBUTE_BRANCH
#define AIROHA_CTC_ONU_SERIAL_NUMBER 0x0001
#define AIROHA_CTC_FIRMWARE_VERSION 0x0002
#define AIROHA_CTC_CHIPSET_ID 0x0003
#define AIROHA_CTC_ONU_CAPABILITIES_1 0x0004
#define AIROHA_CTC_OPTICAL_TRANSCEIVER_DIAGNOSIS 0x0005
#define AIROHA_CTC_HOLDOVER_CONFIG 0x0006
#define AIROHA_CTC_ONU_CAPABILITIES_2 0x0007
#define AIROHA_CTC_ONU_CAPABILITIES_3 0x000c
#define AIROHA_CTC_ETH_LINK_STATE 0x000b
#define AIROHA_CTC_ETHERNET_PAUSE 0x0011
#define AIROHA_CTC_UPSTREAM_POLICING 0x0012
#define AIROHA_CTC_DOWNSTREAM_RATE_LIMITING 0x0015
#define AIROHA_CTC_PORT_MAC_AGING_TIME 0x0016
#define AIROHA_CTC_CLASSIFICATION_MARKING 0x0021
#define AIROHA_CTC_VLAN 0x00a4
#define AIROHA_CTC_PHY_ADMIN_CONTROL 0x0005
#define AIROHA_CTC_AUTONEG_RESTART 0x000b
#define AIROHA_CTC_AUTONEG_ADMIN_CONTROL 0x000c
#define AIROHA_CTC_STANDARD_FEC_MODE 0x013a
#define AIROHA_CTC_STANDARD_FEC_MODE_LENGTH 4
#define AIROHA_CTC_STANDARD_FEC_ENABLED 2
#define AIROHA_CTC_STANDARD_FEC_DISABLED 3
#define AIROHA_CTC_VARIABLE_STATUS_SUCCESS 0x80
#define AIROHA_CTC_VARIABLE_STATUS_UNSUPPORTED 0x86
#define AIROHA_CTC_ONU_SERIAL_NUMBER_LENGTH 38
#define AIROHA_CTC_CHIPSET_ID_LENGTH 8
#define AIROHA_CTC_OPTICAL_TRANSCEIVER_DIAGNOSIS_LENGTH 10
#define AIROHA_CTC_HOLDOVER_CONFIG_LENGTH 3
#define AIROHA_CTC_HOLDOVER_TIME_MIN_MS 50
#define AIROHA_CTC_HOLDOVER_TIME_MAX_MS 1000
#define AIROHA_CTC_ONU_CAPABILITIES_1_LENGTH 26
#define AIROHA_CTC_ONU_CAPABILITIES_2_LENGTH 40
#define AIROHA_CTC_ONU_CAPABILITIES_3_LENGTH 3
#define AIROHA_CTC_FIRMWARE_VERSION_MAX 127
#define AIROHA_CTC_ETHERNET_UNI_COUNT 4
#define AIROHA_CTC_NETDEV_PATH_MAX 256
#define AIROHA_CTC_ETHERNET_PAUSE_LENGTH 1
#define AIROHA_CTC_UPSTREAM_POLICING_DISABLED_LENGTH 1
#define AIROHA_CTC_UPSTREAM_POLICING_ENABLED_LENGTH 10
#define AIROHA_CTC_DOWNSTREAM_RATE_DISABLED_LENGTH 1
#define AIROHA_CTC_DOWNSTREAM_RATE_ENABLED_LENGTH 7
#define AIROHA_CTC_PORT_MAC_AGING_TIME_LENGTH 4
#define AIROHA_CTC_ADMIN_CONTROL_LENGTH 4
#define AIROHA_CTC_ADMIN_ENABLED 1
#define AIROHA_CTC_ADMIN_DISABLED 2
#define AIROHA_CTC_VLAN_MODE_TRANSPARENT 0
#define AIROHA_CTC_VLAN_MODE_TAG 1
#define AIROHA_CTC_VLAN_MODE_TRANSLATION 2
#define AIROHA_CTC_VLAN_MODE_N_TO_ONE 3
#define AIROHA_CTC_VLAN_MODE_TRUNK 4
#define AIROHA_CTC_VLAN_RULE_MAX 8
#define AIROHA_CTC_VLAN_TPID_8021Q 0x8100
#define AIROHA_CTC_VLAN_TPID_8021AD 0x88a8
#define AIROHA_CTC_CLASSIFICATION_DELETE 0
#define AIROHA_CTC_CLASSIFICATION_ADD 1
#define AIROHA_CTC_CLASSIFICATION_CLEAR 2
#define AIROHA_CTC_CLASSIFICATION_RULE_MAX 64
#define AIROHA_CTC_CLASSIFICATION_MATCH_MAX 19
#define AIROHA_CTC_CLASSIFICATION_VALUE_MAX 16
#define AIROHA_CTC_CLASSIFICATION_PRIORITY_UNCHANGED 0xff
#define AIROHA_CTC_CLASSIFICATION_OP_NEVER 0
#define AIROHA_CTC_CLASSIFICATION_OP_EQUAL 1
#define AIROHA_CTC_CLASSIFICATION_OP_NOT_EQUAL 2
#define AIROHA_CTC_CLASSIFICATION_OP_LESS_OR_EQUAL 3
#define AIROHA_CTC_CLASSIFICATION_OP_GREATER_OR_EQUAL 4
#define AIROHA_CTC_CLASSIFICATION_OP_EXISTS 5
#define AIROHA_CTC_CLASSIFICATION_OP_NOT_EXISTS 6
#define AIROHA_CTC_CLASSIFICATION_OP_ALWAYS 7

enum airoha_ctc_classification_field {
	AIROHA_CTC_CLASSIFICATION_DMAC,
	AIROHA_CTC_CLASSIFICATION_SMAC,
	AIROHA_CTC_CLASSIFICATION_PBIT,
	AIROHA_CTC_CLASSIFICATION_VLAN_ID,
	AIROHA_CTC_CLASSIFICATION_ETHERTYPE,
	AIROHA_CTC_CLASSIFICATION_IPV4_DADDR,
	AIROHA_CTC_CLASSIFICATION_IPV4_SADDR,
	AIROHA_CTC_CLASSIFICATION_IPV4_PROTOCOL,
	AIROHA_CTC_CLASSIFICATION_IPV4_DSCP,
	AIROHA_CTC_CLASSIFICATION_IPV6_DSCP,
	AIROHA_CTC_CLASSIFICATION_L4_SPORT,
	AIROHA_CTC_CLASSIFICATION_L4_DPORT,
	AIROHA_CTC_CLASSIFICATION_IP_VERSION,
	AIROHA_CTC_CLASSIFICATION_IPV6_FLOW_LABEL,
	AIROHA_CTC_CLASSIFICATION_IPV6_DADDR,
	AIROHA_CTC_CLASSIFICATION_IPV6_SADDR,
	AIROHA_CTC_CLASSIFICATION_IPV6_DPREFIX,
	AIROHA_CTC_CLASSIFICATION_IPV6_SPREFIX,
	AIROHA_CTC_CLASSIFICATION_IPV6_PROTOCOL,
};

/* XG2010G values produced by the corresponding stock CTC getters. */
static const uint8_t airoha_ctc_onu_capabilities_1[] = {
	0x07, 0x01, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
	0x00, 0x01, 0x03, 0x00, 0x00, 0x00, 0x00, 0x00,
	0x00, 0x00, 0x07, 0x01, 0x00, 0x08, 0x08, 0x08,
	0x08, 0x00,
};

static const uint8_t airoha_ctc_onu_capabilities_2[] = {
	0x00, 0x00, 0x00, 0x00, 0x20, 0x00, 0x01, 0x00,
	0x05,
	0x00, 0x00, 0x00, 0x00, 0x00, 0x01,
	0x00, 0x00, 0x00, 0x01, 0x00, 0x03,
	0x00, 0x00, 0x00, 0x02, 0x00, 0x01,
	0x00, 0x00, 0x00, 0x06, 0x00, 0x01,
	0x00, 0x00, 0x00, 0x07, 0x00, 0x01,
	0x00,
};

static const uint8_t airoha_ctc_onu_capabilities_3[] = {
	0x01, 0x01, 0x01,
};

struct airoha_ctc_identity {
	uint8_t vendor_id[4];
	uint8_t model[4];
	uint8_t onu_mac[6];
	uint8_t hardware_version[8];
	uint8_t software_version[16];
	uint8_t chipset_id[AIROHA_CTC_CHIPSET_ID_LENGTH];
	uint8_t firmware_version[AIROHA_CTC_FIRMWARE_VERSION_MAX];
	size_t firmware_version_length;
};

struct airoha_ctc_variable_result {
	unsigned int descriptors;
	unsigned int supported;
};

struct airoha_ctc_fec_mode {
	bool available;
	bool enabled;
};

struct airoha_ctc_optical_diagnostics {
	bool available;
	uint16_t temperature;
	uint16_t supply_voltage;
	uint16_t laser_bias_current;
	uint16_t transmit_power;
	uint16_t receive_power;
};

struct airoha_ctc_fec_set {
	bool present;
	bool valid;
	bool enabled;
	bool applied;
};

struct airoha_ctc_holdover_config {
	bool available;
	bool enabled;
	uint16_t time_ms;
};

struct airoha_ctc_holdover_set {
	bool present;
	bool valid;
	bool enabled;
	bool applied;
	uint16_t time_ms;
};

struct airoha_ctc_object_context {
	bool present;
	bool valid;
	bool all;
	uint8_t branch;
	uint16_t leaf;
	uint32_t instance;
	unsigned int uni;
};

struct airoha_ctc_uni_links {
	bool available[AIROHA_CTC_ETHERNET_UNI_COUNT];
	bool link_up[AIROHA_CTC_ETHERNET_UNI_COUNT];
};

struct airoha_ctc_classification_match {
	uint8_t field;
	uint8_t operation;
	uint8_t value_length;
	uint8_t value[AIROHA_CTC_CLASSIFICATION_VALUE_MAX];
};

struct airoha_ctc_classification_rule {
	uint8_t precedence;
	uint8_t queue;
	uint8_t priority;
	uint8_t match_count;
	struct airoha_ctc_classification_match
		match[AIROHA_CTC_CLASSIFICATION_MATCH_MAX];
};

struct airoha_ctc_uni_policy {
	bool phy_admin_present;
	bool phy_admin_valid;
	bool phy_admin_enabled;
	bool phy_admin_applied;
	bool autoneg_present;
	bool autoneg_valid;
	bool autoneg_enabled;
	bool autoneg_applied;
	bool autoneg_restart_present;
	bool autoneg_restart_valid;
	bool autoneg_restart_applied;
	bool pause_present;
	bool pause_valid;
	bool pause_enabled;
	bool pause_applied;
	bool upstream_present;
	bool upstream_valid;
	bool upstream_enabled;
	bool upstream_applied;
	uint32_t upstream_cir_kbps;
	uint32_t upstream_cbs_bytes;
	uint32_t upstream_ebs_bytes;
	bool downstream_present;
	bool downstream_valid;
	bool downstream_enabled;
	bool downstream_applied;
	uint32_t downstream_cir_kbps;
	uint32_t downstream_pir_kbps;
	bool vlan_present;
	bool vlan_valid;
	bool vlan_applied;
	uint8_t vlan_mode;
	uint8_t vlan_rule_count;
	uint32_t vlan_default_tag;
	uint32_t vlan_old_tag[AIROHA_CTC_VLAN_RULE_MAX];
	uint32_t vlan_new_tag[AIROHA_CTC_VLAN_RULE_MAX];
	bool classification_present;
	bool classification_valid;
	bool classification_applied;
	uint8_t classification_action;
	uint8_t classification_rule_count;
	struct airoha_ctc_classification_rule
		classification_rule[AIROHA_CTC_CLASSIFICATION_RULE_MAX];
};

struct airoha_ctc_management_set {
	struct airoha_ctc_uni_policy uni[AIROHA_CTC_ETHERNET_UNI_COUNT];
	bool aging_present;
	bool aging_valid;
	bool aging_applied;
	uint32_t aging_time_seconds;
};

static inline bool airoha_ctc_is_object_branch(uint8_t branch)
{
	return branch == AIROHA_CTC_OBJECT_INSTANCE_BRANCH_V1 ||
	       branch == AIROHA_CTC_OBJECT_INSTANCE_BRANCH_V2 ||
	       branch == AIROHA_CTC_OBJECT_INSTANCE_BRANCH_V1_ALT ||
	       branch == AIROHA_CTC_OBJECT_INSTANCE_BRANCH_V2_ALT;
}

static inline bool airoha_ctc_is_object_v1(uint8_t branch)
{
	return branch == AIROHA_CTC_OBJECT_INSTANCE_BRANCH_V1 ||
	       branch == AIROHA_CTC_OBJECT_INSTANCE_BRANCH_V1_ALT;
}

static inline int airoha_ctc_parse_object(
		const uint8_t *descriptor, size_t remaining,
		struct airoha_ctc_object_context *object, size_t *consumed)
{
	size_t width;
	uint32_t instance;
	uint8_t port_type, port;

	if (!descriptor || !object || !consumed || remaining < 4 ||
	    !airoha_ctc_is_object_branch(descriptor[0]))
		return -EINVAL;
	width = airoha_ctc_is_object_v1(descriptor[0]) ? 1 : 4;
	if (descriptor[3] != width || remaining < 4 + width)
		return -EINVAL;

	memset(object, 0, sizeof(*object));
	object->present = true;
	object->branch = descriptor[0];
	object->leaf = (uint16_t)descriptor[1] << 8 | descriptor[2];
	if (width == 1) {
		instance = descriptor[4];
		object->instance = instance;
		object->all = instance == AIROHA_CTC_PORT_ALL;
		if (!object->all && instance >= 1 &&
		    instance <= AIROHA_CTC_ETHERNET_UNI_COUNT) {
			object->valid = true;
			object->uni = instance;
		}
	} else {
		instance = (uint32_t)descriptor[4] << 24 |
			   (uint32_t)descriptor[5] << 16 |
			   (uint32_t)descriptor[6] << 8 | descriptor[7];
		object->instance = instance;
		port_type = instance >> 24;
		port = instance & 0xff;
		object->all = object->leaf == AIROHA_CTC_OBJECT_PORT &&
			      (instance & 0xffff) == AIROHA_CTC_PORT_ETHER_ALL;
		if (!object->all && object->leaf == AIROHA_CTC_OBJECT_PORT &&
		    port_type == AIROHA_CTC_PORT_ETHER && port >= 1 &&
		    port <= AIROHA_CTC_ETHERNET_UNI_COUNT) {
			object->valid = true;
			object->uni = port;
		}
	}
	*consumed = 4 + width;
	return 0;
}

static inline void airoha_ctc_read_uni_links(const char *sysfs_root,
		const char *const netdevs[AIROHA_CTC_ETHERNET_UNI_COUNT],
		struct airoha_ctc_uni_links *links)
{
	char path[AIROHA_CTC_NETDEV_PATH_MAX];
	char line[8];
	unsigned int index;

	if (!links)
		return;
	memset(links, 0, sizeof(*links));
	if (!sysfs_root || !netdevs)
		return;
	for (index = 0; index < AIROHA_CTC_ETHERNET_UNI_COUNT; index++) {
		FILE *file;
		int length;

		if (!netdevs[index] || !netdevs[index][0])
			continue;
		length = snprintf(path, sizeof(path), "%s/%s/carrier",
				  sysfs_root, netdevs[index]);
		if (length < 0 || (size_t)length >= sizeof(path))
			continue;
		file = fopen(path, "re");
		if (!file)
			continue;
		if (fgets(line, sizeof(line), file) &&
		    (line[0] == '0' || line[0] == '1') &&
		    (line[1] == '\0' ||
		     (line[1] == '\n' && line[2] == '\0'))) {
			links->available[index] = true;
			links->link_up[index] = line[0] == '1';
		}
		fclose(file);
	}
}

static inline int airoha_ctc_eth_link_state_value(
		const struct airoha_ctc_object_context *object,
		const struct airoha_ctc_uni_links *links, uint8_t *value,
		size_t capacity)
{
	unsigned int index;

	if (!object || !object->present || !object->valid || object->all ||
	    !object->uni || object->uni > AIROHA_CTC_ETHERNET_UNI_COUNT ||
	    !links)
		return -EOPNOTSUPP;
	index = object->uni - 1;
	if (!links->available[index])
		return -EOPNOTSUPP;
	if (!value || capacity < 1)
		return -ENOSPC;
	value[0] = links->link_up[index] ? 1 : 0;
	return 1;
}

static inline bool airoha_ctc_is_standard_fec(uint8_t branch, uint16_t leaf)
{
	return branch == AIROHA_CTC_STANDARD_ATTRIBUTE_BRANCH &&
	       leaf == AIROHA_CTC_STANDARD_FEC_MODE;
}

static inline int airoha_ctc_decode_standard_fec(const uint8_t *value,
		size_t width, bool *enabled)
{
	uint32_t mode;

	if (!value || !enabled || width != AIROHA_CTC_STANDARD_FEC_MODE_LENGTH)
		return -EINVAL;
	mode = (uint32_t)value[0] << 24 | (uint32_t)value[1] << 16 |
	       (uint32_t)value[2] << 8 | value[3];
	if (mode == AIROHA_CTC_STANDARD_FEC_ENABLED)
		*enabled = true;
	else if (mode == AIROHA_CTC_STANDARD_FEC_DISABLED)
		*enabled = false;
	else
		return -EINVAL;
	return 0;
}

static inline int airoha_ctc_parse_fec_set(const uint8_t *request,
		size_t request_length, struct airoha_ctc_fec_set *fec)
{
	size_t input = 0;
	struct airoha_ctc_object_context object;

	if (!request || !fec)
		return -EINVAL;
	memset(fec, 0, sizeof(*fec));
	fec->valid = true;
	while (input < request_length) {
		uint16_t leaf;
		uint8_t width;
		bool enabled;
		size_t consumed;

		if (request_length - input >= 3 && !request[input] &&
		    !request[input + 1] && !request[input + 2])
			break;
		if (airoha_ctc_is_object_branch(request[input])) {
			if (airoha_ctc_parse_object(request + input,
					request_length - input, &object, &consumed))
				return -EINVAL;
			input += consumed;
			continue;
		}
		if (request_length - input < 4)
			return -EINVAL;
		width = request[input + 3];
		if ((size_t)width > request_length - input - 4)
			return -EINVAL;
		leaf = (uint16_t)request[input + 1] << 8 | request[input + 2];
		if (airoha_ctc_is_standard_fec(request[input], leaf)) {
			if (airoha_ctc_decode_standard_fec(request + input + 4,
							 width, &enabled)) {
				fec->valid = false;
			} else if (fec->present && fec->enabled != enabled) {
				fec->valid = false;
			} else {
				fec->enabled = enabled;
			}
			fec->present = true;
		}
		input += 4 + width;
	}
	return 0;
}

static inline int airoha_ctc_decode_holdover(const uint8_t *value,
		size_t width, bool *enabled, uint16_t *time_ms)
{
	if (!value || !enabled || !time_ms ||
	    width != AIROHA_CTC_HOLDOVER_CONFIG_LENGTH || value[0] > 1)
		return -EINVAL;
	*time_ms = (uint16_t)value[1] << 8 | value[2];
	if (*time_ms < AIROHA_CTC_HOLDOVER_TIME_MIN_MS ||
	    *time_ms > AIROHA_CTC_HOLDOVER_TIME_MAX_MS)
		return -ERANGE;
	*enabled = value[0];
	return 0;
}

static inline uint32_t airoha_ctc_get_be24(const uint8_t value[3])
{
	return (uint32_t)value[0] << 16 | (uint32_t)value[1] << 8 | value[2];
}

static inline uint32_t airoha_ctc_get_be32(const uint8_t value[4])
{
	return (uint32_t)value[0] << 24 | (uint32_t)value[1] << 16 |
	       (uint32_t)value[2] << 8 | value[3];
}

static inline int airoha_ctc_decode_pause(const uint8_t *value, size_t width,
		bool *enabled)
{
	if (!value || !enabled || width != AIROHA_CTC_ETHERNET_PAUSE_LENGTH ||
	    value[0] > 1)
		return -EINVAL;
	*enabled = value[0];
	return 0;
}

static inline int airoha_ctc_decode_admin_control(const uint8_t *value,
		size_t width, bool *enabled)
{
	uint32_t state;

	if (!value || !enabled || width != AIROHA_CTC_ADMIN_CONTROL_LENGTH)
		return -EINVAL;
	state = airoha_ctc_get_be32(value);
	if (state == AIROHA_CTC_ADMIN_ENABLED)
		*enabled = true;
	else if (state == AIROHA_CTC_ADMIN_DISABLED)
		*enabled = false;
	else
		return -EINVAL;
	return 0;
}

static inline int airoha_ctc_decode_upstream_policing(const uint8_t *value,
		size_t width, bool *enabled, uint32_t *cir_kbps,
		uint32_t *cbs_bytes, uint32_t *ebs_bytes)
{
	if (!value || !enabled || !cir_kbps || !cbs_bytes || !ebs_bytes ||
	    !width || value[0] > 1)
		return -EINVAL;
	*enabled = value[0];
	if (!*enabled) {
		if (width != AIROHA_CTC_UPSTREAM_POLICING_DISABLED_LENGTH &&
		    width != AIROHA_CTC_UPSTREAM_POLICING_ENABLED_LENGTH)
			return -EINVAL;
		*cir_kbps = 0;
		*cbs_bytes = 0;
		*ebs_bytes = 0;
		return 0;
	}
	if (width != AIROHA_CTC_UPSTREAM_POLICING_ENABLED_LENGTH)
		return -EINVAL;
	*cir_kbps = airoha_ctc_get_be24(value + 1);
	*cbs_bytes = airoha_ctc_get_be24(value + 4);
	*ebs_bytes = airoha_ctc_get_be24(value + 7);
	return 0;
}

static inline int airoha_ctc_decode_downstream_rate(const uint8_t *value,
		size_t width, bool *enabled, uint32_t *cir_kbps,
		uint32_t *pir_kbps)
{
	if (!value || !enabled || !cir_kbps || !pir_kbps || !width ||
	    value[0] > 1)
		return -EINVAL;
	*enabled = value[0];
	if (!*enabled) {
		if (width != AIROHA_CTC_DOWNSTREAM_RATE_DISABLED_LENGTH &&
		    width != AIROHA_CTC_DOWNSTREAM_RATE_ENABLED_LENGTH)
			return -EINVAL;
		*cir_kbps = 0;
		*pir_kbps = 0;
		return 0;
	}
	if (width != AIROHA_CTC_DOWNSTREAM_RATE_ENABLED_LENGTH)
		return -EINVAL;
	*cir_kbps = airoha_ctc_get_be24(value + 1);
	*pir_kbps = airoha_ctc_get_be24(value + 4);
	if (*pir_kbps && *pir_kbps < *cir_kbps)
		return -ERANGE;
	return 0;
}

static inline int airoha_ctc_decode_mac_aging(const uint8_t *value,
		size_t width, uint32_t *seconds)
{
	if (!value || !seconds ||
	    width != AIROHA_CTC_PORT_MAC_AGING_TIME_LENGTH)
		return -EINVAL;
	*seconds = airoha_ctc_get_be32(value);
	return 0;
}

static inline bool airoha_ctc_vlan_filter_tag_valid(uint32_t tag)
{
	uint16_t tpid = tag >> 16;
	uint16_t vid = tag & 0x0fff;

	return (tpid == AIROHA_CTC_VLAN_TPID_8021Q ||
		tpid == AIROHA_CTC_VLAN_TPID_8021AD) && vid != 0x0fff;
}

static inline bool airoha_ctc_vlan_output_tag_valid(uint32_t tag)
{
	return (tag >> 16) == AIROHA_CTC_VLAN_TPID_8021Q &&
	       (tag & 0x0fff) != 0x0fff;
}

static inline int airoha_ctc_vlan_add_rule(
		struct airoha_ctc_uni_policy *policy, uint32_t old_tag,
		uint32_t new_tag, bool require_unique_new, bool output_8021q)
{
	unsigned int index;

	if (policy->vlan_rule_count >= AIROHA_CTC_VLAN_RULE_MAX ||
	    !airoha_ctc_vlan_filter_tag_valid(old_tag) ||
	    !(output_8021q ? airoha_ctc_vlan_output_tag_valid(new_tag) :
	       airoha_ctc_vlan_filter_tag_valid(new_tag)))
		return -ERANGE;
	for (index = 0; index < policy->vlan_rule_count; index++) {
		if (policy->vlan_old_tag[index] == old_tag)
			return -EINVAL;
		if (require_unique_new && policy->vlan_new_tag[index] == new_tag)
			return -EINVAL;
	}
	index = policy->vlan_rule_count++;
	policy->vlan_old_tag[index] = old_tag;
	policy->vlan_new_tag[index] = new_tag;
	return 0;
}

static inline int airoha_ctc_decode_vlan(const uint8_t *value, size_t width,
		struct airoha_ctc_uni_policy *policy)
{
	size_t offset;
	unsigned int count, group, index;
	uint32_t old_tag, new_tag;

	if (!value || !policy || !width ||
	    value[0] > AIROHA_CTC_VLAN_MODE_TRUNK)
		return -EINVAL;
	memset(policy, 0, sizeof(*policy));
	policy->vlan_present = true;
	policy->vlan_valid = true;
	policy->vlan_mode = value[0];
	if (policy->vlan_mode == AIROHA_CTC_VLAN_MODE_TRANSPARENT)
		return width == 1 ? 0 : -EINVAL;
	if (width < 5)
		return -EINVAL;
	policy->vlan_default_tag = airoha_ctc_get_be32(value + 1);
	if (!airoha_ctc_vlan_output_tag_valid(policy->vlan_default_tag))
		return -ERANGE;
	if (policy->vlan_mode == AIROHA_CTC_VLAN_MODE_TAG)
		return width == 5 ? 0 : -EINVAL;
	if (policy->vlan_mode == AIROHA_CTC_VLAN_MODE_TRANSLATION) {
		if ((width - 5) % 8)
			return -EINVAL;
		count = (width - 5) / 8;
		for (index = 0, offset = 5; index < count;
		     index++, offset += 8) {
			old_tag = airoha_ctc_get_be32(value + offset);
			new_tag = airoha_ctc_get_be32(value + offset + 4);
			if (airoha_ctc_vlan_add_rule(policy, old_tag, new_tag, true,
				true))
				return -EINVAL;
		}
		return 0;
	}
	if (policy->vlan_mode == AIROHA_CTC_VLAN_MODE_TRUNK) {
		if ((width - 5) % 4)
			return -EINVAL;
		count = (width - 5) / 4;
		for (index = 0, offset = 5; index < count;
		     index++, offset += 4) {
			old_tag = airoha_ctc_get_be32(value + offset);
			if (airoha_ctc_vlan_add_rule(policy, old_tag, old_tag, false,
				false))
				return -EINVAL;
		}
		return 0;
	}

	if (width < 7)
		return -EINVAL;
	count = (uint16_t)value[5] << 8 | value[6];
	if (count > AIROHA_CTC_VLAN_RULE_MAX)
		return -ERANGE;
	offset = 7;
	for (group = 0; group < count; group++) {
		if (width - offset < 6)
			return -EINVAL;
		index = (uint16_t)value[offset] << 8 | value[offset + 1];
		new_tag = airoha_ctc_get_be32(value + offset + 2);
		offset += 6;
		if (index > AIROHA_CTC_VLAN_RULE_MAX ||
		    index > (width - offset) / 4)
			return -EINVAL;
		while (index--) {
			old_tag = airoha_ctc_get_be32(value + offset);
			offset += 4;
			if ((old_tag >> 16) != AIROHA_CTC_VLAN_TPID_8021Q)
				return -EINVAL;
			if (airoha_ctc_vlan_add_rule(policy, old_tag, new_tag, false,
				true))
				return -EINVAL;
		}
	}
	return offset == width ? 0 : -EINVAL;
}

static inline bool airoha_ctc_policy_vlan_equal(
		const struct airoha_ctc_uni_policy *left,
		const struct airoha_ctc_uni_policy *right)
{
	if (left->vlan_mode != right->vlan_mode ||
	    left->vlan_rule_count != right->vlan_rule_count ||
	    left->vlan_default_tag != right->vlan_default_tag)
		return false;
	return !memcmp(left->vlan_old_tag, right->vlan_old_tag,
		left->vlan_rule_count * sizeof(left->vlan_old_tag[0])) &&
	       !memcmp(left->vlan_new_tag, right->vlan_new_tag,
		left->vlan_rule_count * sizeof(left->vlan_new_tag[0]));
}

static inline void airoha_ctc_set_vlan_policy(
		struct airoha_ctc_uni_policy *policy,
		const struct airoha_ctc_uni_policy *decoded, bool valid)
{
	if (policy->vlan_present &&
	    (!valid || !policy->vlan_valid ||
	     !airoha_ctc_policy_vlan_equal(policy, decoded)))
		policy->vlan_valid = false;
	else if (!policy->vlan_present) {
		policy->vlan_valid = valid;
		policy->vlan_mode = decoded->vlan_mode;
		policy->vlan_rule_count = decoded->vlan_rule_count;
		policy->vlan_default_tag = decoded->vlan_default_tag;
		memcpy(policy->vlan_old_tag, decoded->vlan_old_tag,
		       sizeof(policy->vlan_old_tag));
		memcpy(policy->vlan_new_tag, decoded->vlan_new_tag,
		       sizeof(policy->vlan_new_tag));
	}
	policy->vlan_present = true;
}

static inline int airoha_ctc_classification_field_layout(uint8_t field,
		size_t *value_offset, uint8_t *value_length, size_t *wire_length)
{
	if (!value_offset || !value_length || !wire_length ||
	    field >= AIROHA_CTC_CLASSIFICATION_MATCH_MAX)
		return -EINVAL;
	*wire_length = field <= AIROHA_CTC_CLASSIFICATION_L4_DPORT ? 8 : 18;
	switch (field) {
	case AIROHA_CTC_CLASSIFICATION_DMAC:
	case AIROHA_CTC_CLASSIFICATION_SMAC:
		*value_offset = 1;
		*value_length = 6;
		break;
	case AIROHA_CTC_CLASSIFICATION_PBIT:
	case AIROHA_CTC_CLASSIFICATION_IPV4_PROTOCOL:
	case AIROHA_CTC_CLASSIFICATION_IPV4_DSCP:
	case AIROHA_CTC_CLASSIFICATION_IPV6_DSCP:
		*value_offset = 6;
		*value_length = 1;
		break;
	case AIROHA_CTC_CLASSIFICATION_VLAN_ID:
	case AIROHA_CTC_CLASSIFICATION_ETHERTYPE:
	case AIROHA_CTC_CLASSIFICATION_L4_SPORT:
	case AIROHA_CTC_CLASSIFICATION_L4_DPORT:
		*value_offset = 5;
		*value_length = 2;
		break;
	case AIROHA_CTC_CLASSIFICATION_IPV4_DADDR:
	case AIROHA_CTC_CLASSIFICATION_IPV4_SADDR:
		*value_offset = 3;
		*value_length = 4;
		break;
	case AIROHA_CTC_CLASSIFICATION_IP_VERSION:
	case AIROHA_CTC_CLASSIFICATION_IPV6_PROTOCOL:
		*value_offset = 16;
		*value_length = 1;
		break;
	case AIROHA_CTC_CLASSIFICATION_IPV6_FLOW_LABEL:
		*value_offset = 14;
		*value_length = 3;
		break;
	case AIROHA_CTC_CLASSIFICATION_IPV6_DADDR:
	case AIROHA_CTC_CLASSIFICATION_IPV6_SADDR:
	case AIROHA_CTC_CLASSIFICATION_IPV6_DPREFIX:
	case AIROHA_CTC_CLASSIFICATION_IPV6_SPREFIX:
		*value_offset = 1;
		*value_length = 16;
		break;
	default:
		return -EINVAL;
	}
	return 0;
}

static inline bool airoha_ctc_classification_match_value_valid(
		const struct airoha_ctc_classification_match *match)
{
	if (!match || match->operation > AIROHA_CTC_CLASSIFICATION_OP_ALWAYS)
		return false;
	switch (match->field) {
	case AIROHA_CTC_CLASSIFICATION_PBIT:
		return match->value[0] <= 7;
	case AIROHA_CTC_CLASSIFICATION_IPV4_DSCP:
	case AIROHA_CTC_CLASSIFICATION_IPV6_DSCP:
		return match->value[0] <= 63;
	case AIROHA_CTC_CLASSIFICATION_IP_VERSION:
		return match->value[0] == 4 || match->value[0] == 6;
	case AIROHA_CTC_CLASSIFICATION_IPV6_FLOW_LABEL:
		return !(match->value[0] & 0xf0);
	case AIROHA_CTC_CLASSIFICATION_IPV6_DPREFIX:
	case AIROHA_CTC_CLASSIFICATION_IPV6_SPREFIX:
		/* The stock SDK stores the prefix length in byte 15. */
		return match->value[15] <= 120;
	default:
		return true;
	}
}

static inline bool airoha_ctc_classification_match_equal(
		const struct airoha_ctc_classification_match *left,
		const struct airoha_ctc_classification_match *right)
{
	return left->field == right->field &&
	       left->operation == right->operation &&
	       left->value_length == right->value_length &&
	       !memcmp(left->value, right->value, left->value_length);
}

static inline bool airoha_ctc_classification_rule_equal(
		const struct airoha_ctc_classification_rule *left,
		const struct airoha_ctc_classification_rule *right,
		bool compare_precedence, bool ordered_matches)
{
	bool matched[AIROHA_CTC_CLASSIFICATION_MATCH_MAX] = { false };
	unsigned int index, other;

	if ((compare_precedence && left->precedence != right->precedence) ||
	    left->queue != right->queue || left->priority != right->priority ||
	    left->match_count != right->match_count)
		return false;
	for (index = 0; index < left->match_count; index++) {
		if (ordered_matches) {
			if (!airoha_ctc_classification_match_equal(&left->match[index],
					&right->match[index]))
				return false;
			continue;
		}
		for (other = 0; other < right->match_count; other++)
			if (!matched[other] &&
			    airoha_ctc_classification_match_equal(&left->match[index],
					&right->match[other]))
				break;
		if (other == right->match_count)
			return false;
		matched[other] = true;
	}
	return true;
}

static inline bool airoha_ctc_policy_classification_equal(
		const struct airoha_ctc_uni_policy *left,
		const struct airoha_ctc_uni_policy *right)
{
	unsigned int index;

	if (left->classification_present != right->classification_present)
		return false;
	if (!left->classification_present)
		return true;
	if (left->classification_rule_count != right->classification_rule_count)
		return false;
	for (index = 0; index < left->classification_rule_count; index++)
		if (!airoha_ctc_classification_rule_equal(
				&left->classification_rule[index],
				&right->classification_rule[index], true, true))
			return false;
	return true;
}

static inline int airoha_ctc_decode_classification(const uint8_t *value,
		size_t width, struct airoha_ctc_uni_policy *policy)
{
	size_t offset = 2;
	unsigned int rule, match;

	if (!value || !policy || !width ||
	    value[0] > AIROHA_CTC_CLASSIFICATION_CLEAR)
		return -EINVAL;
	memset(policy, 0, sizeof(*policy));
	policy->classification_present = true;
	policy->classification_valid = true;
	policy->classification_action = value[0];
	if (value[0] == AIROHA_CTC_CLASSIFICATION_CLEAR)
		return width == 1 ? 0 : -EINVAL;
	if (width < 2 || value[1] > AIROHA_CTC_CLASSIFICATION_RULE_MAX)
		return -EINVAL;
	policy->classification_rule_count = value[1];
	for (rule = 0; rule < policy->classification_rule_count; rule++) {
		struct airoha_ctc_classification_rule *decoded =
			&policy->classification_rule[rule];
		size_t rule_end, expected_length = 3;
		uint8_t encoded_length;

		if (width - offset < 5)
			return -EINVAL;
		encoded_length = value[offset + 1];
		if (encoded_length < 3 || encoded_length > width - offset - 2)
			return -EINVAL;
		rule_end = offset + 2 + encoded_length;
		decoded->precedence = value[offset];
		decoded->queue = value[offset + 2];
		decoded->priority = value[offset + 3];
		decoded->match_count = value[offset + 4];
		if (!decoded->precedence || decoded->queue > 7 ||
		    (decoded->priority > 7 && decoded->priority !=
			AIROHA_CTC_CLASSIFICATION_PRIORITY_UNCHANGED) ||
		    decoded->match_count > AIROHA_CTC_CLASSIFICATION_MATCH_MAX)
			return -ERANGE;
		offset += 5;
		for (match = 0; match < decoded->match_count; match++) {
			struct airoha_ctc_classification_match *output =
				&decoded->match[match];
			size_t value_offset, wire_length;
			uint8_t value_length;

			if (offset >= rule_end ||
			    airoha_ctc_classification_field_layout(value[offset],
				&value_offset, &value_length, &wire_length) ||
			    wire_length > rule_end - offset)
				return -EINVAL;
			output->field = value[offset];
			output->operation = value[offset + wire_length - 1];
			output->value_length = value_length;
			memcpy(output->value, value + offset + value_offset,
			       value_length);
			if (!airoha_ctc_classification_match_value_valid(output))
				return -ERANGE;
			offset += wire_length;
			expected_length += wire_length;
		}
		if (encoded_length != expected_length || offset != rule_end)
			return -EINVAL;
	}
	return offset == width ? 0 : -EINVAL;
}

static inline void airoha_ctc_set_classification_policy(
		struct airoha_ctc_uni_policy *policy,
		const struct airoha_ctc_uni_policy *decoded, bool valid)
{
	unsigned int count;

	if (!policy->classification_present) {
		policy->classification_present = true;
		policy->classification_valid = valid;
		if (valid) {
			policy->classification_action = decoded->classification_action;
			policy->classification_rule_count =
				decoded->classification_rule_count;
			memcpy(policy->classification_rule,
			       decoded->classification_rule,
			       sizeof(policy->classification_rule));
		}
		return;
	}
	if (!valid || !policy->classification_valid ||
	    policy->classification_action != decoded->classification_action ||
	    policy->classification_action == AIROHA_CTC_CLASSIFICATION_CLEAR) {
		policy->classification_valid = false;
		return;
	}
	count = policy->classification_rule_count +
		decoded->classification_rule_count;
	if (count > AIROHA_CTC_CLASSIFICATION_RULE_MAX) {
		policy->classification_valid = false;
		return;
	}
	memcpy(policy->classification_rule + policy->classification_rule_count,
	       decoded->classification_rule,
	       decoded->classification_rule_count *
		       sizeof(decoded->classification_rule[0]));
	policy->classification_rule_count = count;
}

static inline void airoha_ctc_apply_classification_to_object(
		const struct airoha_ctc_object_context *object,
		struct airoha_ctc_management_set *management,
		const struct airoha_ctc_uni_policy *decoded, bool valid)
{
	unsigned int first_uni, last_uni, uni;

	if (!object->present || (!object->valid && !object->all))
		valid = false;
	if (object->all) {
		first_uni = 0;
		last_uni = AIROHA_CTC_ETHERNET_UNI_COUNT;
	} else if (object->valid && object->uni) {
		first_uni = object->uni - 1;
		last_uni = first_uni + 1;
	} else {
		first_uni = 0;
		last_uni = AIROHA_CTC_ETHERNET_UNI_COUNT;
	}
	for (uni = first_uni; uni < last_uni; uni++)
		airoha_ctc_set_classification_policy(&management->uni[uni], decoded,
			valid);
}

static inline bool airoha_ctc_policy_pause_equal(
		const struct airoha_ctc_uni_policy *policy, bool enabled)
{
	return policy->pause_enabled == enabled;
}

static inline void airoha_ctc_set_admin_policy(bool *present, bool *valid,
		bool *current, bool decoded_valid, bool enabled)
{
	if (*present && (!decoded_valid || !*valid || *current != enabled))
		*valid = false;
	else if (!*present) {
		*valid = decoded_valid;
		*current = enabled;
	}
	*present = true;
}

static inline bool airoha_ctc_policy_upstream_equal(
		const struct airoha_ctc_uni_policy *policy, bool enabled,
		uint32_t cir_kbps, uint32_t cbs_bytes, uint32_t ebs_bytes)
{
	return policy->upstream_enabled == enabled &&
	       policy->upstream_cir_kbps == cir_kbps &&
	       policy->upstream_cbs_bytes == cbs_bytes &&
	       policy->upstream_ebs_bytes == ebs_bytes;
}

static inline bool airoha_ctc_policy_downstream_equal(
		const struct airoha_ctc_uni_policy *policy, bool enabled,
		uint32_t cir_kbps, uint32_t pir_kbps)
{
	return policy->downstream_enabled == enabled &&
	       policy->downstream_cir_kbps == cir_kbps &&
	       policy->downstream_pir_kbps == pir_kbps;
}

static inline void airoha_ctc_set_pause_policy(
		struct airoha_ctc_uni_policy *policy, bool valid, bool enabled)
{
	if (policy->pause_present &&
	    (!valid || !policy->pause_valid ||
	     !airoha_ctc_policy_pause_equal(policy, enabled)))
		policy->pause_valid = false;
	else if (!policy->pause_present) {
		policy->pause_valid = valid;
		policy->pause_enabled = enabled;
	}
	policy->pause_present = true;
}

static inline void airoha_ctc_set_upstream_policy(
		struct airoha_ctc_uni_policy *policy, bool valid, bool enabled,
		uint32_t cir_kbps, uint32_t cbs_bytes, uint32_t ebs_bytes)
{
	if (policy->upstream_present &&
	    (!valid || !policy->upstream_valid ||
	     !airoha_ctc_policy_upstream_equal(policy, enabled, cir_kbps,
					       cbs_bytes, ebs_bytes)))
		policy->upstream_valid = false;
	else if (!policy->upstream_present) {
		policy->upstream_valid = valid;
		policy->upstream_enabled = enabled;
		policy->upstream_cir_kbps = cir_kbps;
		policy->upstream_cbs_bytes = cbs_bytes;
		policy->upstream_ebs_bytes = ebs_bytes;
	}
	policy->upstream_present = true;
}

static inline void airoha_ctc_set_downstream_policy(
		struct airoha_ctc_uni_policy *policy, bool valid, bool enabled,
		uint32_t cir_kbps, uint32_t pir_kbps)
{
	if (policy->downstream_present &&
	    (!valid || !policy->downstream_valid ||
	     !airoha_ctc_policy_downstream_equal(policy, enabled, cir_kbps,
						 pir_kbps)))
		policy->downstream_valid = false;
	else if (!policy->downstream_present) {
		policy->downstream_valid = valid;
		policy->downstream_enabled = enabled;
		policy->downstream_cir_kbps = cir_kbps;
		policy->downstream_pir_kbps = pir_kbps;
	}
	policy->downstream_present = true;
}

static inline void airoha_ctc_apply_to_object(
		const struct airoha_ctc_object_context *object,
		struct airoha_ctc_management_set *management, uint16_t leaf,
		bool valid, bool enabled, uint32_t first, uint32_t second,
		uint32_t third)
{
	unsigned int first_uni, last_uni, uni;

	if (!object->present || (!object->valid && !object->all))
		valid = false;
	if (object->all) {
		first_uni = 0;
		last_uni = AIROHA_CTC_ETHERNET_UNI_COUNT;
	} else if (object->valid && object->uni) {
		first_uni = object->uni - 1;
		last_uni = first_uni + 1;
	} else {
		first_uni = 0;
		last_uni = AIROHA_CTC_ETHERNET_UNI_COUNT;
	}
	for (uni = first_uni; uni < last_uni; uni++) {
		struct airoha_ctc_uni_policy *policy = &management->uni[uni];

		switch (leaf) {
		case AIROHA_CTC_PHY_ADMIN_CONTROL:
			airoha_ctc_set_admin_policy(&policy->phy_admin_present,
				&policy->phy_admin_valid, &policy->phy_admin_enabled,
				valid, enabled);
			break;
		case AIROHA_CTC_AUTONEG_ADMIN_CONTROL:
			airoha_ctc_set_admin_policy(&policy->autoneg_present,
				&policy->autoneg_valid, &policy->autoneg_enabled,
				valid, enabled);
			break;
		case AIROHA_CTC_AUTONEG_RESTART:
			if (policy->autoneg_restart_present && !valid)
				policy->autoneg_restart_valid = false;
			else if (!policy->autoneg_restart_present)
				policy->autoneg_restart_valid = valid;
			policy->autoneg_restart_present = true;
			break;
		case AIROHA_CTC_ETHERNET_PAUSE:
			airoha_ctc_set_pause_policy(policy, valid, enabled);
			break;
		case AIROHA_CTC_UPSTREAM_POLICING:
			airoha_ctc_set_upstream_policy(policy, valid, enabled,
				first, second, third);
			break;
		case AIROHA_CTC_DOWNSTREAM_RATE_LIMITING:
			airoha_ctc_set_downstream_policy(policy, valid, enabled,
				first, second);
			break;
		}
	}
}

static inline void airoha_ctc_apply_vlan_to_object(
		const struct airoha_ctc_object_context *object,
		struct airoha_ctc_management_set *management,
		const struct airoha_ctc_uni_policy *decoded, bool valid)
{
	unsigned int first_uni, last_uni, uni;

	if (!object->present || (!object->valid && !object->all))
		valid = false;
	if (object->all) {
		first_uni = 0;
		last_uni = AIROHA_CTC_ETHERNET_UNI_COUNT;
	} else if (object->valid && object->uni) {
		first_uni = object->uni - 1;
		last_uni = first_uni + 1;
	} else {
		first_uni = 0;
		last_uni = AIROHA_CTC_ETHERNET_UNI_COUNT;
	}
	for (uni = first_uni; uni < last_uni; uni++)
		airoha_ctc_set_vlan_policy(&management->uni[uni], decoded, valid);
}

static inline int airoha_ctc_parse_management_set(const uint8_t *request,
		size_t request_length, struct airoha_ctc_management_set *management)
{
	struct airoha_ctc_object_context object = { 0 };
	size_t input = 0;

	if (!request || !management)
		return -EINVAL;
	memset(management, 0, sizeof(*management));
	while (input < request_length) {
		struct airoha_ctc_uni_policy classification = { 0 };
		struct airoha_ctc_uni_policy vlan = { 0 };
		uint32_t first = 0, second = 0, third = 0;
		uint16_t leaf;
		uint8_t width;
		bool enabled = false, valid;
		size_t consumed;

		if (request_length - input >= 3 && !request[input] &&
		    !request[input + 1] && !request[input + 2])
			break;
		if (airoha_ctc_is_object_branch(request[input])) {
			if (airoha_ctc_parse_object(request + input,
					request_length - input, &object, &consumed))
				return -EINVAL;
			input += consumed;
			continue;
		}
		if (request_length - input < 4)
			return -EINVAL;
		width = request[input + 3];
		if ((size_t)width > request_length - input - 4)
			return -EINVAL;
		leaf = (uint16_t)request[input + 1] << 8 | request[input + 2];
		if (request[input] == AIROHA_CTC_STANDARD_ACTION_BRANCH) {
			switch (leaf) {
			case AIROHA_CTC_PHY_ADMIN_CONTROL:
			case AIROHA_CTC_AUTONEG_ADMIN_CONTROL:
				valid = !airoha_ctc_decode_admin_control(
					request + input + 4, width, &enabled);
				airoha_ctc_apply_to_object(&object, management,
					leaf, valid, enabled, 0, 0, 0);
				break;
			case AIROHA_CTC_AUTONEG_RESTART:
				valid = width == 0;
				airoha_ctc_apply_to_object(&object, management,
					leaf, valid, false, 0, 0, 0);
				break;
			}
			goto next;
		}
		if (request[input] != AIROHA_CTC_EXTENDED_ATTRIBUTE_BRANCH)
			goto next;
		switch (leaf) {
		case AIROHA_CTC_ETHERNET_PAUSE:
			valid = !airoha_ctc_decode_pause(request + input + 4,
				width, &enabled);
			airoha_ctc_apply_to_object(&object, management, leaf, valid,
				enabled, 0, 0, 0);
			break;
		case AIROHA_CTC_UPSTREAM_POLICING:
			valid = !airoha_ctc_decode_upstream_policing(
				request + input + 4, width, &enabled, &first,
				&second, &third);
			airoha_ctc_apply_to_object(&object, management, leaf, valid,
				enabled, first, second, third);
			break;
		case AIROHA_CTC_DOWNSTREAM_RATE_LIMITING:
			valid = !airoha_ctc_decode_downstream_rate(
				request + input + 4, width, &enabled, &first, &second);
			airoha_ctc_apply_to_object(&object, management, leaf, valid,
				enabled, first, second, 0);
			break;
		case AIROHA_CTC_PORT_MAC_AGING_TIME:
			valid = !airoha_ctc_decode_mac_aging(request + input + 4,
				width, &first);
			if (management->aging_present &&
			    (!valid || !management->aging_valid ||
			     management->aging_time_seconds != first))
				management->aging_valid = false;
			else if (!management->aging_present) {
				management->aging_valid = valid;
				management->aging_time_seconds = first;
			}
			management->aging_present = true;
			break;
		case AIROHA_CTC_CLASSIFICATION_MARKING:
			valid = !airoha_ctc_decode_classification(
				request + input + 4, width, &classification);
			airoha_ctc_apply_classification_to_object(&object, management,
				&classification, valid);
			break;
		case AIROHA_CTC_VLAN:
			valid = !airoha_ctc_decode_vlan(request + input + 4,
				width, &vlan);
			airoha_ctc_apply_vlan_to_object(&object, management, &vlan,
				valid);
			break;
		}
next:
		input += 4 + width;
	}
	return 0;
}

static inline int airoha_ctc_parse_holdover_set(const uint8_t *request,
		size_t request_length, struct airoha_ctc_holdover_set *holdover)
{
	size_t input = 0;
	struct airoha_ctc_object_context object;

	if (!request || !holdover)
		return -EINVAL;
	memset(holdover, 0, sizeof(*holdover));
	holdover->valid = true;
	while (input < request_length) {
		uint16_t leaf, time_ms = 0;
		uint8_t width;
		bool enabled = false;
		size_t consumed;

		if (request_length - input >= 3 && !request[input] &&
		    !request[input + 1] && !request[input + 2])
			break;
		if (airoha_ctc_is_object_branch(request[input])) {
			if (airoha_ctc_parse_object(request + input,
					request_length - input, &object, &consumed))
				return -EINVAL;
			input += consumed;
			continue;
		}
		if (request_length - input < 4)
			return -EINVAL;
		width = request[input + 3];
		if ((size_t)width > request_length - input - 4)
			return -EINVAL;
		leaf = (uint16_t)request[input + 1] << 8 | request[input + 2];
		if (request[input] == AIROHA_CTC_EXTENDED_ATTRIBUTE_BRANCH &&
		    leaf == AIROHA_CTC_HOLDOVER_CONFIG) {
			if (airoha_ctc_decode_holdover(request + input + 4, width,
						 &enabled, &time_ms)) {
				holdover->valid = false;
			} else if (holdover->present &&
				   (holdover->enabled != enabled ||
				    holdover->time_ms != time_ms)) {
				holdover->valid = false;
			} else {
				holdover->enabled = enabled;
				holdover->time_ms = time_ms;
			}
			holdover->present = true;
		}
		input += 4 + width;
	}
	return 0;
}

static inline int airoha_ctc_holdover_value(
		const struct airoha_ctc_holdover_config *holdover,
		uint8_t *value, size_t capacity)
{
	if (!holdover || !holdover->available)
		return -EOPNOTSUPP;
	if (!value || capacity < AIROHA_CTC_HOLDOVER_CONFIG_LENGTH)
		return -ENOSPC;
	value[0] = holdover->enabled;
	value[1] = holdover->time_ms >> 8;
	value[2] = holdover->time_ms & 0xff;
	return AIROHA_CTC_HOLDOVER_CONFIG_LENGTH;
}

static inline int airoha_ctc_identity_value(uint16_t leaf,
		const struct airoha_ctc_identity *identity,
		const struct airoha_ctc_optical_diagnostics *diagnostics,
		uint8_t *value, size_t capacity)
{
	size_t offset = 0;

	if (!identity || !value)
		return -EINVAL;
	switch (leaf) {
	case AIROHA_CTC_ONU_SERIAL_NUMBER:
		if (capacity < AIROHA_CTC_ONU_SERIAL_NUMBER_LENGTH)
			return -ENOSPC;
		memcpy(value + offset, identity->vendor_id,
		       sizeof(identity->vendor_id));
		offset += sizeof(identity->vendor_id);
		memcpy(value + offset, identity->model, sizeof(identity->model));
		offset += sizeof(identity->model);
		memcpy(value + offset, identity->onu_mac,
		       sizeof(identity->onu_mac));
		offset += sizeof(identity->onu_mac);
		memcpy(value + offset, identity->hardware_version,
		       sizeof(identity->hardware_version));
		offset += sizeof(identity->hardware_version);
		memcpy(value + offset, identity->software_version,
		       sizeof(identity->software_version));
		offset += sizeof(identity->software_version);
		return (int)offset;
	case AIROHA_CTC_FIRMWARE_VERSION:
		if (!identity->firmware_version_length ||
		    identity->firmware_version_length >
			    sizeof(identity->firmware_version))
			return -EOPNOTSUPP;
		if (capacity < identity->firmware_version_length)
			return -ENOSPC;
		memcpy(value, identity->firmware_version,
		       identity->firmware_version_length);
		return (int)identity->firmware_version_length;
	case AIROHA_CTC_CHIPSET_ID:
		if (capacity < sizeof(identity->chipset_id))
			return -ENOSPC;
		memcpy(value, identity->chipset_id, sizeof(identity->chipset_id));
		return sizeof(identity->chipset_id);
	case AIROHA_CTC_ONU_CAPABILITIES_1:
		if (capacity < sizeof(airoha_ctc_onu_capabilities_1))
			return -ENOSPC;
		memcpy(value, airoha_ctc_onu_capabilities_1,
		       sizeof(airoha_ctc_onu_capabilities_1));
		return sizeof(airoha_ctc_onu_capabilities_1);
	case AIROHA_CTC_OPTICAL_TRANSCEIVER_DIAGNOSIS:
		if (!diagnostics || !diagnostics->available)
			return -EOPNOTSUPP;
		if (capacity < AIROHA_CTC_OPTICAL_TRANSCEIVER_DIAGNOSIS_LENGTH)
			return -ENOSPC;
#define AIROHA_CTC_PUT_BE16(member) do { \
		value[offset++] = diagnostics->member >> 8; \
		value[offset++] = diagnostics->member & 0xff; \
	} while (0)
		AIROHA_CTC_PUT_BE16(temperature);
		AIROHA_CTC_PUT_BE16(supply_voltage);
		AIROHA_CTC_PUT_BE16(laser_bias_current);
		AIROHA_CTC_PUT_BE16(transmit_power);
		AIROHA_CTC_PUT_BE16(receive_power);
#undef AIROHA_CTC_PUT_BE16
		return (int)offset;
	case AIROHA_CTC_ONU_CAPABILITIES_2:
		if (capacity < sizeof(airoha_ctc_onu_capabilities_2))
			return -ENOSPC;
		memcpy(value, airoha_ctc_onu_capabilities_2,
		       sizeof(airoha_ctc_onu_capabilities_2));
		return sizeof(airoha_ctc_onu_capabilities_2);
	case AIROHA_CTC_ONU_CAPABILITIES_3:
		if (capacity < sizeof(airoha_ctc_onu_capabilities_3))
			return -ENOSPC;
		memcpy(value, airoha_ctc_onu_capabilities_3,
		       sizeof(airoha_ctc_onu_capabilities_3));
		return sizeof(airoha_ctc_onu_capabilities_3);
	default:
		return -EOPNOTSUPP;
	}
}

static inline int airoha_ctc_build_variable_get_response(
		const uint8_t *request, size_t request_length,
		const struct airoha_ctc_identity *identity,
		const struct airoha_ctc_optical_diagnostics *diagnostics,
		const struct airoha_ctc_fec_mode *fec,
		const struct airoha_ctc_uni_links *links,
		const struct airoha_ctc_holdover_config *holdover,
		uint8_t *response,
		size_t capacity, struct airoha_ctc_variable_result *result)
{
	size_t input = 0, output = 0;
	struct airoha_ctc_object_context object = { 0 };

	if (!request || !identity || !response || !result)
		return -EINVAL;
	memset(result, 0, sizeof(*result));
	while (input < request_length) {
		uint16_t leaf;
		int value_length = -EOPNOTSUPP;
		struct airoha_ieee_variable_state standard = { 0 };
		size_t consumed;

		if (request_length - input >= 3 && !request[input] &&
		    !request[input + 1] && !request[input + 2])
			break;
		if (airoha_ctc_is_object_branch(request[input])) {
			if (airoha_ctc_parse_object(request + input,
					request_length - input, &object, &consumed))
				return -EINVAL;
			if (capacity - output < consumed)
				return -ENOSPC;
			memcpy(response + output, request + input, consumed);
			input += consumed;
			output += consumed;
			continue;
		}
		if (request_length - input < 3)
			return -EINVAL;
		if (capacity - output < 4)
			return -ENOSPC;
		leaf = (uint16_t)request[input + 1] << 8 | request[input + 2];
		if (request[input] == AIROHA_CTC_EXTENDED_ATTRIBUTE_BRANCH) {
			if (leaf == AIROHA_CTC_ETH_LINK_STATE)
				value_length = airoha_ctc_eth_link_state_value(
					&object, links, response + output + 4,
					capacity - output - 4);
			else if (leaf == AIROHA_CTC_HOLDOVER_CONFIG)
				value_length = airoha_ctc_holdover_value(holdover,
					response + output + 4,
					capacity - output - 4);
			else
				value_length = airoha_ctc_identity_value(leaf,
					identity, diagnostics, response + output + 4,
					capacity - output - 4);
		} else {
			standard.fec_available = fec && fec->available;
			standard.fec_enabled = fec && fec->enabled;
			value_length = airoha_ieee_standard_value(request[input],
				leaf, &standard, response + output + 4,
				capacity - output - 4);
		}
		memcpy(response + output, request + input, 3);
		if (value_length == -EOPNOTSUPP) {
			response[output + 3] =
				AIROHA_CTC_VARIABLE_STATUS_UNSUPPORTED;
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

static inline bool airoha_ctc_management_port_applied(
		const struct airoha_ctc_uni_policy *policy, uint8_t branch,
		uint16_t leaf,
		const uint8_t *value, size_t width)
{
	struct airoha_ctc_uni_policy decoded = { 0 };
	uint32_t first = 0, second = 0, third = 0;
	bool enabled = false;

	if (!policy)
		return false;
	if (branch == AIROHA_CTC_STANDARD_ACTION_BRANCH) {
		switch (leaf) {
		case AIROHA_CTC_PHY_ADMIN_CONTROL:
			return !airoha_ctc_decode_admin_control(value, width,
				&enabled) && policy->phy_admin_present &&
			       policy->phy_admin_valid &&
			       policy->phy_admin_applied &&
			       policy->phy_admin_enabled == enabled;
		case AIROHA_CTC_AUTONEG_ADMIN_CONTROL:
			return !airoha_ctc_decode_admin_control(value, width,
				&enabled) && policy->autoneg_present &&
			       policy->autoneg_valid &&
			       policy->autoneg_applied &&
			       policy->autoneg_enabled == enabled;
		case AIROHA_CTC_AUTONEG_RESTART:
			return width == 0 && policy->autoneg_restart_present &&
			       policy->autoneg_restart_valid &&
			       policy->autoneg_restart_applied;
		default:
			return false;
		}
	}
	if (branch != AIROHA_CTC_EXTENDED_ATTRIBUTE_BRANCH)
		return false;
	switch (leaf) {
	case AIROHA_CTC_ETHERNET_PAUSE:
		return !airoha_ctc_decode_pause(value, width, &enabled) &&
		       policy->pause_present && policy->pause_valid &&
		       policy->pause_applied && policy->pause_enabled == enabled;
	case AIROHA_CTC_UPSTREAM_POLICING:
		return !airoha_ctc_decode_upstream_policing(value, width, &enabled,
			&first, &second, &third) && policy->upstream_present &&
		       policy->upstream_valid && policy->upstream_applied &&
		       airoha_ctc_policy_upstream_equal(policy, enabled, first,
						 second, third);
	case AIROHA_CTC_DOWNSTREAM_RATE_LIMITING:
		return !airoha_ctc_decode_downstream_rate(value, width, &enabled,
			&first, &second) && policy->downstream_present &&
		       policy->downstream_valid && policy->downstream_applied &&
		       airoha_ctc_policy_downstream_equal(policy, enabled, first,
						   second);
	case AIROHA_CTC_CLASSIFICATION_MARKING:
		return !airoha_ctc_decode_classification(value, width, &decoded) &&
		       policy->classification_present &&
		       policy->classification_valid &&
		       policy->classification_applied &&
		       policy->classification_action ==
			       decoded.classification_action &&
		       airoha_ctc_policy_classification_equal(policy, &decoded);
	case AIROHA_CTC_VLAN:
		return !airoha_ctc_decode_vlan(value, width, &decoded) &&
		       policy->vlan_present && policy->vlan_valid &&
		       policy->vlan_applied &&
		       airoha_ctc_policy_vlan_equal(policy, &decoded);
	default:
		return false;
	}
}

static inline bool airoha_ctc_management_descriptor_applied(
		const struct airoha_ctc_management_set *management,
		const struct airoha_ctc_object_context *object, uint8_t branch,
		uint16_t leaf,
		const uint8_t *value, size_t width)
{
	uint32_t seconds;
	unsigned int first_uni, last_uni, uni;

	if (!management)
		return false;
	if (branch == AIROHA_CTC_EXTENDED_ATTRIBUTE_BRANCH &&
	    leaf == AIROHA_CTC_PORT_MAC_AGING_TIME)
		return !airoha_ctc_decode_mac_aging(value, width, &seconds) &&
		       management->aging_present && management->aging_valid &&
		       management->aging_applied &&
		       management->aging_time_seconds == seconds;
	if (!object || !object->present || (!object->valid && !object->all))
		return false;
	if (object->all) {
		first_uni = 0;
		last_uni = AIROHA_CTC_ETHERNET_UNI_COUNT;
	} else {
		first_uni = object->uni - 1;
		last_uni = first_uni + 1;
	}
	for (uni = first_uni; uni < last_uni; uni++)
		if (!airoha_ctc_management_port_applied(&management->uni[uni],
				branch, leaf, value, width))
			return false;
	return true;
}

static inline int airoha_ctc_build_variable_set_response(
		const uint8_t *request, size_t request_length, uint8_t *response,
		size_t capacity, const struct airoha_ctc_fec_set *fec,
		const struct airoha_ctc_holdover_set *holdover,
		const struct airoha_ctc_management_set *management,
		struct airoha_ctc_variable_result *result)
{
	size_t input = 0, output = 0;
	struct airoha_ctc_object_context object = { 0 };

	if (!request || !response || !result)
		return -EINVAL;
	memset(result, 0, sizeof(*result));
	while (input < request_length) {
		uint16_t holdover_time_ms, leaf;
		uint8_t width;
		bool enabled;
		size_t consumed;

		if (request_length - input >= 3 && !request[input] &&
		    !request[input + 1] && !request[input + 2])
			break;
		if (airoha_ctc_is_object_branch(request[input])) {
			if (airoha_ctc_parse_object(request + input,
					request_length - input, &object, &consumed))
				return -EINVAL;
			if (capacity - output < consumed)
				return -ENOSPC;
			memcpy(response + output, request + input, consumed);
			input += consumed;
			output += consumed;
			continue;
		}
		if (request_length - input < 4)
			return -EINVAL;
		width = request[input + 3];
		if ((size_t)width > request_length - input - 4)
			return -EINVAL;
		if (capacity - output < 4)
			return -ENOSPC;
		memcpy(response + output, request + input, 3);
		leaf = (uint16_t)request[input + 1] << 8 | request[input + 2];
		if (airoha_ctc_is_standard_fec(request[input], leaf) && fec &&
		    fec->present && fec->valid && fec->applied &&
		    !airoha_ctc_decode_standard_fec(request + input + 4, width,
						  &enabled) &&
		    enabled == fec->enabled) {
			response[output + 3] = AIROHA_CTC_VARIABLE_STATUS_SUCCESS;
			result->supported++;
		} else if (request[input] ==
			   AIROHA_CTC_EXTENDED_ATTRIBUTE_BRANCH &&
			   leaf == AIROHA_CTC_HOLDOVER_CONFIG && holdover &&
			   holdover->present && holdover->valid &&
			   holdover->applied &&
			   !airoha_ctc_decode_holdover(request + input + 4, width,
						  &enabled,
						  &holdover_time_ms) &&
			   enabled == holdover->enabled &&
			   holdover_time_ms == holdover->time_ms) {
			response[output + 3] = AIROHA_CTC_VARIABLE_STATUS_SUCCESS;
			result->supported++;
		} else if (airoha_ctc_management_descriptor_applied(management,
				&object, request[input], leaf,
				request + input + 4, width)) {
			response[output + 3] = AIROHA_CTC_VARIABLE_STATUS_SUCCESS;
			result->supported++;
		} else {
			response[output + 3] =
				AIROHA_CTC_VARIABLE_STATUS_UNSUPPORTED;
		}
		result->descriptors++;
		input += 4 + width;
		output += 4;
	}
	return (int)output;
}

static inline int airoha_ctc_build_organization_frame(
		const uint8_t onu_mac[6], const uint8_t oui[3], uint16_t flags,
		uint8_t opcode, const uint8_t *payload, size_t payload_length,
		uint8_t *frame, size_t capacity)
{
	static const uint8_t slow_protocols_mac[6] = {
		0x01, 0x80, 0xc2, 0x00, 0x00, 0x02
	};
	size_t length = AIROHA_CTC_OAM_HEADER_LENGTH +
		AIROHA_CTC_ORGANIZATION_HEADER_LENGTH + payload_length;

	if (!onu_mac || !oui || (!payload && payload_length) || !frame)
		return -EINVAL;
	if (capacity < length)
		return -ENOSPC;
	memcpy(frame, slow_protocols_mac, sizeof(slow_protocols_mac));
	memcpy(frame + 6, onu_mac, 6);
	frame[12] = 0x88;
	frame[13] = 0x09;
	frame[14] = 0x03;
	frame[15] = flags >> 8;
	frame[16] = flags & 0xff;
	frame[17] = 0xfe;
	memcpy(frame + AIROHA_CTC_OAM_HEADER_LENGTH, oui, 3);
	frame[AIROHA_CTC_OAM_HEADER_LENGTH + 3] = opcode;
	if (payload_length)
		memcpy(frame + AIROHA_CTC_OAM_HEADER_LENGTH +
		       AIROHA_CTC_ORGANIZATION_HEADER_LENGTH,
		       payload, payload_length);
	return (int)length;
}

static inline int airoha_ctc_build_auth_response(uint8_t requested_type,
		const uint8_t loid[AIROHA_CTC_LOID_LENGTH],
		const uint8_t password[AIROHA_CTC_PASSWORD_LENGTH],
		uint8_t *response, size_t capacity)
{
	if (!response)
		return -EINVAL;
	if (requested_type != AIROHA_CTC_AUTH_LOID_PASSWORD) {
		if (capacity < 5)
			return -ENOSPC;
		response[0] = AIROHA_CTC_AUTH_RESPONSE;
		response[1] = 0;
		response[2] = 2;
		response[3] = AIROHA_CTC_AUTH_NAK;
		response[4] = AIROHA_CTC_AUTH_LOID_PASSWORD;
		return 5;
	}
	if (!loid || !password ||
	    capacity < AIROHA_CTC_AUTH_CREDENTIAL_RESPONSE_LENGTH)
		return -ENOSPC;
	response[0] = AIROHA_CTC_AUTH_RESPONSE;
	response[1] = 0;
	response[2] = 1 + AIROHA_CTC_LOID_LENGTH + AIROHA_CTC_PASSWORD_LENGTH;
	response[3] = AIROHA_CTC_AUTH_LOID_PASSWORD;
	memcpy(response + 4, loid, AIROHA_CTC_LOID_LENGTH);
	memcpy(response + 4 + AIROHA_CTC_LOID_LENGTH, password,
	       AIROHA_CTC_PASSWORD_LENGTH);
	return AIROHA_CTC_AUTH_CREDENTIAL_RESPONSE_LENGTH;
}

static inline int airoha_ctc_build_churning_response(uint8_t key_index,
		const uint8_t key[AIROHA_CTC_CHURNING_KEY_LENGTH],
		uint8_t *response, size_t capacity)
{
	if (key_index > 1 || !key || !response)
		return -EINVAL;
	if (capacity < AIROHA_CTC_CHURNING_RESPONSE_LENGTH)
		return -ENOSPC;
	response[0] = AIROHA_CTC_CHURNING_RESPONSE;
	response[1] = key_index;
	memcpy(response + 2, key, AIROHA_CTC_CHURNING_KEY_LENGTH);
	return AIROHA_CTC_CHURNING_RESPONSE_LENGTH;
}

static inline int airoha_ctc_build_dba_get_response(uint8_t queue_set_count,
		const uint8_t report_bitmap[AIROHA_CTC_DBA_THRESHOLD_SET_COUNT],
		const uint16_t threshold[AIROHA_CTC_DBA_THRESHOLD_SET_COUNT]
					[AIROHA_CTC_DBA_QUEUE_COUNT],
		uint8_t *response, size_t capacity)
{
	size_t offset = 0;
	unsigned int set, queue;

	if (!report_bitmap || !threshold || !response || !queue_set_count ||
	    queue_set_count > AIROHA_CTC_DBA_QUEUE_SET_COUNT)
		return -EINVAL;
	if (capacity < 3)
		return -ENOSPC;
	response[offset++] = AIROHA_CTC_DBA_GET_RESPONSE;
	response[offset++] = queue_set_count;
	for (set = 0; set + 1 < queue_set_count; set++) {
		uint8_t bitmap = report_bitmap[set];

		if (offset == capacity)
			return -ENOSPC;
		response[offset++] = bitmap;
		for (queue = 0; queue < AIROHA_CTC_DBA_QUEUE_COUNT; queue++) {
			if (!(bitmap & (UINT8_C(1) << queue)))
				continue;
			if (capacity - offset < 2)
				return -ENOSPC;
			response[offset++] = threshold[set][queue] >> 8;
			response[offset++] = threshold[set][queue] & 0xff;
		}
	}
	if (offset == capacity)
		return -ENOSPC;
	response[offset++] = 0;
	return (int)offset;
}

static inline int airoha_ctc_parse_dba_set(const uint8_t *payload,
		size_t length, uint8_t *queue_set_count,
		uint8_t report_bitmap[AIROHA_CTC_DBA_THRESHOLD_SET_COUNT],
		uint16_t threshold[AIROHA_CTC_DBA_THRESHOLD_SET_COUNT]
				  [AIROHA_CTC_DBA_QUEUE_COUNT])
{
	unsigned int set, queue;
	uint8_t count;
	size_t offset = 2;

	if (!payload || !queue_set_count || !report_bitmap || !threshold ||
	    length < 2 || payload[0] != AIROHA_CTC_DBA_SET_REQUEST)
		return -EINVAL;
	count = payload[1];
	if (count < 2 || count > AIROHA_CTC_DBA_QUEUE_SET_COUNT ||
	    length != 2 + (size_t)(count - 1) *
		      AIROHA_CTC_DBA_SET_BYTES_PER_SET)
		return -EINVAL;
	memset(report_bitmap, 0, AIROHA_CTC_DBA_THRESHOLD_SET_COUNT);
	memset(threshold, 0, sizeof(uint16_t) *
	       AIROHA_CTC_DBA_THRESHOLD_SET_COUNT * AIROHA_CTC_DBA_QUEUE_COUNT);
	for (set = 0; set + 1 < count; set++) {
		report_bitmap[set] = payload[offset++];
		for (queue = 0; queue < AIROHA_CTC_DBA_QUEUE_COUNT; queue++) {
			threshold[set][queue] = (uint16_t)payload[offset] << 8 |
				payload[offset + 1];
			offset += 2;
		}
	}
	*queue_set_count = count;
	return 0;
}

static inline int airoha_ctc_build_dba_set_response(bool acknowledged,
		uint8_t response[2], size_t capacity)
{
	if (!response)
		return -EINVAL;
	if (capacity < 2)
		return -ENOSPC;
	response[0] = AIROHA_CTC_DBA_SET_RESPONSE;
	response[1] = acknowledged ? AIROHA_CTC_DBA_ACK : AIROHA_CTC_DBA_NACK;
	return 2;
}

#endif
