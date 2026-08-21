/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef _AIROHA_EPON_SESSION_H_
#define _AIROHA_EPON_SESSION_H_

#include <stdbool.h>
#include <stdint.h>
#include <string.h>

#include "airoha-epon-ctc.h"
#include "airoha-epon-dpoe.h"

#define AIROHA_EPON_INFORMATION_LENGTH	16
#define AIROHA_EPON_LOST_LINK_TIMEOUT_DEFAULT_MS 30000U
#define AIROHA_EPON_LOST_LINK_TIMEOUT_MIN_MS 1000U
#define AIROHA_EPON_LOST_LINK_TIMEOUT_MAX_MS 300000U
#define AIROHA_EPON_SESSION_CLEAR_RETRY_MS 1000U

enum airoha_epon_discovery_state {
	AIROHA_EPON_DISCOVERY_WAITING,
	AIROHA_EPON_DISCOVERY_LOCAL_SENT,
	AIROHA_EPON_DISCOVERY_ESTABLISHED,
};

enum airoha_epon_authentication_state {
	AIROHA_EPON_AUTHENTICATION_IDLE,
	AIROHA_EPON_AUTHENTICATION_CREDENTIALS_SENT,
	AIROHA_EPON_AUTHENTICATION_SUCCEEDED,
	AIROHA_EPON_AUTHENTICATION_FAILED,
};

struct airoha_epon_session_state {
	enum airoha_epon_discovery_state discovery;
	uint8_t remote_information[AIROHA_EPON_INFORMATION_LENGTH];
	bool remote_information_valid;
	uint64_t last_information_ms;
	uint64_t last_remote_information_ms;
	uint64_t clear_retry_at_ms;
	bool loopback_enabled;
	uint8_t olt_mac[6];
	bool olt_mac_valid;
	bool dpoe_key_event_pending;
	uint8_t dpoe_active_key_index;
	uint8_t dpoe_event_directions;
	uint64_t dpoe_retry_at_ms;
	struct airoha_dpoe_rekey_timer dpoe_rekey;
	struct airoha_dpoe_key_exchange dpoe_exchange;
	enum airoha_epon_authentication_state authentication;
	uint8_t authentication_failure;
	uint8_t ctc_key[AIROHA_CTC_CHURNING_KEY_LENGTH];
	uint8_t ctc_key_index;
	uint8_t ctc_request_index;
	bool ctc_key_valid;
};

static inline void airoha_epon_session_secure_clear(void *memory, size_t length)
{
	volatile uint8_t *byte = memory;

	while (length--)
		*byte++ = 0;
}

static inline bool airoha_epon_session_expired(
		const struct airoha_epon_session_state *session, uint64_t now_ms,
		uint32_t timeout_ms)
{
	return session &&
	       session->discovery == AIROHA_EPON_DISCOVERY_ESTABLISHED &&
	       session->last_remote_information_ms &&
	       now_ms >= session->last_remote_information_ms &&
	       now_ms - session->last_remote_information_ms >= timeout_ms &&
	       now_ms >= session->clear_retry_at_ms;
}

static inline void airoha_epon_session_establish(
		struct airoha_epon_session_state *session,
		const uint8_t information[AIROHA_EPON_INFORMATION_LENGTH],
		uint64_t now_ms)
{
	memcpy(session->remote_information, information,
	       sizeof(session->remote_information));
	session->remote_information_valid = true;
	session->last_remote_information_ms = now_ms;
	session->clear_retry_at_ms = 0;
	session->discovery = AIROHA_EPON_DISCOVERY_ESTABLISHED;
}

static inline bool airoha_epon_session_bind_olt(
		struct airoha_epon_session_state *session, const uint8_t olt_mac[6])
{
	if (session->olt_mac_valid)
		return !memcmp(session->olt_mac, olt_mac, sizeof(session->olt_mac));
	memcpy(session->olt_mac, olt_mac, sizeof(session->olt_mac));
	session->olt_mac_valid = true;
	return true;
}

static inline bool airoha_epon_session_commit_clear(
		struct airoha_epon_session_state *session, int hardware_result)
{
	if (!session || hardware_result)
		return false;

	airoha_epon_session_secure_clear(session->remote_information,
					 sizeof(session->remote_information));
	session->remote_information_valid = false;
	session->last_information_ms = 0;
	session->last_remote_information_ms = 0;
	session->clear_retry_at_ms = 0;
	session->loopback_enabled = false;
	airoha_epon_session_secure_clear(session->olt_mac,
					 sizeof(session->olt_mac));
	session->olt_mac_valid = false;
	session->dpoe_key_event_pending = false;
	session->dpoe_active_key_index = 0;
	session->dpoe_event_directions = 0;
	session->dpoe_retry_at_ms = 0;
	airoha_dpoe_rekey_cancel(&session->dpoe_rekey);
	airoha_dpoe_exchange_clear(&session->dpoe_exchange);
	session->authentication = AIROHA_EPON_AUTHENTICATION_IDLE;
	session->authentication_failure = 0;
	airoha_epon_session_secure_clear(session->ctc_key,
					 sizeof(session->ctc_key));
	session->ctc_key_index = 0;
	session->ctc_request_index = 0;
	session->ctc_key_valid = false;
	session->discovery = AIROHA_EPON_DISCOVERY_WAITING;
	return true;
}

#endif
