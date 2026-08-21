#!/bin/sh
set -eu

script_dir="$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)"
openwrt_dir="${1:-$(CDPATH= cd -- "$script_dir/../../../.." && pwd)}"
sdk_archive="${AIROHA_SDK_ARCHIVE:-$openwrt_dir/../airoha_sdk.tar.gz}"
driver="$openwrt_dir/package/kernel/airoha-xpon/src/airoha-xgspon.c"
security="$openwrt_dir/package/kernel/airoha-xpon/src/airoha-xgs-security.c"
transaction="$openwrt_dir/package/kernel/airoha-xpon/src/airoha-xgs-eqd.h"
test_source="$script_dir/xgs-eqd-test.c"
tmp_dir="$(mktemp -d "${TMPDIR:-/tmp}/en7581-xgs-eqd.XXXXXX")"
trap 'rm -rf "$tmp_dir"' EXIT INT TERM

[ -f "$sdk_archive" ] || {
	echo "Airoha SDK archive not found at $sdk_archive" >&2
	exit 2
}

tar -xOf "$sdk_archive" airoha_sdk/private/xpon_10g/src/gpon/gpon_ploam.c \
	> "$tmp_dir/gpon_ploam.c"
tar -xOf "$sdk_archive" airoha_sdk/private/xpon_10g/src/gpon/gpon_dev.c \
	> "$tmp_dir/gpon_dev.c"

require_fixed() {
	pattern="$1"
	file="$2"
	description="$3"

	if ! grep -Fq -- "$pattern" "$file"; then
		echo "missing $description: $pattern" >&2
		exit 1
	fi
}

require_fixed 'GPON_CURR_STATE == GPON_10G_STATE_O4' \
	"$tmp_dir/gpon_ploam.c" 'SDK O4 ranging state gate'
require_fixed 'ignore directed relative ranging time set in O4' \
	"$tmp_dir/gpon_ploam.c" 'SDK O4 relative rejection'
require_fixed 'GPON_CURR_STATE == GPON_10G_STATE_O5 ||  GPON_CURR_STATE == GPON_10G_STATE_O9' \
	"$tmp_dir/gpon_ploam.c" 'SDK operational ranging state gate'
require_fixed 'gpGponPriv->gponCfg.eqd_olt_absolute + newEqd' \
	"$tmp_dir/gpon_ploam.c" 'SDK positive relative EqD conversion'
require_fixed 'gpGponPriv->gponCfg.eqd_olt_absolute - newEqd' \
	"$tmp_dir/gpon_ploam.c" 'SDK negative relative EqD conversion'
require_fixed 'msgDestId == PLOAM_BROADCAST_ADDR' \
	"$tmp_dir/gpon_ploam.c" 'SDK relative EqD broadcast acceptance'
require_fixed 'newEqd = (newEqd << 2);' "$tmp_dir/gpon_dev.c" \
	'SDK XGS-PON relative EqD scaling'
require_fixed 'gponEqd.Bits.eqd += newEqd;' "$tmp_dir/gpon_dev.c" \
	'SDK hardware positive EqD adjustment'
require_fixed 'gponEqd.Bits.eqd -= newEqd;' "$tmp_dir/gpon_dev.c" \
	'SDK hardware negative EqD adjustment'

require_fixed '#include "airoha-xgs-eqd.h"' "$driver" \
	'production EqD transaction include'
require_fixed 'activation_state == EN7581_XGSPON_ACTIVATION_O4' "$driver" \
	'production O4 absolute-only state gate'
require_fixed 'activation_state == EN7581_XGSPON_ACTIVATION_O5' "$driver" \
	'production O5 relative adjustment state gate'
require_fixed 'priv->equalization_delay = resolved;' "$driver" \
	'absolute EqD software commit after hardware success'
require_fixed 'ranging.directed && ranging.equalization_delay' "$driver" \
	'directed nonzero O5 ACK gate'
require_fixed 'destination != AIROHA_XGS_BROADCAST_ONU_ID' "$security" \
	'authenticated standard broadcast relative Ranging Time'
require_fixed 'destination != AIROHA_XGS_BROADCAST_XGS_ONU_ID' "$security" \
	'authenticated XGS broadcast relative Ranging Time'
require_fixed 'if (rollback_ret && ops->fail_closed)' "$transaction" \
	'rollback failure fail-closed hook'

cc -std=c11 -Wall -Wextra -Werror -pedantic \
	-I"$(dirname "$transaction")" "$test_source" -o "$tmp_dir/test"
"$tmp_dir/test"

echo 'EN7581 XG-PON/XGS-PON relative EqD matches SDK and rolls back transactionally'
