/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef _EN7572_CALIBRATION_H_
#define _EN7572_CALIBRATION_H_

#ifdef __KERNEL__
#include <linux/types.h>
typedef u8 en7572_u8;
typedef u32 en7572_u32;
#else
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
typedef uint8_t en7572_u8;
typedef uint32_t en7572_u32;
#endif

#define EN7572_BOB_SIZE			512
#define EN7572_BOB_FACTORY_RECORD_SIZE	513
#define EN7572_BOB_BANK_SIZE		256
#define EN7572_BOB_BANK_A0		0
#define EN7572_BOB_BANK_A2		1
#define EN7572_BOB_VENDOR_OFFSET	20
#define EN7572_BOB_PART_OFFSET		40
#define EN7572_BOB_IDENTITY_SIZE	16
#define EN7572_BOB_IBIAS_OFFSET		0x84
#define EN7572_BOB_APC_OFFSET		0x8a
#define EN7572_BOB_ERC_OFFSET		0x8d

static inline bool en7572_calibration_ascii_valid(const en7572_u8 *data)
{
	size_t i;
	bool nonempty = false;

	for (i = 0; i < EN7572_BOB_IDENTITY_SIZE; i++) {
		if (data[i] < 0x20 || data[i] > 0x7e)
			return false;
		if (data[i] != ' ')
			nonempty = true;
	}

	return nonempty;
}

static inline bool en7572_calibration_field_programmed(const en7572_u8 *data,
							 size_t len)
{
	size_t i;

	for (i = 0; i < len; i++)
		if (data[i] != 0x00 && data[i] != 0xff)
			return true;

	return false;
}

static inline bool en7572_calibration_bank_valid(const en7572_u8 *bank)
{
	return en7572_calibration_field_programmed(
			bank + EN7572_BOB_IBIAS_OFFSET,
			EN7572_BOB_APC_OFFSET - EN7572_BOB_IBIAS_OFFSET) &&
	       bank[EN7572_BOB_APC_OFFSET] != 0x00 &&
	       bank[EN7572_BOB_APC_OFFSET] != 0xff &&
	       en7572_calibration_field_programmed(
			bank + EN7572_BOB_ERC_OFFSET, 3);
}

static inline bool
en7572_calibration_identity_valid(const en7572_u8 *data, size_t len)
{
	return len == EN7572_BOB_SIZE &&
	       en7572_calibration_ascii_valid(
			data + EN7572_BOB_VENDOR_OFFSET) &&
	       en7572_calibration_ascii_valid(data + EN7572_BOB_PART_OFFSET);
}

static inline bool
en7572_calibration_bank_index_valid(const en7572_u8 *data, size_t len,
				     unsigned int bank)
{
	if (!en7572_calibration_identity_valid(data, len) ||
	    bank > EN7572_BOB_BANK_A2)
		return false;

	return en7572_calibration_bank_valid(
		data + bank * EN7572_BOB_BANK_SIZE);
}

static inline bool en7572_calibration_valid(const en7572_u8 *data, size_t len)
{
	if (!en7572_calibration_identity_valid(data, len))
		return false;

	return en7572_calibration_bank_valid(data) ||
	       en7572_calibration_bank_valid(data + EN7572_BOB_BANK_SIZE);
}

static inline bool
en7572_calibration_factory_record_bank_valid(const en7572_u8 *data,
					      size_t len,
					      unsigned int bank)
{
	return len == EN7572_BOB_FACTORY_RECORD_SIZE &&
	       en7572_calibration_bank_index_valid(data, EN7572_BOB_SIZE, bank);
}

static inline bool
en7572_calibration_factory_record_valid(const en7572_u8 *data, size_t len)
{
	/* Stock passes all 513 bytes through, while LDDLA consumes the first 512. */
	return len == EN7572_BOB_FACTORY_RECORD_SIZE &&
	       en7572_calibration_valid(data, EN7572_BOB_SIZE);
}

#endif
