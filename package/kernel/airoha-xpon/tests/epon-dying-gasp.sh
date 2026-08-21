#!/bin/sh
set -eu

ROOT="$(CDPATH= cd -- "$(dirname -- "$0")/../../../.." && pwd)"
TEST_DIR="$ROOT/package/kernel/airoha-xpon/tests"
DRIVER="$TEST_DIR/../src/airoha-epon.c"
OUT="$(mktemp)"
trap 'rm -f "$OUT" "$OUT".init "$OUT".ready' EXIT INT TERM

cc -std=c11 -Wall -Wextra -Werror \
	-I"$TEST_DIR/../src" "$TEST_DIR/epon-dying-gasp-test.c" -o "$OUT"
"$OUT"

SDK_ROOT="${AIROHA_SDK_ROOT:-$ROOT/../tmp/airoha-sdk/airoha_sdk}"
SDK_IC="$SDK_ROOT/private/xpon_10g/src/ic/AN7581.c"
SDK_ACT="$SDK_ROOT/private/xpon_10g/src/epon/epon_act.c"
[ -f "$SDK_IC" ] && [ -f "$SDK_ACT" ]

sed -n '/int epon_phy_ready_hw_init(/,/^}/p' "$SDK_ACT" > "$OUT.ready"
release_line="$(grep -n 'EPON_LOGIC_RESET_HOLD_OFF' "$OUT.ready" |
	cut -d: -f1)"
burst_line="$(grep -n 'epon_dev_set_tx_burst_mode(TRUE)' "$OUT.ready" |
	cut -d: -f1)"
dying_gasp_line="$(grep -n 'REGISTER_ACTION_EPON_SET_DYGASP_HW_EN' \
	"$OUT.ready" | cut -d: -f1)"
[ "$release_line" -lt "$burst_line" ]
[ "$burst_line" -lt "$dying_gasp_line" ]
grep -q 'e_dyinggsp_cfg.*0x62ac, 0x00000100, 0x8000ff00' "$SDK_IC"
grep -q 'e_dyinggsp_cfg_SET_hw_dygasp_en' "$SDK_IC"

sed -n '/static int en7581_epon_hw_initialize(/,/^}/p' "$DRIVER" > "$OUT.init"
release_line="$(grep -n 'en7581_epon_scu_logic_reset_ops, priv, false' \
	"$OUT.init" | cut -d: -f1)"
burst_line="$(grep -n 'en7581_epon_set_tx_burst_mode(priv, true)' \
	"$OUT.init" | cut -d: -f1)"
dying_gasp_line="$(grep -n 'airoha_epon_dying_gasp_set' "$OUT.init" |
	cut -d: -f1)"
[ "$release_line" -lt "$burst_line" ]
[ "$burst_line" -lt "$dying_gasp_line" ]
grep -q '&en7581_epon_dying_gasp_ops, priv, true' "$OUT.init"
grep -q 'regmap_update_bits(priv->scu, EN7581_SCU_SSR3' "$DRIVER"
grep -q 'EN7581_SCU_EPON_LOGIC_RESET);' "$DRIVER"
echo "EPON post-reset Dying Gasp SDK ordering checks passed"
