#!/bin/sh
# SPDX-License-Identifier: GPL-2.0-only
set -eu

input="$1"
output="$2"

if [ "$(wc -c < "$input")" -ne 512 ]; then
	echo "$input: expected a 512-byte EN7572 BOB table" >&2
	exit 1
fi

cp -- "$input" "$output"

# A0 also owns the table-wide identity fields. Copy the complete A2 bank over
# A0, then restore those fields so the resulting dual-bank table remains valid.
dd if="$input" of="$output" bs=256 skip=1 seek=0 count=1 conv=notrunc status=none
dd if="$input" of="$output" bs=1 skip=20 seek=20 count=16 conv=notrunc status=none
dd if="$input" of="$output" bs=1 skip=40 seek=40 count=16 conv=notrunc status=none
