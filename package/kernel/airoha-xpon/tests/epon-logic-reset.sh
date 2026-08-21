#!/bin/sh
set -eu

ROOT="$(CDPATH= cd -- "$(dirname -- "$0")/../../../.." && pwd)"
TEST_DIR="$ROOT/package/kernel/airoha-xpon/tests"
DRIVER="$TEST_DIR/../src/airoha-epon.c"
OUT="$(mktemp)"
trap 'rm -f "$OUT" "$OUT".init "$OUT".sdk "$OUT".sdk-mac "$OUT".sdk-reset "$OUT".sdk-ready' EXIT

cc -std=c11 -Wall -Wextra -Werror \
	-I"$TEST_DIR/../src" "$TEST_DIR/epon-logic-reset-test.c" -o "$OUT"
"$OUT"

SDK_ROOT="${AIROHA_SDK_ROOT:-$ROOT/../tmp/airoha-sdk/airoha_sdk}"
SDK_IC="$SDK_ROOT/private/xpon_10g/src/ic/AN7581.c"
SDK_ACT="$SDK_ROOT/private/xpon_10g/src/epon/epon_act.c"
[ -f "$SDK_IC" ]
[ -f "$SDK_ACT" ]
sed -n '/int an7581_epon_mac_logic_reset(/,/^}/p' "$SDK_IC" > "$OUT.sdk"
grep -q 'Raw = GET_SSR3()' "$OUT.sdk"
grep -q 'Raw |= EPON_LOGIC_RESET_BIT' "$OUT.sdk"
grep -q 'Raw &= (~EPON_LOGIC_RESET_BIT)' "$OUT.sdk"
grep -q 'SET_SSR3(Raw)' "$OUT.sdk"

sed -n '/int an7581_pon_mac_scu_reset(/,/^}/p' "$SDK_IC" > "$OUT.sdk-mac"
grep -q 'Raw = GET_SCU_RSTCTRL1()' "$OUT.sdk-mac"
grep -q 'Raw |= SCU_PON_MAC_RESET' "$OUT.sdk-mac"
grep -q 'Raw &= ~SCU_PON_MAC_RESET' "$OUT.sdk-mac"
grep -q 'SET_SCU_RSTCTRL1(Raw)' "$OUT.sdk-mac"
grep -q 'udelay(1)' "$OUT.sdk-mac"

sed -n '/int epon_mac_reset(/,/^}/p' "$SDK_ACT" > "$OUT.sdk-reset"
grep -q 'epon_logic_state = EPON_LOGIC_RESET_HOLD_ON' "$OUT.sdk-reset"
grep -q 'REGISTER_ACTION_PON_MAC_SCU_RESET' "$OUT.sdk-reset"
grep -q 'REGISTER_ACTION_EPON_MAC_LOGIC_RESET' "$OUT.sdk-reset"

sed -n '/int epon_phy_ready_hw_init(/,/^}/p' "$SDK_ACT" > "$OUT.sdk-ready"
grep -q 'epon_logic_state = EPON_LOGIC_RESET_HOLD_OFF' "$OUT.sdk-ready"
grep -q 'REGISTER_ACTION_EPON_MAC_LOGIC_RESET' "$OUT.sdk-ready"

sed -n '/static int en7581_epon_hw_initialize(/,/^}/p' "$DRIVER" > "$OUT.init"
mac_reset_line="$(grep -n 'en7581_epon_scu_pon_mac_reset_ops' "$OUT.init" |
	cut -d: -f1)"
logic_reset_line="$(grep -n 'en7581_epon_scu_logic_reset_ops' "$OUT.init" |
	head -n 1 | cut -d: -f1)"
mac_init_line="$(grep -n 'airoha_epon_mac_init_transaction' "$OUT.init" |
	cut -d: -f1)"
logic_release_line="$(grep -n 'en7581_epon_scu_logic_reset_ops' "$OUT.init" |
	tail -n 1 | cut -d: -f1)"
burst_line="$(grep -n 'en7581_epon_set_tx_burst_mode' "$OUT.init" |
	cut -d: -f1)"
[ "$mac_reset_line" -lt "$logic_reset_line" ]
[ "$logic_reset_line" -lt "$mac_init_line" ]
[ "$mac_init_line" -lt "$logic_release_line" ]
[ "$logic_release_line" -lt "$burst_line" ]
grep -q 'en7581_epon_scu_logic_reset_ops, priv, true' "$OUT.init"
grep -q 'en7581_epon_scu_logic_reset_ops, priv, false' "$OUT.init"
! grep -q 'airoha_epon_logic_reset_pulse.*en7581_epon_scu_logic_reset_ops' \
	"$OUT.init"
grep -q '#define EN7581_SCU_RSTCTRL1.*0x834' "$DRIVER"
grep -q '#define  EN7581_SCU_PON_MAC_RESET.*BIT(31)' "$DRIVER"
grep -q 'regmap_update_bits(priv->scu, EN7581_SCU_RSTCTRL1' "$DRIVER"
grep -q 'regmap_read(priv->scu, EN7581_SCU_RSTCTRL1' "$DRIVER"
! grep -q 'regmap_set_bits(priv->scu, EN7581_SCU_SSR3' "$OUT.init"
! grep -q 'regmap_clear_bits(priv->scu, EN7581_SCU_SSR3' "$OUT.init"
grep -q 'regmap_read(priv->scu, EN7581_SCU_SSR3' "$DRIVER"
grep -q 'en7581_epon_mac_init_fail_closed' "$DRIVER"
echo "EPON PON-MAC pulse and SSR3 hold/release SDK checks passed"
