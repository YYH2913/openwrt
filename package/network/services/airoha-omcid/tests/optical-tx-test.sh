#!/bin/sh
set -eu

ROOT=$(CDPATH= cd -- "$(dirname "$0")/../../../../.." && pwd)
PROGRAM="$ROOT/package/network/services/airoha-omcid/files/airoha-optical-tx-test"
temporary=$(mktemp -d)
trap 'rm -rf "$temporary"' EXIT HUP INT TERM

	mkdir -p "$temporary/bin" \
		"$temporary/sys/devices/platform/xpon-controller" \
		"$temporary/sys/bus/i2c/drivers/airoha-en7572/0-0050" \
		"$temporary/sys/bus/platform/drivers/airoha-gpon/gpon0" \
	"$temporary/sys/bus/platform/drivers/airoha-xgspon/xgspon0" \
	"$temporary/sys/bus/platform/drivers/airoha-en7581-epon/epon0"
cat > "$temporary/bin/id" <<'EOF'
#!/bin/sh
[ "${1:-}" = -u ] || exit 2
printf '%s\n' "${AIROHA_TEST_UID:-0}"
EOF
cat > "$temporary/bin/sleep" <<'EOF'
#!/bin/sh
exit 0
EOF
chmod +x "$temporary/bin/id" "$temporary/bin/sleep"

core="$temporary/sys/devices/platform/xpon-controller"
factory="$core/factory_tx_test"
mode="$core/mode"
switch_state="$core/switch_state"
gpon_init="$temporary/sys/bus/platform/drivers/airoha-gpon/gpon0/init_status"
bosa="$temporary/sys/bus/i2c/drivers/airoha-en7572/0-0050"
printf '%s\n' \
	'allowed=1 active=0 wavelength_nm=0 remaining_ms=0 tx_disabled=1 protocol_enabled=0' \
	> "$factory"
printf '%s\n' xgspon > "$mode"
printf '%s\n' \
	'switching=0 stage=16 failed_stage=0 error=0 committed=1 rollback_failed=0 tx_disabled=1 fault_locked=0' \
	> "$switch_state"
printf '%s\n' \
	'attempts=0 stage=never error=0 bosa_mode=invalid bosa_ready=0 tx_disabled=0 phy_setting=0x00000000 errcnt_enable=0x00000000 pma_setting0=0x00000000 pma_setting1=0x00000000 pma_interrupt_enable=0x00000000 scu_gpio_force=0x00000000' \
	> "$gpon_init"
printf '%s\n' 'experimental-a2-copy:gpon-a0' > "$bosa/calibration_source"
printf '%s\n' \
	'dcl_ctrl2=0x064a0478 configured_iav=1144 configured_imod=1610 csr_iav=0x00000508 actual_iav=1288 csr_ibias_imod=0x067c01c6 actual_ibias=454 actual_imod=1660 tx_status=0x00001818 tx_disabled=0 system_status=0x00000001 ben=1' \
	> "$bosa/tx_diagnostics"
printf '0\n' > "$temporary/sys/bus/platform/drivers/airoha-gpon/gpon0/enabled"
printf '0\n' > "$temporary/sys/bus/platform/drivers/airoha-xgspon/xgspon0/enabled"
printf '0\n' > "$temporary/sys/bus/platform/drivers/airoha-en7581-epon/epon0/enabled"
export PATH="$temporary/bin:$PATH"
export AIROHA_OPTICAL_TX_SYSFS_ROOT="$temporary/sys"

expect_exit() {
	expected=$1
	shift
	set +e
	"$@" > "$temporary/stdout" 2> "$temporary/stderr"
	actual=$?
	set -e
	[ "$actual" -eq "$expected" ] || {
		echo "expected exit $expected, got $actual: $*" >&2
		cat "$temporary/stderr" >&2
		exit 1
	}
}

"$PROGRAM" status > "$temporary/status"
cmp -s "$factory" "$temporary/status"
"$PROGRAM" --help > "$temporary/help" 2>&1
grep -Fq 'start {1270|1310}' "$temporary/help"
"$PROGRAM" diagnostics > "$temporary/diagnostics"
grep -Fq 'mode: xgspon' "$temporary/diagnostics"
grep -Fq 'BOSA calibration: experimental-a2-copy:gpon-a0' "$temporary/diagnostics"
grep -Fq 'EN7572 TX: dcl_ctrl2=0x064a0478' "$temporary/diagnostics"
grep -Fq 'GPON init: attempts=0 stage=never' "$temporary/diagnostics"
expect_exit 2 "$PROGRAM" 1310 10
expect_exit 2 "$PROGRAM" --confirm 1490 10
expect_exit 2 "$PROGRAM" prepare 1490
expect_exit 2 "$PROGRAM" --confirm 1270 9
expect_exit 2 "$PROGRAM" --confirm 1270 5001
expect_exit 2 "$PROGRAM" --confirm 1270 10 extra
expect_exit 2 "$PROGRAM" --confirm WRONG-TOKEN 1270 10
expect_exit 3 "$PROGRAM" --confirm 1310 10
expect_exit 1 env AIROHA_TEST_UID=1000 "$PROGRAM" prepare 1310
expect_exit 2 "$PROGRAM" off extra

printf '%s\n' \
	'allowed=1 active=1 wavelength_nm=1270 remaining_ms=10 tx_disabled=0 protocol_enabled=0' \
	> "$factory"
expect_exit 4 "$PROGRAM" prepare 1310
[ "$(cat "$mode")" = xgspon ]

printf '%s\n' \
	'allowed=1 active=0 wavelength_nm=0 remaining_ms=0 tx_disabled=0 protocol_enabled=0' \
	> "$factory"
expect_exit 4 "$PROGRAM" prepare 1310
[ "$(cat "$mode")" = xgspon ]

printf '%s\n' \
	'allowed=1 active=0 wavelength_nm=0 remaining_ms=0 tx_disabled=1 protocol_enabled=0' \
	> "$factory"
printf '2\n' > "$temporary/sys/bus/platform/drivers/airoha-gpon/gpon0/enabled"
expect_exit 1 "$PROGRAM" prepare 1310
[ "$(cat "$mode")" = xgspon ]
printf '0\n' > "$temporary/sys/bus/platform/drivers/airoha-gpon/gpon0/enabled"

printf '1\n' > "$temporary/sys/bus/platform/drivers/airoha-xgspon/xgspon0/enabled"
expect_exit 4 "$PROGRAM" --confirm 1270 10
[ "$(cat "$factory")" = 'allowed=1 active=0 wavelength_nm=0 remaining_ms=0 tx_disabled=1 protocol_enabled=0' ]
printf '0\n' > "$temporary/sys/bus/platform/drivers/airoha-xgspon/xgspon0/enabled"

printf '1\n' > "$temporary/sys/bus/platform/drivers/airoha-xgspon/xgspon0/enabled"
expect_exit 4 "$PROGRAM" prepare 1310
[ "$(cat "$mode")" = xgspon ]
printf '0\n' > "$temporary/sys/bus/platform/drivers/airoha-xgspon/xgspon0/enabled"

printf '%s\n' \
	'switching=0 stage=10 failed_stage=11 error=-5 committed=0 rollback_failed=0 tx_disabled=1 fault_locked=0' \
	> "$switch_state"
expect_exit 1 "$PROGRAM" prepare 1310
[ "$(cat "$mode")" = gpon ]
printf '%s\n' xgspon > "$mode"

printf '%s\n' \
	'switching=0 stage=10 failed_stage=11 error=0 committed=1 rollback_failed=1 tx_disabled=1 fault_locked=1' \
	> "$switch_state"
expect_exit 1 "$PROGRAM" prepare 1310
[ "$(cat "$mode")" = gpon ]
printf '%s\n' xgspon > "$mode"
printf '%s\n' \
	'switching=0 stage=16 failed_stage=0 error=0 committed=1 rollback_failed=0 tx_disabled=1 fault_locked=0' \
	> "$switch_state"

"$PROGRAM" prepare 1310 > "$temporary/prepare"
[ "$(cat "$mode")" = gpon ]

"$PROGRAM" --confirm 1310 5000 > "$temporary/request"
[ "$(cat "$factory")" = 'XG2010G-OPTICAL-TEST 1310 5000' ]
expect_exit 1 env AIROHA_TEST_UID=1000 "$PROGRAM" off
[ "$(cat "$factory")" = 'XG2010G-OPTICAL-TEST 1310 5000' ]
"$PROGRAM" off
[ "$(cat "$factory")" = 'XG2010G-OPTICAL-TEST off' ]

printf '%s\n' \
	'allowed=1 active=0 wavelength_nm=0 remaining_ms=0 tx_disabled=1 protocol_enabled=0' \
	> "$factory"
"$PROGRAM" prepare 1270 > "$temporary/prepare"
[ "$(cat "$mode")" = xgspon ]
"$PROGRAM" --confirm 1270 10 > "$temporary/request"
[ "$(cat "$factory")" = 'XG2010G-OPTICAL-TEST 1270 10' ]
"$PROGRAM" start 1270 10 > "$temporary/request"
[ "$(cat "$factory")" = 'XG2010G-OPTICAL-TEST 1270 10' ]
"$PROGRAM" start XG2010G-OPTICAL-TEST 1270 10 > "$temporary/request"
[ "$(cat "$factory")" = 'XG2010G-OPTICAL-TEST 1270 10' ]
"$PROGRAM" stop
[ "$(cat "$factory")" = 'XG2010G-OPTICAL-TEST off' ]

echo 'optical TX command validates preparation, confirmation, wavelength, duration, mode and stop'
