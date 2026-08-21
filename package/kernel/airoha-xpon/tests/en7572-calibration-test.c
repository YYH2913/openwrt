/* SPDX-License-Identifier: GPL-2.0-only */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../src/en7572-calibration.h"

static int expect(const char *name, bool actual, bool wanted)
{
	if (actual == wanted)
		return 0;
	fprintf(stderr, "%s: got %s, want %s\n", name,
		actual ? "valid" : "invalid", wanted ? "valid" : "invalid");
	return 1;
}

int main(int argc, char **argv)
{
	en7572_u8 sample[EN7572_BOB_SIZE];
	en7572_u8 changed[EN7572_BOB_SIZE];
	en7572_u8 record[EN7572_BOB_FACTORY_RECORD_SIZE];
	FILE *input;
	bool expect_a0, expect_a2;
	int failed = 0;

	if (argc != 3 || (strcmp(argv[2], "a0") && strcmp(argv[2], "a2") &&
			  strcmp(argv[2], "both"))) {
		fprintf(stderr, "usage: %s EN7572_BOB a0|a2|both\n", argv[0]);
		return 2;
	}
	expect_a0 = !strcmp(argv[2], "a0") || !strcmp(argv[2], "both");
	expect_a2 = !strcmp(argv[2], "a2") || !strcmp(argv[2], "both");
	input = fopen(argv[1], "rb");
	if (!input) {
		perror(argv[1]);
		return 2;
	}
	if (fread(sample, 1, sizeof(sample), input) != sizeof(sample) ||
	    fgetc(input) != EOF) {
		fprintf(stderr, "%s: expected exactly %u bytes\n", argv[1],
			EN7572_BOB_SIZE);
		fclose(input);
		return 2;
	}
	fclose(input);

	failed += expect("stock calibration",
		en7572_calibration_valid(sample, sizeof(sample)), true);
	failed += expect("A0 calibration bank",
		en7572_calibration_bank_index_valid(sample, sizeof(sample),
			EN7572_BOB_BANK_A0), expect_a0);
	failed += expect("A2 calibration bank",
		en7572_calibration_bank_index_valid(sample, sizeof(sample),
			EN7572_BOB_BANK_A2), expect_a2);
	failed += expect("out-of-range calibration bank",
		en7572_calibration_bank_index_valid(sample, sizeof(sample), 2),
		false);
	failed += expect("short calibration",
		en7572_calibration_valid(sample, sizeof(sample) - 1), false);
	memcpy(record, sample, sizeof(sample));
	record[EN7572_BOB_SIZE] = 0xff;
	failed += expect("stock factory record",
		en7572_calibration_factory_record_valid(record, sizeof(record)),
		true);
	failed += expect("factory A2 calibration bank",
		en7572_calibration_factory_record_bank_valid(record,
			sizeof(record), EN7572_BOB_BANK_A2), expect_a2);
	failed += expect("factory A0 calibration bank",
		en7572_calibration_factory_record_bank_valid(record,
			sizeof(record), EN7572_BOB_BANK_A0),
		expect_a0);
	failed += expect("bare table is not a factory record",
		en7572_calibration_factory_record_valid(record, EN7572_BOB_SIZE),
		false);
	failed += expect("oversized artifact is not a factory record",
		en7572_calibration_factory_record_valid(record, sizeof(record) + 1),
		false);

	memset(changed, 0xff, sizeof(changed));
	failed += expect("erased calibration",
		en7572_calibration_valid(changed, sizeof(changed)), false);
	memcpy(changed, sample, sizeof(changed));
	changed[EN7572_BOB_VENDOR_OFFSET] = 0x00;
	failed += expect("non-printable vendor",
		en7572_calibration_valid(changed, sizeof(changed)), false);
	memcpy(changed, sample, sizeof(changed));
	memset(changed + EN7572_BOB_PART_OFFSET, ' ',
	       EN7572_BOB_IDENTITY_SIZE);
	failed += expect("empty part number",
		en7572_calibration_valid(changed, sizeof(changed)), false);
	memcpy(changed, sample, sizeof(changed));
	memset(changed + EN7572_BOB_IBIAS_OFFSET, 0xff,
	       EN7572_BOB_ERC_OFFSET + 3 - EN7572_BOB_IBIAS_OFFSET);
	memset(changed + EN7572_BOB_BANK_SIZE + EN7572_BOB_IBIAS_OFFSET, 0xff,
	       EN7572_BOB_ERC_OFFSET + 3 - EN7572_BOB_IBIAS_OFFSET);
	failed += expect("erased calibration banks",
		en7572_calibration_valid(changed, sizeof(changed)), false);
	memcpy(record, changed, sizeof(changed));
	failed += expect("factory record with erased banks",
		en7572_calibration_factory_record_valid(record, sizeof(record)),
		false);

	return failed ? 1 : 0;
}
