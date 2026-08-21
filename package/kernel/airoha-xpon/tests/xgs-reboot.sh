#!/bin/sh
set -eu

ROOT="$(CDPATH= cd -- "$(dirname -- "$0")/../../../.." && pwd)"
TEST_DIR="$ROOT/package/kernel/airoha-xpon/tests"
OUT="$(mktemp)"
trap 'rm -f "$OUT"' EXIT

cc -std=c11 -Wall -Wextra -Werror \
	-I"$TEST_DIR/../src" "$TEST_DIR/xgs-reboot-test.c" -o "$OUT"
"$OUT"

SDK_ROOT="${AIROHA_SDK_ROOT:-$ROOT/../tmp/airoha-sdk/airoha_sdk}"
sdk_ploam="$SDK_ROOT/private/xpon_10g/src/gpon/gpon_ploam.c"
sdk_raw="$SDK_ROOT/private/xpon_10g/inc/gpon/gpon_ploam_raw.h"
driver="$TEST_DIR/../src/airoha-xgspon.c"
security="$TEST_DIR/../src/airoha-xgs-security.c"

grep -F '#define PLOAM_DOWN_MSG_REBOOT_ONU' "$sdk_raw" >/dev/null
grep -F 'ploam_recv_reboot_onu' "$sdk_ploam" >/dev/null
grep -F 'XMCS_EVENT_GPON_REBOOT_ONU' "$sdk_ploam" >/dev/null
grep -F 'airoha_xgs_authenticate_reboot_onu_words' "$security" >/dev/null
grep -F 'AIROHA_XPON_EVENT=reboot-request' "$driver" >/dev/null
if grep -E 'kernel_(restart|power_off)|orderly_reboot' "$driver" >/dev/null; then
	echo 'Reboot ONU must be reported to userspace, not executed in IRQ context' >&2
	exit 1
fi
echo 'XG-PON/XGS-PON Reboot ONU authentication and event boundary matches SDK'
