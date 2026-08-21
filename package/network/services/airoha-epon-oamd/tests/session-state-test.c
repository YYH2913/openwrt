/* SPDX-License-Identifier: GPL-2.0-only */
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "airoha-epon-session.h"

#define CHECK(condition) do { \
	if (!(condition)) { \
		fprintf(stderr, "check failed at line %d: %s\n", __LINE__, \
			#condition); \
		return 1; \
	} \
} while (0)

static bool all_zero(const void *memory, size_t length)
{
	const uint8_t *byte = memory;

	while (length--)
		if (*byte++)
			return false;
	return true;
}

int main(void)
{
	const uint8_t information[AIROHA_EPON_INFORMATION_LENGTH] = {
		1, 16, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14,
	};
	const uint8_t old_olt[6] = { 0x02, 0, 0, 0, 0, 1 };
	const uint8_t new_olt[6] = { 0x02, 0, 0, 0, 0, 2 };
	struct airoha_epon_session_state before;
	struct airoha_epon_session_state session = { 0 };

	session.last_remote_information_ms = 1000;
	CHECK(!airoha_epon_session_expired(&session, 31000, 30000));

	airoha_epon_session_establish(&session, information, 1000);
	CHECK(session.discovery == AIROHA_EPON_DISCOVERY_ESTABLISHED);
	CHECK(!airoha_epon_session_expired(&session, 30999, 30000));
	CHECK(airoha_epon_session_expired(&session, 31000, 30000));
	session.clear_retry_at_ms = 32000;
	CHECK(!airoha_epon_session_expired(&session, 31000, 30000));
	session.clear_retry_at_ms = 0;

	CHECK(airoha_epon_session_bind_olt(&session, old_olt));
	CHECK(airoha_epon_session_bind_olt(&session, old_olt));
	CHECK(!airoha_epon_session_bind_olt(&session, new_olt));
	session.last_information_ms = 900;
	session.loopback_enabled = true;
	session.dpoe_key_event_pending = true;
	session.dpoe_active_key_index = 1;
	session.dpoe_event_directions = 3;
	session.dpoe_retry_at_ms = 1234;
	session.dpoe_rekey.armed = true;
	session.dpoe_rekey.deadline_ms = 4321;
	memset(&session.dpoe_exchange, 0xa5, sizeof(session.dpoe_exchange));
	session.authentication = AIROHA_EPON_AUTHENTICATION_FAILED;
	session.authentication_failure = 7;
	memset(session.ctc_key, 0x5a, sizeof(session.ctc_key));
	session.ctc_key_index = 1;
	session.ctc_request_index = 1;
	session.ctc_key_valid = true;

	before = session;
	CHECK(!airoha_epon_session_commit_clear(&session, -5));
	CHECK(!memcmp(&session, &before, sizeof(session)));

	CHECK(airoha_epon_session_commit_clear(&session, 0));
	CHECK(session.discovery == AIROHA_EPON_DISCOVERY_WAITING);
	CHECK(!session.remote_information_valid);
	CHECK(all_zero(session.remote_information,
		       sizeof(session.remote_information)));
	CHECK(!session.last_information_ms);
	CHECK(!session.last_remote_information_ms);
	CHECK(!session.clear_retry_at_ms);
	CHECK(!session.loopback_enabled);
	CHECK(!session.olt_mac_valid);
	CHECK(all_zero(session.olt_mac, sizeof(session.olt_mac)));
	CHECK(!session.dpoe_key_event_pending);
	CHECK(!session.dpoe_active_key_index);
	CHECK(!session.dpoe_event_directions);
	CHECK(!session.dpoe_retry_at_ms);
	CHECK(!session.dpoe_rekey.armed);
	CHECK(all_zero(&session.dpoe_exchange, sizeof(session.dpoe_exchange)));
	CHECK(session.authentication == AIROHA_EPON_AUTHENTICATION_IDLE);
	CHECK(!session.authentication_failure);
	CHECK(!session.ctc_key_valid);
	CHECK(all_zero(session.ctc_key, sizeof(session.ctc_key)));
	CHECK(!session.ctc_key_index);
	CHECK(!session.ctc_request_index);

	CHECK(airoha_epon_session_bind_olt(&session, new_olt));
	CHECK(session.olt_mac_valid);
	CHECK(!memcmp(session.olt_mac, new_olt, sizeof(new_olt)));

	puts("EPON remote-session timeout and reset vectors passed");
	return 0;
}
