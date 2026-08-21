#!/bin/sh
set -eu

ROOT="$(CDPATH= cd -- "$(dirname -- "$0")/../../../.." && pwd)"
TEST_DIR="$ROOT/package/kernel/airoha-xpon/tests"
DRIVER="$ROOT/package/kernel/airoha-xpon/src/airoha-epon.c"
SDK_ROOT="${AIROHA_SDK_ROOT:-$ROOT/../tmp/airoha-sdk/airoha_sdk}"
SDK_ACT="$SDK_ROOT/private/xpon_10g/src/epon/epon_act.c"
OUT="$(mktemp)"
trap 'rm -f "$OUT" "$OUT.sdk" "$OUT.stop"' EXIT INT TERM

cc -std=c11 -Wall -Wextra -Werror -pedantic \
	-I"$TEST_DIR/../src" "$TEST_DIR/epon-stop-test.c" -o "$OUT"
"$OUT"

[ -f "$SDK_ACT" ]
sed -n '/int epon_stop(/,/^}/p' "$SDK_ACT" > "$OUT.sdk"
grep -q 'epon_dev_tx_rx_disable();' "$OUT.sdk"
grep -q 'epon_clear_all_queue_threshold_cfg();' "$OUT.sdk"
grep -q 'epon_clear_all_llid_info();' "$OUT.sdk"

sed -n '/static int en7581_epon_stop_mac(/,/^}/p' "$DRIVER" > "$OUT.stop"
grep -q 'airoha_epon_stop_cleanup(&en7581_epon_stop_ops, priv)' "$OUT.stop"
grep -q '\.clear_report_thresholds = en7581_epon_stop_clear_report_thresholds' \
	"$DRIVER"
grep -q 'return en7581_epon_clear_report_thresholds(context);' "$DRIVER"

echo "EPON stop cleanup matches SDK queue-threshold teardown"
