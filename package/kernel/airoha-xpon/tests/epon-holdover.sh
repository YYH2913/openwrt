#!/bin/sh
set -eu

ROOT="$(CDPATH= cd -- "$(dirname -- "$0")/../../../.." && pwd)"
TEST_DIR="$ROOT/package/kernel/airoha-xpon/tests"
DRIVER="$TEST_DIR/../src/airoha-epon.c"
OUT="$(mktemp)"
trap 'rm -f "$OUT"' EXIT INT TERM

cc -std=c11 -Wall -Wextra -Werror \
	-I"$TEST_DIR/../src" "$TEST_DIR/epon-holdover-test.c" -o "$OUT"
"$OUT"

SDK_ROOT="${AIROHA_SDK_ROOT:-$ROOT/../tmp/airoha-sdk/airoha_sdk}"
grep -q 'EPON_HOLD_OVER_TME_DEFAULT.*(1000)' \
	"$SDK_ROOT/private/xpon_10g/inc/epon/epon.h"
grep -q 'EPON_MSG_LOS_ENABLE_TYPEB' \
	"$SDK_ROOT/private/xpon_10g/src/epon/epon_phy_event.c"
grep -q 'EPON_DRIVER_CHECK(TYPE_B_ENABLE,TRUE)' \
	"$SDK_ROOT/private/xpon_10g/src/epon/epon_phy_event.c"
grep -q 'XPON_PHY_GET(PON_GET_PHY_IS_SYNC) == PHY_FALSE' \
	"$SDK_ROOT/private/xpon_10g/src/epon/epon_act.c"

grep -q 'struct delayed_work holdover_work' "$DRIVER"
grep -q 'cancel_delayed_work_sync(&priv->holdover_work)' "$DRIVER"
grep -q 'AIROHA_EPON_OAM_IOC_GET_HOLDOVER' "$DRIVER"
grep -q 'AIROHA_EPON_OAM_IOC_SET_HOLDOVER' "$DRIVER"
grep -q 'priv->holdover_blocked = !forced' "$DRIVER"

recovery_body="$(sed -n \
	'/^static void en7581_epon_phy_recovery_work/,/^}/p' "$DRIVER")"
start_line="$(printf '%s\n' "$recovery_body" | \
	grep -n -m1 'en7581_epon_holdover_start_locked' | cut -d: -f1)"
clear_line="$(printf '%s\n' "$recovery_body" | \
	grep -n -m1 'en7581_epon_clear_sessions_locked' | cut -d: -f1)"
[ "$start_line" -lt "$clear_line" ]
printf '%s\n' "$recovery_body" | \
	grep -q 'if (priv->holdover.active)'
printf '%s\n' "$recovery_body" | \
	grep -q 'en7581_epon_phy_set_rx(priv, true)'

phy_irq_body="$(sed -n \
	'/^static irqreturn_t en7581_epon_phy_irq_thread/,/^}/p' "$DRIVER")"
printf '%s\n' "$phy_irq_body" | \
	grep -q 'priv->removing || !priv->enabled || !priv->hardware_selected'
printf '%s\n' "$phy_irq_body" | \
	grep -q 'en7581_epon_holdover_start_locked'

echo "EPON SDK holdover correspondence checks passed"
