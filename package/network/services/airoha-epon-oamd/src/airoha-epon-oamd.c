// SPDX-License-Identifier: GPL-2.0-only
/* Dedicated IEEE 802.3ah OAM endpoint for the Airoha EPON kernel ABI. */

#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <glob.h>
#include <limits.h>
#include <poll.h>
#include <signal.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/random.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>
#include <uci.h>

#include "airoha-epon-ctc.h"
#include "airoha-epon-ctc-manager.h"
#include "airoha-epon-ctc-transaction.h"
#include "airoha-epon-dpoe.h"
#include "airoha-epon-loopback.h"
#include "airoha-epon-oam-abi.h"
#include "airoha-epon-session.h"

#define EPON_LLID_COUNT 32
#define EPON_FRAME_MAX 2048
#define EPON_WIRE_MAX (EPON_FRAME_MAX + 2)
#define EPON_HEADER_LEN 18
#define EPON_ETHERTYPE 0x8809
#define EPON_SUBTYPE 0x03
#define DPOE_KEY_RETRY_MS 500
#define CTC_CLASSIFICATION_TEXT_MAX 49152
#define EN7572_DIAGNOSTICS_GLOB \
	"/sys/bus/i2c/drivers/airoha-en7572/*/optical_diagnostics"

#define OAM_CODE_INFORMATION 0x00
#define OAM_CODE_EVENT 0x01
#define OAM_CODE_VARIABLE_REQUEST 0x02
#define OAM_CODE_VARIABLE_RESPONSE 0x03
#define OAM_CODE_LOOPBACK 0x04
#define OAM_CODE_ORGANIZATION 0xfe

#define OAM_TLV_END 0x00
#define OAM_TLV_LOCAL_INFORMATION 0x01
#define OAM_TLV_REMOTE_INFORMATION 0x02
#define OAM_TLV_ORGANIZATION 0xfe
#define OAM_INFORMATION_LENGTH AIROHA_EPON_INFORMATION_LENGTH

#define OAM_FLAG_LOCAL_STABLE 0x0010
#define OAM_FLAG_REMOTE_STABLE 0x0040
static const uint8_t slow_protocols_mac[6] = {
	0x01, 0x80, 0xc2, 0x00, 0x00, 0x02
};
static const uint8_t dpoe_oui[3] = { 0x00, 0x10, 0x00 };
static const char *const ctc_uni_netdevs[AIROHA_CTC_ETHERNET_UNI_COUNT] = {
	"lan1", "lan2", "lan3", "lan4",
};

struct llid_state {
	struct airoha_epon_session_state session;
	uint64_t rx_frames;
	uint64_t tx_frames;
	uint64_t malformed_frames;
	uint64_t variable_requests;
	uint64_t event_notifications;
	uint64_t loopback_commands;
	uint64_t loopback_errors;
	uint64_t link_lost_events;
	uint64_t link_lost_errors;
	uint64_t ctc_frames;
	uint64_t unsupported_ctc_opcodes;
	uint64_t dpoe_frames;
	uint64_t dpoe_key_events;
	uint64_t dpoe_key_exchanges;
	uint64_t dpoe_key_retries;
	uint64_t dpoe_key_errors;
	uint64_t dpoe_sia_get_requests;
	uint64_t dpoe_sia_set_requests;
	uint64_t dpoe_sia_attributes;
	uint64_t dpoe_sia_read_errors;
	uint64_t dpoe_rekey_triggers;
	uint64_t unsupported_oui_frames;
	uint64_t ctc_auth_frames;
	uint64_t ctc_key_requests;
	uint64_t ctc_key_programs;
	uint64_t ctc_key_errors;
	uint64_t ctc_dba_frames;
	uint64_t ctc_dba_programs;
	uint64_t ctc_dba_errors;
	uint64_t ctc_variable_get_frames;
	uint64_t ctc_variable_set_frames;
	uint64_t ctc_variable_attributes;
	uint64_t ctc_management_requests;
	uint64_t ctc_management_transactions;
	uint64_t ctc_management_errors;
};

struct configuration {
	char pon_mode[sizeof("epon-10g-10g")];
	uint8_t onu_mac[6];
	uint8_t local_oui[3];
	uint8_t ctc_oui[3];
	struct airoha_ctc_identity identity;
	struct airoha_dpoe_inventory dpoe_inventory;
	uint32_t llid_mask;
	unsigned int information_interval_ms;
	unsigned int lost_link_timeout_ms;
	unsigned int max_pdu_rate;
	bool ctc_enabled;
	bool dpoe_enabled;
	uint8_t loid[AIROHA_CTC_LOID_LENGTH];
	uint8_t password[AIROHA_CTC_PASSWORD_LENGTH];
	size_t loid_length;
	size_t password_length;
};

struct daemon_state {
	struct configuration config;
	struct llid_state llids[EPON_LLID_COUNT];
	const char *device_path;
	const char *status_path;
	const char *ready_path;
	const char *ctc_platform_path;
	int device;
	uint64_t started_ms;
	uint64_t rx_frames;
	uint64_t tx_frames;
	uint64_t malformed_frames;
	uint64_t dropped_frames;
	uint64_t rate_window_ms;
	unsigned int rate_window_pdus;
	uint64_t key_programs;
	uint64_t key_errors;
	struct airoha_epon_oam_key key_request;
	struct airoha_epon_oam_dba dba_request;
	struct airoha_epon_oam_fec fec_request;
	struct airoha_epon_oam_loopback loopback_request;
	struct airoha_epon_oam_session session_request;
	struct airoha_epon_oam_key_events key_events_request;
	struct airoha_ctc_management_manager *ctc_management;
	bool memory_locked;
};

static volatile sig_atomic_t stopping;

static void secure_clear(void *memory, size_t length)
{
	volatile uint8_t *byte = memory;

	while (length--)
		*byte++ = 0;
}

static uint64_t monotonic_ms(void)
{
	struct timespec now;

	if (clock_gettime(CLOCK_MONOTONIC, &now))
		return 0;
	return (uint64_t)now.tv_sec * 1000 + now.tv_nsec / 1000000;
}

static void signal_handler(int signal_number)
{
	(void)signal_number;
	stopping = 1;
}

static bool parse_hex_octets(const char *value, uint8_t *output, size_t count)
{
	size_t index = 0;
	unsigned int high = 0, low = 0;

	if (!value)
		return false;
	while (*value && index < count) {
		if (*value == ':' || *value == '-') {
			value++;
			continue;
		}
		if (sscanf(value, "%1x%1x", &high, &low) != 2)
			return false;
		output[index++] = (high << 4) | low;
		value += 2;
	}
	while (*value == ':' || *value == '-')
		value++;
	return index == count && *value == '\0';
}

static bool valid_unicast_mac(const uint8_t mac[6])
{
	uint8_t combined = 0;
	size_t index;

	if (!mac || (mac[0] & 1))
		return false;
	for (index = 0; index < 6; index++)
		combined |= mac[index];
	return combined != 0;
}

static bool parse_unsigned(const char *value, unsigned long maximum,
			   unsigned long *result)
{
	char *end = NULL;
	unsigned long parsed;

	if (!value || !*value || *value == '-')
		return false;
	errno = 0;
	parsed = strtoul(value, &end, 0);
	if (errno || !end || *end || parsed > maximum)
		return false;
	*result = parsed;
	return true;
}

static void read_optical_diagnostics(
		struct airoha_ctc_optical_diagnostics *diagnostics)
{
	unsigned long value[5];
	char line[160], trailing;
	glob_t matches = { 0 };
	FILE *file = NULL;
	int fields;
	size_t index;

	memset(diagnostics, 0, sizeof(*diagnostics));
	if (glob(EN7572_DIAGNOSTICS_GLOB, 0, NULL, &matches) ||
	    matches.gl_pathc != 1)
		goto out;
	file = fopen(matches.gl_pathv[0], "re");
	if (!file || !fgets(line, sizeof(line), file))
		goto out;
	fields = sscanf(line, "%lu %lu %lu %lu %lu %c", &value[0], &value[1],
			&value[2], &value[3], &value[4], &trailing);
	if (fields != 5)
		goto out;
	for (index = 0; index < 5; index++)
		if (value[index] > UINT16_MAX)
			goto out;
	diagnostics->temperature = value[0];
	diagnostics->supply_voltage = value[1];
	diagnostics->laser_bias_current = value[2];
	diagnostics->transmit_power = value[3];
	diagnostics->receive_power = value[4];
	diagnostics->available = true;
out:
	if (file)
		fclose(file);
	globfree(&matches);
}

static void read_dpoe_optical_diagnostics(
		struct airoha_dpoe_optical_diagnostics *diagnostics)
{
	struct airoha_ctc_optical_diagnostics ctc = { 0 };

	read_optical_diagnostics(&ctc);
	diagnostics->available = ctc.available;
	diagnostics->temperature = ctc.temperature;
	diagnostics->supply_voltage = ctc.supply_voltage;
	diagnostics->laser_bias_current = ctc.laser_bias_current;
	diagnostics->transmit_power = ctc.transmit_power;
	diagnostics->receive_power = ctc.receive_power;
	secure_clear(&ctc, sizeof(ctc));
}

static bool copy_fixed_secret(const char *value, uint8_t *destination,
			      size_t capacity, size_t *length)
{
	size_t value_length;

	if (!value)
		value = "";
	value_length = strlen(value);
	if (value_length > capacity)
		return false;
	memset(destination, 0, capacity);
	memcpy(destination, value, value_length);
	*length = value_length;
	return true;
}

static void copy_fixed_text(const char *value, const char *fallback,
			    uint8_t *destination, size_t capacity)
{
	size_t length;

	if (!value || !*value)
		value = fallback;
	length = strlen(value);
	if (length > capacity)
		length = capacity;
	memset(destination, ' ', capacity);
	memcpy(destination, value, length);
}

static bool copy_variable_text(const char *value, const char *fallback,
			       uint8_t *destination, size_t capacity,
			       size_t *length)
{
	if (!value || !*value)
		value = fallback;
	*length = strlen(value);
	if (!*length || *length > capacity)
		return false;
	memset(destination, 0, capacity);
	memcpy(destination, value, *length);
	return true;
}

static bool copy_dpoe_text(const char *value, const char *fallback,
			    struct airoha_dpoe_text *text)
{
	size_t length;

	if (!value || !*value)
		value = fallback;
	if (!value)
		return false;
	length = strlen(value);
	if (!length || length > sizeof(text->value))
		return false;
	memset(text, 0, sizeof(*text));
	memcpy(text->value, value, length);
	text->length = length;
	return true;
}

static const char *uci_option(struct uci_context *context,
			      struct uci_package *package,
			      const char *section_name, const char *option)
{
	struct uci_section *section;

	section = uci_lookup_section(context, package, section_name);
	return section ? uci_lookup_option_string(context, section, option) : NULL;
}

static bool uci_boolean(const char *value, bool default_value)
{
	if (!value)
		return default_value;
	return !strcmp(value, "1") || !strcmp(value, "true") ||
	       !strcmp(value, "yes") || !strcmp(value, "on");
}

static bool valid_epon_mode(const char *mode)
{
	return mode && (!strcmp(mode, "epon-10g-1g") ||
			!strcmp(mode, "epon-10g-10g"));
}

static int load_configuration(struct configuration *config,
			      const char *pon_mode_override)
{
	struct uci_context *context;
	struct uci_package *gpon = NULL, *epon = NULL;
	const char *value;
	unsigned long number;
	int result = -EINVAL;

	memset(config, 0, sizeof(*config));
	config->information_interval_ms = 1000;
	config->lost_link_timeout_ms =
		AIROHA_EPON_LOST_LINK_TIMEOUT_DEFAULT_MS;
	config->max_pdu_rate = 20;
	config->ctc_enabled = true;
	config->dpoe_enabled = true;
	memcpy(config->local_oui, (uint8_t[]){ 0x00, 0x0d, 0xb6 }, 3);
	memcpy(config->ctc_oui, (uint8_t[]){ 0x11, 0x11, 0x11 }, 3);

	context = uci_alloc_context();
	if (!context)
		return -ENOMEM;
	if (uci_load(context, "airoha-gpon", &gpon) != UCI_OK ||
	    uci_load(context, "airoha-epon", &epon) != UCI_OK)
		goto out;
	value = pon_mode_override ? pon_mode_override :
		uci_option(context, gpon, "main", "pon_mode");
	if (!valid_epon_mode(value) ||
	    snprintf(config->pon_mode, sizeof(config->pon_mode), "%s", value) >=
		    (int)sizeof(config->pon_mode))
		goto out;
	/* procd resolves rollback overrides before launching us. Without an
	 * explicit mode, a direct invocation must still honor both UCI gates. */
	if (!pon_mode_override &&
	    (!uci_boolean(uci_option(context, gpon, "main", "enabled"), false) ||
	     !uci_boolean(uci_option(context, epon, "main", "enabled"), true)))
		goto out;
	value = uci_option(context, gpon, "main", "epon_onu_mac");
	if (!parse_hex_octets(value, config->onu_mac, sizeof(config->onu_mac)) ||
	    (config->onu_mac[0] & 1))
		goto out;
	memcpy(config->identity.onu_mac, config->onu_mac,
	       sizeof(config->identity.onu_mac));
	value = uci_option(context, gpon, "main", "epon_llid_mask");
	if (!parse_unsigned(value, UINT32_MAX, &number) || !number)
		goto out;
	config->llid_mask = (uint32_t)number;
	/* Match the legacy SDK wire image; LLID state remains zero until known. */
	memset(config->dpoe_inventory.llid_count, 0,
	       sizeof(config->dpoe_inventory.llid_count));
	config->dpoe_inventory.pon_port_count[0] = 1;
	config->dpoe_inventory.pon_port_count[1] = 1;
	config->dpoe_inventory.uni_port_count = AIROHA_CTC_ETHERNET_UNI_COUNT;
	config->dpoe_inventory.packet_buffer_kib = 4;
	config->dpoe_inventory.uni_port_type = 1;

	value = uci_option(context, epon, "main", "local_oui");
	if (value && !parse_hex_octets(value, config->local_oui, 3))
		goto out;
	value = uci_option(context, epon, "main", "ctc_oui");
	if (value && !parse_hex_octets(value, config->ctc_oui, 3))
		goto out;
	config->ctc_enabled = uci_boolean(
		uci_option(context, epon, "main", "ctc_enabled"), true);
	config->dpoe_enabled = uci_boolean(
		uci_option(context, epon, "main", "dpoe_enabled"), true);
	copy_fixed_text(uci_option(context, epon, "main", "vendor_id"),
		"AIRO", config->identity.vendor_id,
		sizeof(config->identity.vendor_id));
	copy_fixed_text(uci_option(context, epon, "main", "model"),
		"XG2010G", config->identity.model,
		sizeof(config->identity.model));
	copy_fixed_text(uci_option(context, epon, "main", "hardware_version"),
		"XG2010G", config->identity.hardware_version,
		sizeof(config->identity.hardware_version));
	copy_fixed_text(uci_option(context, epon, "main", "software_version"),
		"OpenWrt", config->identity.software_version,
		sizeof(config->identity.software_version));
	copy_fixed_text(uci_option(context, epon, "main", "chipset_id"),
		"AN7581", config->identity.chipset_id,
		sizeof(config->identity.chipset_id));
	if (!copy_variable_text(
		    uci_option(context, epon, "main", "firmware_version"),
		    "OpenWrt", config->identity.firmware_version,
		    sizeof(config->identity.firmware_version),
		    &config->identity.firmware_version_length))
		goto out;
	if (!copy_dpoe_text(uci_option(context, epon, "main", "firmware_version"),
			    "OpenWrt", &config->dpoe_inventory.firmware_version) ||
	    !copy_dpoe_text(uci_option(context, epon, "main", "chipset_id"),
			    "AN7581", &config->dpoe_inventory.chipset) ||
	    !copy_dpoe_text(uci_option(context, epon, "main", "manufacture_date"),
			    "unknown", &config->dpoe_inventory.manufacture_date) ||
	    !copy_dpoe_text(uci_option(context, epon, "main", "manufacturer"),
			    "Airoha", &config->dpoe_inventory.manufacturer) ||
	    !copy_dpoe_text(uci_option(context, epon, "main", "organization_name"),
			    "Airoha", &config->dpoe_inventory.organization_name) ||
	    !copy_dpoe_text(uci_option(context, epon, "main", "vendor_id"),
			    "AIRO", &config->dpoe_inventory.vendor_name) ||
	    !copy_dpoe_text(uci_option(context, epon, "main", "model"),
			    "XG2010G", &config->dpoe_inventory.model_number) ||
	    !copy_dpoe_text(uci_option(context, epon, "main", "hardware_version"),
			    "XG2010G", &config->dpoe_inventory.hardware_version) ||
	    !copy_dpoe_text(!strcmp(config->pon_mode, "epon-10g-1g") ?
			    "10G/1G" : "10G/10G",
			    NULL, &config->dpoe_inventory.line_rate_mode) ||
	    !copy_dpoe_text(uci_option(context, epon, "main", "software_version"),
			    "OpenWrt", &config->dpoe_inventory.software_bundle) ||
	    !copy_dpoe_text(uci_option(context, epon, "main", "firmware_filename"),
			    "OpenWrt", &config->dpoe_inventory.firmware_filename))
		goto out;
	if (!copy_fixed_secret(uci_option(context, epon, "main", "loid"),
			       config->loid, sizeof(config->loid),
			       &config->loid_length) ||
	    !copy_fixed_secret(uci_option(context, epon, "main", "password"),
			       config->password, sizeof(config->password),
			       &config->password_length))
		goto out;
	value = uci_option(context, epon, "main", "info_interval_ms");
	if (value) {
		if (!parse_unsigned(value, 60000, &number) || number < 100)
			goto out;
		config->information_interval_ms = number;
	}
	value = uci_option(context, epon, "main", "lost_link_timeout_ms");
	if (value) {
		if (!parse_unsigned(value,
				    AIROHA_EPON_LOST_LINK_TIMEOUT_MAX_MS, &number) ||
		    number < AIROHA_EPON_LOST_LINK_TIMEOUT_MIN_MS)
			goto out;
		config->lost_link_timeout_ms = number;
	}
	value = uci_option(context, epon, "main", "max_pdu_rate");
	if (value) {
		if (!parse_unsigned(value, 1000, &number) || !number)
			goto out;
		config->max_pdu_rate = number;
	}
	result = 0;
out:
	if (gpon)
		uci_unload(context, gpon);
	if (epon)
		uci_unload(context, epon);
	uci_free_context(context);
	return result;
}

static int ensure_parent_directory(const char *path)
{
	char directory[PATH_MAX];
	char *separator;

	if (strlen(path) >= sizeof(directory))
		return -ENAMETOOLONG;
	strcpy(directory, path);
	separator = strrchr(directory, '/');
	if (!separator || separator == directory)
		return 0;
	*separator = '\0';
	if (!mkdir(directory, 0755) || errno == EEXIST)
		return 0;
	return -errno;
}

static const char *discovery_name(enum airoha_epon_discovery_state state)
{
	switch (state) {
	case AIROHA_EPON_DISCOVERY_WAITING:
		return "waiting";
	case AIROHA_EPON_DISCOVERY_LOCAL_SENT:
		return "local-information-sent";
	case AIROHA_EPON_DISCOVERY_ESTABLISHED:
		return "established";
	}
	return "invalid";
}

static const char *authentication_name(
		enum airoha_epon_authentication_state state)
{
	switch (state) {
	case AIROHA_EPON_AUTHENTICATION_IDLE:
		return "idle";
	case AIROHA_EPON_AUTHENTICATION_CREDENTIALS_SENT:
		return "credentials-sent";
	case AIROHA_EPON_AUTHENTICATION_SUCCEEDED:
		return "succeeded";
	case AIROHA_EPON_AUTHENTICATION_FAILED:
		return "failed";
	}
	return "invalid";
}

static int write_status(const struct daemon_state *state)
{
	char temporary[PATH_MAX];
	FILE *file;
	int result;
	unsigned int index;
	bool first = true;

	if (snprintf(temporary, sizeof(temporary), "%s.tmp.%ld",
		     state->status_path, (long)getpid()) >= (int)sizeof(temporary))
		return -ENAMETOOLONG;
	result = ensure_parent_directory(state->status_path);
	if (result)
		return result;
	file = fopen(temporary, "w");
	if (!file)
		return -errno;
	fprintf(file,
		"{\"version\":1,\"state\":\"online\",\"control_plane\":\"epon-oam\","
		"\"pon_mode\":\"%s\",\"omci\":false,\"ieee8023ah\":true,"
		"\"ctc_dispatch\":true,"
		"\"ctc_management\":true,\"ctc_authentication\":true,"
		"\"ctc_phy_admin\":true,\"ctc_autoneg_admin\":true,"
		"\"ctc_autoneg_restart\":true,"
		"\"ctc_ethernet_pause\":true,\"ctc_upstream_policing\":true,"
		"\"ctc_downstream_rate_limiting\":true,\"ctc_mac_aging\":true,"
		"\"ctc_vlan\":true,\"ctc_classification\":true,"
		"\"ctc_dba\":true,\"ctc_fec\":true,"
		"\"ctc_key_exchange\":true,\"dpoe_dispatch\":true,"
		"\"dpoe_management\":true,\"dpoe_sia_inventory\":true,"
		"\"dpoe_sia_optical_diagnostics\":true,"
		"\"dpoe_sia_encryption\":true,"
		"\"dpoe_key_exchange\":true,\"remote_loopback\":true,"
		"\"uptime_ms\":%llu,\"rx_frames\":%llu,"
		"\"tx_frames\":%llu,\"malformed_frames\":%llu,"
		"\"dropped_frames\":%llu,\"key_programs\":%llu,"
		"\"key_errors\":%llu,\"lost_link_timeout_ms\":%u,"
		"\"llids\":[",
		state->config.pon_mode,
		(unsigned long long)(monotonic_ms() - state->started_ms),
		(unsigned long long)state->rx_frames,
		(unsigned long long)state->tx_frames,
		(unsigned long long)state->malformed_frames,
		(unsigned long long)state->dropped_frames,
		(unsigned long long)state->key_programs,
		(unsigned long long)state->key_errors,
		state->config.lost_link_timeout_ms);
	for (index = 0; index < EPON_LLID_COUNT; index++) {
		const struct llid_state *llid;

		if (!(state->config.llid_mask & (UINT32_C(1) << index)))
			continue;
		llid = &state->llids[index];
		fprintf(file,
			"%s{\"index\":%u,\"discovery\":\"%s\",\"rx_frames\":%llu,"
			"\"tx_frames\":%llu,\"ctc_frames\":%llu,"
			"\"unsupported_ctc_opcodes\":%llu,"
			"\"dpoe_frames\":%llu,\"dpoe_key_events\":%llu,"
			"\"dpoe_key_exchanges\":%llu,\"dpoe_key_retries\":%llu,"
			"\"dpoe_key_errors\":%llu,\"dpoe_sia_get_requests\":%llu,"
			"\"dpoe_sia_set_requests\":%llu,"
			"\"dpoe_sia_attributes\":%llu,\"dpoe_sia_read_errors\":%llu,"
			"\"dpoe_rekey_triggers\":%llu,"
			"\"dpoe_rekey_armed\":%s,\"unsupported_oui_frames\":%llu,"
			"\"ctc_authentication\":\"%s\",\"ctc_auth_frames\":%llu,"
			"\"ctc_key_requests\":%llu,\"ctc_key_programs\":%llu,"
			"\"ctc_key_errors\":%llu,\"ctc_dba_frames\":%llu,"
			"\"ctc_dba_programs\":%llu,\"ctc_dba_errors\":%llu,"
			"\"ctc_variable_get_frames\":%llu,"
			"\"ctc_variable_set_frames\":%llu,"
			"\"ctc_variable_attributes\":%llu,"
			"\"ctc_management_requests\":%llu,"
			"\"ctc_management_transactions\":%llu,"
			"\"ctc_management_errors\":%llu,"
			"\"loopback_enabled\":%s,\"loopback_commands\":%llu,"
			"\"loopback_errors\":%llu,\"link_lost_events\":%llu,"
			"\"link_lost_errors\":%llu}",
			first ? "" : ",", index,
			discovery_name(llid->session.discovery),
			(unsigned long long)llid->rx_frames,
			(unsigned long long)llid->tx_frames,
			(unsigned long long)llid->ctc_frames,
			(unsigned long long)llid->unsupported_ctc_opcodes,
			(unsigned long long)llid->dpoe_frames,
			(unsigned long long)llid->dpoe_key_events,
			(unsigned long long)llid->dpoe_key_exchanges,
			(unsigned long long)llid->dpoe_key_retries,
			(unsigned long long)llid->dpoe_key_errors,
			(unsigned long long)llid->dpoe_sia_get_requests,
			(unsigned long long)llid->dpoe_sia_set_requests,
			(unsigned long long)llid->dpoe_sia_attributes,
			(unsigned long long)llid->dpoe_sia_read_errors,
			(unsigned long long)llid->dpoe_rekey_triggers,
			llid->session.dpoe_rekey.armed ? "true" : "false",
			(unsigned long long)llid->unsupported_oui_frames,
			authentication_name(llid->session.authentication),
			(unsigned long long)llid->ctc_auth_frames,
			(unsigned long long)llid->ctc_key_requests,
			(unsigned long long)llid->ctc_key_programs,
			(unsigned long long)llid->ctc_key_errors,
			(unsigned long long)llid->ctc_dba_frames,
			(unsigned long long)llid->ctc_dba_programs,
			(unsigned long long)llid->ctc_dba_errors,
			(unsigned long long)llid->ctc_variable_get_frames,
			(unsigned long long)llid->ctc_variable_set_frames,
			(unsigned long long)llid->ctc_variable_attributes,
			(unsigned long long)llid->ctc_management_requests,
			(unsigned long long)llid->ctc_management_transactions,
			(unsigned long long)llid->ctc_management_errors,
			llid->session.loopback_enabled ? "true" : "false",
			(unsigned long long)llid->loopback_commands,
			(unsigned long long)llid->loopback_errors,
			(unsigned long long)llid->link_lost_events,
			(unsigned long long)llid->link_lost_errors);
		first = false;
	}
	fputs("]}\n", file);
	result = fflush(file);
	if (!result)
		result = fsync(fileno(file));
	if (fclose(file) && !result)
		result = -1;
	if (result) {
		int error = errno ? errno : EIO;
		unlink(temporary);
		return -error;
	}
	if (rename(temporary, state->status_path)) {
		int error = errno;
		unlink(temporary);
		return -error;
	}
	return 0;
}

static bool transmit_allowed(struct daemon_state *state)
{
	uint64_t now = monotonic_ms();

	if (now - state->rate_window_ms >= 1000) {
		state->rate_window_ms = now;
		state->rate_window_pdus = 0;
	}
	if (state->rate_window_pdus >= state->config.max_pdu_rate)
		return false;
	state->rate_window_pdus++;
	return true;
}

static int transmit_frame(struct daemon_state *state, unsigned int llid_index,
			  const uint8_t *frame, size_t length)
{
	uint8_t wire[EPON_WIRE_MAX];
	uint16_t encoded_index;
	ssize_t written;

	if (llid_index >= EPON_LLID_COUNT || length < EPON_HEADER_LEN ||
	    length > EPON_FRAME_MAX)
		return -EINVAL;
	if (!transmit_allowed(state)) {
		state->dropped_frames++;
		return -EAGAIN;
	}
	encoded_index = htons(llid_index);
	memcpy(wire, &encoded_index, sizeof(encoded_index));
	memcpy(wire + sizeof(encoded_index), frame, length);
	written = write(state->device, wire, length + sizeof(encoded_index));
	if (written < 0) {
		int error = errno;

		secure_clear(wire, sizeof(wire));
		errno = error;
		return -errno;
	}
	secure_clear(wire, sizeof(wire));
	if ((size_t)written != length + sizeof(encoded_index))
		return -EIO;
	state->tx_frames++;
	state->llids[llid_index].tx_frames++;
	return 0;
}

static size_t build_header(const struct daemon_state *state, uint8_t *frame,
			   uint16_t flags, uint8_t code)
{
	memcpy(frame, slow_protocols_mac, 6);
	memcpy(frame + 6, state->config.onu_mac, 6);
	frame[12] = EPON_ETHERTYPE >> 8;
	frame[13] = EPON_ETHERTYPE & 0xff;
	frame[14] = EPON_SUBTYPE;
	frame[15] = flags >> 8;
	frame[16] = flags & 0xff;
	frame[17] = code;
	return EPON_HEADER_LEN;
}

static int install_key(struct daemon_state *state, unsigned int llid_index,
		       uint8_t suite, uint8_t key_index, const uint8_t *key,
		       size_t key_length, const uint8_t *olt_mac)
{
	struct airoha_epon_oam_key *request = &state->key_request;
	int saved_error;
	int result;

	if (!key || key_length > sizeof(request->key))
		return -EINVAL;
	secure_clear(request, sizeof(*request));
	request->version = AIROHA_EPON_OAM_ABI_VERSION;
	request->suite = suite;
	request->llid_index = llid_index;
	request->key_index = key_index;
	request->key_length = key_length;
	memcpy(request->key, key, key_length);
	if (olt_mac) {
		request->flags = AIROHA_EPON_OAM_KEY_F_OLT_MAC;
		memcpy(request->olt_mac, olt_mac, sizeof(request->olt_mac));
	}
	result = ioctl(state->device, AIROHA_EPON_OAM_IOC_SET_KEY, request);
	saved_error = errno;
	secure_clear(request, sizeof(*request));
	if (result) {
		state->key_errors++;
		return -saved_error;
	}
	state->key_programs++;
	return 0;
}

static int random_bytes(uint8_t *output, size_t length)
{
	size_t offset = 0;

	while (offset < length) {
		ssize_t received = getrandom(output + offset, length - offset, 0);

		if (received < 0 && errno == EINTR)
			continue;
		if (received <= 0)
			return received ? -errno : -EIO;
		offset += received;
	}
	return 0;
}

static int fetch_dpoe_key_events(struct daemon_state *state)
{
	struct airoha_epon_oam_key_events *events = &state->key_events_request;
	uint32_t pending;
	unsigned int index;

	memset(events, 0, sizeof(*events));
	events->version = AIROHA_EPON_OAM_KEY_EVENTS_VERSION;
	if (ioctl(state->device, AIROHA_EPON_OAM_IOC_GET_KEY_EVENTS, events)) {
		int error = errno;

		secure_clear(events, sizeof(*events));
		return -error;
	}
	pending = (events->downstream_llids | events->upstream_llids) &
		state->config.llid_mask;
	for (index = 0; index < EPON_LLID_COUNT; index++) {
		struct llid_state *llid;

		if (!(pending & (UINT32_C(1) << index)))
			continue;
		llid = &state->llids[index];
		llid->dpoe_key_events++;
		llid->session.dpoe_key_event_pending = true;
		if (events->downstream_llids & (UINT32_C(1) << index))
			llid->session.dpoe_event_directions |= 1;
		if (events->upstream_llids & (UINT32_C(1) << index))
			llid->session.dpoe_event_directions |= 2;
	}
	secure_clear(events, sizeof(*events));
	return 0;
}

static int service_dpoe_key_exchange(struct daemon_state *state,
				      unsigned int index)
{
	struct llid_state *llid = &state->llids[index];
	struct airoha_dpoe_key_exchange *exchange =
		&llid->session.dpoe_exchange;
	uint8_t random_key[AIROHA_DPOE_AES_KEY_LENGTH];
	uint8_t frame[AIROHA_DPOE_KEY_FRAME_LENGTH];
	uint64_t now = monotonic_ms();
	int length;
	int result;

	if (airoha_dpoe_rekey_consume(&llid->session.dpoe_rekey, now)) {
		llid->session.dpoe_key_event_pending = true;
		llid->session.dpoe_event_directions |= 3;
		llid->dpoe_rekey_triggers++;
	}
	if (!llid->session.dpoe_key_event_pending ||
	    !state->config.dpoe_enabled || !llid->dpoe_frames ||
	    llid->session.discovery != AIROHA_EPON_DISCOVERY_ESTABLISHED ||
	    !llid->session.olt_mac_valid ||
	    now < llid->session.dpoe_retry_at_ms)
		return 0;
	if (!exchange->pending) {
		result = random_bytes(random_key, sizeof(random_key));
		if (result)
			goto retry;
		result = airoha_dpoe_exchange_begin(exchange,
			llid->session.dpoe_active_key_index, random_key);
		secure_clear(random_key, sizeof(random_key));
		if (result)
			goto retry;
	}
	if (!exchange->installed) {
		result = install_key(state, index, AIROHA_EPON_OAM_KEY_DPOE_AES_128,
			exchange->key_index, exchange->key, sizeof(exchange->key),
			llid->session.olt_mac);
		if (result)
			goto retry;
		exchange->installed = true;
	}
	length = airoha_dpoe_build_key_frame(state->config.onu_mac,
		exchange->key_index, exchange->key, frame, sizeof(frame));
	if (length < 0) {
		result = length;
		goto retry;
	}
	result = transmit_frame(state, index, frame, (size_t)length);
	secure_clear(frame, sizeof(frame));
	if (result)
		goto retry;

	llid->session.dpoe_active_key_index = exchange->key_index;
	llid->session.dpoe_key_event_pending = false;
	llid->session.dpoe_event_directions = 0;
	llid->session.dpoe_retry_at_ms = 0;
	llid->dpoe_key_exchanges++;
	airoha_dpoe_exchange_clear(exchange);
	return 0;

retry:
	secure_clear(random_key, sizeof(random_key));
	secure_clear(frame, sizeof(frame));
	llid->session.dpoe_retry_at_ms = now + DPOE_KEY_RETRY_MS;
	llid->dpoe_key_retries++;
	llid->dpoe_key_errors++;
	return result;
}

static void service_dpoe_key_exchanges(struct daemon_state *state)
{
	unsigned int index;

	for (index = 0; index < EPON_LLID_COUNT; index++)
		if (state->config.llid_mask & (UINT32_C(1) << index))
			(void)service_dpoe_key_exchange(state, index);
}

static int send_ctc_organization(struct daemon_state *state,
				 unsigned int index, uint16_t flags,
				 uint8_t opcode, const uint8_t *payload,
				 size_t payload_length)
{
	uint8_t response[EPON_FRAME_MAX] = { 0 };
	int length;
	int result;

	length = airoha_ctc_build_organization_frame(state->config.onu_mac,
		state->config.ctc_oui, flags, opcode, payload, payload_length,
		response, sizeof(response));
	if (length < 0)
		return length;
	result = transmit_frame(state, index, response, (size_t)length);
	secure_clear(response, sizeof(response));
	return result;
}

static int wait_ctc_platform(pid_t child)
{
	pid_t waited;
	int status;

	do {
		waited = waitpid(child, &status, 0);
	} while (waited < 0 && errno == EINTR);
	if (waited < 0)
		return -errno;
	if (!WIFEXITED(status))
		return -EIO;
	if (WEXITSTATUS(status) == 3)
		return -EUCLEAN;
	if (WEXITSTATUS(status))
		return -EIO;
	return 0;
}

static int run_ctc_platform(char *const arguments[])
{
	pid_t child;

	child = fork();
	if (child < 0)
		return -errno;
	if (!child) {
		execv(arguments[0], arguments);
		_exit(127);
	}
	return wait_ctc_platform(child);
}

static int write_all(int descriptor, const void *data, size_t length)
{
	const uint8_t *position = data;

	while (length) {
		ssize_t written = write(descriptor, position, length);

		if (written < 0) {
			if (errno == EINTR)
				continue;
			return -errno;
		}
		position += written;
		length -= (size_t)written;
	}
	return 0;
}

static int run_ctc_platform_input(char *const arguments[],
		const char *const input[], size_t input_count)
{
	int descriptors[2], result = 0, wait_result;
	pid_t child;
	size_t index;

	if (pipe2(descriptors, O_CLOEXEC))
		return -errno;
	child = fork();
	if (child < 0) {
		result = -errno;
		close(descriptors[0]);
		close(descriptors[1]);
		return result;
	}
	if (!child) {
		close(descriptors[1]);
		if (dup2(descriptors[0], STDIN_FILENO) < 0)
			_exit(127);
		close(descriptors[0]);
		execv(arguments[0], arguments);
		_exit(127);
	}
	close(descriptors[0]);
	for (index = 0; index < input_count && !result; index++) {
		result = write_all(descriptors[1], input[index], strlen(input[index]));
		if (!result)
			result = write_all(descriptors[1], "\n", 1);
	}
	if (close(descriptors[1]) && !result)
		result = -errno;
	wait_result = wait_ctc_platform(child);
	return wait_result ? wait_result : result;
}

static int recover_ctc_platform(struct daemon_state *state)
{
	char *arguments[] = {
		(char *)state->ctc_platform_path,
		"recover",
		NULL,
	};

	return run_ctc_platform(arguments);
}

static int format_ctc_upstream(const struct airoha_ctc_uni_policy *policy,
		char *output, size_t capacity)
{
	int length;

	if (!policy->upstream_present)
		length = snprintf(output, capacity, "-");
	else if (!policy->upstream_enabled)
		length = snprintf(output, capacity, "0");
	else
		length = snprintf(output, capacity, "1,%u,%u,%u",
			policy->upstream_cir_kbps, policy->upstream_cbs_bytes,
			policy->upstream_ebs_bytes);
	return length < 0 || (size_t)length >= capacity ? -ENOSPC : 0;
}

static int format_ctc_downstream(const struct airoha_ctc_uni_policy *policy,
		char *output, size_t capacity)
{
	int length;

	if (!policy->downstream_present)
		length = snprintf(output, capacity, "-");
	else if (!policy->downstream_enabled)
		length = snprintf(output, capacity, "0");
	else
		length = snprintf(output, capacity, "1,%u,%u",
			policy->downstream_cir_kbps,
			policy->downstream_pir_kbps);
	return length < 0 || (size_t)length >= capacity ? -ENOSPC : 0;
}

static int format_ctc_vlan(const struct airoha_ctc_uni_policy *policy,
		char *output, size_t capacity)
{
	size_t offset = 0;
	unsigned int rule;
	int length;

	if (!policy->vlan_present)
		length = snprintf(output, capacity, "-");
	else if (policy->vlan_mode == AIROHA_CTC_VLAN_MODE_TRANSPARENT)
		length = snprintf(output, capacity, "%u", policy->vlan_mode);
	else
		length = snprintf(output, capacity, "%u,0x%08x",
			policy->vlan_mode, policy->vlan_default_tag);
	if (length < 0 || (size_t)length >= capacity)
		return -ENOSPC;
	offset = length;
	for (rule = 0; rule < policy->vlan_rule_count; rule++) {
		length = snprintf(output + offset, capacity - offset,
			",0x%08x=0x%08x", policy->vlan_old_tag[rule],
			policy->vlan_new_tag[rule]);
		if (length < 0 || (size_t)length >= capacity - offset)
			return -ENOSPC;
		offset += length;
	}
	return 0;
}

static int format_ctc_classification(
		const struct airoha_ctc_uni_policy *policy, char *output,
		size_t capacity)
{
	size_t offset = 0;
	unsigned int rule, match, byte;
	int length;

	if (!policy->classification_present)
		length = snprintf(output, capacity, "-");
	else if (!policy->classification_rule_count)
		length = snprintf(output, capacity, "0");
	else
		length = 0;
	if (length < 0 || (size_t)length >= capacity)
		return -ENOSPC;
	offset = length;
	for (rule = 0; rule < policy->classification_rule_count; rule++) {
		const struct airoha_ctc_classification_rule *current =
			&policy->classification_rule[rule];

		length = snprintf(output + offset, capacity - offset,
			"%s%u,%u,%u", rule ? ";" : "", current->precedence,
			current->queue, current->priority);
		if (length < 0 || (size_t)length >= capacity - offset)
			return -ENOSPC;
		offset += length;
		for (match = 0; match < current->match_count; match++) {
			const struct airoha_ctc_classification_match *field =
				&current->match[match];

			length = snprintf(output + offset, capacity - offset,
				"+%u.%u.", field->field, field->operation);
			if (length < 0 || (size_t)length >= capacity - offset)
				return -ENOSPC;
			offset += length;
			for (byte = 0; byte < field->value_length; byte++) {
				length = snprintf(output + offset, capacity - offset,
					"%02x", field->value[byte]);
				if (length != 2 || (size_t)length >= capacity - offset)
					return -ENOSPC;
				offset += length;
			}
		}
	}
	return 0;
}

static int apply_ctc_platform(void *context,
		const struct airoha_ctc_management_set *effective)
{
	struct daemon_state *state = context;
	char aging[12], pause[AIROHA_CTC_ETHERNET_UNI_COUNT][2];
	char phy_admin[AIROHA_CTC_ETHERNET_UNI_COUNT][2];
	char autoneg[AIROHA_CTC_ETHERNET_UNI_COUNT][2];
	char autoneg_restart[AIROHA_CTC_ETHERNET_UNI_COUNT][2];
	char upstream[AIROHA_CTC_ETHERNET_UNI_COUNT][32];
	char downstream[AIROHA_CTC_ETHERNET_UNI_COUNT][24];
	char vlan[AIROHA_CTC_ETHERNET_UNI_COUNT][192];
	char (*classification)[CTC_CLASSIFICATION_TEXT_MAX];
	const char *input[1 + AIROHA_CTC_ETHERNET_UNI_COUNT * 8];
	char *arguments[] = {
		(char *)state->ctc_platform_path,
		"apply-stdin",
		NULL,
	};
	unsigned int field = 0, uni;
	int length, result = -ENOSPC;

	if (airoha_ctc_management_set_empty(effective))
		return recover_ctc_platform(state);
	classification = calloc(AIROHA_CTC_ETHERNET_UNI_COUNT,
		sizeof(*classification));
	if (!classification)
		return -ENOMEM;
	if (effective->aging_present)
		length = snprintf(aging, sizeof(aging), "%u",
			effective->aging_time_seconds);
	else
		length = snprintf(aging, sizeof(aging), "-");
	if (length < 0 || (size_t)length >= sizeof(aging))
		goto out;
	input[field++] = aging;
	for (uni = 0; uni < AIROHA_CTC_ETHERNET_UNI_COUNT; uni++) {
		const struct airoha_ctc_uni_policy *policy =
			&effective->uni[uni];

		length = snprintf(pause[uni], sizeof(pause[uni]), "%s",
			policy->pause_present ?
			(policy->pause_enabled ? "1" : "0") : "-");
		if (length < 0 || (size_t)length >= sizeof(pause[uni]))
			goto out;
		length = snprintf(phy_admin[uni], sizeof(phy_admin[uni]), "%s",
			policy->phy_admin_present ?
			(policy->phy_admin_enabled ? "1" : "0") : "-");
		if (length < 0 || (size_t)length >= sizeof(phy_admin[uni]))
			goto out;
		length = snprintf(autoneg[uni], sizeof(autoneg[uni]), "%s",
			policy->autoneg_present ?
			(policy->autoneg_enabled ? "1" : "0") : "-");
		if (length < 0 || (size_t)length >= sizeof(autoneg[uni]))
			goto out;
		length = snprintf(autoneg_restart[uni],
			sizeof(autoneg_restart[uni]), "%s",
			policy->autoneg_restart_present ? "1" : "-");
		if (length < 0 ||
		    (size_t)length >= sizeof(autoneg_restart[uni]) ||
		    format_ctc_upstream(policy, upstream[uni],
			    sizeof(upstream[uni])) ||
			    format_ctc_downstream(policy, downstream[uni],
				    sizeof(downstream[uni])) ||
			    format_ctc_vlan(policy, vlan[uni], sizeof(vlan[uni])) ||
				    format_ctc_classification(policy, classification[uni],
					    sizeof(classification[uni])))
			goto out;
		input[field++] = pause[uni];
		input[field++] = upstream[uni];
		input[field++] = downstream[uni];
		input[field++] = phy_admin[uni];
		input[field++] = autoneg[uni];
		input[field++] = autoneg_restart[uni];
		input[field++] = vlan[uni];
		input[field++] = classification[uni];
	}
	result = run_ctc_platform_input(arguments, input, field);
out:
	free(classification);
	return result;
}

static int ctc_transaction_get_fec(void *context, unsigned int llid,
		bool *rx_enabled, bool *tx_enabled)
{
	struct daemon_state *state = context;
	struct airoha_epon_oam_fec fec = {
		.version = AIROHA_EPON_OAM_ABI_VERSION,
		.llid_index = llid,
	};

	if (ioctl(state->device, AIROHA_EPON_OAM_IOC_GET_FEC, &fec))
		return -errno;
	*rx_enabled = fec.rx_enabled;
	*tx_enabled = fec.tx_enabled;
	return 0;
}

static int ctc_transaction_set_fec(void *context, unsigned int llid,
		bool rx_enabled, bool tx_enabled)
{
	struct daemon_state *state = context;
	struct airoha_epon_oam_fec fec = {
		.version = AIROHA_EPON_OAM_ABI_VERSION,
		.llid_index = llid,
		.rx_enabled = rx_enabled,
		.tx_enabled = tx_enabled,
	};

	return ioctl(state->device, AIROHA_EPON_OAM_IOC_SET_FEC, &fec) ?
		-errno : 0;
}

static int ctc_transaction_get_holdover(void *context, bool *enabled,
		uint32_t *time_ms)
{
	struct daemon_state *state = context;
	struct airoha_epon_oam_holdover holdover = {
		.version = AIROHA_EPON_OAM_ABI_VERSION,
	};

	if (ioctl(state->device, AIROHA_EPON_OAM_IOC_GET_HOLDOVER, &holdover))
		return -errno;
	*enabled = holdover.enabled;
	*time_ms = holdover.time_ms;
	return 0;
}

static int ctc_transaction_set_holdover(void *context, bool enabled,
		uint32_t time_ms)
{
	struct daemon_state *state = context;
	struct airoha_epon_oam_holdover holdover = {
		.version = AIROHA_EPON_OAM_ABI_VERSION,
		.enabled = enabled,
		.time_ms = time_ms,
	};

	return ioctl(state->device, AIROHA_EPON_OAM_IOC_SET_HOLDOVER,
		&holdover) ? -errno : 0;
}

static int ctc_transaction_apply_management(void *context,
		unsigned int llid,
		struct airoha_ctc_management_set *management)
{
	struct daemon_state *state = context;

	return airoha_ctc_management_apply(state->ctc_management, llid,
		management);
}

static int handle_ctc_variable_get(struct daemon_state *state,
				   unsigned int index, uint16_t flags,
				   const uint8_t *payload, size_t length)
{
	struct airoha_ctc_variable_result variables = { 0 };
	struct airoha_ctc_fec_mode fec_mode = { 0 };
	struct airoha_ctc_holdover_config holdover_config = { 0 };
	struct airoha_ctc_optical_diagnostics diagnostics = { 0 };
	struct airoha_ctc_uni_links links = { 0 };
	struct airoha_epon_oam_fec *fec = &state->fec_request;
	struct airoha_epon_oam_holdover holdover = { 0 };
	struct llid_state *llid = &state->llids[index];
	uint8_t response[EPON_FRAME_MAX] = { 0 };
	int response_length;
	int result;

	secure_clear(fec, sizeof(*fec));
	fec->version = AIROHA_EPON_OAM_ABI_VERSION;
	fec->llid_index = index;
	if (!ioctl(state->device, AIROHA_EPON_OAM_IOC_GET_FEC, fec)) {
		fec_mode.available = true;
		fec_mode.enabled = fec->tx_enabled;
	}
	holdover.version = AIROHA_EPON_OAM_ABI_VERSION;
	if (!ioctl(state->device, AIROHA_EPON_OAM_IOC_GET_HOLDOVER,
		   &holdover)) {
		holdover_config.available = true;
		holdover_config.enabled = holdover.enabled;
		holdover_config.time_ms = holdover.time_ms;
	}
	read_optical_diagnostics(&diagnostics);
	airoha_ctc_read_uni_links("/sys/class/net", ctc_uni_netdevs, &links);
	response_length = airoha_ctc_build_variable_get_response(payload,
		length, &state->config.identity, &diagnostics, &fec_mode, &links,
		&holdover_config, response, sizeof(response), &variables);
	if (response_length < 0) {
		result = response_length;
		goto out;
	}
	llid->ctc_variable_get_frames++;
	llid->ctc_variable_attributes += variables.supported;
	result = send_ctc_organization(state, index, flags,
		AIROHA_CTC_OPCODE_VARIABLE_RESPONSE, response, response_length);
out:
	secure_clear(fec, sizeof(*fec));
	secure_clear(&holdover, sizeof(holdover));
	secure_clear(response, sizeof(response));
	return result;
}

static int handle_ctc_variable_set(struct daemon_state *state,
				   unsigned int index, uint16_t flags,
				   const uint8_t *payload, size_t length)
{
	struct airoha_ctc_variable_result variables = { 0 };
	struct airoha_ctc_fec_set fec_set = { 0 };
	struct airoha_ctc_holdover_set holdover_set = { 0 };
	struct airoha_ctc_management_set management = { 0 };
	const struct airoha_ctc_set_transaction_ops transaction_ops = {
		.get_fec = ctc_transaction_get_fec,
		.set_fec = ctc_transaction_set_fec,
		.get_holdover = ctc_transaction_get_holdover,
		.set_holdover = ctc_transaction_set_holdover,
		.apply_management = ctc_transaction_apply_management,
		.context = state,
	};
	struct airoha_ctc_set_transaction_request transaction = { 0 };
	struct airoha_ctc_set_transaction_result transaction_state = { 0 };
	struct llid_state *llid = &state->llids[index];
	uint8_t response[EPON_FRAME_MAX] = { 0 };
	int response_length;
	int transaction_result;
	int result;

	if (state->config.loid_length &&
	    llid->session.authentication !=
		AIROHA_EPON_AUTHENTICATION_SUCCEEDED) {
		result = -EACCES;
		goto out;
	}
	result = airoha_ctc_parse_fec_set(payload, length, &fec_set);
	if (result)
		goto out;
	result = airoha_ctc_parse_holdover_set(payload, length, &holdover_set);
	if (result)
		goto out;
	result = airoha_ctc_parse_management_set(payload, length, &management);
	if (result)
		goto out;
	transaction.llid = index;
	transaction.fec_present = fec_set.present && fec_set.valid;
	transaction.fec_enabled = fec_set.enabled;
	transaction.holdover_present =
		holdover_set.present && holdover_set.valid;
	transaction.holdover_enabled = holdover_set.enabled;
	transaction.holdover_time_ms = holdover_set.time_ms;
	transaction.management_present =
		airoha_ctc_management_has_valid_request(&management);
	transaction.management = &management;
	if (!airoha_ctc_management_set_empty(&management))
		llid->ctc_management_requests++;
	transaction_result = airoha_ctc_execute_set_transaction(
		&transaction_ops, &transaction, &transaction_state);
	if (transaction_result) {
		if (transaction.management_present)
			llid->ctc_management_errors++;
		fprintf(stderr,
			"airoha-epon-oamd: LLID %u CTC set transaction failed: %s\n",
			index, strerror(-transaction_result));
		if (airoha_ctc_set_transaction_fatal(&transaction_state)) {
			fprintf(stderr,
				"airoha-epon-oamd: LLID %u CTC set rollback failed: %s\n",
				index,
				strerror(-transaction_state.rollback_failure));
			stopping = 1;
		}
	} else {
		fec_set.applied = transaction_state.fec_applied;
		holdover_set.applied = transaction_state.holdover_applied;
		if (transaction_state.management_applied)
			llid->ctc_management_transactions++;
	}
	response_length = airoha_ctc_build_variable_set_response(payload,
		length, response, sizeof(response), &fec_set, &holdover_set,
		&management, &variables);
	if (response_length < 0) {
		result = response_length;
		goto out;
	}
	llid->ctc_variable_set_frames++;
	llid->ctc_variable_attributes += variables.supported;
	result = send_ctc_organization(state, index, flags,
		AIROHA_CTC_OPCODE_SET_RESPONSE, response, response_length);
out:
	secure_clear(&transaction_state, sizeof(transaction_state));
	secure_clear(&management, sizeof(management));
	secure_clear(response, sizeof(response));
	return result;
}

static int handle_ctc_authentication(struct daemon_state *state,
				     unsigned int index, uint16_t flags,
				     const uint8_t *payload, size_t length)
{
	struct llid_state *llid = &state->llids[index];
	uint8_t response[AIROHA_CTC_AUTH_CREDENTIAL_RESPONSE_LENGTH] = { 0 };
	uint16_t data_length;
	int response_length;
	int result = 0;

	if (length < 3) {
		result = -EINVAL;
		goto out_wipe;
	}
	data_length = (uint16_t)payload[1] << 8 | payload[2];
	if (data_length != length - 3) {
		result = -EINVAL;
		goto out_wipe;
	}
	llid->ctc_auth_frames++;
	switch (payload[0]) {
	case AIROHA_CTC_AUTH_REQUEST:
		if (data_length != 1) {
			result = -EINVAL;
			break;
		}
		response_length = airoha_ctc_build_auth_response(payload[3],
			state->config.loid, state->config.password, response,
			sizeof(response));
		if (response_length < 0) {
			result = response_length;
			break;
		}
		result = send_ctc_organization(state, index, flags,
			AIROHA_CTC_OPCODE_AUTHENTICATION, response,
			response_length);
		if (!result && payload[3] == AIROHA_CTC_AUTH_LOID_PASSWORD)
				llid->session.authentication =
					AIROHA_EPON_AUTHENTICATION_CREDENTIALS_SENT;
		break;
	case AIROHA_CTC_AUTH_SUCCESS:
		if (data_length) {
			result = -EINVAL;
			break;
		}
		llid->session.authentication =
			AIROHA_EPON_AUTHENTICATION_SUCCEEDED;
		llid->session.authentication_failure = 0;
		break;
	case AIROHA_CTC_AUTH_FAILURE:
		if (data_length != 1) {
			result = -EINVAL;
			break;
		}
		llid->session.authentication =
			AIROHA_EPON_AUTHENTICATION_FAILED;
		llid->session.authentication_failure = payload[3];
		break;
	default:
		result = -EOPNOTSUPP;
		break;
	}
out_wipe:
	secure_clear(response, sizeof(response));
	return result;
}

static int handle_ctc_churning(struct daemon_state *state, unsigned int index,
			       uint16_t flags, const uint8_t *payload,
			       size_t length)
{
	struct llid_state *llid = &state->llids[index];
	uint8_t response[AIROHA_CTC_CHURNING_RESPONSE_LENGTH] = { 0 };
	uint8_t requested_index, new_index;
	int response_length;
	int result;

	if (length != 2 || payload[0] != AIROHA_CTC_CHURNING_REQUEST ||
	    payload[1] > 1)
		return -EINVAL;
	if (state->config.loid_length &&
	    llid->session.authentication !=
		AIROHA_EPON_AUTHENTICATION_SUCCEEDED)
		return -EACCES;
	requested_index = payload[1];
	new_index = requested_index ^ 1;
	llid->ctc_key_requests++;
	if (!llid->session.ctc_key_valid ||
	    llid->session.ctc_request_index != requested_index ||
	    llid->session.ctc_key_index != new_index) {
		secure_clear(llid->session.ctc_key,
			     sizeof(llid->session.ctc_key));
		result = random_bytes(llid->session.ctc_key,
				      sizeof(llid->session.ctc_key));
		if (result)
			goto error;
		result = install_key(state, index,
			AIROHA_EPON_OAM_KEY_CTC_TRIPLE_CHURNING, new_index,
			llid->session.ctc_key, sizeof(llid->session.ctc_key), NULL);
		if (result)
			goto error;
		llid->session.ctc_key_index = new_index;
		llid->session.ctc_request_index = requested_index;
		llid->session.ctc_key_valid = true;
		llid->ctc_key_programs++;
	}
	response_length = airoha_ctc_build_churning_response(new_index,
		llid->session.ctc_key, response, sizeof(response));
	if (response_length < 0) {
		result = response_length;
		goto error;
	}
	result = send_ctc_organization(state, index, flags,
			AIROHA_CTC_OPCODE_CHURNING, response, response_length);
	secure_clear(response, sizeof(response));
	return result;

error:
	llid->ctc_key_errors++;
	llid->session.ctc_key_valid = false;
	secure_clear(llid->session.ctc_key, sizeof(llid->session.ctc_key));
	secure_clear(response, sizeof(response));
	return result;
}

static int handle_ctc_dba(struct daemon_state *state, unsigned int index,
			  uint16_t flags, const uint8_t *payload, size_t length)
{
	struct airoha_epon_oam_dba *dba = &state->dba_request;
	struct llid_state *llid = &state->llids[index];
	uint8_t response[AIROHA_CTC_DBA_RESPONSE_MAX] = { 0 };
	int response_length;
	int result;

	if (!length)
		return -EINVAL;
	if (state->config.loid_length &&
	    llid->session.authentication !=
		AIROHA_EPON_AUTHENTICATION_SUCCEEDED)
		return -EACCES;
	llid->ctc_dba_frames++;
	secure_clear(dba, sizeof(*dba));
	dba->version = AIROHA_EPON_OAM_ABI_VERSION;
	dba->llid_index = index;
	switch (payload[0]) {
	case AIROHA_CTC_DBA_GET_REQUEST:
		if (length != 1) {
			result = -EINVAL;
			goto out;
		}
		result = ioctl(state->device, AIROHA_EPON_OAM_IOC_GET_DBA, dba);
		if (result) {
			result = -errno;
			goto error;
		}
		response_length = airoha_ctc_build_dba_get_response(
			dba->queue_set_count, dba->report_bitmap, dba->threshold,
			response, sizeof(response));
		if (response_length < 0) {
			result = response_length;
			goto error;
		}
		result = send_ctc_organization(state, index, flags,
			AIROHA_CTC_OPCODE_DBA, response, response_length);
		break;
	case AIROHA_CTC_DBA_SET_REQUEST:
		result = airoha_ctc_parse_dba_set(payload, length,
			&dba->queue_set_count, dba->report_bitmap,
			dba->threshold);
		if (result)
			goto out;
		result = ioctl(state->device, AIROHA_EPON_OAM_IOC_SET_DBA, dba);
		if (result) {
			result = -errno;
			llid->ctc_dba_errors++;
		}
		response_length = airoha_ctc_build_dba_set_response(!result,
			response, sizeof(response));
		if (response_length < 0)
			goto error;
		if (!result)
			llid->ctc_dba_programs++;
		result = send_ctc_organization(state, index, flags,
			AIROHA_CTC_OPCODE_DBA, response, response_length);
		break;
	default:
		result = -EOPNOTSUPP;
		break;
	}
	goto out;

error:
	llid->ctc_dba_errors++;
out:
	secure_clear(dba, sizeof(*dba));
	secure_clear(response, sizeof(response));
	return result;
}

static int send_information(struct daemon_state *state, unsigned int index)
{
	struct llid_state *llid = &state->llids[index];
	uint8_t frame[128] = { 0 };
	uint16_t flags = OAM_FLAG_LOCAL_STABLE;
	size_t length;

	if (llid->session.remote_information_valid)
		flags |= OAM_FLAG_REMOTE_STABLE;
	length = build_header(state, frame, flags, OAM_CODE_INFORMATION);
	frame[length++] = OAM_TLV_LOCAL_INFORMATION;
	frame[length++] = OAM_INFORMATION_LENGTH;
	frame[length++] = 1;
	frame[length++] = 0;
	frame[length++] = 1;
	frame[length++] = llid->session.loopback_enabled ?
		AIROHA_EPON_OAM_LOOPBACK_STATE : 0;
	frame[length++] = AIROHA_EPON_OAM_CONFIGURATION;
	frame[length++] = 1518 >> 8;
	frame[length++] = 1518 & 0xff;
	memcpy(frame + length, state->config.local_oui, 3);
	length += 3;
	memcpy(frame + length, "XG20", 4);
	length += 4;
	if (state->config.ctc_enabled) {
		frame[length++] = OAM_TLV_ORGANIZATION;
		frame[length++] = 7;
		memcpy(frame + length, state->config.ctc_oui, 3);
		length += 3;
		frame[length++] = 1;
		frame[length++] = 0x21;
	}
	if (llid->session.remote_information_valid) {
		memcpy(frame + length, llid->session.remote_information,
		       OAM_INFORMATION_LENGTH);
		frame[length] = OAM_TLV_REMOTE_INFORMATION;
		length += OAM_INFORMATION_LENGTH;
	}
	frame[length++] = OAM_TLV_END;
	if (transmit_frame(state, index, frame, length))
		return -1;
	llid->session.last_information_ms = monotonic_ms();
	if (llid->session.discovery == AIROHA_EPON_DISCOVERY_WAITING)
		llid->session.discovery = AIROHA_EPON_DISCOVERY_LOCAL_SENT;
	return 0;
}

static void dispatch_organization(struct daemon_state *state,
				  unsigned int index, const uint8_t *oui)
{
	struct llid_state *llid = &state->llids[index];

	if (!memcmp(oui, state->config.ctc_oui, 3) && state->config.ctc_enabled)
		llid->ctc_frames++;
	else if (!memcmp(oui, dpoe_oui, 3) && state->config.dpoe_enabled)
		llid->dpoe_frames++;
	else
		llid->unsupported_oui_frames++;
}

static int handle_dpoe_organization(struct daemon_state *state,
				    unsigned int index, const uint8_t *frame,
				    size_t length)
{
	struct airoha_dpoe_sia_get_result get = { 0 };
	struct airoha_dpoe_sia_set_result sia = { 0 };
	struct airoha_dpoe_fec_set fec_set = { 0 };
	struct airoha_dpoe_optical_diagnostics diagnostics = { 0 };
	struct airoha_epon_oam_fec *fec = &state->fec_request;
	struct llid_state *llid = &state->llids[index];
	uint8_t response[EPON_FRAME_MAX] = { 0 };
	const uint8_t *payload = frame + EPON_HEADER_LEN;
	size_t payload_length = length - EPON_HEADER_LEN;
	uint16_t flags = (uint16_t)frame[15] << 8 | frame[16];
	int response_length;

	if (payload_length < 4)
		return -EINVAL;
	if (payload[3] == AIROHA_DPOE_SIA_GET_REQUEST) {
		read_dpoe_optical_diagnostics(&diagnostics);
		response_length = airoha_dpoe_build_sia_get_response(payload,
			payload_length, response + EPON_HEADER_LEN,
			sizeof(response) - EPON_HEADER_LEN,
			&state->config.dpoe_inventory, &diagnostics, &get);
		if (response_length < 0)
			goto out;
		llid->dpoe_sia_get_requests++;
		llid->dpoe_sia_attributes += get.supported;
		llid->dpoe_sia_read_errors += get.read_errors;
		build_header(state, response, flags, OAM_CODE_ORGANIZATION);
		response_length = transmit_frame(state, index, response,
			EPON_HEADER_LEN + (size_t)response_length);
		goto out;
	}
	if (payload[3] != AIROHA_DPOE_SIA_SET_REQUEST)
		return 0;
	response_length = airoha_dpoe_parse_extended_fec(payload,
		payload_length, &fec_set);
	if (response_length < 0)
		goto out;
	secure_clear(fec, sizeof(*fec));
	if (fec_set.present && fec_set.valid) {
		fec->version = AIROHA_EPON_OAM_ABI_VERSION;
		fec->llid_index = index;
		fec->rx_enabled = fec_set.rx_enabled;
		fec->tx_enabled = fec_set.tx_enabled;
		fec_set.applied = !ioctl(state->device,
			AIROHA_EPON_OAM_IOC_SET_FEC, fec);
	}
	response_length = airoha_dpoe_build_sia_set_response(payload,
		payload_length, response + EPON_HEADER_LEN,
		sizeof(response) - EPON_HEADER_LEN, &fec_set, &sia);
	if (response_length < 0)
		goto out;

	llid->dpoe_sia_set_requests++;
	llid->dpoe_sia_attributes += sia.supported;
	/* Stock arms a one-shot 10000 ms timer and ignores the supplied value. */
	if (sia.arm_rekey)
		(void)airoha_dpoe_rekey_schedule(&llid->session.dpoe_rekey,
			monotonic_ms());
	build_header(state, response, flags, OAM_CODE_ORGANIZATION);
	response_length = transmit_frame(state, index, response,
		EPON_HEADER_LEN + (size_t)response_length);
out:
	secure_clear(fec, sizeof(*fec));
	secure_clear(&diagnostics, sizeof(diagnostics));
	secure_clear(response, sizeof(response));
	return response_length;
}

static int handle_organization(struct daemon_state *state, unsigned int index,
			       const uint8_t *frame, size_t length)
{
	const uint8_t *oui;
	const uint8_t *payload;
	size_t payload_length;
	uint16_t flags;
	uint8_t opcode;

	if (length < EPON_HEADER_LEN + 4)
		return -EINVAL;
	oui = frame + EPON_HEADER_LEN;
	dispatch_organization(state, index, oui);
	if (!memcmp(oui, dpoe_oui, 3) && state->config.dpoe_enabled) {
		if (state->llids[index].session.discovery !=
		    AIROHA_EPON_DISCOVERY_ESTABLISHED)
			return -EAGAIN;
		return handle_dpoe_organization(state, index, frame, length);
	}
	if (memcmp(oui, state->config.ctc_oui, 3) || !state->config.ctc_enabled)
		return 0;
	if (state->llids[index].session.discovery !=
	    AIROHA_EPON_DISCOVERY_ESTABLISHED)
		return -EAGAIN;
	opcode = frame[EPON_HEADER_LEN + 3];
	payload = frame + EPON_HEADER_LEN + 4;
	payload_length = length - EPON_HEADER_LEN - 4;
	flags = (uint16_t)frame[15] << 8 | frame[16];
	switch (opcode) {
	case AIROHA_CTC_OPCODE_VARIABLE_REQUEST:
		return handle_ctc_variable_get(state, index, flags, payload,
					       payload_length);
	case AIROHA_CTC_OPCODE_SET_REQUEST:
		return handle_ctc_variable_set(state, index, flags, payload,
					       payload_length);
	case AIROHA_CTC_OPCODE_AUTHENTICATION:
		return handle_ctc_authentication(state, index, flags, payload,
					 length - EPON_HEADER_LEN - 4);
	case AIROHA_CTC_OPCODE_CHURNING:
		return handle_ctc_churning(state, index, flags, payload,
					    payload_length);
	case AIROHA_CTC_OPCODE_DBA:
		return handle_ctc_dba(state, index, flags, payload,
				      payload_length);
	default:
		state->llids[index].unsupported_ctc_opcodes++;
		return 0;
	}
}

static int handle_information(struct daemon_state *state, unsigned int index,
			      const uint8_t *frame, size_t length)
{
	struct llid_state *llid = &state->llids[index];
	uint8_t remote_information[OAM_INFORMATION_LENGTH];
	size_t offset = EPON_HEADER_LEN;
	bool local_found = false;

	while (offset < length) {
		uint8_t type = frame[offset];
		uint8_t tlv_length;

		if (type == OAM_TLV_END)
			break;
		if (offset + 2 > length)
			return -EINVAL;
		tlv_length = frame[offset + 1];
		if (tlv_length < 2 || offset + tlv_length > length)
			return -EINVAL;
		if (type == OAM_TLV_LOCAL_INFORMATION &&
		    tlv_length == OAM_INFORMATION_LENGTH) {
			memcpy(remote_information, frame + offset,
			       OAM_INFORMATION_LENGTH);
			local_found = true;
		} else if (type == OAM_TLV_ORGANIZATION && tlv_length >= 5) {
			dispatch_organization(state, index, frame + offset + 2);
		}
		offset += tlv_length;
	}
	if (!local_found)
		return -EINVAL;
	airoha_epon_session_establish(&llid->session,
		remote_information, monotonic_ms());
	secure_clear(remote_information, sizeof(remote_information));
	return send_information(state, index);
}

static int handle_variable_request(struct daemon_state *state,
				   unsigned int index, const uint8_t *frame,
				   size_t length)
{
	struct airoha_ieee_variable_result variables = { 0 };
	struct airoha_ieee_variable_state variable_state = { 0 };
	struct airoha_epon_oam_fec *fec = &state->fec_request;
	uint8_t response[EPON_FRAME_MAX];
	size_t output;
	uint16_t flags = ((uint16_t)frame[15] << 8) | frame[16];
	int response_length;
	int result;

	output = build_header(state, response, flags, OAM_CODE_VARIABLE_RESPONSE);
	secure_clear(fec, sizeof(*fec));
	fec->version = AIROHA_EPON_OAM_ABI_VERSION;
	fec->llid_index = index;
	if (!ioctl(state->device, AIROHA_EPON_OAM_IOC_GET_FEC, fec)) {
		variable_state.fec_available = true;
		variable_state.fec_enabled = fec->tx_enabled;
	}
	response_length = airoha_ieee_build_variable_response(
		frame + EPON_HEADER_LEN, length - EPON_HEADER_LEN,
		&variable_state, response + output, sizeof(response) - output,
		&variables);
	if (response_length < 0) {
		result = response_length;
		goto out;
	}
	output += (size_t)response_length;
	state->llids[index].variable_requests++;
	result = transmit_frame(state, index, response, output);
out:
	secure_clear(fec, sizeof(*fec));
	secure_clear(response, sizeof(response));
	return result;
}

static int handle_loopback(struct daemon_state *state, unsigned int index,
			   const uint8_t *frame, size_t length)
{
	struct airoha_epon_oam_loopback *request = &state->loopback_request;
	struct llid_state *llid = &state->llids[index];
	bool enabled;
	int result;

	if (llid->session.discovery != AIROHA_EPON_DISCOVERY_ESTABLISHED)
		return -EAGAIN;
	if (length <= EPON_HEADER_LEN)
		return -EINVAL;
	result = airoha_epon_parse_loopback_control(frame + EPON_HEADER_LEN,
		length - EPON_HEADER_LEN, &enabled);
	llid->loopback_commands++;
	if (result) {
		llid->loopback_errors++;
		return result;
	}

	memset(request, 0, sizeof(*request));
	request->version = AIROHA_EPON_OAM_ABI_VERSION;
	request->llid_index = index;
	request->enabled = enabled;
	if (ioctl(state->device, AIROHA_EPON_OAM_IOC_SET_LOOPBACK, request)) {
		result = -errno;
		llid->loopback_errors++;
		memset(request, 0, sizeof(*request));
		return result;
	}
	llid->session.loopback_enabled = enabled;
	memset(request, 0, sizeof(*request));

	/* Publish the parser/MUX state transition immediately to the OLT. */
	return send_information(state, index);
}

static int handle_frame(struct daemon_state *state, const uint8_t *wire,
			 size_t wire_length)
{
	const uint8_t *frame;
	uint16_t index;
	size_t length;
	uint8_t code;
	bool newly_bound;
	int result;

	if (wire_length < 2 + EPON_HEADER_LEN)
		return -EINVAL;
	memcpy(&index, wire, sizeof(index));
	index = ntohs(index);
	frame = wire + 2;
	length = wire_length - 2;
	if (index >= EPON_LLID_COUNT ||
	    !(state->config.llid_mask & (UINT32_C(1) << index)) ||
	    memcmp(frame, slow_protocols_mac, 6) || !valid_unicast_mac(frame + 6) ||
	    frame[12] != (EPON_ETHERTYPE >> 8) ||
	    frame[13] != (EPON_ETHERTYPE & 0xff) || frame[14] != EPON_SUBTYPE)
		return -EINVAL;
	code = frame[17];
	newly_bound = !state->llids[index].session.olt_mac_valid;
	if (newly_bound && code != OAM_CODE_INFORMATION)
		return -EAGAIN;
	if (!airoha_epon_session_bind_olt(&state->llids[index].session,
					    frame + 6))
		return -EPERM;
	state->rx_frames++;
	state->llids[index].rx_frames++;
	switch (code) {
	case OAM_CODE_INFORMATION:
		result = handle_information(state, index, frame, length);
		if (newly_bound && result == -EINVAL) {
			secure_clear(state->llids[index].session.olt_mac,
				     sizeof(state->llids[index].session.olt_mac));
			state->llids[index].session.olt_mac_valid = false;
		}
		return result;
	case OAM_CODE_EVENT:
		if (length < EPON_HEADER_LEN + 2)
			return -EINVAL;
		state->llids[index].event_notifications++;
		return 0;
	case OAM_CODE_VARIABLE_REQUEST:
		return handle_variable_request(state, index, frame, length);
	case OAM_CODE_VARIABLE_RESPONSE:
		return 0;
	case OAM_CODE_LOOPBACK:
		return handle_loopback(state, index, frame, length);
	case OAM_CODE_ORGANIZATION:
		return handle_organization(state, index, frame, length);
	default:
		return -EOPNOTSUPP;
	}
}

static void expire_lost_sessions(struct daemon_state *state)
{
	uint64_t now = monotonic_ms();
	unsigned int index;

	for (index = 0; index < EPON_LLID_COUNT; index++) {
		struct airoha_epon_oam_session *request =
			&state->session_request;
		struct llid_state *llid = &state->llids[index];
		int result;

		if (!(state->config.llid_mask & (UINT32_C(1) << index)) ||
		    !airoha_epon_session_expired(&llid->session, now,
			state->config.lost_link_timeout_ms))
			continue;
		result = airoha_ctc_management_clear_llid(
			state->ctc_management, index);
		if (result) {
			llid->link_lost_errors++;
			llid->ctc_management_errors++;
			llid->session.clear_retry_at_ms =
				now + AIROHA_EPON_SESSION_CLEAR_RETRY_MS;
			fprintf(stderr,
				"airoha-epon-oamd: LLID %u CTC policy clear failed: %s\n",
				index, strerror(-result));
			continue;
		}
		memset(request, 0, sizeof(*request));
		request->version = AIROHA_EPON_OAM_ABI_VERSION;
		request->llid_index = index;
		result = ioctl(state->device, AIROHA_EPON_OAM_IOC_CLEAR_SESSION,
			       request) ? -errno : 0;
		if (!airoha_epon_session_commit_clear(&llid->session, result)) {
			llid->link_lost_errors++;
			llid->session.clear_retry_at_ms =
				now + AIROHA_EPON_SESSION_CLEAR_RETRY_MS;
			fprintf(stderr,
				"airoha-epon-oamd: LLID %u session clear failed: %s\n",
				index, strerror(-result));
			continue;
		}
		llid->link_lost_events++;
	}
	memset(&state->session_request, 0, sizeof(state->session_request));
}

static void send_due_information(struct daemon_state *state)
{
	uint64_t now = monotonic_ms();
	unsigned int index;

	for (index = 0; index < EPON_LLID_COUNT; index++) {
		struct llid_state *llid = &state->llids[index];

		if (!(state->config.llid_mask & (UINT32_C(1) << index)))
			continue;
		if (now - llid->session.last_information_ms <
		    state->config.information_interval_ms)
			continue;
		(void)send_information(state, index);
	}
}

static int mark_ready(struct daemon_state *state)
{
	int descriptor;
	int result;

	result = ensure_parent_directory(state->ready_path);
	if (result)
		return result;
	descriptor = open(state->ready_path,
			  O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0644);
	if (descriptor < 0)
		return -1;
	if (dprintf(descriptor,
		    "pid=%ld control_plane=epon-oam pon_mode=%s omci=0\n",
		    (long)getpid(), state->config.pon_mode) < 0) {
		close(descriptor);
		return -1;
	}
	return close(descriptor);
}

static void clear_loopbacks(struct daemon_state *state)
{
	unsigned int index;

	for (index = 0; index < EPON_LLID_COUNT; index++) {
		struct airoha_epon_oam_loopback *request =
			&state->loopback_request;

		if (!state->llids[index].session.loopback_enabled)
			continue;
		memset(request, 0, sizeof(*request));
		request->version = AIROHA_EPON_OAM_ABI_VERSION;
		request->llid_index = index;
		(void)ioctl(state->device, AIROHA_EPON_OAM_IOC_SET_LOOPBACK,
			request);
		state->llids[index].session.loopback_enabled = false;
	}
	memset(&state->loopback_request, 0, sizeof(state->loopback_request));
}

static int run_daemon(struct daemon_state *state)
{
	uint8_t wire[EPON_WIRE_MAX];
	struct pollfd poll_descriptor;
	uint64_t last_status = 0;
	int platform_result;

	platform_result = recover_ctc_platform(state);
	if (platform_result) {
		fprintf(stderr,
			"airoha-epon-oamd: cannot recover CTC platform state: %s\n",
			strerror(-platform_result));
		return 1;
	}
	state->device = open(state->device_path, O_RDWR | O_NONBLOCK | O_CLOEXEC);
	if (state->device < 0) {
		fprintf(stderr, "airoha-epon-oamd: cannot open %s: %s\n",
			state->device_path, strerror(errno));
		return 1;
	}
	state->started_ms = monotonic_ms();
	state->rate_window_ms = state->started_ms;
	if (mark_ready(state)) {
		fprintf(stderr, "airoha-epon-oamd: cannot publish ready state\n");
		close(state->device);
		return 1;
	}
	(void)write_status(state);
	poll_descriptor.fd = state->device;
	poll_descriptor.events = POLLIN | POLLPRI;

	while (!stopping) {
		int result = poll(&poll_descriptor, 1, 100);

		if (result < 0 && errno != EINTR) {
			fprintf(stderr, "airoha-epon-oamd: poll failed: %s\n",
				strerror(errno));
			break;
		}
		if (result > 0 && (poll_descriptor.revents & POLLIN)) {
			for (;;) {
				ssize_t received = read(state->device, wire, sizeof(wire));

				if (received < 0 && (errno == EAGAIN || errno == EWOULDBLOCK))
					break;
				if (received < 0) {
					if (errno != EINTR)
						fprintf(stderr,
							"airoha-epon-oamd: read failed: %s\n",
							strerror(errno));
					break;
				}
				if (!received)
					break;
				if (handle_frame(state, wire, received)) {
					state->malformed_frames++;
					if (received >= 2) {
						uint16_t index;
						memcpy(&index, wire, sizeof(index));
						index = ntohs(index);
						if (index < EPON_LLID_COUNT)
							state->llids[index].malformed_frames++;
					}
				}
			}
		}
		if (result > 0 && (poll_descriptor.revents & POLLPRI) &&
		    fetch_dpoe_key_events(state))
			fprintf(stderr,
				"airoha-epon-oamd: cannot fetch key events: %s\n",
				strerror(errno));
		if (poll_descriptor.revents & (POLLERR | POLLHUP | POLLNVAL))
			break;
		expire_lost_sessions(state);
		service_dpoe_key_exchanges(state);
		send_due_information(state);
		if (monotonic_ms() - last_status >= 1000) {
			(void)write_status(state);
			last_status = monotonic_ms();
		}
	}
	platform_result = airoha_ctc_management_clear_all(
		state->ctc_management);
	if (platform_result)
		fprintf(stderr,
			"airoha-epon-oamd: cannot restore CTC platform state: %s\n",
			strerror(-platform_result));
	clear_loopbacks(state);
	unlink(state->ready_path);
	close(state->device);
	state->device = -1;
	return stopping && !platform_result ? 0 : 1;
}

static void clear_sensitive_state(struct daemon_state *state)
{
	unsigned int index;

	secure_clear(state->config.loid, sizeof(state->config.loid));
	secure_clear(state->config.password, sizeof(state->config.password));
	state->config.loid_length = 0;
	state->config.password_length = 0;
	for (index = 0; index < EPON_LLID_COUNT; index++)
		(void)airoha_epon_session_commit_clear(
			&state->llids[index].session, 0);
	secure_clear(&state->key_request, sizeof(state->key_request));
	secure_clear(&state->fec_request, sizeof(state->fec_request));
	secure_clear(&state->loopback_request, sizeof(state->loopback_request));
	secure_clear(&state->session_request, sizeof(state->session_request));
	secure_clear(&state->key_events_request, sizeof(state->key_events_request));
}

static void usage(const char *program)
{
	fprintf(stderr,
		"usage: %s [--device PATH] [--status PATH] [--ready PATH] [--ctc-platform PATH] [--pon-mode MODE]\n",
		program);
}

int main(int argc, char **argv)
{
	struct daemon_state state = {
		.device_path = "/dev/airoha-epon-oam",
		.status_path = "/var/run/airoha-epon-oamd/status.json",
		.ready_path = "/var/run/airoha-epon-oamd/ready",
		.ctc_platform_path = "/usr/libexec/airoha-epon-ctc-platform",
		.device = -1,
	};
	struct sigaction action = { .sa_handler = signal_handler };
	struct sigaction ignore = { .sa_handler = SIG_IGN };
	const char *pon_mode = NULL;
	int result;
	int argument;

	for (argument = 1; argument < argc; argument++) {
		if (!strcmp(argv[argument], "--device") && argument + 1 < argc)
			state.device_path = argv[++argument];
		else if (!strcmp(argv[argument], "--status") && argument + 1 < argc)
			state.status_path = argv[++argument];
		else if (!strcmp(argv[argument], "--ready") && argument + 1 < argc)
			state.ready_path = argv[++argument];
		else if (!strcmp(argv[argument], "--ctc-platform") &&
			 argument + 1 < argc)
			state.ctc_platform_path = argv[++argument];
		else if (!strcmp(argv[argument], "--pon-mode") &&
			 argument + 1 < argc)
			pon_mode = argv[++argument];
		else {
			usage(argv[0]);
			return 2;
		}
	}
	if (load_configuration(&state.config, pon_mode)) {
		fprintf(stderr,
			"airoha-epon-oamd: valid enabled EPON mode, ONU MAC and OAM configuration are required\n");
		return 1;
	}
	state.ctc_management = calloc(1, sizeof(*state.ctc_management));
	if (!state.ctc_management) {
		clear_sensitive_state(&state);
		return 1;
	}
	state.ctc_management->apply = apply_ctc_platform;
	state.ctc_management->apply_context = &state;
	if (mlock(&state, sizeof(state))) {
		fprintf(stderr,
			"airoha-epon-oamd: cannot lock credential and key memory: %s\n",
				strerror(errno));
		clear_sensitive_state(&state);
		free(state.ctc_management);
		return 1;
	}
	state.memory_locked = true;
	unlink(state.ready_path);
	sigemptyset(&action.sa_mask);
	sigemptyset(&ignore.sa_mask);
	action.sa_flags = 0;
	ignore.sa_flags = 0;
	if (sigaction(SIGTERM, &action, NULL) || sigaction(SIGINT, &action, NULL) ||
	    sigaction(SIGPIPE, &ignore, NULL)) {
		clear_sensitive_state(&state);
		(void)munlock(&state, sizeof(state));
		free(state.ctc_management);
		return 1;
	}
	result = run_daemon(&state);
	clear_sensitive_state(&state);
	(void)munlock(&state, sizeof(state));
	free(state.ctc_management);
	return result;
}
