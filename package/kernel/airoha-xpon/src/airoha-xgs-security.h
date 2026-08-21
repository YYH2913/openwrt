/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef _AIROHA_XGS_SECURITY_H_
#define _AIROHA_XGS_SECURITY_H_

#include <linux/types.h>

#include "airoha-xgs-reboot.h"
#include "airoha-xpon-mode.h"

#define AIROHA_XGS_KEY_SIZE		16
#define AIROHA_XGS_REGISTRATION_ID_SIZE	36
#define AIROHA_XGS_SERIAL_SIZE		8
#define AIROHA_XGS_PON_TAG_SIZE		8
#define AIROHA_XGS_PROFILE_PATTERN_SIZE	8
#define AIROHA_XGS_PLOAM_CONTENT_SIZE	40
#define AIROHA_XGS_PLOAM_MIC_SIZE	8
#define AIROHA_XGS_PLOAM_FRAME_SIZE	(AIROHA_XGS_PLOAM_CONTENT_SIZE + \
					 AIROHA_XGS_PLOAM_MIC_SIZE)
#define AIROHA_XGS_PLOAM_FRAME_WORDS	(AIROHA_XGS_PLOAM_FRAME_SIZE / \
					 sizeof(u32))
#define AIROHA_XGS_PLOAM_FIFO_WORDS	13
#define AIROHA_XGS_PLOAM_UPSTREAM_FIFO_WORDS	11
#define AIROHA_XGS_PLOAM_KEY_INDEX_0	0
#define AIROHA_XGS_PLOAM_KEY_INDEX_1	1
#define AIROHA_XGS_ACK_COMPLETION_OK	0
#define AIROHA_XGS_ACK_COMPLETION_PROCESS_ERROR	5
#define AIROHA_XGS_KEY_CONTROL_MESSAGE_ID	0x0d
#define AIROHA_XGS_KEY_REPORT_MESSAGE_ID	0x05
#define AIROHA_XGS_DISABLE_SERIAL_NUMBER_MESSAGE_ID	0x06
#define AIROHA_XGS_SLEEP_ALLOW_MESSAGE_ID	0x12
#define AIROHA_XGS_KEY_REPORT_FRAGMENT_SIZE	32
#define AIROHA_XGS_KEY_REPORT_GENERATE	0
#define AIROHA_XGS_KEY_REPORT_CONFIRM	1
#define AIROHA_XGS_OMCI_MIC_SIZE		4
#define AIROHA_XGS_OMCI_MAX_CONTENT_SIZE	1980
#define AIROHA_XGS_OMCI_BASELINE_CONTENT_SIZE	44
#define AIROHA_XGS_OMCI_BASELINE_WIRE_SIZE	\
	(AIROHA_XGS_OMCI_BASELINE_CONTENT_SIZE + AIROHA_XGS_OMCI_MIC_SIZE)
#define AIROHA_XGS_OMCI_MAX_WIRE_SIZE	1980
#define AIROHA_XGS_OMCI_MAX_WIRE_CONTENT_SIZE	\
	(AIROHA_XGS_OMCI_MAX_WIRE_SIZE - AIROHA_XGS_OMCI_MIC_SIZE)

enum airoha_xgs_direction {
	AIROHA_XGS_DOWNSTREAM = 1,
	AIROHA_XGS_UPSTREAM = 2,
};

struct airoha_xgs_security_keys {
	u8 msk[AIROHA_XGS_KEY_SIZE];
	u8 session[AIROHA_XGS_KEY_SIZE];
	u8 omci[AIROHA_XGS_KEY_SIZE];
	u8 ploam[AIROHA_XGS_KEY_SIZE];
	u8 kek[AIROHA_XGS_KEY_SIZE];
};

struct airoha_xgs_ranging_time {
	u32 equalization_delay;
	u8 sequence;
	bool absolute;
	bool negative;
	bool directed;
};

struct airoha_xgs_profile {
	u16 destination;
	u8 sequence;
	u8 index;
	u8 version;
	u8 preamble_repeat_count;
	u8 preamble_length;
	u8 delimiter_length;
	bool fec;
	u8 preamble[AIROHA_XGS_PROFILE_PATTERN_SIZE];
	u8 delimiter[AIROHA_XGS_PROFILE_PATTERN_SIZE];
	u8 pon_tag[AIROHA_XGS_PON_TAG_SIZE];
};

enum airoha_xgs_alloc_id_operation {
	AIROHA_XGS_ALLOC_ID_ASSIGN = 1,
	AIROHA_XGS_ALLOC_ID_DEALLOCATE = 0xff,
};

enum airoha_xgs_disable_serial_action {
	AIROHA_XGS_SERIAL_ALLOW = 0,
	AIROHA_XGS_SERIAL_DISABLE,
	AIROHA_XGS_SERIAL_DISABLE_DISCOVERY,
};

struct airoha_xgs_alloc_id_assignment {
	u16 alloc_id;
	u8 sequence;
	enum airoha_xgs_alloc_id_operation operation;
};

int airoha_xgs_derive_registration_msk(
	const u8 registration_id[AIROHA_XGS_REGISTRATION_ID_SIZE],
	u8 msk[AIROHA_XGS_KEY_SIZE]);
void airoha_xgs_copy_default_ploam_key(
	u8 key[AIROHA_XGS_KEY_SIZE]);
int airoha_xgs_derive_shared_keys(
	const u8 msk[AIROHA_XGS_KEY_SIZE],
	const u8 serial[AIROHA_XGS_SERIAL_SIZE],
	const u8 pon_tag[AIROHA_XGS_PON_TAG_SIZE],
	struct airoha_xgs_security_keys *keys);
int airoha_xgs_ploam_mic(
	const u8 key[AIROHA_XGS_KEY_SIZE], enum airoha_xgs_direction direction,
	const u8 content[AIROHA_XGS_PLOAM_CONTENT_SIZE],
	u8 mic[AIROHA_XGS_PLOAM_MIC_SIZE]);
int airoha_xgs_verify_ploam_mic(
	const u8 key[AIROHA_XGS_KEY_SIZE], enum airoha_xgs_direction direction,
	const u8 content[AIROHA_XGS_PLOAM_CONTENT_SIZE],
	const u8 mic[AIROHA_XGS_PLOAM_MIC_SIZE]);
int airoha_xgs_authenticate_initial_profile(
	const u8 frame[AIROHA_XGS_PLOAM_FRAME_SIZE],
	u8 pon_tag[AIROHA_XGS_PON_TAG_SIZE]);
int airoha_xgs_authenticate_initial_profile_mode(
	const u8 frame[AIROHA_XGS_PLOAM_FRAME_SIZE], bool xgspon,
	u8 pon_tag[AIROHA_XGS_PON_TAG_SIZE]);
int airoha_xgs_authenticate_initial_profile_words(
	const u32 words[AIROHA_XGS_PLOAM_FIFO_WORDS],
	u8 pon_tag[AIROHA_XGS_PON_TAG_SIZE]);
int airoha_xgs_authenticate_initial_profile_words_mode(
	const u32 words[AIROHA_XGS_PLOAM_FIFO_WORDS], bool xgspon,
	u8 pon_tag[AIROHA_XGS_PON_TAG_SIZE]);
int airoha_xgs_authenticate_profile_words_mode(
	const u32 words[AIROHA_XGS_PLOAM_FIFO_WORDS], bool xgspon,
	struct airoha_xgs_profile *profile);
int airoha_xgs_authenticate_profile_words_mode_key(
	const u32 words[AIROHA_XGS_PLOAM_FIFO_WORDS],
	const u8 key[AIROHA_XGS_KEY_SIZE], bool xgspon,
	struct airoha_xgs_profile *profile);
int airoha_xgs_authenticate_assign_onu_id_words(
	const u32 words[AIROHA_XGS_PLOAM_FIFO_WORDS],
	const u8 key[AIROHA_XGS_KEY_SIZE],
	const u8 serial[AIROHA_XGS_SERIAL_SIZE], u16 *onu_id, u8 *sequence);
int airoha_xgs_authenticate_request_registration_words(
	const u32 words[AIROHA_XGS_PLOAM_FIFO_WORDS],
	const u8 key[AIROHA_XGS_KEY_SIZE], u16 onu_id, u8 *sequence);
int airoha_xgs_authenticate_ranging_time_words(
	const u32 words[AIROHA_XGS_PLOAM_FIFO_WORDS],
	const u8 key[AIROHA_XGS_KEY_SIZE], u16 onu_id,
	struct airoha_xgs_ranging_time *ranging);
int airoha_xgs_authenticate_deactivate_onu_id_words(
	const u32 words[AIROHA_XGS_PLOAM_FIFO_WORDS],
	const u8 key[AIROHA_XGS_KEY_SIZE], u16 onu_id, u8 *sequence);
int airoha_xgs_authenticate_disable_serial_number_words(
	const u32 words[AIROHA_XGS_PLOAM_FIFO_WORDS],
	const u8 key[AIROHA_XGS_KEY_SIZE],
	const u8 serial[AIROHA_XGS_SERIAL_SIZE],
	enum airoha_xgs_disable_serial_action *action, u8 *sequence);
int airoha_xgs_authenticate_sleep_allow_words(
	const u32 words[AIROHA_XGS_PLOAM_FIFO_WORDS],
	const u8 key[AIROHA_XGS_KEY_SIZE], u16 onu_id,
	bool *sleep_type, u8 *sequence);
int airoha_xgs_authenticate_reboot_onu_words(
	const u32 words[AIROHA_XGS_PLOAM_FIFO_WORDS],
	const u8 key[AIROHA_XGS_KEY_SIZE], u16 onu_id,
	struct airoha_xgs_reboot_request *request);
int airoha_xgs_authenticate_assign_alloc_id_words(
	const u32 words[AIROHA_XGS_PLOAM_FIFO_WORDS],
	const u8 key[AIROHA_XGS_KEY_SIZE], u16 onu_id,
	struct airoha_xgs_alloc_id_assignment *assignment);
/*
 * These helpers build the standard 40-byte content plus its 8-byte MIC.
 * EN7581's 11-word upstream FIFO has a separate leading key-index word.
 */
int airoha_xgs_build_serial_number_ploam(
	const u8 serial[AIROHA_XGS_SERIAL_SIZE], u32 random_delay,
	enum airoha_xpon_mode mode,
	u8 frame[AIROHA_XGS_PLOAM_FRAME_SIZE]);
int airoha_xgs_build_registration_ploam(
	const u8 key[AIROHA_XGS_KEY_SIZE], u16 onu_id, u8 sequence,
	const u8 registration_id[AIROHA_XGS_REGISTRATION_ID_SIZE],
	u8 frame[AIROHA_XGS_PLOAM_FRAME_SIZE]);
int airoha_xgs_build_acknowledge_ploam(
	const u8 key[AIROHA_XGS_KEY_SIZE], u16 onu_id, u8 sequence,
	u8 completion_code, u8 frame[AIROHA_XGS_PLOAM_FRAME_SIZE]);
/*
 * Encode the EN7581 upstream FIFO metadata word followed by the standard
 * 40-byte PLOAM content. The MAC generates the MIC selected by key_index, so
 * the software MIC at frame[40..47] must not be written to this FIFO record.
 */
int airoha_xgs_encode_upstream_ploam_fifo(
	const u8 content[AIROHA_XGS_PLOAM_CONTENT_SIZE], u8 key_index,
	u32 words[AIROHA_XGS_PLOAM_UPSTREAM_FIFO_WORDS]);
int airoha_xgs_omci_mic(
	const u8 key[AIROHA_XGS_KEY_SIZE], enum airoha_xgs_direction direction,
	const u8 *content, size_t content_len,
	u8 mic[AIROHA_XGS_OMCI_MIC_SIZE]);
int airoha_xgs_verify_omci_mic(
	const u8 key[AIROHA_XGS_KEY_SIZE], enum airoha_xgs_direction direction,
	const u8 *content, size_t content_len,
	const u8 mic[AIROHA_XGS_OMCI_MIC_SIZE]);
int airoha_xgs_omci_content_length(const u8 *wire, size_t wire_len,
				    size_t *content_len);
/*
 * Validate the G.988 baseline/extended frame boundary before authenticating.
 * Downstream output excludes the verified MIC; upstream output includes the
 * generated MIC. On failure the output and returned length are cleared.
 */
int airoha_xgs_authenticate_downstream_omci(
	const u8 key[AIROHA_XGS_KEY_SIZE], const u8 *wire, size_t wire_len,
	u8 *content, size_t content_capacity, size_t *content_len);
int airoha_xgs_sign_upstream_omci(
	const u8 key[AIROHA_XGS_KEY_SIZE], const u8 *content,
	size_t content_len, u8 *wire, size_t wire_capacity,
	size_t *wire_len);
int airoha_xgs_aes_ecb_encrypt(
	const u8 key[AIROHA_XGS_KEY_SIZE],
	const u8 input[AIROHA_XGS_KEY_SIZE],
	u8 output[AIROHA_XGS_KEY_SIZE]);
int airoha_xgs_data_key_proof(
	const u8 kek[AIROHA_XGS_KEY_SIZE],
	const u8 data_key[AIROHA_XGS_KEY_SIZE],
	u8 proof[AIROHA_XGS_KEY_SIZE]);
int airoha_xgs_parse_key_control_words(
	const u32 words[AIROHA_XGS_PLOAM_FIFO_WORDS],
	const u8 key[AIROHA_XGS_KEY_SIZE], u16 onu_id,
	u8 *sequence, u8 *control, u8 *key_index);
int airoha_xgs_build_key_report(
	const u8 key[AIROHA_XGS_KEY_SIZE], u16 onu_id, u8 sequence,
	u8 report_type, u8 key_index,
	const u8 fragment[AIROHA_XGS_KEY_REPORT_FRAGMENT_SIZE],
	u8 frame[AIROHA_XGS_PLOAM_FRAME_SIZE]);

#endif
