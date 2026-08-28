#!/bin/sh

set -eu

openwrt_root="${1:?OpenWrt tree path is required}"
package="$openwrt_root/package/network/services/airoha-omcid"
program="$package/files/airoha-gpon-config"
init_script="$package/files/airoha-gpon-config.init"
functions="$package/tests/gpon-config-functions"
temporary="$(mktemp -d)"
gpon="$temporary/sys/bus/platform/drivers/airoha-gpon/gpon0"
xgspon="$temporary/sys/bus/platform/drivers/airoha-xgspon/xgspon0"
epon="$temporary/sys/bus/platform/drivers/airoha-en7581-epon/epon0"
xpon="$temporary/sys/bus/platform/drivers/airoha-xpon-core/xpon0"
monitor_pid=

cleanup() {
	[ -z "$monitor_pid" ] || kill "$monitor_pid" 2>/dev/null || true
	rm -rf "$temporary"
}
trap cleanup EXIT INT TERM

grep -Fq 'procd_set_param command /bin/sh "$PROG" monitor' "$init_script" || {
	echo 'PON monitor must use an explicit shell under procd' >&2
	exit 1
}
if grep -Eq 'procd_set_param (stdout|stderr)' "$init_script"; then
	echo 'PON monitor must not enable procd libsetlbf logging' >&2
	exit 1
fi
grep -Fq 'wait_for_selected_backend || exit 1' "$program" || {
	echo 'PON monitor must wait for the selected backend before syncing' >&2
	exit 1
}
grep -Fq 'devices/platform/*xgspon*' "$program" || {
	echo 'PON monitor must support direct platform-device sysfs paths' >&2
	exit 1
}

mkdir -p "$gpon" "$xgspon" "$epon" "$xpon"
printf '0\n' > "$gpon/enabled"
printf '5 active\n' > "$gpon/state"
printf '2:100\n' > "$gpon/tconts"
printf 'off\n' > "$gpon/data_gem"
: > "$gpon/serial_number"
: > "$gpon/password"
printf '0\n' > "$xgspon/enabled"
printf '0\n' > "$xgspon/activation_ready"
printf '0\n' > "$xgspon/ready"
printf 'disabled\n' > "$xgspon/state"
printf 'phy calibration tc-ploam secure-omcc xgem\n' > "$xgspon/activation_blockers"
printf '0\n' > "$xgspon/registration_id_configured"
: > "$xgspon/serial_number"
: > "$xgspon/registration_id"
printf '0\n' > "$epon/enabled"
printf 'unset\n' > "$epon/onu_mac"
printf '0x00000001\n' > "$epon/llid_mask"
printf '60\n' > "$epon/silent_time"
printf 'gpon\n' > "$xpon/mode"
printf 'switching=0 stage=18 error=0 committed=1 rollback_failed=0 tx_disabled=1 fault_locked=0\n' > "$xpon/switch_state"

run_sync() {
	AIROHA_GPON_FUNCTIONS="$functions" \
	AIROHA_GPON_SYSFS_ROOT="$temporary/sys" \
	AIROHA_GPON_STATE_DIR="$temporary/run" \
	AIROHA_GPON_MANAGE_CONTROL_PLANES=0 \
	AIROHA_TEST_ENABLED="$2" \
	AIROHA_TEST_OMCI_MODE="$1" \
	AIROHA_TEST_MANUAL_GEM="$3" \
	AIROHA_TEST_GEM_ID="$4" \
	AIROHA_TEST_TCONT="$5" \
		AIROHA_TEST_ENCRYPTED="$6" \
		AIROHA_TEST_PASSWORD="${7:-}" \
		AIROHA_TEST_PON_MODE="${8:-gpon}" \
		AIROHA_TEST_REGISTRATION_ID="${9:-}" \
		AIROHA_TEST_EPON_ONU_MAC="${10:-02:11:22:33:44:55}" \
		sh "$program" sync
}

run_sync_managed_epon() {
	AIROHA_GPON_FUNCTIONS="$functions" \
	AIROHA_GPON_SYSFS_ROOT="$temporary/sys" \
	AIROHA_GPON_STATE_DIR="$temporary/run" \
	AIROHA_GPON_SERVICE_ROOT="$temporary/services" \
	AIROHA_GPON_EPON_OAM_READY="$temporary/epon-oam-ready" \
	AIROHA_TEST_SERVICE_LOG="$temporary/service.log" \
	AIROHA_TEST_EPON_ENABLED="$epon/enabled" \
	AIROHA_TEST_EPON_READY="$temporary/epon-oam-ready" \
	AIROHA_TEST_ENABLED=1 \
	AIROHA_TEST_OMCI_MODE=auto \
	AIROHA_TEST_MANUAL_GEM=0 \
	AIROHA_TEST_GEM_ID=0 \
	AIROHA_TEST_TCONT=0 \
	AIROHA_TEST_ENCRYPTED=0 \
	AIROHA_TEST_PASSWORD= \
	AIROHA_TEST_PON_MODE=epon-10g-1g \
	AIROHA_TEST_REGISTRATION_ID= \
	AIROHA_TEST_EPON_ONU_MAC=02:aa:bb:cc:dd:ee \
		sh "$program" sync
}

expect_file() {
	actual="$(cat "$1")"
	[ "$actual" = "$2" ] || {
		echo "$1 = $actual, want $2" >&2
		exit 1
	}
}

# A manual selection owns the exact legacy data_gem setting it installed.
run_sync manual 1 1 42 2 1
expect_file "$gpon/data_gem" '42 2 1'
expect_file "$temporary/run/manual-gem" '42 2 1'

# Switching to OMCI clears only the configuration that was installed manually.
run_sync auto 1 0 0 0 0
expect_file "$gpon/data_gem" off
[ ! -e "$temporary/run/manual-gem" ]

# Do not remove a multi-GEM configuration that the OMCI backend replaced while
# the config service was being reloaded.
run_sync manual 1 1 43 2 0
printf 'multi 2\n' > "$gpon/data_gem"
run_sync auto 1 0 0 0 0
expect_file "$gpon/data_gem" 'multi 2'
[ ! -e "$temporary/run/manual-gem" ]

# Disabling the manual channel retracts a still-owned legacy setting as well.
run_sync manual 1 1 44 2 0
run_sync manual 1 0 0 0 0
expect_file "$gpon/data_gem" off

# The explicit hex form preserves all ten bytes, including an all-zero value.
run_sync auto 1 0 0 0 0 hex:00000000000000000000
expect_file "$gpon/password" hex:00000000000000000000

# A reserved hex: value must contain exactly ten encoded bytes.
if run_sync auto 1 0 0 0 0 hex:0000; then
	echo "short hexadecimal PLOAM password was accepted" >&2
	exit 1
fi

# Printable passwords remain compatible with existing configurations.
run_sync auto 1 0 0 0 0 legacy123
expect_file "$gpon/password" legacy123

# A failed XGS-PON readiness check rolls the runtime owner back to GPON.
printf '1\n' > "$gpon/enabled"
if run_sync auto 1 0 0 0 0 '' xgspon registration-123; then
	echo "XGS-PON activation bypassed the readiness gate" >&2
	exit 1
fi
expect_file "$xpon/mode" gpon
expect_file "$gpon/enabled" 1
expect_file "$xgspon/enabled" 0
expect_file "$xgspon/serial_number" HWTC12345678
expect_file "$xgspon/registration_id" registration-123

# The exact 36-byte binary form is accepted without disclosing it via status.
registration_hex=hex:000102030405060708090a0b0c0d0e0f101112131415161718191a1b1c1d1e1f20212223
if run_sync auto 1 0 0 0 0 '' xgspon "$registration_hex"; then
	echo "XGS-PON activation bypassed the readiness gate" >&2
	exit 1
fi
expect_file "$xgspon/registration_id" "$registration_hex"

if run_sync auto 1 0 0 0 0 '' xgspon hex:0011; then
	echo "short hexadecimal Registration-ID was accepted" >&2
	exit 1
fi

# Both EPON line rates use the dedicated endpoint and preserve the configured
# SDK-compatible ONU MAC/LLID values. EPON does not touch OMCI identity files.
run_sync auto 0 0 0 0 0 '' epon-10g-1g '' 02:aa:bb:cc:dd:ee
expect_file "$xpon/mode" epon-10g-1g
expect_file "$epon/onu_mac" 02:aa:bb:cc:dd:ee
expect_file "$epon/enabled" 0

run_sync auto 1 0 0 0 0 '' epon-10g-10g '' 02:aa:bb:cc:dd:ee
expect_file "$xpon/mode" epon-10g-10g
expect_file "$epon/enabled" 1

# The coordinator keeps EPON disabled until its dedicated OAM service owns the
# endpoint, then stops the mutually exclusive OMCI control plane.
mkdir -p "$temporary/services"
printf '%s\n' '#!/bin/sh' \
	'current="$(cat "$AIROHA_TEST_EPON_ENABLED")"' \
	'printf "epon:%s:enabled=%s\n" "$1" "$current" >> "$AIROHA_TEST_SERVICE_LOG"' \
	'[ "$1" != restart ] || [ "$current" = 0 ] || exit 1' \
	'[ "$1" != restart ] || printf "pid=1 control_plane=epon-oam pon_mode=%s omci=0\n" "$AIROHA_TEST_PON_MODE" > "$AIROHA_TEST_EPON_READY"' \
	> "$temporary/services/airoha-epon-oamd"
printf '%s\n' '#!/bin/sh' \
	'printf "omci:%s\n" "$1" >> "$AIROHA_TEST_SERVICE_LOG"' \
	> "$temporary/services/airoha-omcid"
chmod +x "$temporary/services/airoha-epon-oamd" "$temporary/services/airoha-omcid"
run_sync_managed_epon
expect_file "$xpon/mode" epon-10g-1g
expect_file "$epon/enabled" 1
grep -qx 'epon:restart:enabled=0' "$temporary/service.log"
grep -qx 'omci:stop' "$temporary/service.log"

if run_sync auto 1 0 0 0 0 '' epon-10g-1g '' 01:aa:bb:cc:dd:ee; then
	echo "multicast EPON ONU MAC was accepted" >&2
	exit 1
fi
expect_file "$xpon/mode" epon-10g-1g

# The procd instance must remain resident outside GPON as well. LuCI performs
# its own synchronous transaction and does not reload this monitor after a
# later hot switch back to manual GPON provisioning.
AIROHA_GPON_FUNCTIONS="$functions" \
AIROHA_GPON_SYSFS_ROOT="$temporary/sys" \
AIROHA_GPON_STATE_DIR="$temporary/run" \
AIROHA_GPON_MANAGE_CONTROL_PLANES=0 \
AIROHA_TEST_ENABLED=0 \
AIROHA_TEST_OMCI_MODE=auto \
AIROHA_TEST_MANUAL_GEM=0 \
AIROHA_TEST_GEM_ID=0 \
AIROHA_TEST_TCONT=0 \
AIROHA_TEST_ENCRYPTED=0 \
AIROHA_TEST_PASSWORD= \
AIROHA_TEST_PON_MODE=epon-10g-1g \
AIROHA_TEST_REGISTRATION_ID= \
AIROHA_TEST_EPON_ONU_MAC=02:aa:bb:cc:dd:ee \
	sh "$program" monitor &
monitor_pid=$!
sleep 0.2
kill -0 "$monitor_pid" 2>/dev/null || {
	echo 'PON monitor exited in EPON mode' >&2
	exit 1
}
kill "$monitor_pid"
wait "$monitor_pid" 2>/dev/null || true
monitor_pid=
