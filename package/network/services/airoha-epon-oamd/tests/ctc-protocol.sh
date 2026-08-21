#!/bin/sh
set -eu

base="$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)"
daemon="$base/../src/airoha-epon-oamd.c"
temporary="$(mktemp -d)"
trap 'rm -rf "$temporary"' EXIT INT TERM

cc -std=c11 -Wall -Wextra -Werror \
	-I"$base/../src" "$base/ctc-protocol-test.c" \
	-o "$temporary/ctc-protocol-test"
"$temporary/ctc-protocol-test"

cmp "$base/../../../../kernel/airoha-xpon/src/airoha-epon-oam-abi.h" \
	"$base/../src/airoha-epon-oam-abi.h"
grep -F '\"ctc_management\":true' "$daemon" >/dev/null
grep -F '\"ctc_classification\":true' "$daemon" >/dev/null

echo "CTC objects, UNI links, authentication, churning and ABI tests passed"
