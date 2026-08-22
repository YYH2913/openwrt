// SPDX-License-Identifier: GPL-2.0-only
/* Trusted G.9807.1 key derivation for the EN7581 XGS-PON resource owner. */

#include <crypto/hash.h>
#include <crypto/skcipher.h>
#include <crypto/utils.h>
#include <linux/err.h>
#include <linux/module.h>
#include <linux/slab.h>
#include <linux/string.h>
#include <linux/scatterlist.h>
#include <linux/unaligned.h>

#include "airoha-xgs-security.h"

static const u8 airoha_xgs_default_ploam_key[AIROHA_XGS_KEY_SIZE] = {
	[0 ... AIROHA_XGS_KEY_SIZE - 1] = 0x55,
};
static const u8 airoha_xgs_session_label[] = "SessionK";
static const u8 airoha_xgs_omci_label[] = "OMCIIntegrityKey";
/* The spelling is defined by G.9807.1 and is not a typo. */
static const u8 airoha_xgs_ploam_label[] = "PLOAMIntegrtyKey";
static const u8 airoha_xgs_kek_label[] = "KeyEncryptionKey";

#define AIROHA_XGS_PROFILE_MESSAGE_ID	0x01
#define AIROHA_XGS_ASSIGN_ONU_ID_MESSAGE_ID	0x03
#define AIROHA_XGS_RANGING_TIME_MESSAGE_ID	0x04
#define AIROHA_XGS_DEACTIVATE_ONU_ID_MESSAGE_ID	0x05
#define AIROHA_XGS_DISABLE_SERIAL_NUMBER_MESSAGE_ID	0x06
#define AIROHA_XGS_REQUEST_REGISTRATION_MESSAGE_ID	0x09
#define AIROHA_XGS_ASSIGN_ALLOC_ID_MESSAGE_ID	0x0a
#define AIROHA_XGS_SLEEP_ALLOW_MESSAGE_ID	0x12
#define AIROHA_XGS_SERIAL_NUMBER_MESSAGE_ID	0x01
#define AIROHA_XGS_REGISTRATION_MESSAGE_ID	0x02
#define AIROHA_XGS_ACKNOWLEDGE_MESSAGE_ID	0x09
#define AIROHA_XGS_KEY_CONTROL_RESERVED_OFFSET	4
#define AIROHA_XGS_KEY_CONTROL_CONTROL_OFFSET	5
#define AIROHA_XGS_KEY_CONTROL_INDEX_OFFSET	6
#define AIROHA_XGS_KEY_CONTROL_LENGTH_OFFSET	7
#define AIROHA_XGS_KEY_CONTROL_PADDING_OFFSET	8
#define AIROHA_XGS_KEY_CONTROL_128_BIT_LENGTH	16
#define AIROHA_XGS_KEY_CONTROL_FIELD_RESERVED	GENMASK(7, 1)
#define AIROHA_XGS_KEY_CONTROL_INDEX_RESERVED	GENMASK(7, 2)
#define AIROHA_XGS_KEY_PROOF_LABEL	"3141592653589793"
#define AIROHA_XGS_PROFILE_LINE_RATE	BIT(2)
#define AIROHA_XGPON_UPSTREAM_LINE_RATE_CAP	0x00
#define AIROHA_XGSPON_UPSTREAM_LINE_RATE_CAP	0x03
#define AIROHA_XGS_PROFILE_INDEX	GENMASK(1, 0)
#define AIROHA_XGS_PROFILE_VERSION	GENMASK(7, 4)
#define AIROHA_XGS_PROFILE_FEC	BIT(0)
#define AIROHA_XGS_PROFILE_DELIMITER_LENGTH	GENMASK(3, 0)
#define AIROHA_XGS_PROFILE_PREAMBLE_LENGTH	GENMASK(3, 0)
#define AIROHA_XGS_PROFILE_DELIMITER_OFFSET	7
#define AIROHA_XGS_PROFILE_PREAMBLE_LENGTH_OFFSET	15
#define AIROHA_XGS_PROFILE_PREAMBLE_REPEAT_OFFSET	16
#define AIROHA_XGS_PROFILE_PREAMBLE_OFFSET	17
#define AIROHA_XGS_PROFILE_PON_TAG_OFFSET	25
#define AIROHA_XGPON_PROFILE_PREAMBLE_REPEAT_MASK	0x1f
#define AIROHA_XGS_ASSIGN_ONU_ID_OFFSET	4
#define AIROHA_XGS_ASSIGN_ONU_ID_SERIAL_OFFSET	6
#define AIROHA_XGS_ASSIGN_ONU_ID_RATE_OFFSET	14
#define AIROHA_XGS_ASSIGN_ONU_ID_PADDING_OFFSET	15
#define AIROHA_XGS_ASSIGN_ONU_ID_RATE_RESERVED	GENMASK(7, 1)
#define AIROHA_XGS_RANGING_FLAGS_OFFSET	4
#define AIROHA_XGS_RANGING_EQD_OFFSET	5
#define AIROHA_XGS_RANGING_PON_ID_OFFSET	9
#define AIROHA_XGS_RANGING_PON_ID_SIZE	8
#define AIROHA_XGS_RANGING_PADDING_OFFSET	17
#define AIROHA_XGS_RANGING_ABSOLUTE	BIT(0)
#define AIROHA_XGS_RANGING_NEGATIVE	BIT(1)
#define AIROHA_XGS_RANGING_RESERVED	GENMASK(7, 2)
#define AIROHA_XGS_ALLOC_ID_OFFSET	4
#define AIROHA_XGS_ALLOC_ID_TYPE_OFFSET	6
#define AIROHA_XGS_ALLOC_ID_SCOPE_OFFSET	7
#define AIROHA_XGS_ALLOC_ID_SCOPE_SIZE	2
#define AIROHA_XGS_ALLOC_ID_PADDING_OFFSET	9
#define AIROHA_XGS_ALLOC_ID_RESERVED	GENMASK(7, 6)
#define AIROHA_XGS_DISABLE_SERIAL_MODE_OFFSET	4
#define AIROHA_XGS_DISABLE_SERIAL_NUMBER_OFFSET	5
#define AIROHA_XGS_DISABLE_SERIAL_PADDING_OFFSET	13
#define AIROHA_XGS_DISABLE_SERIAL_DENIED_SPECIFIC	0xff
#define AIROHA_XGS_DISABLE_SERIAL_ALLOWED_SPECIFIC	0x00
#define AIROHA_XGS_DISABLE_SERIAL_DENIED_ALL	0x0f
#define AIROHA_XGS_DISABLE_SERIAL_DISCOVERY	0x3f
#define AIROHA_XGS_DISABLE_SERIAL_ALLOWED_ALL	0xf0
#define AIROHA_XGS_SLEEP_ALLOW_TYPE_OFFSET	4
#define AIROHA_XGS_SLEEP_ALLOW_TYPE	BIT(0)
#define AIROHA_XGS_SLEEP_ALLOW_RESERVED	GENMASK(7, 1)
#define AIROHA_XGS_SLEEP_ALLOW_PADDING_OFFSET	5
#define AIROHA_XGS_BROADCAST_ONU_ID	0x03ff
#define AIROHA_XGS_BROADCAST_XGS_ONU_ID	0x03fe
#define AIROHA_XGS_BROADCAST_NOKIA_ONU_ID	0x07ff
#define AIROHA_XGS_OMCI_BASELINE_DEVICE_ID	0x0a
#define AIROHA_XGS_OMCI_EXTENDED_DEVICE_ID	0x0b
#define AIROHA_XGS_OMCI_EXTENDED_HEADER_SIZE	10
#define AIROHA_XGS_OMCI_BASELINE_LENGTH_OFFSET	40
#define AIROHA_XGS_OMCI_BASELINE_LENGTH	40
#define AIROHA_XGS_OMCI_EXTENDED_LENGTH_OFFSET	8

static int airoha_xgs_cmac(const u8 key[AIROHA_XGS_KEY_SIZE],
			   const u8 *message, size_t message_len,
			   u8 tag[AIROHA_XGS_KEY_SIZE])
{
	struct crypto_shash *tfm;
	int ret;

	tfm = crypto_alloc_shash("cmac(aes)", 0, 0);
	if (IS_ERR(tfm))
		return PTR_ERR(tfm);
	if (crypto_shash_digestsize(tfm) != AIROHA_XGS_KEY_SIZE) {
		ret = -EINVAL;
		goto out;
	}
	ret = crypto_shash_setkey(tfm, key, AIROHA_XGS_KEY_SIZE);
	if (!ret)
		ret = crypto_shash_tfm_digest(tfm, message, message_len, tag);
out:
	crypto_free_shash(tfm);
	if (ret)
		memzero_explicit(tag, AIROHA_XGS_KEY_SIZE);
	return ret;
}

int airoha_xgs_aes_ecb_encrypt(
	const u8 key[AIROHA_XGS_KEY_SIZE],
	const u8 input[AIROHA_XGS_KEY_SIZE],
	u8 output[AIROHA_XGS_KEY_SIZE])
{
	struct crypto_sync_skcipher *tfm;
	struct skcipher_request *request;
	struct scatterlist source, destination;
	u8 *source_buffer = NULL;
	u8 *destination_buffer = NULL;
	int ret;

	if (!output)
		return -EINVAL;
	memzero_explicit(output, AIROHA_XGS_KEY_SIZE);
	if (!key || !input)
		return -EINVAL;

	tfm = crypto_alloc_sync_skcipher("ecb(aes)", 0, 0);
	if (IS_ERR(tfm))
		return PTR_ERR(tfm);
	request = skcipher_request_alloc(&tfm->base, GFP_KERNEL);
	if (!request) {
		ret = -ENOMEM;
		goto out_tfm;
	}
	ret = crypto_sync_skcipher_setkey(tfm, key, AIROHA_XGS_KEY_SIZE);
	if (ret)
		goto out_request;

	/*
	 * Scatterlists must point at linear, page-backed memory.  Callers of
	 * this helper include the module self-test, which uses VMAP_STACK on
	 * arm64; passing those stack addresses to sg_init_one() makes the
	 * crypto walk derive an invalid struct page and panic in
	 * flush_dcache_page().
	 */
	source_buffer = kmemdup(input, AIROHA_XGS_KEY_SIZE, GFP_KERNEL);
	destination_buffer = kzalloc(AIROHA_XGS_KEY_SIZE, GFP_KERNEL);
	if (!source_buffer || !destination_buffer) {
		ret = -ENOMEM;
		goto out_request;
	}
	sg_init_one(&source, source_buffer, AIROHA_XGS_KEY_SIZE);
	sg_init_one(&destination, destination_buffer, AIROHA_XGS_KEY_SIZE);
	skcipher_request_set_sync_tfm(request, tfm);
	skcipher_request_set_callback(request, 0, NULL, NULL);
	skcipher_request_set_crypt(request, &source, &destination,
				    AIROHA_XGS_KEY_SIZE, NULL);
	ret = crypto_skcipher_encrypt(request);
	if (!ret)
		memcpy(output, destination_buffer, AIROHA_XGS_KEY_SIZE);
out_request:
	skcipher_request_free(request);
out_tfm:
	crypto_free_sync_skcipher(tfm);
	kfree_sensitive(destination_buffer);
	kfree_sensitive(source_buffer);
	if (ret)
		memzero_explicit(output, AIROHA_XGS_KEY_SIZE);
	return ret;
}
EXPORT_SYMBOL_GPL(airoha_xgs_aes_ecb_encrypt);

int airoha_xgs_data_key_proof(
	const u8 kek[AIROHA_XGS_KEY_SIZE],
	const u8 data_key[AIROHA_XGS_KEY_SIZE],
	u8 proof[AIROHA_XGS_KEY_SIZE])
{
	u8 input[AIROHA_XGS_KEY_SIZE * 2];
	int ret;

	if (!proof)
		return -EINVAL;
	memzero_explicit(proof, AIROHA_XGS_KEY_SIZE);
	if (!kek || !data_key)
		return -EINVAL;
	memcpy(input, data_key, AIROHA_XGS_KEY_SIZE);
	memcpy(input + AIROHA_XGS_KEY_SIZE, AIROHA_XGS_KEY_PROOF_LABEL,
	       AIROHA_XGS_KEY_SIZE);
	ret = airoha_xgs_cmac(kek, input, sizeof(input), proof);
	memzero_explicit(input, sizeof(input));
	return ret;
}
EXPORT_SYMBOL_GPL(airoha_xgs_data_key_proof);

static int airoha_xgs_directional_cmac(
	const u8 key[AIROHA_XGS_KEY_SIZE], enum airoha_xgs_direction direction,
	const u8 *content, size_t content_len, u8 *tag, size_t tag_len)
{
	struct crypto_shash *tfm;
	SHASH_DESC_ON_STACK(desc, tfm);
	u8 full_tag[AIROHA_XGS_KEY_SIZE];
	u8 direction_code = direction;
	int ret;

	if (!tag || !tag_len || tag_len > sizeof(full_tag))
		return -EINVAL;
	if (!key || !content || !content_len ||
	    (direction != AIROHA_XGS_DOWNSTREAM &&
	     direction != AIROHA_XGS_UPSTREAM)) {
		memzero_explicit(tag, tag_len);
		return -EINVAL;
	}

	tfm = crypto_alloc_shash("cmac(aes)", 0, 0);
	if (IS_ERR(tfm)) {
		memzero_explicit(tag, tag_len);
		return PTR_ERR(tfm);
	}
	desc->tfm = tfm;
	if (crypto_shash_digestsize(tfm) != sizeof(full_tag)) {
		ret = -EINVAL;
		goto out;
	}
	ret = crypto_shash_setkey(tfm, key, AIROHA_XGS_KEY_SIZE);
	if (!ret)
		ret = crypto_shash_init(desc);
	if (!ret)
		ret = crypto_shash_update(desc, &direction_code,
					 sizeof(direction_code));
	if (!ret)
		ret = crypto_shash_update(desc, content, content_len);
	if (!ret)
		ret = crypto_shash_final(desc, full_tag);
	if (!ret)
		memcpy(tag, full_tag, tag_len);
out:
	memzero_explicit(full_tag, sizeof(full_tag));
	shash_desc_zero(desc);
	crypto_free_shash(tfm);
	if (ret)
		memzero_explicit(tag, tag_len);
	return ret;
}

int airoha_xgs_derive_registration_msk(
	const u8 registration_id[AIROHA_XGS_REGISTRATION_ID_SIZE],
	u8 msk[AIROHA_XGS_KEY_SIZE])
{
	if (!registration_id || !msk)
		return -EINVAL;

	return airoha_xgs_cmac(airoha_xgs_default_ploam_key,
			       registration_id,
			       AIROHA_XGS_REGISTRATION_ID_SIZE, msk);
}
EXPORT_SYMBOL_GPL(airoha_xgs_derive_registration_msk);

void airoha_xgs_copy_default_ploam_key(u8 key[AIROHA_XGS_KEY_SIZE])
{
	if (key)
		memcpy(key, airoha_xgs_default_ploam_key,
		       AIROHA_XGS_KEY_SIZE);
}
EXPORT_SYMBOL_GPL(airoha_xgs_copy_default_ploam_key);

int airoha_xgs_derive_shared_keys(
	const u8 msk[AIROHA_XGS_KEY_SIZE],
	const u8 serial[AIROHA_XGS_SERIAL_SIZE],
	const u8 pon_tag[AIROHA_XGS_PON_TAG_SIZE],
	struct airoha_xgs_security_keys *keys)
{
	u8 session_context[AIROHA_XGS_SERIAL_SIZE + AIROHA_XGS_PON_TAG_SIZE +
			   sizeof(airoha_xgs_session_label) - 1];
	int ret;

	if (!msk || !serial || !pon_tag || !keys)
		return -EINVAL;
	memset(keys, 0, sizeof(*keys));
	memcpy(keys->msk, msk, sizeof(keys->msk));
	memcpy(session_context, serial, AIROHA_XGS_SERIAL_SIZE);
	memcpy(session_context + AIROHA_XGS_SERIAL_SIZE, pon_tag,
	       AIROHA_XGS_PON_TAG_SIZE);
	memcpy(session_context + AIROHA_XGS_SERIAL_SIZE + AIROHA_XGS_PON_TAG_SIZE,
	       airoha_xgs_session_label, sizeof(airoha_xgs_session_label) - 1);

	ret = airoha_xgs_cmac(keys->msk, session_context,
			      sizeof(session_context), keys->session);
	if (ret)
		goto out;
	ret = airoha_xgs_cmac(keys->session, airoha_xgs_omci_label,
			      sizeof(airoha_xgs_omci_label) - 1, keys->omci);
	if (ret)
		goto out;
	ret = airoha_xgs_cmac(keys->session, airoha_xgs_ploam_label,
			      sizeof(airoha_xgs_ploam_label) - 1, keys->ploam);
	if (ret)
		goto out;
	ret = airoha_xgs_cmac(keys->session, airoha_xgs_kek_label,
			      sizeof(airoha_xgs_kek_label) - 1, keys->kek);
out:
	memzero_explicit(session_context, sizeof(session_context));
	if (ret)
		memzero_explicit(keys, sizeof(*keys));
	return ret;
}
EXPORT_SYMBOL_GPL(airoha_xgs_derive_shared_keys);

int airoha_xgs_ploam_mic(
	const u8 key[AIROHA_XGS_KEY_SIZE], enum airoha_xgs_direction direction,
	const u8 content[AIROHA_XGS_PLOAM_CONTENT_SIZE],
	u8 mic[AIROHA_XGS_PLOAM_MIC_SIZE])
{
	return airoha_xgs_directional_cmac(key, direction, content,
					   AIROHA_XGS_PLOAM_CONTENT_SIZE,
					   mic, AIROHA_XGS_PLOAM_MIC_SIZE);
}
EXPORT_SYMBOL_GPL(airoha_xgs_ploam_mic);

int airoha_xgs_verify_ploam_mic(
	const u8 key[AIROHA_XGS_KEY_SIZE], enum airoha_xgs_direction direction,
	const u8 content[AIROHA_XGS_PLOAM_CONTENT_SIZE],
	const u8 mic[AIROHA_XGS_PLOAM_MIC_SIZE])
{
	u8 expected[AIROHA_XGS_PLOAM_MIC_SIZE];
	int ret;

	if (!mic)
		return -EINVAL;
	ret = airoha_xgs_ploam_mic(key, direction, content, expected);
	if (!ret && crypto_memneq(expected, mic, sizeof(expected)))
		ret = -EBADMSG;
	memzero_explicit(expected, sizeof(expected));
	return ret;
}
EXPORT_SYMBOL_GPL(airoha_xgs_verify_ploam_mic);

static bool airoha_xgs_profile_is_broadcast(u16 destination, bool xgspon)
{
	return destination == AIROHA_XGS_BROADCAST_ONU_ID ||
	       (xgspon &&
		(destination == AIROHA_XGS_BROADCAST_XGS_ONU_ID ||
		 destination == AIROHA_XGS_BROADCAST_NOKIA_ONU_ID));
}

static int airoha_xgs_authenticate_profile_frame_mode_key(
	const u8 frame[AIROHA_XGS_PLOAM_FRAME_SIZE],
	const u8 key[AIROHA_XGS_KEY_SIZE], bool xgspon, bool broadcast_only,
	struct airoha_xgs_profile *profile)
{
	u16 destination;
	int ret;

	if (!profile)
		return -EINVAL;
	memzero_explicit(profile, sizeof(*profile));
	if (!frame || !key)
		return -EINVAL;

	destination = get_unaligned_be16(frame);
	if (broadcast_only &&
	    !airoha_xgs_profile_is_broadcast(destination, xgspon))
		return -EADDRNOTAVAIL;
	if (frame[2] != AIROHA_XGS_PROFILE_MESSAGE_ID ||
	    !!(frame[4] & AIROHA_XGS_PROFILE_LINE_RATE) != xgspon)
		return -EPROTO;

	ret = airoha_xgs_verify_ploam_mic(key,
					   AIROHA_XGS_DOWNSTREAM, frame,
					   frame + AIROHA_XGS_PLOAM_CONTENT_SIZE);
	if (ret)
		return ret;

	profile->destination = destination;
	profile->sequence = frame[3];
	profile->index = FIELD_GET(AIROHA_XGS_PROFILE_INDEX, frame[4]);
	profile->version = FIELD_GET(AIROHA_XGS_PROFILE_VERSION, frame[4]);
	profile->fec = !!(frame[5] & AIROHA_XGS_PROFILE_FEC);
	profile->delimiter_length = FIELD_GET(
		AIROHA_XGS_PROFILE_DELIMITER_LENGTH, frame[6]);
	memcpy(profile->delimiter,
	       frame + AIROHA_XGS_PROFILE_DELIMITER_OFFSET,
	       sizeof(profile->delimiter));
	profile->preamble_length = FIELD_GET(
		AIROHA_XGS_PROFILE_PREAMBLE_LENGTH,
		frame[AIROHA_XGS_PROFILE_PREAMBLE_LENGTH_OFFSET]);
	profile->preamble_repeat_count =
		frame[AIROHA_XGS_PROFILE_PREAMBLE_REPEAT_OFFSET] &
		(xgspon ? U8_MAX : AIROHA_XGPON_PROFILE_PREAMBLE_REPEAT_MASK);
	memcpy(profile->preamble,
	       frame + AIROHA_XGS_PROFILE_PREAMBLE_OFFSET,
	       sizeof(profile->preamble));
	memcpy(profile->pon_tag, frame + AIROHA_XGS_PROFILE_PON_TAG_OFFSET,
	       sizeof(profile->pon_tag));
	return 0;
}

int airoha_xgs_authenticate_initial_profile_mode(
	const u8 frame[AIROHA_XGS_PLOAM_FRAME_SIZE], bool xgspon,
	u8 pon_tag[AIROHA_XGS_PON_TAG_SIZE])
{
	struct airoha_xgs_profile profile;
	int ret;

	if (!pon_tag)
		return -EINVAL;
	memzero_explicit(pon_tag, AIROHA_XGS_PON_TAG_SIZE);
	ret = airoha_xgs_authenticate_profile_frame_mode_key(
		frame, airoha_xgs_default_ploam_key, xgspon, true, &profile);
	if (!ret)
		memcpy(pon_tag, profile.pon_tag, AIROHA_XGS_PON_TAG_SIZE);
	memzero_explicit(&profile, sizeof(profile));

	return ret;
}
EXPORT_SYMBOL_GPL(airoha_xgs_authenticate_initial_profile_mode);

int airoha_xgs_authenticate_initial_profile(
	const u8 frame[AIROHA_XGS_PLOAM_FRAME_SIZE],
	u8 pon_tag[AIROHA_XGS_PON_TAG_SIZE])
{
	return airoha_xgs_authenticate_initial_profile_mode(
		frame, true, pon_tag);
}
EXPORT_SYMBOL_GPL(airoha_xgs_authenticate_initial_profile);

int airoha_xgs_authenticate_initial_profile_words_mode(
	const u32 words[AIROHA_XGS_PLOAM_FIFO_WORDS], bool xgspon,
	u8 pon_tag[AIROHA_XGS_PON_TAG_SIZE])
{
	struct airoha_xgs_profile profile;
	int ret;

	if (!pon_tag)
		return -EINVAL;
	memzero_explicit(pon_tag, AIROHA_XGS_PON_TAG_SIZE);
	ret = airoha_xgs_authenticate_profile_words_mode(
		words, xgspon, &profile);
	if (!ret)
		memcpy(pon_tag, profile.pon_tag, AIROHA_XGS_PON_TAG_SIZE);
	memzero_explicit(&profile, sizeof(profile));

	return ret;
}
EXPORT_SYMBOL_GPL(airoha_xgs_authenticate_initial_profile_words_mode);

int airoha_xgs_authenticate_profile_words_mode(
	const u32 words[AIROHA_XGS_PLOAM_FIFO_WORDS], bool xgspon,
	struct airoha_xgs_profile *profile)
{
	u8 frame[AIROHA_XGS_PLOAM_FRAME_SIZE];
	int i, ret;

	if (!profile)
		return -EINVAL;
	memzero_explicit(profile, sizeof(*profile));
	if (!words)
		return -EINVAL;

	/* The SDK FIFO record has one non-standard status word after the MIC. */
	for (i = 0; i < AIROHA_XGS_PLOAM_FRAME_WORDS; i++)
		put_unaligned_be32(words[i], frame + i * sizeof(u32));
	ret = airoha_xgs_authenticate_profile_frame_mode_key(
		frame, airoha_xgs_default_ploam_key, xgspon, true, profile);
	if (ret)
		memzero_explicit(profile, sizeof(*profile));
	memzero_explicit(frame, sizeof(frame));
	return ret;
}
EXPORT_SYMBOL_GPL(airoha_xgs_authenticate_profile_words_mode);

int airoha_xgs_authenticate_profile_words_mode_key(
	const u32 words[AIROHA_XGS_PLOAM_FIFO_WORDS],
	const u8 key[AIROHA_XGS_KEY_SIZE], bool xgspon,
	struct airoha_xgs_profile *profile)
{
	u8 frame[AIROHA_XGS_PLOAM_FRAME_SIZE];
	int i, ret;

	if (!profile)
		return -EINVAL;
	memzero_explicit(profile, sizeof(*profile));
	if (!words || !key)
		return -EINVAL;

	for (i = 0; i < AIROHA_XGS_PLOAM_FRAME_WORDS; i++)
		put_unaligned_be32(words[i], frame + i * sizeof(u32));
	ret = airoha_xgs_authenticate_profile_frame_mode_key(
		frame, key, xgspon, false, profile);
	if (ret)
		memzero_explicit(profile, sizeof(*profile));
	memzero_explicit(frame, sizeof(frame));

	return ret;
}
EXPORT_SYMBOL_GPL(airoha_xgs_authenticate_profile_words_mode_key);

int airoha_xgs_authenticate_initial_profile_words(
	const u32 words[AIROHA_XGS_PLOAM_FIFO_WORDS],
	u8 pon_tag[AIROHA_XGS_PON_TAG_SIZE])
{
	return airoha_xgs_authenticate_initial_profile_words_mode(
		words, true, pon_tag);
}
EXPORT_SYMBOL_GPL(airoha_xgs_authenticate_initial_profile_words);

int airoha_xgs_authenticate_assign_onu_id_words(
	const u32 words[AIROHA_XGS_PLOAM_FIFO_WORDS],
	const u8 key[AIROHA_XGS_KEY_SIZE],
	const u8 serial[AIROHA_XGS_SERIAL_SIZE], u16 *onu_id, u8 *sequence)
{
	u8 frame[AIROHA_XGS_PLOAM_FRAME_SIZE];
	u16 assigned, destination;
	int i, ret;

	if (!onu_id || !sequence)
		return -EINVAL;
	*onu_id = AIROHA_XGS_BROADCAST_ONU_ID;
	*sequence = 0;
	if (!words || !key || !serial)
		return -EINVAL;

	for (i = 0; i < AIROHA_XGS_PLOAM_FRAME_WORDS; i++)
		put_unaligned_be32(words[i], frame + i * sizeof(u32));
	ret = airoha_xgs_verify_ploam_mic(key, AIROHA_XGS_DOWNSTREAM,
					  frame,
					  frame + AIROHA_XGS_PLOAM_CONTENT_SIZE);
	if (ret)
		goto out;

	destination = get_unaligned_be16(frame);
	if (destination != AIROHA_XGS_BROADCAST_ONU_ID &&
	    destination != AIROHA_XGS_BROADCAST_XGS_ONU_ID) {
		ret = -EADDRNOTAVAIL;
		goto out;
	}
	if (frame[2] != AIROHA_XGS_ASSIGN_ONU_ID_MESSAGE_ID ||
	    (frame[AIROHA_XGS_ASSIGN_ONU_ID_OFFSET] & 0xfc) ||
	    (frame[AIROHA_XGS_ASSIGN_ONU_ID_RATE_OFFSET] &
	     AIROHA_XGS_ASSIGN_ONU_ID_RATE_RESERVED)) {
		ret = -EPROTO;
		goto out;
	}
	if (memcmp(frame + AIROHA_XGS_ASSIGN_ONU_ID_SERIAL_OFFSET, serial,
		   AIROHA_XGS_SERIAL_SIZE)) {
		ret = -ENODEV;
		goto out;
	}
	assigned = ((u16)frame[AIROHA_XGS_ASSIGN_ONU_ID_OFFSET] << 8) |
		   frame[AIROHA_XGS_ASSIGN_ONU_ID_OFFSET + 1];
	if (assigned == AIROHA_XGS_BROADCAST_ONU_ID ||
	    assigned == AIROHA_XGS_BROADCAST_XGS_ONU_ID) {
		ret = -ERANGE;
		goto out;
	}

	*onu_id = assigned;
	*sequence = frame[3];
	ret = 0;
out:
	memzero_explicit(frame, sizeof(frame));
	return ret;
}
EXPORT_SYMBOL_GPL(airoha_xgs_authenticate_assign_onu_id_words);

int airoha_xgs_authenticate_request_registration_words(
	const u32 words[AIROHA_XGS_PLOAM_FIFO_WORDS],
	const u8 key[AIROHA_XGS_KEY_SIZE], u16 onu_id, u8 *sequence)
{
	u8 frame[AIROHA_XGS_PLOAM_FRAME_SIZE];
	u16 destination;
	int i, ret;

	if (!sequence)
		return -EINVAL;
	*sequence = 0;
	if (!words || !key)
		return -EINVAL;
	if (onu_id >= AIROHA_XGS_BROADCAST_XGS_ONU_ID)
		return -ERANGE;

	for (i = 0; i < AIROHA_XGS_PLOAM_FRAME_WORDS; i++)
		put_unaligned_be32(words[i], frame + i * sizeof(u32));
	ret = airoha_xgs_verify_ploam_mic(key, AIROHA_XGS_DOWNSTREAM,
					  frame,
					  frame + AIROHA_XGS_PLOAM_CONTENT_SIZE);
	if (ret)
		goto out;

	destination = get_unaligned_be16(frame);
	if (destination != onu_id) {
		ret = -EADDRNOTAVAIL;
		goto out;
	}
	if (frame[2] != AIROHA_XGS_REQUEST_REGISTRATION_MESSAGE_ID) {
		ret = -EPROTO;
		goto out;
	}

	*sequence = frame[3];
	ret = 0;
out:
	memzero_explicit(frame, sizeof(frame));
	return ret;
}
EXPORT_SYMBOL_GPL(airoha_xgs_authenticate_request_registration_words);

int airoha_xgs_authenticate_ranging_time_words(
	const u32 words[AIROHA_XGS_PLOAM_FIFO_WORDS],
	const u8 key[AIROHA_XGS_KEY_SIZE], u16 onu_id,
	struct airoha_xgs_ranging_time *ranging)
{
	u8 frame[AIROHA_XGS_PLOAM_FRAME_SIZE];
	u16 destination;
	bool absolute;
	int i, ret;

	if (!ranging)
		return -EINVAL;
	memset(ranging, 0, sizeof(*ranging));
	if (!words || !key)
		return -EINVAL;
	if (onu_id >= AIROHA_XGS_BROADCAST_XGS_ONU_ID)
		return -ERANGE;

	for (i = 0; i < AIROHA_XGS_PLOAM_FRAME_WORDS; i++)
		put_unaligned_be32(words[i], frame + i * sizeof(u32));
	ret = airoha_xgs_verify_ploam_mic(key, AIROHA_XGS_DOWNSTREAM,
					  frame,
					  frame + AIROHA_XGS_PLOAM_CONTENT_SIZE);
	if (ret)
		goto out;

	if (frame[2] != AIROHA_XGS_RANGING_TIME_MESSAGE_ID ||
	    (frame[AIROHA_XGS_RANGING_FLAGS_OFFSET] &
	     AIROHA_XGS_RANGING_RESERVED) ||
	    memchr_inv(frame + AIROHA_XGS_RANGING_PON_ID_OFFSET, 0,
		       AIROHA_XGS_RANGING_PON_ID_SIZE)) {
		ret = -EPROTO;
		goto out;
	}
	absolute = !!(frame[AIROHA_XGS_RANGING_FLAGS_OFFSET] &
		      AIROHA_XGS_RANGING_ABSOLUTE);
	destination = get_unaligned_be16(frame);
	if (destination == onu_id) {
		ranging->directed = true;
	} else if (absolute ||
		   (destination != AIROHA_XGS_BROADCAST_ONU_ID &&
		    destination != AIROHA_XGS_BROADCAST_XGS_ONU_ID)) {
		ret = -EADDRNOTAVAIL;
		goto out;
	}

	ranging->equalization_delay = get_unaligned_be32(
			frame + AIROHA_XGS_RANGING_EQD_OFFSET);
	ranging->sequence = frame[3];
	ranging->absolute = absolute;
	ranging->negative = !!(frame[AIROHA_XGS_RANGING_FLAGS_OFFSET] &
				       AIROHA_XGS_RANGING_NEGATIVE);
	ret = 0;
out:
	if (ret)
		memset(ranging, 0, sizeof(*ranging));
	memzero_explicit(frame, sizeof(frame));
	return ret;
}
EXPORT_SYMBOL_GPL(airoha_xgs_authenticate_ranging_time_words);

int airoha_xgs_authenticate_deactivate_onu_id_words(
	const u32 words[AIROHA_XGS_PLOAM_FIFO_WORDS],
	const u8 key[AIROHA_XGS_KEY_SIZE], u16 onu_id, u8 *sequence)
{
	u8 frame[AIROHA_XGS_PLOAM_FRAME_SIZE];
	u16 destination;
	int i, ret;

	if (!sequence)
		return -EINVAL;
	*sequence = 0;
	if (!words || !key)
		return -EINVAL;
	if (onu_id >= AIROHA_XGS_BROADCAST_XGS_ONU_ID)
		return -ERANGE;

	for (i = 0; i < AIROHA_XGS_PLOAM_FRAME_WORDS; i++)
		put_unaligned_be32(words[i], frame + i * sizeof(u32));
	ret = airoha_xgs_verify_ploam_mic(key, AIROHA_XGS_DOWNSTREAM,
					  frame,
					  frame + AIROHA_XGS_PLOAM_CONTENT_SIZE);
	if (ret)
		goto out;

	destination = get_unaligned_be16(frame);
	if (destination != onu_id &&
	    destination != AIROHA_XGS_BROADCAST_ONU_ID &&
	    destination != AIROHA_XGS_BROADCAST_XGS_ONU_ID) {
		ret = -EADDRNOTAVAIL;
		goto out;
	}
	if (frame[2] != AIROHA_XGS_DEACTIVATE_ONU_ID_MESSAGE_ID) {
		ret = -EPROTO;
		goto out;
	}

	*sequence = frame[3];
	ret = 0;
out:
	memzero_explicit(frame, sizeof(frame));
	return ret;
}
EXPORT_SYMBOL_GPL(airoha_xgs_authenticate_deactivate_onu_id_words);

int airoha_xgs_authenticate_disable_serial_number_words(
	const u32 words[AIROHA_XGS_PLOAM_FIFO_WORDS],
	const u8 key[AIROHA_XGS_KEY_SIZE],
	const u8 serial[AIROHA_XGS_SERIAL_SIZE],
	enum airoha_xgs_disable_serial_action *action, u8 *sequence)
{
	u8 frame[AIROHA_XGS_PLOAM_FRAME_SIZE];
	u16 destination;
	int i, ret;

	if (!action || !sequence)
		return -EINVAL;
	*action = AIROHA_XGS_SERIAL_ALLOW;
	*sequence = 0;
	if (!words || !key || !serial)
		return -EINVAL;

	for (i = 0; i < AIROHA_XGS_PLOAM_FRAME_WORDS; i++)
		put_unaligned_be32(words[i], frame + i * sizeof(u32));
	ret = airoha_xgs_verify_ploam_mic(key, AIROHA_XGS_DOWNSTREAM,
					  frame,
					  frame + AIROHA_XGS_PLOAM_CONTENT_SIZE);
	if (ret)
		goto out;
	destination = get_unaligned_be16(frame);
	if (destination != AIROHA_XGS_BROADCAST_ONU_ID &&
	    destination != AIROHA_XGS_BROADCAST_XGS_ONU_ID) {
		ret = -EADDRNOTAVAIL;
		goto out;
	}
	if (frame[2] != AIROHA_XGS_DISABLE_SERIAL_NUMBER_MESSAGE_ID) {
		ret = -EPROTO;
		goto out;
	}

	switch (frame[AIROHA_XGS_DISABLE_SERIAL_MODE_OFFSET]) {
	case AIROHA_XGS_DISABLE_SERIAL_DENIED_SPECIFIC:
		if (crypto_memneq(frame + AIROHA_XGS_DISABLE_SERIAL_NUMBER_OFFSET,
				  serial, AIROHA_XGS_SERIAL_SIZE)) {
			ret = -ENOMSG;
			goto out;
		}
		*action = AIROHA_XGS_SERIAL_DISABLE;
		break;
	case AIROHA_XGS_DISABLE_SERIAL_DENIED_ALL:
		*action = AIROHA_XGS_SERIAL_DISABLE;
		break;
	case AIROHA_XGS_DISABLE_SERIAL_DISCOVERY:
		*action = AIROHA_XGS_SERIAL_DISABLE_DISCOVERY;
		break;
	case AIROHA_XGS_DISABLE_SERIAL_ALLOWED_SPECIFIC:
		if (crypto_memneq(frame + AIROHA_XGS_DISABLE_SERIAL_NUMBER_OFFSET,
				  serial, AIROHA_XGS_SERIAL_SIZE)) {
			ret = -ENOMSG;
			goto out;
		}
		break;
	case AIROHA_XGS_DISABLE_SERIAL_ALLOWED_ALL:
		break;
	default:
		ret = -EPROTO;
		goto out;
	}
	*sequence = frame[3];
	ret = 0;
out:
	if (ret) {
		*action = AIROHA_XGS_SERIAL_ALLOW;
		*sequence = 0;
	}
	memzero_explicit(frame, sizeof(frame));
	return ret;
}
EXPORT_SYMBOL_GPL(airoha_xgs_authenticate_disable_serial_number_words);

int airoha_xgs_authenticate_sleep_allow_words(
	const u32 words[AIROHA_XGS_PLOAM_FIFO_WORDS],
	const u8 key[AIROHA_XGS_KEY_SIZE], u16 onu_id,
	bool *sleep_type, u8 *sequence)
{
	u8 frame[AIROHA_XGS_PLOAM_FRAME_SIZE];
	u16 destination;
	int i, ret;

	if (!sleep_type || !sequence)
		return -EINVAL;
	*sleep_type = false;
	*sequence = 0;
	if (!words || !key || onu_id >= AIROHA_XGS_BROADCAST_XGS_ONU_ID)
		return -EINVAL;

	for (i = 0; i < AIROHA_XGS_PLOAM_FRAME_WORDS; i++)
		put_unaligned_be32(words[i], frame + i * sizeof(u32));
	ret = airoha_xgs_verify_ploam_mic(key, AIROHA_XGS_DOWNSTREAM,
					  frame,
					  frame + AIROHA_XGS_PLOAM_CONTENT_SIZE);
	if (ret)
		goto out;
	destination = get_unaligned_be16(frame);
	if (destination != onu_id &&
	    destination != AIROHA_XGS_BROADCAST_ONU_ID &&
	    destination != AIROHA_XGS_BROADCAST_XGS_ONU_ID) {
		ret = -EADDRNOTAVAIL;
		goto out;
	}
	if (frame[2] != AIROHA_XGS_SLEEP_ALLOW_MESSAGE_ID ||
	    (frame[AIROHA_XGS_SLEEP_ALLOW_TYPE_OFFSET] &
	     AIROHA_XGS_SLEEP_ALLOW_RESERVED)) {
		ret = -EPROTO;
		goto out;
	}
	*sleep_type = !!(frame[AIROHA_XGS_SLEEP_ALLOW_TYPE_OFFSET] &
			AIROHA_XGS_SLEEP_ALLOW_TYPE);
	*sequence = frame[3];
	ret = 0;
out:
	if (ret) {
		*sleep_type = false;
		*sequence = 0;
	}
	memzero_explicit(frame, sizeof(frame));
	return ret;
}
EXPORT_SYMBOL_GPL(airoha_xgs_authenticate_sleep_allow_words);

int airoha_xgs_authenticate_reboot_onu_words(
	const u32 words[AIROHA_XGS_PLOAM_FIFO_WORDS],
	const u8 key[AIROHA_XGS_KEY_SIZE], u16 onu_id,
	struct airoha_xgs_reboot_request *request)
{
	u8 frame[AIROHA_XGS_PLOAM_FRAME_SIZE];
	u16 destination;
	int i, ret;

	if (!request)
		return -EINVAL;
	memset(request, 0, sizeof(*request));
	if (!words || !key || onu_id >= AIROHA_XGS_BROADCAST_XGS_ONU_ID)
		return -EINVAL;
	for (i = 0; i < AIROHA_XGS_PLOAM_FRAME_WORDS; i++)
		put_unaligned_be32(words[i], frame + i * sizeof(u32));
	ret = airoha_xgs_verify_ploam_mic(key, AIROHA_XGS_DOWNSTREAM,
					  frame,
					  frame + AIROHA_XGS_PLOAM_CONTENT_SIZE);
	if (ret)
		goto out;
	destination = get_unaligned_be16(frame);
	if (destination != onu_id &&
	    destination != AIROHA_XGS_BROADCAST_ONU_ID &&
	    destination != AIROHA_XGS_BROADCAST_XGS_ONU_ID) {
		ret = -EADDRNOTAVAIL;
		goto out;
	}
	if (!airoha_xgs_parse_reboot_content(frame, request)) {
		ret = -EPROTO;
		goto out;
	}
	ret = 0;
out:
	if (ret)
		memset(request, 0, sizeof(*request));
	memzero_explicit(frame, sizeof(frame));
	return ret;
}
EXPORT_SYMBOL_GPL(airoha_xgs_authenticate_reboot_onu_words);

int airoha_xgs_authenticate_assign_alloc_id_words(
	const u32 words[AIROHA_XGS_PLOAM_FIFO_WORDS],
	const u8 key[AIROHA_XGS_KEY_SIZE], u16 onu_id,
	struct airoha_xgs_alloc_id_assignment *assignment)
{
	u8 frame[AIROHA_XGS_PLOAM_FRAME_SIZE];
	u16 destination;
	u8 operation;
	int i, ret;

	if (!assignment)
		return -EINVAL;
	memset(assignment, 0, sizeof(*assignment));
	if (!words || !key)
		return -EINVAL;
	if (onu_id >= AIROHA_XGS_BROADCAST_XGS_ONU_ID)
		return -ERANGE;

	for (i = 0; i < AIROHA_XGS_PLOAM_FRAME_WORDS; i++)
		put_unaligned_be32(words[i], frame + i * sizeof(u32));
	ret = airoha_xgs_verify_ploam_mic(key, AIROHA_XGS_DOWNSTREAM,
					  frame,
					  frame + AIROHA_XGS_PLOAM_CONTENT_SIZE);
	if (ret)
		goto out;

	destination = get_unaligned_be16(frame);
	if (destination != onu_id) {
		ret = -EADDRNOTAVAIL;
		goto out;
	}
	operation = frame[AIROHA_XGS_ALLOC_ID_TYPE_OFFSET];
	if (frame[2] != AIROHA_XGS_ASSIGN_ALLOC_ID_MESSAGE_ID ||
	    (operation != AIROHA_XGS_ALLOC_ID_ASSIGN &&
	     operation != AIROHA_XGS_ALLOC_ID_DEALLOCATE) ||
	    memchr_inv(frame + AIROHA_XGS_ALLOC_ID_SCOPE_OFFSET, 0,
		       AIROHA_XGS_ALLOC_ID_SCOPE_SIZE)) {
		ret = -EPROTO;
		goto out;
	}

	assignment->alloc_id =
		((u16)(frame[AIROHA_XGS_ALLOC_ID_OFFSET] &
		       ~AIROHA_XGS_ALLOC_ID_RESERVED) << 8) |
		frame[AIROHA_XGS_ALLOC_ID_OFFSET + 1];
	assignment->sequence = frame[3];
	assignment->operation = operation;
	ret = 0;
out:
	if (ret)
		memset(assignment, 0, sizeof(*assignment));
	memzero_explicit(frame, sizeof(frame));
	return ret;
}
EXPORT_SYMBOL_GPL(airoha_xgs_authenticate_assign_alloc_id_words);

int airoha_xgs_parse_key_control_words(
	const u32 words[AIROHA_XGS_PLOAM_FIFO_WORDS],
	const u8 key[AIROHA_XGS_KEY_SIZE], u16 onu_id,
	u8 *sequence, u8 *control, u8 *key_index)
{
	u8 frame[AIROHA_XGS_PLOAM_FRAME_SIZE];
	u16 destination;
	int i, ret;

	if (!sequence || !control || !key_index)
		return -EINVAL;
	*sequence = 0;
	*control = 0;
	*key_index = 0;
	if (!words || !key || onu_id >= AIROHA_XGS_BROADCAST_XGS_ONU_ID)
		return -EINVAL;

	for (i = 0; i < AIROHA_XGS_PLOAM_FRAME_WORDS; i++)
		put_unaligned_be32(words[i], frame + i * sizeof(u32));
	ret = airoha_xgs_verify_ploam_mic(key, AIROHA_XGS_DOWNSTREAM,
					  frame,
					  frame + AIROHA_XGS_PLOAM_CONTENT_SIZE);
	if (ret)
		goto out;
	destination = get_unaligned_be16(frame);
	if (destination != onu_id &&
	    destination != AIROHA_XGS_BROADCAST_ONU_ID &&
	    destination != AIROHA_XGS_BROADCAST_XGS_ONU_ID) {
		ret = -EADDRNOTAVAIL;
		goto out;
	}
	if (frame[2] != AIROHA_XGS_KEY_CONTROL_MESSAGE_ID ||
	    (frame[AIROHA_XGS_KEY_CONTROL_CONTROL_OFFSET] &
	     AIROHA_XGS_KEY_CONTROL_FIELD_RESERVED) ||
	    (frame[AIROHA_XGS_KEY_CONTROL_INDEX_OFFSET] &
	     AIROHA_XGS_KEY_CONTROL_INDEX_RESERVED) ||
	    frame[AIROHA_XGS_KEY_CONTROL_LENGTH_OFFSET] !=
		AIROHA_XGS_KEY_CONTROL_128_BIT_LENGTH) {
		ret = -EPROTO;
		goto out;
	}
	*control = frame[AIROHA_XGS_KEY_CONTROL_CONTROL_OFFSET] & BIT(0);
	*key_index = frame[AIROHA_XGS_KEY_CONTROL_INDEX_OFFSET] & GENMASK(1, 0);
	if (*key_index != 1 && *key_index != 2) {
		ret = -ERANGE;
		goto out;
	}
	*sequence = frame[3];
	ret = 0;
out:
	if (ret) {
		*sequence = 0;
		*control = 0;
		*key_index = 0;
	}
	memzero_explicit(frame, sizeof(frame));
	return ret;
}
EXPORT_SYMBOL_GPL(airoha_xgs_parse_key_control_words);

int airoha_xgs_build_serial_number_ploam(
	const u8 serial[AIROHA_XGS_SERIAL_SIZE], u32 random_delay,
	enum airoha_xpon_mode mode,
	u8 frame[AIROHA_XGS_PLOAM_FRAME_SIZE])
{
	u8 upstream_line_rate_cap;
	int ret;

	if (!frame)
		return -EINVAL;
	memset(frame, 0, AIROHA_XGS_PLOAM_FRAME_SIZE);
	if (!serial)
		return -EINVAL;
	switch (mode) {
	case AIROHA_XPON_MODE_XGPON:
		upstream_line_rate_cap = AIROHA_XGPON_UPSTREAM_LINE_RATE_CAP;
		break;
	case AIROHA_XPON_MODE_XGSPON:
		upstream_line_rate_cap = AIROHA_XGSPON_UPSTREAM_LINE_RATE_CAP;
		break;
	default:
		return -EINVAL;
	}
	put_unaligned_be16(AIROHA_XGS_BROADCAST_ONU_ID, frame);
	frame[2] = AIROHA_XGS_SERIAL_NUMBER_MESSAGE_ID;
	memcpy(frame + 4, serial, AIROHA_XGS_SERIAL_SIZE);
	put_unaligned_be32(random_delay, frame + 12);
	frame[32] = upstream_line_rate_cap;
	ret = airoha_xgs_ploam_mic(airoha_xgs_default_ploam_key,
				    AIROHA_XGS_UPSTREAM, frame,
				    frame + AIROHA_XGS_PLOAM_CONTENT_SIZE);
	if (ret)
		memzero_explicit(frame, AIROHA_XGS_PLOAM_FRAME_SIZE);

	return ret;
}
EXPORT_SYMBOL_GPL(airoha_xgs_build_serial_number_ploam);

int airoha_xgs_build_registration_ploam(
	const u8 key[AIROHA_XGS_KEY_SIZE], u16 onu_id, u8 sequence,
	const u8 registration_id[AIROHA_XGS_REGISTRATION_ID_SIZE],
	u8 frame[AIROHA_XGS_PLOAM_FRAME_SIZE])
{
	int ret;

	if (!frame)
		return -EINVAL;
	memset(frame, 0, AIROHA_XGS_PLOAM_FRAME_SIZE);
	if (!key || !registration_id)
		return -EINVAL;
	if (onu_id >= AIROHA_XGS_BROADCAST_XGS_ONU_ID)
		return -ERANGE;
	put_unaligned_be16(onu_id, frame);
	frame[2] = AIROHA_XGS_REGISTRATION_MESSAGE_ID;
	frame[3] = sequence;
	memcpy(frame + 4, registration_id,
	       AIROHA_XGS_REGISTRATION_ID_SIZE);
	ret = airoha_xgs_ploam_mic(key, AIROHA_XGS_UPSTREAM, frame,
				    frame + AIROHA_XGS_PLOAM_CONTENT_SIZE);
	if (ret)
		memzero_explicit(frame, AIROHA_XGS_PLOAM_FRAME_SIZE);

	return ret;
}
EXPORT_SYMBOL_GPL(airoha_xgs_build_registration_ploam);

int airoha_xgs_build_acknowledge_ploam(
	const u8 key[AIROHA_XGS_KEY_SIZE], u16 onu_id, u8 sequence,
	u8 completion_code, u8 frame[AIROHA_XGS_PLOAM_FRAME_SIZE])
{
	int ret;

	if (!frame)
		return -EINVAL;
	memset(frame, 0, AIROHA_XGS_PLOAM_FRAME_SIZE);
	if (!key)
		return -EINVAL;
	if (onu_id >= AIROHA_XGS_BROADCAST_XGS_ONU_ID)
		return -ERANGE;
	if (completion_code > AIROHA_XGS_ACK_COMPLETION_PROCESS_ERROR)
		return -EINVAL;

	put_unaligned_be16(onu_id, frame);
	frame[2] = AIROHA_XGS_ACKNOWLEDGE_MESSAGE_ID;
	frame[3] = sequence;
	frame[4] = completion_code;
	ret = airoha_xgs_ploam_mic(key, AIROHA_XGS_UPSTREAM, frame,
				    frame + AIROHA_XGS_PLOAM_CONTENT_SIZE);
	if (ret)
		memzero_explicit(frame, AIROHA_XGS_PLOAM_FRAME_SIZE);

	return ret;
}
EXPORT_SYMBOL_GPL(airoha_xgs_build_acknowledge_ploam);

int airoha_xgs_build_key_report(
	const u8 key[AIROHA_XGS_KEY_SIZE], u16 onu_id, u8 sequence,
	u8 report_type, u8 key_index,
	const u8 fragment[AIROHA_XGS_KEY_REPORT_FRAGMENT_SIZE],
	u8 frame[AIROHA_XGS_PLOAM_FRAME_SIZE])
{
	int ret;

	if (!frame)
		return -EINVAL;
	memset(frame, 0, AIROHA_XGS_PLOAM_FRAME_SIZE);
	if (!key || !fragment || onu_id >= AIROHA_XGS_BROADCAST_XGS_ONU_ID ||
	    report_type > AIROHA_XGS_KEY_REPORT_CONFIRM ||
	    (key_index != 1 && key_index != 2))
		return -EINVAL;

	put_unaligned_be16(onu_id, frame);
	frame[2] = AIROHA_XGS_KEY_REPORT_MESSAGE_ID;
	frame[3] = sequence;
	frame[4] = report_type;
	frame[5] = key_index;
	/* Fragment number zero and the reserved byte are already zero. */
	memcpy(frame + 8, fragment, AIROHA_XGS_KEY_REPORT_FRAGMENT_SIZE);
	ret = airoha_xgs_ploam_mic(key, AIROHA_XGS_UPSTREAM, frame,
				    frame + AIROHA_XGS_PLOAM_CONTENT_SIZE);
	if (ret)
		memzero_explicit(frame, AIROHA_XGS_PLOAM_FRAME_SIZE);
	return ret;
}
EXPORT_SYMBOL_GPL(airoha_xgs_build_key_report);

int airoha_xgs_encode_upstream_ploam_fifo(
	const u8 content[AIROHA_XGS_PLOAM_CONTENT_SIZE], u8 key_index,
	u32 words[AIROHA_XGS_PLOAM_UPSTREAM_FIFO_WORDS])
{
	int i;

	if (!words)
		return -EINVAL;
	memset(words, 0,
	       sizeof(*words) * AIROHA_XGS_PLOAM_UPSTREAM_FIFO_WORDS);
	if (!content || key_index > AIROHA_XGS_PLOAM_KEY_INDEX_1)
		return -EINVAL;

	/* SDK word 0 is three reserved bytes followed by the one-bit key index. */
	words[0] = key_index;
	for (i = 0; i < AIROHA_XGS_PLOAM_CONTENT_SIZE / sizeof(u32); i++)
		words[i + 1] = get_unaligned_be32(content + i * sizeof(u32));

	return 0;
}
EXPORT_SYMBOL_GPL(airoha_xgs_encode_upstream_ploam_fifo);

int airoha_xgs_omci_mic(
	const u8 key[AIROHA_XGS_KEY_SIZE], enum airoha_xgs_direction direction,
	const u8 *content, size_t content_len,
	u8 mic[AIROHA_XGS_OMCI_MIC_SIZE])
{
	if (content_len > AIROHA_XGS_OMCI_MAX_CONTENT_SIZE) {
		if (mic)
			memzero_explicit(mic, AIROHA_XGS_OMCI_MIC_SIZE);
		return -EMSGSIZE;
	}

	return airoha_xgs_directional_cmac(key, direction, content, content_len,
					   mic, AIROHA_XGS_OMCI_MIC_SIZE);
}
EXPORT_SYMBOL_GPL(airoha_xgs_omci_mic);

int airoha_xgs_verify_omci_mic(
	const u8 key[AIROHA_XGS_KEY_SIZE], enum airoha_xgs_direction direction,
	const u8 *content, size_t content_len,
	const u8 mic[AIROHA_XGS_OMCI_MIC_SIZE])
{
	u8 expected[AIROHA_XGS_OMCI_MIC_SIZE];
	int ret;

	if (!mic)
		return -EINVAL;
	ret = airoha_xgs_omci_mic(key, direction, content, content_len,
				   expected);
	if (!ret && crypto_memneq(expected, mic, sizeof(expected)))
		ret = -EBADMSG;
	memzero_explicit(expected, sizeof(expected));
	return ret;
}
EXPORT_SYMBOL_GPL(airoha_xgs_verify_omci_mic);

int airoha_xgs_omci_content_length(const u8 *wire, size_t wire_len,
				    size_t *content_len)
{
	size_t expected_len;
	u16 payload_len;

	if (!content_len)
		return -EINVAL;
	*content_len = 0;
	if (!wire || wire_len < 4 || wire_len > AIROHA_XGS_OMCI_MAX_WIRE_SIZE)
		return -EINVAL;

	switch (wire[3]) {
	case AIROHA_XGS_OMCI_BASELINE_DEVICE_ID:
		expected_len = AIROHA_XGS_OMCI_BASELINE_CONTENT_SIZE;
		if (wire_len < expected_len)
			return -EMSGSIZE;
		if (get_unaligned_be32(wire +
				       AIROHA_XGS_OMCI_BASELINE_LENGTH_OFFSET) !=
		    AIROHA_XGS_OMCI_BASELINE_LENGTH)
			return -EPROTO;
		break;
	case AIROHA_XGS_OMCI_EXTENDED_DEVICE_ID:
		if (wire_len < AIROHA_XGS_OMCI_EXTENDED_HEADER_SIZE)
			return -EMSGSIZE;
		payload_len = get_unaligned_be16(
			wire + AIROHA_XGS_OMCI_EXTENDED_LENGTH_OFFSET);
		expected_len = AIROHA_XGS_OMCI_EXTENDED_HEADER_SIZE +
			       payload_len;
		if (expected_len > AIROHA_XGS_OMCI_MAX_WIRE_CONTENT_SIZE)
			return -EMSGSIZE;
		if (wire_len < expected_len)
			return -EMSGSIZE;
		break;
	default:
		return -EPROTONOSUPPORT;
	}

	if (wire_len != expected_len &&
	    wire_len != expected_len + AIROHA_XGS_OMCI_MIC_SIZE)
		return -EPROTO;
	*content_len = expected_len;
	return 0;
}
EXPORT_SYMBOL_GPL(airoha_xgs_omci_content_length);

static int airoha_xgs_validate_omci_content(const u8 *content,
					    size_t content_len)
{
	size_t expected_len;
	int ret;

	if (content_len > AIROHA_XGS_OMCI_MAX_WIRE_CONTENT_SIZE)
		return -EMSGSIZE;
	ret = airoha_xgs_omci_content_length(content, content_len,
					     &expected_len);
	return ret ?: (expected_len == content_len ? 0 : -EPROTO);
}

int airoha_xgs_authenticate_downstream_omci(
	const u8 key[AIROHA_XGS_KEY_SIZE], const u8 *wire, size_t wire_len,
	u8 *content, size_t content_capacity, size_t *content_len)
{
	size_t authenticated_len;
	int ret;

	if (!content_len)
		return -EINVAL;
	*content_len = 0;
	if (!key || !wire || !content ||
	    wire_len < AIROHA_XGS_OMCI_MIC_SIZE ||
	    wire_len > AIROHA_XGS_OMCI_MAX_WIRE_SIZE) {
		ret = -EINVAL;
		goto clear;
	}
	authenticated_len = wire_len - AIROHA_XGS_OMCI_MIC_SIZE;
	if (content_capacity < authenticated_len) {
		ret = -ENOSPC;
		goto clear;
	}
	ret = airoha_xgs_validate_omci_content(wire, authenticated_len);
	if (ret)
		goto clear;
	ret = airoha_xgs_verify_omci_mic(
		key, AIROHA_XGS_DOWNSTREAM, wire, authenticated_len,
		wire + authenticated_len);
	if (ret)
		goto clear;

	memmove(content, wire, authenticated_len);
	*content_len = authenticated_len;
	return 0;

clear:
	if (content && content_capacity)
		memzero_explicit(content, content_capacity);
	return ret;
}
EXPORT_SYMBOL_GPL(airoha_xgs_authenticate_downstream_omci);

int airoha_xgs_sign_upstream_omci(
	const u8 key[AIROHA_XGS_KEY_SIZE], const u8 *content,
	size_t content_len, u8 *wire, size_t wire_capacity,
	size_t *wire_len)
{
	u8 mic[AIROHA_XGS_OMCI_MIC_SIZE];
	size_t signed_len;
	int ret;

	if (!wire_len)
		return -EINVAL;
	*wire_len = 0;
	if (!key || !content || !wire) {
		ret = -EINVAL;
		goto clear;
	}
	ret = airoha_xgs_validate_omci_content(content, content_len);
	if (ret)
		goto clear;
	signed_len = content_len + AIROHA_XGS_OMCI_MIC_SIZE;
	if (signed_len > AIROHA_XGS_OMCI_MAX_WIRE_SIZE ||
	    wire_capacity < signed_len) {
		ret = -ENOSPC;
		goto clear;
	}
	ret = airoha_xgs_omci_mic(key, AIROHA_XGS_UPSTREAM,
				   content, content_len, mic);
	if (ret)
		goto clear;

	memmove(wire, content, content_len);
	memcpy(wire + content_len, mic, sizeof(mic));
	*wire_len = signed_len;
	memzero_explicit(mic, sizeof(mic));
	return 0;

clear:
	memzero_explicit(mic, sizeof(mic));
	if (wire && wire_capacity)
		memzero_explicit(wire, wire_capacity);
	return ret;
}
EXPORT_SYMBOL_GPL(airoha_xgs_sign_upstream_omci);

static int __init airoha_xgs_security_init(void)
{
	static const u8 registration_id[AIROHA_XGS_REGISTRATION_ID_SIZE];
	static const u8 expected_registration_msk[AIROHA_XGS_KEY_SIZE] = {
		0x24, 0x37, 0xbe, 0x54, 0xe9, 0x5e, 0x6e, 0xe3,
		0x53, 0x8b, 0xb1, 0xb4, 0xb5, 0xd4, 0x32, 0xeb,
	};
	static const u8 msk[AIROHA_XGS_KEY_SIZE] = {
		0x11, 0x22, 0x33, 0x44, 0x55, 0x66, 0x77, 0x88,
		0x99, 0xaa, 0xbb, 0xcc, 0xdd, 0xee, 0xff, 0x00,
	};
	static const u8 serial[AIROHA_XGS_SERIAL_SIZE] = {
		0x56, 0x4e, 0x44, 0x52, 0x00, 0x11, 0x22, 0x33,
	};
	static const u8 pon_tag[AIROHA_XGS_PON_TAG_SIZE] = {
		0x4f, 0x4c, 0x54, 0x23, 0x44, 0x55, 0x66, 0x77,
	};
	static const struct airoha_xgs_security_keys expected = {
		.msk = { 0x11, 0x22, 0x33, 0x44, 0x55, 0x66, 0x77, 0x88,
			 0x99, 0xaa, 0xbb, 0xcc, 0xdd, 0xee, 0xff, 0x00 },
		.session = { 0x79, 0x5f, 0xcf, 0x6c, 0xb2, 0x15, 0x22, 0x40,
			     0x87, 0x43, 0x06, 0x00, 0xdd, 0x17, 0x0f, 0x07 },
		.omci = { 0x18, 0x4b, 0x8a, 0xd4, 0xd1, 0xac, 0x4a, 0xf4,
			  0xdd, 0x4b, 0x33, 0x9e, 0xcc, 0x0d, 0x33, 0x70 },
		.ploam = { 0xe2, 0x56, 0xce, 0x76, 0x78, 0x5c, 0x78, 0x71,
			   0x7c, 0x7b, 0x30, 0x44, 0xab, 0x28, 0xe2, 0xcd },
		.kek = { 0x6f, 0x9c, 0x99, 0xb8, 0x36, 0x17, 0x68, 0x93,
			 0x7e, 0x45, 0x3b, 0x16, 0x5f, 0x60, 0x97, 0x10 },
	};
	static const u8 downstream_ploam[AIROHA_XGS_PLOAM_CONTENT_SIZE] = {
		0x00, 0x13, 0x0a, 0x03, 0x04, 0x45, 0x01,
	};
	static const u8 expected_downstream_ploam_mic[AIROHA_XGS_PLOAM_MIC_SIZE] = {
		0x46, 0x39, 0x87, 0x56, 0x28, 0x08, 0x14, 0xe6,
	};
	static const u8 upstream_ploam[AIROHA_XGS_PLOAM_CONTENT_SIZE] = {
		0x00, 0x13, 0x10, 0x00, 0x03,
	};
	static const u8 expected_upstream_ploam_mic[AIROHA_XGS_PLOAM_MIC_SIZE] = {
		0xfe, 0xaf, 0x8d, 0x09, 0x20, 0x8f, 0x0d, 0x9b,
	};
	static const u8 downstream_omci[] = {
		0x80, 0x00, 0x49, 0x0a, 0x01, 0x00, 0x00, 0x00,
		0x00, 0x80, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
		0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
		0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
		0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
		0x00, 0x00, 0x00, 0x28,
	};
	static const u8 expected_downstream_omci_mic[AIROHA_XGS_OMCI_MIC_SIZE] = {
		0x78, 0xdc, 0xa5, 0x3d,
	};
	static const u8 expected_upstream_omci_mic[AIROHA_XGS_OMCI_MIC_SIZE] = {
		0x68, 0x2f, 0x5c, 0x73,
	};
	static const u8 extended_omci[] = {
		0x80, 0x01, 0x49, 0x0b, 0x00, 0x00, 0x00, 0x00,
		0x00, 0x05, 0x01, 0x02, 0x03, 0x04, 0x05,
	};
	static const u8 expected_downstream_extended_omci_mic[
		AIROHA_XGS_OMCI_MIC_SIZE] = { 0x6e, 0xa5, 0x9c, 0xee };
	static const u8 expected_upstream_extended_omci_mic[
		AIROHA_XGS_OMCI_MIC_SIZE] = { 0x7b, 0x7b, 0x7c, 0x8f };
	static const u8 initial_profile[AIROHA_XGS_PLOAM_FRAME_SIZE] = {
		0x03, 0xfe, 0x01, 0x01, 0x04,
		[AIROHA_XGS_PROFILE_PON_TAG_OFFSET] = 0x4f,
		0x4c, 0x54, 0x23, 0x44, 0x55, 0x66, 0x77,
		[AIROHA_XGS_PLOAM_CONTENT_SIZE] = 0xc9,
		0xb5, 0x16, 0x8d, 0xe6, 0xe8, 0xc2, 0x6e,
	};
	static const u32 initial_profile_words[AIROHA_XGS_PLOAM_FIFO_WORDS] = {
		0x03fe0101, 0x04000000, 0x00000000, 0x00000000,
		0x00000000, 0x00000000, 0x004f4c54, 0x23445566,
		0x77000000, 0x00000000, 0xc9b5168d, 0xe6e8c26e,
		0xa5a5a5a5,
	};
	static const u32 assign_onu_id_words[AIROHA_XGS_PLOAM_FIFO_WORDS] = {
		0x03fe0322, 0x0123564e, 0x44520011, 0x22330000,
		0x00000000, 0x00000000, 0x00000000, 0x00000000,
		0x00000000, 0x00000000, 0x2dedc831, 0x8aff6976,
		0x5a5a5a5a,
	};
	static const u32 request_registration_words[
		AIROHA_XGS_PLOAM_FIFO_WORDS] = {
		0x0123095a, 0x00000000, 0x00000000, 0x00000000,
		0x00000000, 0x00000000, 0x00000000, 0x00000000,
		0x00000000, 0x00000000, 0x4b405a9f, 0xce554c85,
		0xa5a5a5a5,
	};
	static const u32 ranging_time_words[AIROHA_XGS_PLOAM_FIFO_WORDS] = {
		0x01230433, 0x01001234, 0x56000000, 0x00000000,
		0x00000000, 0x00000000, 0x00000000, 0x00000000,
		0x00000000, 0x00000000, 0x74958638, 0x1f964887,
		0xa5a5a5a5,
	};
	static const u32 deactivate_onu_id_words[
		AIROHA_XGS_PLOAM_FIFO_WORDS] = {
		0x01230566, 0x00000000, 0x00000000, 0x00000000,
		0x00000000, 0x00000000, 0x00000000, 0x00000000,
		0x00000000, 0x00000000, 0xbe0f58a3, 0x44cb3b15,
		0xa5a5a5a5,
	};
	static const u32 assign_alloc_id_words[
		AIROHA_XGS_PLOAM_FIFO_WORDS] = {
		0x01230a44, 0x04560100, 0x00000000, 0x00000000,
		0x00000000, 0x00000000, 0x00000000, 0x00000000,
		0x00000000, 0x00000000, 0x79dd442d, 0x40ea1ae5,
		0x5a5a5a5a,
	};
	static const u32 key_control_words[AIROHA_XGS_PLOAM_FIFO_WORDS] = {
		0x01230d77, 0x00000110, 0x00000000, 0x00000000,
		0x00000000, 0x00000000, 0x00000000, 0x00000000,
		0x00000000, 0x00000000, 0x32c07d56, 0x05057bc3,
		0xa5a5a5a5,
	};
	static const u8 data_key[AIROHA_XGS_KEY_SIZE] = {
		0x00, 0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07,
		0x08, 0x09, 0x0a, 0x0b, 0x0c, 0x0d, 0x0e, 0x0f,
	};
	static const u8 expected_wrapped_key[AIROHA_XGS_KEY_SIZE] = {
		0x28, 0xc6, 0xa5, 0x26, 0x43, 0x52, 0xdb, 0x66,
		0x80, 0x65, 0x69, 0x50, 0xd1, 0x2d, 0x3a, 0x30,
	};
	static const u8 expected_key_proof[AIROHA_XGS_KEY_SIZE] = {
		0x39, 0x97, 0xda, 0x87, 0x65, 0xbe, 0x74, 0x9e,
		0xb5, 0xd3, 0xc6, 0x68, 0xf0, 0x4d, 0xb5, 0x72,
	};
	static const u8 expected_key_report[AIROHA_XGS_PLOAM_FRAME_SIZE] = {
		0x01, 0x23, 0x05, 0x55, 0x00, 0x01, 0x00, 0x00,
		0x28, 0xc6, 0xa5, 0x26, 0x43, 0x52, 0xdb, 0x66,
		0x80, 0x65, 0x69, 0x50, 0xd1, 0x2d, 0x3a, 0x30,
		[AIROHA_XGS_PLOAM_CONTENT_SIZE] = 0xde, 0xd3, 0x5a, 0xc1,
		0xdf, 0x69, 0xcc, 0xb8,
	};
	static const u32 expected_key_report_fifo[
		AIROHA_XGS_PLOAM_UPSTREAM_FIFO_WORDS] = {
		0x00000000, 0x01230555, 0x00010000, 0x28c6a526,
		0x4352db66, 0x80656950, 0xd12d3a30, 0x00000000,
		0x00000000, 0x00000000, 0x00000000,
	};
	static const u8 message_registration_id[AIROHA_XGS_REGISTRATION_ID_SIZE] = {
		0x00, 0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07,
		0x08, 0x09, 0x0a, 0x0b, 0x0c, 0x0d, 0x0e, 0x0f,
		0x10, 0x11, 0x12, 0x13, 0x14, 0x15, 0x16, 0x17,
		0x18, 0x19, 0x1a, 0x1b, 0x1c, 0x1d, 0x1e, 0x1f,
		0x20, 0x21, 0x22, 0x23,
	};
	static const u8 expected_xgpon_serial_number_ploam[
		AIROHA_XGS_PLOAM_FRAME_SIZE] = {
		0x03, 0xff, 0x01, 0x00, 0x56, 0x4e, 0x44, 0x52,
		0x00, 0x11, 0x22, 0x33, 0x12, 0x34, 0x56, 0x78,
		[AIROHA_XGS_PLOAM_CONTENT_SIZE] = 0xdb, 0x79, 0xf1, 0xb4,
		0x44, 0x98, 0xaf, 0x26,
	};
	static const u8 expected_xgspon_serial_number_ploam[
		AIROHA_XGS_PLOAM_FRAME_SIZE] = {
		0x03, 0xff, 0x01, 0x00, 0x56, 0x4e, 0x44, 0x52,
		0x00, 0x11, 0x22, 0x33, 0x12, 0x34, 0x56, 0x78,
		[32] = 0x03,
		[AIROHA_XGS_PLOAM_CONTENT_SIZE] = 0x90, 0x58, 0x6c, 0xd0,
		0x34, 0x1b, 0xd3, 0x25,
	};
	static const u8 expected_registration_ploam[AIROHA_XGS_PLOAM_FRAME_SIZE] = {
		0x01, 0x23, 0x02, 0x22,
		0x00, 0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07,
		0x08, 0x09, 0x0a, 0x0b, 0x0c, 0x0d, 0x0e, 0x0f,
		0x10, 0x11, 0x12, 0x13, 0x14, 0x15, 0x16, 0x17,
		0x18, 0x19, 0x1a, 0x1b, 0x1c, 0x1d, 0x1e, 0x1f,
		0x20, 0x21, 0x22, 0x23,
		0xea, 0x91, 0x1f, 0xf7, 0x43, 0x23, 0x70, 0x25,
	};
	static const u32 expected_xgpon_serial_number_fifo[
		AIROHA_XGS_PLOAM_UPSTREAM_FIFO_WORDS] = {
		0x00000001, 0x03ff0100, 0x564e4452, 0x00112233,
		0x12345678, 0x00000000, 0x00000000, 0x00000000,
		0x00000000, 0x00000000, 0x00000000,
	};
	static const u32 expected_xgspon_serial_number_fifo[
		AIROHA_XGS_PLOAM_UPSTREAM_FIFO_WORDS] = {
		0x00000001, 0x03ff0100, 0x564e4452, 0x00112233,
		0x12345678, 0x00000000, 0x00000000, 0x00000000,
		0x00000000, 0x03000000, 0x00000000,
	};
	static const enum airoha_xpon_mode invalid_serial_number_modes[] = {
		AIROHA_XPON_MODE_GPON,
		AIROHA_XPON_MODE_EPON_10G_1G,
		AIROHA_XPON_MODE_EPON_10G_10G,
		AIROHA_XPON_MODE_INVALID,
	};
	static const u32 expected_registration_fifo[
		AIROHA_XGS_PLOAM_UPSTREAM_FIFO_WORDS] = {
		0x00000000, 0x01230222, 0x00010203, 0x04050607,
		0x08090a0b, 0x0c0d0e0f, 0x10111213, 0x14151617,
		0x18191a1b, 0x1c1d1e1f, 0x20212223,
	};
	static const u8 expected_acknowledge_ploam[
		AIROHA_XGS_PLOAM_FRAME_SIZE] = {
		0x01, 0x23, 0x09, 0x44, 0x00,
		[AIROHA_XGS_PLOAM_CONTENT_SIZE] = 0x95, 0xc5, 0x75, 0x37,
		0x0f, 0x05, 0x0b, 0xa9,
	};
	static const u32 expected_acknowledge_fifo[
		AIROHA_XGS_PLOAM_UPSTREAM_FIFO_WORDS] = {
		0x00000000, 0x01230944, 0x00000000, 0x00000000,
		0x00000000, 0x00000000, 0x00000000, 0x00000000,
		0x00000000, 0x00000000, 0x00000000,
	};
	struct airoha_xgs_security_keys keys;
	u8 registration_msk[AIROHA_XGS_KEY_SIZE];
	u8 ploam_mic[AIROHA_XGS_PLOAM_MIC_SIZE];
	u8 omci_mic[AIROHA_XGS_OMCI_MIC_SIZE];
	u8 downstream_omci_wire[AIROHA_XGS_OMCI_BASELINE_WIRE_SIZE];
	u8 authenticated_omci[AIROHA_XGS_OMCI_BASELINE_CONTENT_SIZE];
	u8 signed_omci[AIROHA_XGS_OMCI_BASELINE_WIRE_SIZE];
	u8 malformed_omci[AIROHA_XGS_OMCI_BASELINE_CONTENT_SIZE];
	u8 extended_omci_wire[sizeof(extended_omci) + AIROHA_XGS_OMCI_MIC_SIZE];
	u8 authenticated_pon_tag[AIROHA_XGS_PON_TAG_SIZE];
	u8 altered_profile[AIROHA_XGS_PLOAM_FRAME_SIZE];
	u32 profile_words[AIROHA_XGS_PLOAM_FIFO_WORDS];
	u32 altered_assign_onu_id[AIROHA_XGS_PLOAM_FIFO_WORDS];
	u32 altered_request_registration[AIROHA_XGS_PLOAM_FIFO_WORDS];
	u8 altered_request_registration_frame[AIROHA_XGS_PLOAM_FRAME_SIZE];
	u32 altered_control_ploam[AIROHA_XGS_PLOAM_FIFO_WORDS];
	u8 altered_control_frame[AIROHA_XGS_PLOAM_FRAME_SIZE];
	u8 default_ploam[AIROHA_XGS_KEY_SIZE];
	u8 upstream_frame[AIROHA_XGS_PLOAM_FRAME_SIZE];
	u32 upstream_fifo[AIROHA_XGS_PLOAM_UPSTREAM_FIFO_WORDS];
	struct airoha_xgs_ranging_time ranging;
	struct airoha_xgs_profile profile;
	struct airoha_xgs_alloc_id_assignment assignment;
	enum airoha_xgs_disable_serial_action serial_action;
	u16 assigned_onu_id;
	u8 assign_sequence;
	u8 request_sequence;
	u8 deactivate_sequence;
	u8 control_sequence;
	bool sleep_type;
	u8 key_control_sequence, key_control, key_control_index;
	u8 wrapped_key[AIROHA_XGS_KEY_SIZE];
	u8 key_proof[AIROHA_XGS_KEY_SIZE];
	size_t authenticated_omci_len;
	size_t signed_omci_len;
	int i, ret;

	ret = airoha_xgs_derive_registration_msk(registration_id,
						 registration_msk);
	if (ret || crypto_memneq(registration_msk, expected_registration_msk,
				 sizeof(registration_msk))) {
		ret = ret ?: -EBADMSG;
		goto out;
	}
	ret = airoha_xgs_derive_shared_keys(msk, serial, pon_tag, &keys);
	if (!ret && crypto_memneq(&keys, &expected, sizeof(keys)))
		ret = -EBADMSG;
	if (ret)
		goto out;

	ret = airoha_xgs_ploam_mic(keys.ploam, AIROHA_XGS_DOWNSTREAM,
				    downstream_ploam, ploam_mic);
	if (!ret && crypto_memneq(ploam_mic, expected_downstream_ploam_mic,
				  sizeof(ploam_mic)))
		ret = -EBADMSG;
	if (ret)
		goto out;
	ret = airoha_xgs_ploam_mic(keys.ploam, AIROHA_XGS_UPSTREAM,
				    upstream_ploam, ploam_mic);
	if (!ret && crypto_memneq(ploam_mic, expected_upstream_ploam_mic,
				  sizeof(ploam_mic)))
		ret = -EBADMSG;
	if (ret)
		goto out;
	ret = airoha_xgs_authenticate_initial_profile(initial_profile,
						      authenticated_pon_tag);
	if (!ret && crypto_memneq(authenticated_pon_tag, pon_tag,
				  sizeof(authenticated_pon_tag)))
		ret = -EBADMSG;
	if (ret)
		goto out;
	ret = airoha_xgs_authenticate_initial_profile_words(
		initial_profile_words, authenticated_pon_tag);
	if (!ret && crypto_memneq(authenticated_pon_tag, pon_tag,
				  sizeof(authenticated_pon_tag)))
		ret = -EBADMSG;
	if (ret)
		goto out;
	memcpy(altered_profile, initial_profile, sizeof(altered_profile));
	altered_profile[0] = 0x03;
	altered_profile[1] = 0xff;
	altered_profile[4] = 0xa2;
	altered_profile[5] = AIROHA_XGS_PROFILE_FEC;
	altered_profile[6] = 5;
	for (i = 0; i < AIROHA_XGS_PROFILE_PATTERN_SIZE; i++) {
		altered_profile[AIROHA_XGS_PROFILE_DELIMITER_OFFSET + i] =
			0x10 + i;
		altered_profile[AIROHA_XGS_PROFILE_PREAMBLE_OFFSET + i] =
			0x20 + i;
	}
	altered_profile[AIROHA_XGS_PROFILE_PREAMBLE_LENGTH_OFFSET] = 6;
	altered_profile[AIROHA_XGS_PROFILE_PREAMBLE_REPEAT_OFFSET] = 0xff;
	ret = airoha_xgs_ploam_mic(
		airoha_xgs_default_ploam_key, AIROHA_XGS_DOWNSTREAM,
		altered_profile,
		altered_profile + AIROHA_XGS_PLOAM_CONTENT_SIZE);
	if (ret)
		goto out;
	for (i = 0; i < AIROHA_XGS_PLOAM_FRAME_WORDS; i++)
		profile_words[i] = get_unaligned_be32(
			altered_profile + i * sizeof(u32));
	profile_words[AIROHA_XGS_PLOAM_FIFO_WORDS - 1] = 0xa5a5a5a5;
	ret = airoha_xgs_authenticate_profile_words_mode(
		profile_words, false, &profile);
	if (!ret && (profile.destination != AIROHA_XGS_BROADCAST_ONU_ID ||
		     profile.sequence != 1 || profile.index != 2 ||
		     profile.version != 0xa ||
		     !profile.fec || profile.delimiter_length != 5 ||
		     profile.preamble_length != 6 ||
		     profile.preamble_repeat_count != 0x1f ||
		     crypto_memneq(profile.delimiter,
			altered_profile + AIROHA_XGS_PROFILE_DELIMITER_OFFSET,
			sizeof(profile.delimiter)) ||
		     crypto_memneq(profile.preamble,
			altered_profile + AIROHA_XGS_PROFILE_PREAMBLE_OFFSET,
			sizeof(profile.preamble)) ||
		     crypto_memneq(profile.pon_tag, pon_tag,
				sizeof(profile.pon_tag))))
		ret = -EBADMSG;
	if (ret)
		goto out;
	altered_profile[1] = 0xfe;
	ret = airoha_xgs_authenticate_initial_profile_mode(
		altered_profile, false, authenticated_pon_tag);
	if (ret != -EADDRNOTAVAIL) {
		ret = ret ?: -EBADMSG;
		goto out;
	}

	/* Operational Profile updates are authenticated with the caller-selected
	 * current PLOAM key and preserve the address/sequence needed for ACK. */
	memcpy(altered_profile, initial_profile, sizeof(altered_profile));
	put_unaligned_be16(0x0123, altered_profile);
	altered_profile[3] = 0x6a;
	ret = airoha_xgs_ploam_mic(
		keys.ploam, AIROHA_XGS_DOWNSTREAM, altered_profile,
		altered_profile + AIROHA_XGS_PLOAM_CONTENT_SIZE);
	if (ret)
		goto out;
	for (i = 0; i < AIROHA_XGS_PLOAM_FRAME_WORDS; i++)
		profile_words[i] = get_unaligned_be32(
			altered_profile + i * sizeof(u32));
	profile_words[AIROHA_XGS_PLOAM_FIFO_WORDS - 1] = 0x5a5a5a5a;
	ret = airoha_xgs_authenticate_profile_words_mode_key(
		profile_words, keys.ploam, true, &profile);
	if (ret || profile.destination != 0x0123 || profile.sequence != 0x6a ||
	    crypto_memneq(profile.pon_tag, pon_tag, sizeof(profile.pon_tag))) {
		ret = ret ?: -EBADMSG;
		goto out;
	}
	ret = airoha_xgs_authenticate_profile_words_mode(
		profile_words, true, &profile);
	if (ret != -EADDRNOTAVAIL ||
	    memchr_inv(&profile, 0, sizeof(profile))) {
		ret = ret ?: -EBADMSG;
		goto out;
	}
	airoha_xgs_copy_default_ploam_key(default_ploam);
	ret = airoha_xgs_authenticate_profile_words_mode_key(
		profile_words, default_ploam, true, &profile);
	if (ret != -EBADMSG || memchr_inv(&profile, 0, sizeof(profile))) {
		ret = ret ?: -EBADMSG;
		goto out;
	}
	ret = 0;
	ret = airoha_xgs_authenticate_assign_onu_id_words(
		assign_onu_id_words, default_ploam, serial, &assigned_onu_id,
		&assign_sequence);
	if (ret || assigned_onu_id != 0x123 || assign_sequence != 0x22) {
		ret = ret ?: -EBADMSG;
		goto out;
	}
	memcpy(altered_control_ploam, assign_onu_id_words,
	       sizeof(altered_control_ploam));
	for (i = 0; i < AIROHA_XGS_PLOAM_CONTENT_SIZE / sizeof(u32); i++)
		put_unaligned_be32(altered_control_ploam[i],
				   altered_control_frame + i * sizeof(u32));
	altered_control_frame[AIROHA_XGS_ASSIGN_ONU_ID_PADDING_OFFSET] = 1;
	ret = airoha_xgs_ploam_mic(
		default_ploam, AIROHA_XGS_DOWNSTREAM, altered_control_frame,
		altered_control_frame + AIROHA_XGS_PLOAM_CONTENT_SIZE);
	if (ret)
		goto out;
	for (i = 0; i < AIROHA_XGS_PLOAM_FRAME_WORDS; i++)
		altered_control_ploam[i] = get_unaligned_be32(
			altered_control_frame + i * sizeof(u32));
	ret = airoha_xgs_authenticate_assign_onu_id_words(
		altered_control_ploam, default_ploam, serial, &assigned_onu_id,
		&assign_sequence);
	if (ret || assigned_onu_id != 0x123 || assign_sequence != 0x22) {
		ret = ret ?: -EBADMSG;
		goto out;
	}
	altered_control_frame[AIROHA_XGS_ASSIGN_ONU_ID_RATE_OFFSET] |= BIT(7);
	ret = airoha_xgs_ploam_mic(
		default_ploam, AIROHA_XGS_DOWNSTREAM, altered_control_frame,
		altered_control_frame + AIROHA_XGS_PLOAM_CONTENT_SIZE);
	if (ret)
		goto out;
	for (i = 0; i < AIROHA_XGS_PLOAM_FRAME_WORDS; i++)
		altered_control_ploam[i] = get_unaligned_be32(
			altered_control_frame + i * sizeof(u32));
	ret = airoha_xgs_authenticate_assign_onu_id_words(
		altered_control_ploam, default_ploam, serial, &assigned_onu_id,
		&assign_sequence);
	if (ret != -EPROTO || assigned_onu_id != AIROHA_XGS_BROADCAST_ONU_ID ||
	    assign_sequence) {
		ret = ret == -EPROTO ? -EBADMSG : ret;
		goto out;
	}
	ret = airoha_xgs_authenticate_request_registration_words(
		request_registration_words, keys.ploam, 0x123,
		&request_sequence);
	if (ret || request_sequence != 0x5a) {
		ret = ret ?: -EBADMSG;
		goto out;
	}
	memcpy(altered_request_registration, request_registration_words,
	       sizeof(altered_request_registration));
	altered_request_registration[1] = 1;
	for (i = 0; i < AIROHA_XGS_PLOAM_CONTENT_SIZE / sizeof(u32); i++)
		put_unaligned_be32(altered_request_registration[i],
				   altered_request_registration_frame +
					i * sizeof(u32));
	ret = airoha_xgs_ploam_mic(
		keys.ploam, AIROHA_XGS_DOWNSTREAM,
		altered_request_registration_frame,
		altered_request_registration_frame + AIROHA_XGS_PLOAM_CONTENT_SIZE);
	if (ret)
		goto out;
	altered_request_registration[10] = get_unaligned_be32(
		altered_request_registration_frame + AIROHA_XGS_PLOAM_CONTENT_SIZE);
	altered_request_registration[11] = get_unaligned_be32(
		altered_request_registration_frame + AIROHA_XGS_PLOAM_CONTENT_SIZE +
		sizeof(u32));
	ret = airoha_xgs_authenticate_request_registration_words(
		altered_request_registration, keys.ploam, 0x123,
		&request_sequence);
	if (ret || request_sequence != 0x5a) {
		ret = ret ?: -EBADMSG;
		goto out;
	}
	ret = airoha_xgs_authenticate_request_registration_words(
		request_registration_words, keys.ploam, 0x124,
		&request_sequence);
	if (ret != -EADDRNOTAVAIL) {
		ret = ret ?: -EBADMSG;
		goto out;
	}
	ret = airoha_xgs_authenticate_ranging_time_words(
		ranging_time_words, keys.ploam, 0x123, &ranging);
	if (ret || ranging.equalization_delay != 0x00123456 ||
	    ranging.sequence != 0x33 || !ranging.absolute || ranging.negative ||
	    !ranging.directed) {
		ret = ret ?: -EBADMSG;
		goto out;
	}
	memset(&ranging, 0xa5, sizeof(ranging));
	ret = airoha_xgs_authenticate_ranging_time_words(
		ranging_time_words, keys.ploam, 0x124, &ranging);
	if (ret != -EADDRNOTAVAIL ||
	    memchr_inv(&ranging, 0, sizeof(ranging))) {
		ret = ret == -EADDRNOTAVAIL ? -EBADMSG : ret;
		goto out;
	}
	memset(altered_control_frame, 0, sizeof(altered_control_frame));
	put_unaligned_be16(AIROHA_XGS_BROADCAST_XGS_ONU_ID,
			   altered_control_frame);
	altered_control_frame[2] = AIROHA_XGS_RANGING_TIME_MESSAGE_ID;
	altered_control_frame[3] = 0x34;
	put_unaligned_be32(0x10203040,
			   altered_control_frame + AIROHA_XGS_RANGING_EQD_OFFSET);
	ret = airoha_xgs_ploam_mic(
		keys.ploam, AIROHA_XGS_DOWNSTREAM, altered_control_frame,
		altered_control_frame + AIROHA_XGS_PLOAM_CONTENT_SIZE);
	if (ret)
		goto out;
	for (i = 0; i < AIROHA_XGS_PLOAM_FRAME_WORDS; i++)
		altered_control_ploam[i] = get_unaligned_be32(
			altered_control_frame + i * sizeof(u32));
	ret = airoha_xgs_authenticate_ranging_time_words(
		altered_control_ploam, keys.ploam, 0x123, &ranging);
	if (ret || ranging.equalization_delay != 0x10203040 ||
	    ranging.sequence != 0x34 || ranging.absolute || ranging.negative ||
	    ranging.directed) {
		ret = ret ?: -EBADMSG;
		goto out;
	}
	altered_control_frame[AIROHA_XGS_RANGING_FLAGS_OFFSET] =
		AIROHA_XGS_RANGING_NEGATIVE;
	ret = airoha_xgs_ploam_mic(
		keys.ploam, AIROHA_XGS_DOWNSTREAM, altered_control_frame,
		altered_control_frame + AIROHA_XGS_PLOAM_CONTENT_SIZE);
	if (ret)
		goto out;
	for (i = 0; i < AIROHA_XGS_PLOAM_FRAME_WORDS; i++)
		altered_control_ploam[i] = get_unaligned_be32(
			altered_control_frame + i * sizeof(u32));
	ret = airoha_xgs_authenticate_ranging_time_words(
		altered_control_ploam, keys.ploam, 0x123, &ranging);
	if (ret || ranging.absolute || !ranging.negative || ranging.directed) {
		ret = ret ?: -EBADMSG;
		goto out;
	}
	altered_control_frame[AIROHA_XGS_RANGING_FLAGS_OFFSET] =
		AIROHA_XGS_RANGING_ABSOLUTE;
	ret = airoha_xgs_ploam_mic(
		keys.ploam, AIROHA_XGS_DOWNSTREAM, altered_control_frame,
		altered_control_frame + AIROHA_XGS_PLOAM_CONTENT_SIZE);
	if (ret)
		goto out;
	for (i = 0; i < AIROHA_XGS_PLOAM_FRAME_WORDS; i++)
		altered_control_ploam[i] = get_unaligned_be32(
			altered_control_frame + i * sizeof(u32));
	ret = airoha_xgs_authenticate_ranging_time_words(
		altered_control_ploam, keys.ploam, 0x123, &ranging);
	if (ret != -EADDRNOTAVAIL ||
	    memchr_inv(&ranging, 0, sizeof(ranging))) {
		ret = ret == -EADDRNOTAVAIL ? -EBADMSG : ret;
		goto out;
	}
	memcpy(altered_control_ploam, ranging_time_words,
	       sizeof(altered_control_ploam));
	for (i = 0; i < AIROHA_XGS_PLOAM_CONTENT_SIZE / sizeof(u32); i++)
		put_unaligned_be32(altered_control_ploam[i],
				   altered_control_frame + i * sizeof(u32));
	altered_control_frame[AIROHA_XGS_RANGING_FLAGS_OFFSET] |= BIT(2);
	ret = airoha_xgs_ploam_mic(
		keys.ploam, AIROHA_XGS_DOWNSTREAM, altered_control_frame,
		altered_control_frame + AIROHA_XGS_PLOAM_CONTENT_SIZE);
	if (ret)
		goto out;
	for (i = 0; i < AIROHA_XGS_PLOAM_FRAME_WORDS; i++)
		altered_control_ploam[i] = get_unaligned_be32(
			altered_control_frame + i * sizeof(u32));
	memset(&ranging, 0xa5, sizeof(ranging));
	ret = airoha_xgs_authenticate_ranging_time_words(
		altered_control_ploam, keys.ploam, 0x123, &ranging);
	if (ret != -EPROTO || memchr_inv(&ranging, 0, sizeof(ranging))) {
		ret = ret == -EPROTO ? -EBADMSG : ret;
		goto out;
	}
	memcpy(altered_control_ploam, ranging_time_words,
	       sizeof(altered_control_ploam));
	for (i = 0; i < AIROHA_XGS_PLOAM_CONTENT_SIZE / sizeof(u32); i++)
		put_unaligned_be32(altered_control_ploam[i],
				   altered_control_frame + i * sizeof(u32));
	altered_control_frame[AIROHA_XGS_RANGING_PADDING_OFFSET] = 1;
	ret = airoha_xgs_ploam_mic(
		keys.ploam, AIROHA_XGS_DOWNSTREAM, altered_control_frame,
		altered_control_frame + AIROHA_XGS_PLOAM_CONTENT_SIZE);
	if (ret)
		goto out;
	for (i = 0; i < AIROHA_XGS_PLOAM_FRAME_WORDS; i++)
		altered_control_ploam[i] = get_unaligned_be32(
			altered_control_frame + i * sizeof(u32));
	ret = airoha_xgs_authenticate_ranging_time_words(
		altered_control_ploam, keys.ploam, 0x123, &ranging);
	if (ret || ranging.equalization_delay != 0x00123456 ||
	    ranging.sequence != 0x33 || !ranging.absolute || ranging.negative ||
	    !ranging.directed) {
		ret = ret ?: -EBADMSG;
		goto out;
	}
	ret = airoha_xgs_authenticate_deactivate_onu_id_words(
		deactivate_onu_id_words, keys.ploam, 0x123,
		&deactivate_sequence);
	if (ret || deactivate_sequence != 0x66) {
		ret = ret ?: -EBADMSG;
		goto out;
	}
	ret = airoha_xgs_authenticate_deactivate_onu_id_words(
		deactivate_onu_id_words, keys.ploam, 0x124,
		&deactivate_sequence);
	if (ret != -EADDRNOTAVAIL || deactivate_sequence) {
		ret = ret == -EADDRNOTAVAIL ? -EBADMSG : ret;
		goto out;
	}
	memcpy(altered_control_ploam, deactivate_onu_id_words,
	       sizeof(altered_control_ploam));
	for (i = 0; i < AIROHA_XGS_PLOAM_CONTENT_SIZE / sizeof(u32); i++)
		put_unaligned_be32(altered_control_ploam[i],
				   altered_control_frame + i * sizeof(u32));
	altered_control_frame[4] = 1;
	ret = airoha_xgs_ploam_mic(
		keys.ploam, AIROHA_XGS_DOWNSTREAM, altered_control_frame,
		altered_control_frame + AIROHA_XGS_PLOAM_CONTENT_SIZE);
	if (ret)
		goto out;
	for (i = 0; i < AIROHA_XGS_PLOAM_FRAME_WORDS; i++)
		altered_control_ploam[i] = get_unaligned_be32(
			altered_control_frame + i * sizeof(u32));
	ret = airoha_xgs_authenticate_deactivate_onu_id_words(
		altered_control_ploam, keys.ploam, 0x123,
		&deactivate_sequence);
	if (ret || deactivate_sequence != 0x66) {
		ret = ret ?: -EBADMSG;
		goto out;
	}
	memset(altered_control_frame, 0, sizeof(altered_control_frame));
	put_unaligned_be16(AIROHA_XGS_BROADCAST_XGS_ONU_ID,
			   altered_control_frame);
	altered_control_frame[2] = AIROHA_XGS_DISABLE_SERIAL_NUMBER_MESSAGE_ID;
	altered_control_frame[3] = 0x65;
	altered_control_frame[AIROHA_XGS_DISABLE_SERIAL_MODE_OFFSET] =
		AIROHA_XGS_DISABLE_SERIAL_DENIED_SPECIFIC;
	memcpy(altered_control_frame + AIROHA_XGS_DISABLE_SERIAL_NUMBER_OFFSET,
	       serial, sizeof(serial));
	ret = airoha_xgs_ploam_mic(default_ploam, AIROHA_XGS_DOWNSTREAM,
				    altered_control_frame,
				    altered_control_frame + AIROHA_XGS_PLOAM_CONTENT_SIZE);
	if (ret)
		goto out;
	for (i = 0; i < AIROHA_XGS_PLOAM_FRAME_WORDS; i++)
		altered_control_ploam[i] = get_unaligned_be32(
			altered_control_frame + i * sizeof(u32));
	ret = airoha_xgs_authenticate_disable_serial_number_words(
		altered_control_ploam, default_ploam, serial, &serial_action,
		&control_sequence);
	if (ret || serial_action != AIROHA_XGS_SERIAL_DISABLE ||
	    control_sequence != 0x65) {
		ret = ret ?: -EBADMSG;
		goto out;
	}
	altered_control_frame[AIROHA_XGS_DISABLE_SERIAL_PADDING_OFFSET] = 1;
	ret = airoha_xgs_ploam_mic(default_ploam, AIROHA_XGS_DOWNSTREAM,
				    altered_control_frame,
				    altered_control_frame + AIROHA_XGS_PLOAM_CONTENT_SIZE);
	if (ret)
		goto out;
	for (i = 0; i < AIROHA_XGS_PLOAM_FRAME_WORDS; i++)
		altered_control_ploam[i] = get_unaligned_be32(
			altered_control_frame + i * sizeof(u32));
	ret = airoha_xgs_authenticate_disable_serial_number_words(
		altered_control_ploam, default_ploam, serial, &serial_action,
		&control_sequence);
	if (ret || serial_action != AIROHA_XGS_SERIAL_DISABLE ||
	    control_sequence != 0x65) {
		ret = ret ?: -EBADMSG;
		goto out;
	}
	altered_control_frame[AIROHA_XGS_DISABLE_SERIAL_NUMBER_OFFSET] ^= 1;
	ret = airoha_xgs_ploam_mic(default_ploam, AIROHA_XGS_DOWNSTREAM,
				    altered_control_frame,
				    altered_control_frame + AIROHA_XGS_PLOAM_CONTENT_SIZE);
	if (ret)
		goto out;
	for (i = 0; i < AIROHA_XGS_PLOAM_FRAME_WORDS; i++)
		altered_control_ploam[i] = get_unaligned_be32(
			altered_control_frame + i * sizeof(u32));
	ret = airoha_xgs_authenticate_disable_serial_number_words(
		altered_control_ploam, default_ploam, serial, &serial_action,
		&control_sequence);
	if (ret != -ENOMSG || serial_action || control_sequence) {
		ret = ret == -ENOMSG ? -EBADMSG : ret;
		goto out;
	}
	memset(altered_control_frame, 0, sizeof(altered_control_frame));
	put_unaligned_be16(0x123, altered_control_frame);
	altered_control_frame[2] = AIROHA_XGS_SLEEP_ALLOW_MESSAGE_ID;
	altered_control_frame[3] = 0x76;
	altered_control_frame[AIROHA_XGS_SLEEP_ALLOW_TYPE_OFFSET] =
		AIROHA_XGS_SLEEP_ALLOW_TYPE;
	ret = airoha_xgs_ploam_mic(keys.ploam, AIROHA_XGS_DOWNSTREAM,
				    altered_control_frame,
				    altered_control_frame + AIROHA_XGS_PLOAM_CONTENT_SIZE);
	if (ret)
		goto out;
	for (i = 0; i < AIROHA_XGS_PLOAM_FRAME_WORDS; i++)
		altered_control_ploam[i] = get_unaligned_be32(
			altered_control_frame + i * sizeof(u32));
	ret = airoha_xgs_authenticate_sleep_allow_words(
		altered_control_ploam, keys.ploam, 0x123, &sleep_type,
		&control_sequence);
	if (ret || !sleep_type || control_sequence != 0x76) {
		ret = ret ?: -EBADMSG;
		goto out;
	}
	altered_control_frame[AIROHA_XGS_SLEEP_ALLOW_PADDING_OFFSET] = 1;
	ret = airoha_xgs_ploam_mic(keys.ploam, AIROHA_XGS_DOWNSTREAM,
				    altered_control_frame,
				    altered_control_frame + AIROHA_XGS_PLOAM_CONTENT_SIZE);
	if (ret)
		goto out;
	for (i = 0; i < AIROHA_XGS_PLOAM_FRAME_WORDS; i++)
		altered_control_ploam[i] = get_unaligned_be32(
			altered_control_frame + i * sizeof(u32));
	ret = airoha_xgs_authenticate_sleep_allow_words(
		altered_control_ploam, keys.ploam, 0x123, &sleep_type,
		&control_sequence);
	if (ret || !sleep_type || control_sequence != 0x76) {
		ret = ret ?: -EBADMSG;
		goto out;
	}
	altered_control_frame[AIROHA_XGS_SLEEP_ALLOW_TYPE_OFFSET] |= BIT(1);
	ret = airoha_xgs_ploam_mic(keys.ploam, AIROHA_XGS_DOWNSTREAM,
				    altered_control_frame,
				    altered_control_frame + AIROHA_XGS_PLOAM_CONTENT_SIZE);
	if (ret)
		goto out;
	for (i = 0; i < AIROHA_XGS_PLOAM_FRAME_WORDS; i++)
		altered_control_ploam[i] = get_unaligned_be32(
			altered_control_frame + i * sizeof(u32));
	ret = airoha_xgs_authenticate_sleep_allow_words(
		altered_control_ploam, keys.ploam, 0x123, &sleep_type,
		&control_sequence);
	if (ret != -EPROTO || sleep_type || control_sequence) {
		ret = ret == -EPROTO ? -EBADMSG : ret;
		goto out;
	}
	ret = airoha_xgs_authenticate_assign_alloc_id_words(
		assign_alloc_id_words, keys.ploam, 0x123, &assignment);
	if (ret || assignment.alloc_id != 0x456 ||
	    assignment.sequence != 0x44 ||
	    assignment.operation != AIROHA_XGS_ALLOC_ID_ASSIGN) {
		ret = ret ?: -EBADMSG;
		goto out;
	}
	ret = airoha_xgs_parse_key_control_words(
		key_control_words, keys.ploam, 0x123, &key_control_sequence,
		&key_control, &key_control_index);
	if (ret || key_control_sequence != 0x77 ||
	    key_control != AIROHA_XGS_KEY_REPORT_GENERATE ||
	    key_control_index != 1) {
		ret = ret ?: -EBADMSG;
		goto out;
	}
	memcpy(altered_control_ploam, key_control_words,
	       sizeof(altered_control_ploam));
	for (i = 0; i < AIROHA_XGS_PLOAM_CONTENT_SIZE / sizeof(u32); i++)
		put_unaligned_be32(altered_control_ploam[i],
				   altered_control_frame + i * sizeof(u32));
	altered_control_frame[AIROHA_XGS_KEY_CONTROL_RESERVED_OFFSET] = 1;
	altered_control_frame[AIROHA_XGS_KEY_CONTROL_PADDING_OFFSET] = 1;
	ret = airoha_xgs_ploam_mic(
		keys.ploam, AIROHA_XGS_DOWNSTREAM, altered_control_frame,
		altered_control_frame + AIROHA_XGS_PLOAM_CONTENT_SIZE);
	if (ret)
		goto out;
	for (i = 0; i < AIROHA_XGS_PLOAM_FRAME_WORDS; i++)
		altered_control_ploam[i] = get_unaligned_be32(
			altered_control_frame + i * sizeof(u32));
	ret = airoha_xgs_parse_key_control_words(
		altered_control_ploam, keys.ploam, 0x123,
		&key_control_sequence, &key_control, &key_control_index);
	if (ret || key_control_sequence != 0x77 ||
	    key_control != AIROHA_XGS_KEY_REPORT_GENERATE ||
	    key_control_index != 1) {
		ret = ret ?: -EBADMSG;
		goto out;
	}
	altered_control_frame[AIROHA_XGS_KEY_CONTROL_CONTROL_OFFSET] |= BIT(1);
	ret = airoha_xgs_ploam_mic(
		keys.ploam, AIROHA_XGS_DOWNSTREAM, altered_control_frame,
		altered_control_frame + AIROHA_XGS_PLOAM_CONTENT_SIZE);
	if (ret)
		goto out;
	for (i = 0; i < AIROHA_XGS_PLOAM_FRAME_WORDS; i++)
		altered_control_ploam[i] = get_unaligned_be32(
			altered_control_frame + i * sizeof(u32));
	ret = airoha_xgs_parse_key_control_words(
		altered_control_ploam, keys.ploam, 0x123,
		&key_control_sequence, &key_control, &key_control_index);
	if (ret != -EPROTO || key_control_sequence || key_control ||
	    key_control_index) {
		ret = ret == -EPROTO ? -EBADMSG : ret;
		goto out;
	}
	memcpy(altered_control_ploam, key_control_words,
	       sizeof(altered_control_ploam));
	altered_control_ploam[2] = 1;
	ret = airoha_xgs_parse_key_control_words(
		altered_control_ploam, keys.ploam, 0x123,
		&key_control_sequence, &key_control, &key_control_index);
	if (ret != -EBADMSG || key_control_sequence || key_control ||
	    key_control_index) {
		ret = ret == -EBADMSG ? -EBADMSG : ret;
		goto out;
	}
	memcpy(altered_control_ploam, assign_alloc_id_words,
	       sizeof(altered_control_ploam));
	for (i = 0; i < AIROHA_XGS_PLOAM_CONTENT_SIZE / sizeof(u32); i++)
		put_unaligned_be32(altered_control_ploam[i],
				   altered_control_frame + i * sizeof(u32));
	altered_control_frame[AIROHA_XGS_ALLOC_ID_PADDING_OFFSET] = 1;
	ret = airoha_xgs_ploam_mic(
		keys.ploam, AIROHA_XGS_DOWNSTREAM, altered_control_frame,
		altered_control_frame + AIROHA_XGS_PLOAM_CONTENT_SIZE);
	if (ret)
		goto out;
	for (i = 0; i < AIROHA_XGS_PLOAM_FRAME_WORDS; i++)
		altered_control_ploam[i] = get_unaligned_be32(
			altered_control_frame + i * sizeof(u32));
	memset(&assignment, 0xa5, sizeof(assignment));
	ret = airoha_xgs_authenticate_assign_alloc_id_words(
		altered_control_ploam, keys.ploam, 0x123, &assignment);
	if (ret || assignment.alloc_id != 0x456 ||
	    assignment.sequence != 0x44 ||
	    assignment.operation != AIROHA_XGS_ALLOC_ID_ASSIGN) {
		ret = ret ?: -EBADMSG;
		goto out;
	}
	memcpy(altered_control_ploam, assign_alloc_id_words,
	       sizeof(altered_control_ploam));
	for (i = 0; i < AIROHA_XGS_PLOAM_CONTENT_SIZE / sizeof(u32); i++)
		put_unaligned_be32(altered_control_ploam[i],
				   altered_control_frame + i * sizeof(u32));
	altered_control_frame[AIROHA_XGS_ALLOC_ID_OFFSET] |=
		AIROHA_XGS_ALLOC_ID_RESERVED;
	ret = airoha_xgs_ploam_mic(
		keys.ploam, AIROHA_XGS_DOWNSTREAM, altered_control_frame,
		altered_control_frame + AIROHA_XGS_PLOAM_CONTENT_SIZE);
	if (ret)
		goto out;
	for (i = 0; i < AIROHA_XGS_PLOAM_FRAME_WORDS; i++)
		altered_control_ploam[i] = get_unaligned_be32(
			altered_control_frame + i * sizeof(u32));
	ret = airoha_xgs_authenticate_assign_alloc_id_words(
		altered_control_ploam, keys.ploam, 0x123, &assignment);
	if (ret || assignment.alloc_id != 0x456 ||
	    assignment.sequence != 0x44 ||
	    assignment.operation != AIROHA_XGS_ALLOC_ID_ASSIGN) {
		ret = ret ?: -EBADMSG;
		goto out;
	}
	memcpy(altered_control_ploam, assign_alloc_id_words,
	       sizeof(altered_control_ploam));
	for (i = 0; i < AIROHA_XGS_PLOAM_CONTENT_SIZE / sizeof(u32); i++)
		put_unaligned_be32(altered_control_ploam[i],
				   altered_control_frame + i * sizeof(u32));
	altered_control_frame[AIROHA_XGS_ALLOC_ID_SCOPE_OFFSET] = 1;
	ret = airoha_xgs_ploam_mic(
		keys.ploam, AIROHA_XGS_DOWNSTREAM, altered_control_frame,
		altered_control_frame + AIROHA_XGS_PLOAM_CONTENT_SIZE);
	if (ret)
		goto out;
	for (i = 0; i < AIROHA_XGS_PLOAM_FRAME_WORDS; i++)
		altered_control_ploam[i] = get_unaligned_be32(
			altered_control_frame + i * sizeof(u32));
	memset(&assignment, 0xa5, sizeof(assignment));
	ret = airoha_xgs_authenticate_assign_alloc_id_words(
		altered_control_ploam, keys.ploam, 0x123, &assignment);
	if (ret != -EPROTO ||
	    memchr_inv(&assignment, 0, sizeof(assignment))) {
		ret = ret == -EPROTO ? -EBADMSG : ret;
		goto out;
	}
	memcpy(altered_control_ploam, assign_alloc_id_words,
	       sizeof(altered_control_ploam));
	for (i = 0; i < AIROHA_XGS_PLOAM_CONTENT_SIZE / sizeof(u32); i++)
		put_unaligned_be32(altered_control_ploam[i],
				   altered_control_frame + i * sizeof(u32));
	altered_control_frame[AIROHA_XGS_ALLOC_ID_TYPE_OFFSET] = 2;
	ret = airoha_xgs_ploam_mic(
		keys.ploam, AIROHA_XGS_DOWNSTREAM, altered_control_frame,
		altered_control_frame + AIROHA_XGS_PLOAM_CONTENT_SIZE);
	if (ret)
		goto out;
	for (i = 0; i < AIROHA_XGS_PLOAM_FRAME_WORDS; i++)
		altered_control_ploam[i] = get_unaligned_be32(
			altered_control_frame + i * sizeof(u32));
	memset(&assignment, 0xa5, sizeof(assignment));
	ret = airoha_xgs_authenticate_assign_alloc_id_words(
		altered_control_ploam, keys.ploam, 0x123, &assignment);
	if (ret != -EPROTO ||
	    memchr_inv(&assignment, 0, sizeof(assignment))) {
		ret = ret == -EPROTO ? -EBADMSG : ret;
		goto out;
	}
	ret = airoha_xgs_build_serial_number_ploam(
		serial, 0x12345678, AIROHA_XPON_MODE_XGPON, upstream_frame);
	if (ret || crypto_memneq(upstream_frame,
				 expected_xgpon_serial_number_ploam,
				 sizeof(upstream_frame))) {
		ret = ret ?: -EBADMSG;
		goto out;
	}
	ret = airoha_xgs_encode_upstream_ploam_fifo(
		upstream_frame, AIROHA_XGS_PLOAM_KEY_INDEX_1, upstream_fifo);
	if (ret || crypto_memneq(upstream_fifo,
				 expected_xgpon_serial_number_fifo,
				 sizeof(upstream_fifo))) {
		ret = ret ?: -EBADMSG;
		goto out;
	}
	ret = airoha_xgs_build_serial_number_ploam(
		serial, 0x12345678, AIROHA_XPON_MODE_XGSPON, upstream_frame);
	if (ret || crypto_memneq(upstream_frame,
				 expected_xgspon_serial_number_ploam,
				 sizeof(upstream_frame))) {
		ret = ret ?: -EBADMSG;
		goto out;
	}
	ret = airoha_xgs_encode_upstream_ploam_fifo(
		upstream_frame, AIROHA_XGS_PLOAM_KEY_INDEX_1, upstream_fifo);
	if (ret || crypto_memneq(upstream_fifo,
				 expected_xgspon_serial_number_fifo,
				 sizeof(upstream_fifo))) {
		ret = ret ?: -EBADMSG;
		goto out;
	}
	for (i = 0; i < ARRAY_SIZE(invalid_serial_number_modes); i++) {
		memset(upstream_frame, 0xa5, sizeof(upstream_frame));
		ret = airoha_xgs_build_serial_number_ploam(
			serial, 0x12345678, invalid_serial_number_modes[i],
			upstream_frame);
		if (ret != -EINVAL ||
		    memchr_inv(upstream_frame, 0, sizeof(upstream_frame))) {
			ret = ret == -EINVAL ? -EBADMSG : ret;
			goto out;
		}
	}
	ret = airoha_xgs_build_registration_ploam(
		keys.ploam, 0x123, 0x22, message_registration_id,
		upstream_frame);
	if (ret || crypto_memneq(upstream_frame, expected_registration_ploam,
				 sizeof(upstream_frame))) {
		ret = ret ?: -EBADMSG;
		goto out;
	}
	ret = airoha_xgs_encode_upstream_ploam_fifo(
		upstream_frame, AIROHA_XGS_PLOAM_KEY_INDEX_0, upstream_fifo);
	if (ret || crypto_memneq(upstream_fifo, expected_registration_fifo,
				 sizeof(upstream_fifo))) {
		ret = ret ?: -EBADMSG;
		goto out;
	}
	ret = airoha_xgs_build_acknowledge_ploam(
		keys.ploam, 0x123, 0x44, AIROHA_XGS_ACK_COMPLETION_OK,
		upstream_frame);
	if (ret || crypto_memneq(upstream_frame, expected_acknowledge_ploam,
				 sizeof(upstream_frame))) {
		ret = ret ?: -EBADMSG;
		goto out;
	}
	ret = airoha_xgs_aes_ecb_encrypt(keys.kek, data_key, wrapped_key);
	if (ret || crypto_memneq(wrapped_key, expected_wrapped_key,
				 sizeof(wrapped_key))) {
		ret = ret ?: -EBADMSG;
		goto out;
	}
	ret = airoha_xgs_data_key_proof(keys.kek, data_key, key_proof);
	if (ret || crypto_memneq(key_proof, expected_key_proof,
				 sizeof(key_proof))) {
		ret = ret ?: -EBADMSG;
		goto out;
	}
	memset(altered_control_frame, 0, sizeof(altered_control_frame));
	memcpy(altered_control_frame, wrapped_key, sizeof(wrapped_key));
	ret = airoha_xgs_build_key_report(
		keys.ploam, 0x123, 0x55, AIROHA_XGS_KEY_REPORT_GENERATE, 1,
		altered_control_frame, upstream_frame);
	if (ret || crypto_memneq(upstream_frame, expected_key_report,
				 sizeof(upstream_frame))) {
		ret = ret ?: -EBADMSG;
		goto out;
	}
	ret = airoha_xgs_encode_upstream_ploam_fifo(
		upstream_frame, AIROHA_XGS_PLOAM_KEY_INDEX_0, upstream_fifo);
	if (ret || crypto_memneq(upstream_fifo, expected_key_report_fifo,
				 sizeof(upstream_fifo))) {
		ret = ret ?: -EBADMSG;
		goto out;
	}
	ret = airoha_xgs_encode_upstream_ploam_fifo(
		upstream_frame, AIROHA_XGS_PLOAM_KEY_INDEX_0, upstream_fifo);
	if (ret || crypto_memneq(upstream_fifo, expected_acknowledge_fifo,
				 sizeof(upstream_fifo))) {
		ret = ret ?: -EBADMSG;
		goto out;
	}
	ret = airoha_xgs_encode_upstream_ploam_fifo(
		upstream_frame, 2, upstream_fifo);
	if (ret != -EINVAL || memchr_inv(upstream_fifo, 0,
					 sizeof(upstream_fifo))) {
		ret = ret == -EINVAL ? -EBADMSG : ret;
		goto out;
	}
	ret = airoha_xgs_build_registration_ploam(
		keys.ploam, AIROHA_XGS_BROADCAST_XGS_ONU_ID, 0x22,
		message_registration_id, upstream_frame);
	if (ret != -ERANGE || memchr_inv(upstream_frame, 0,
					 sizeof(upstream_frame))) {
		ret = ret == -ERANGE ? -EBADMSG : ret;
		goto out;
	}
	ret = 0;
	memcpy(altered_assign_onu_id, assign_onu_id_words,
	       sizeof(altered_assign_onu_id));
	altered_assign_onu_id[2] ^= BIT(0);
	ret = airoha_xgs_authenticate_assign_onu_id_words(
		altered_assign_onu_id, default_ploam, serial, &assigned_onu_id,
		&assign_sequence);
	if (ret != -EBADMSG) {
		ret = ret ?: -EBADMSG;
		goto out;
	}
	ret = 0;
	memcpy(altered_profile, initial_profile, sizeof(altered_profile));
	altered_profile[AIROHA_XGS_PROFILE_PON_TAG_OFFSET] ^= 1;
	ret = airoha_xgs_authenticate_initial_profile(altered_profile,
						      authenticated_pon_tag);
	if (ret != -EBADMSG) {
		ret = ret ?: -EBADMSG;
		goto out;
	}
	memcpy(altered_profile, initial_profile, sizeof(altered_profile));
	altered_profile[0] = 0;
	altered_profile[1] = 1;
	ret = airoha_xgs_authenticate_initial_profile(altered_profile,
						      authenticated_pon_tag);
	if (ret != -EADDRNOTAVAIL) {
		ret = ret ?: -EBADMSG;
		goto out;
	}
	memcpy(altered_profile, initial_profile, sizeof(altered_profile));
	altered_profile[4] &= ~AIROHA_XGS_PROFILE_LINE_RATE;
	ret = airoha_xgs_authenticate_initial_profile(altered_profile,
						      authenticated_pon_tag);
	if (ret != -EPROTO) {
		ret = ret ?: -EBADMSG;
		goto out;
	}
	ret = 0;
	ploam_mic[0] ^= 1;
	ret = airoha_xgs_verify_ploam_mic(keys.ploam, AIROHA_XGS_UPSTREAM,
					   upstream_ploam, ploam_mic);
	if (ret != -EBADMSG) {
		ret = ret ?: -EBADMSG;
		goto out;
	}

	ret = airoha_xgs_omci_mic(keys.omci, AIROHA_XGS_DOWNSTREAM,
				   downstream_omci, sizeof(downstream_omci),
				   omci_mic);
	if (!ret && crypto_memneq(omci_mic, expected_downstream_omci_mic,
				  sizeof(omci_mic)))
		ret = -EBADMSG;
	if (ret)
		goto out;
	memcpy(downstream_omci_wire, downstream_omci,
	       sizeof(downstream_omci));
	memcpy(downstream_omci_wire + sizeof(downstream_omci),
	       expected_downstream_omci_mic,
	       sizeof(expected_downstream_omci_mic));
	ret = airoha_xgs_authenticate_downstream_omci(
		keys.omci, downstream_omci_wire, sizeof(downstream_omci_wire),
		authenticated_omci, sizeof(authenticated_omci),
		&authenticated_omci_len);
	if (ret || authenticated_omci_len != sizeof(downstream_omci) ||
	    crypto_memneq(authenticated_omci, downstream_omci,
			   sizeof(downstream_omci))) {
		ret = ret ?: -EBADMSG;
		goto out;
	}
	ret = airoha_xgs_sign_upstream_omci(
		keys.omci, downstream_omci, sizeof(downstream_omci),
		signed_omci, sizeof(signed_omci), &signed_omci_len);
	if (ret || signed_omci_len != sizeof(signed_omci) ||
	    crypto_memneq(signed_omci, downstream_omci,
			   sizeof(downstream_omci)) ||
	    crypto_memneq(signed_omci + sizeof(downstream_omci),
			   expected_upstream_omci_mic,
			   sizeof(expected_upstream_omci_mic))) {
		ret = ret ?: -EBADMSG;
		goto out;
	}
	downstream_omci_wire[sizeof(downstream_omci_wire) - 1] ^= 1;
	ret = airoha_xgs_authenticate_downstream_omci(
		keys.omci, downstream_omci_wire, sizeof(downstream_omci_wire),
		authenticated_omci, sizeof(authenticated_omci),
		&authenticated_omci_len);
	if (ret != -EBADMSG || authenticated_omci_len ||
	    memchr_inv(authenticated_omci, 0, sizeof(authenticated_omci))) {
		ret = ret == -EBADMSG ? -EBADMSG : ret;
		goto out;
	}
	memcpy(malformed_omci, downstream_omci, sizeof(malformed_omci));
	malformed_omci[AIROHA_XGS_OMCI_BASELINE_LENGTH_OFFSET + 3]--;
	ret = airoha_xgs_sign_upstream_omci(
		keys.omci, malformed_omci, sizeof(malformed_omci),
		signed_omci, sizeof(signed_omci), &signed_omci_len);
	if (ret != -EPROTO || signed_omci_len ||
	    memchr_inv(signed_omci, 0, sizeof(signed_omci))) {
		ret = ret == -EPROTO ? -EBADMSG : ret;
		goto out;
	}
	memcpy(extended_omci_wire, extended_omci, sizeof(extended_omci));
	memcpy(extended_omci_wire + sizeof(extended_omci),
	       expected_downstream_extended_omci_mic,
	       sizeof(expected_downstream_extended_omci_mic));
	ret = airoha_xgs_authenticate_downstream_omci(
		keys.omci, extended_omci_wire, sizeof(extended_omci_wire),
		authenticated_omci, sizeof(authenticated_omci),
		&authenticated_omci_len);
	if (ret || authenticated_omci_len != sizeof(extended_omci) ||
	    crypto_memneq(authenticated_omci, extended_omci,
			   sizeof(extended_omci))) {
		ret = ret ?: -EBADMSG;
		goto out;
	}
	ret = airoha_xgs_sign_upstream_omci(
		keys.omci, extended_omci, sizeof(extended_omci),
		signed_omci, sizeof(signed_omci), &signed_omci_len);
	if (ret || signed_omci_len != sizeof(extended_omci_wire) ||
	    crypto_memneq(signed_omci, extended_omci, sizeof(extended_omci)) ||
	    crypto_memneq(signed_omci + sizeof(extended_omci),
			   expected_upstream_extended_omci_mic,
			   sizeof(expected_upstream_extended_omci_mic))) {
		ret = ret ?: -EBADMSG;
		goto out;
	}
	memcpy(malformed_omci, downstream_omci, sizeof(malformed_omci));
	malformed_omci[3] = AIROHA_XGS_OMCI_EXTENDED_DEVICE_ID;
	malformed_omci[AIROHA_XGS_OMCI_EXTENDED_LENGTH_OFFSET] = 0;
	malformed_omci[AIROHA_XGS_OMCI_EXTENDED_LENGTH_OFFSET + 1] = 5;
	ret = airoha_xgs_sign_upstream_omci(
		keys.omci, malformed_omci, sizeof(malformed_omci),
		signed_omci, sizeof(signed_omci), &signed_omci_len);
	if (ret != -EPROTO || signed_omci_len ||
	    memchr_inv(signed_omci, 0, sizeof(signed_omci))) {
		ret = ret == -EPROTO ? -EBADMSG : ret;
		goto out;
	}
	ret = 0;
	omci_mic[0] ^= 1;
	ret = airoha_xgs_verify_omci_mic(keys.omci, AIROHA_XGS_DOWNSTREAM,
					 downstream_omci,
					 sizeof(downstream_omci), omci_mic);
	if (ret == -EBADMSG)
		ret = 0;
	else
		ret = ret ?: -EBADMSG;
out:
	memzero_explicit(registration_msk, sizeof(registration_msk));
	memzero_explicit(ploam_mic, sizeof(ploam_mic));
	memzero_explicit(omci_mic, sizeof(omci_mic));
	memzero_explicit(downstream_omci_wire,
			 sizeof(downstream_omci_wire));
	memzero_explicit(authenticated_omci, sizeof(authenticated_omci));
	memzero_explicit(signed_omci, sizeof(signed_omci));
	memzero_explicit(malformed_omci, sizeof(malformed_omci));
	memzero_explicit(extended_omci_wire, sizeof(extended_omci_wire));
	memzero_explicit(authenticated_pon_tag,
			 sizeof(authenticated_pon_tag));
	memzero_explicit(altered_profile, sizeof(altered_profile));
	memzero_explicit(profile_words, sizeof(profile_words));
	memzero_explicit(&profile, sizeof(profile));
	memzero_explicit(altered_assign_onu_id,
			 sizeof(altered_assign_onu_id));
	memzero_explicit(altered_request_registration,
			 sizeof(altered_request_registration));
	memzero_explicit(altered_request_registration_frame,
			 sizeof(altered_request_registration_frame));
	memzero_explicit(altered_control_ploam,
			 sizeof(altered_control_ploam));
	memzero_explicit(altered_control_frame,
			 sizeof(altered_control_frame));
	memzero_explicit(&ranging, sizeof(ranging));
	memzero_explicit(&assignment, sizeof(assignment));
	memzero_explicit(default_ploam, sizeof(default_ploam));
	memzero_explicit(wrapped_key, sizeof(wrapped_key));
	memzero_explicit(key_proof, sizeof(key_proof));
	memzero_explicit(upstream_frame, sizeof(upstream_frame));
	memzero_explicit(upstream_fifo, sizeof(upstream_fifo));
	memzero_explicit(&keys, sizeof(keys));
	if (ret)
		pr_err("Airoha XGS-PON G.9807.1 security self-test failed: %d\n",
		       ret);
	return ret;
}
module_init(airoha_xgs_security_init);

static void __exit airoha_xgs_security_exit(void)
{
}
module_exit(airoha_xgs_security_exit);

MODULE_AUTHOR("OpenWrt contributors");
MODULE_DESCRIPTION("Airoha XGS-PON trusted G.9807.1 security primitives");
MODULE_LICENSE("GPL");
