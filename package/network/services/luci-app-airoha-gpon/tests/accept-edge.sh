#!/bin/sh

set -eu

openwrt="${1:?OpenWrt tree path is required}"
program="$openwrt/package/network/services/luci-app-airoha-gpon/root/usr/sbin/airoha-xpon-accept-edge"
rpc="$openwrt/package/network/services/luci-app-airoha-gpon/root/usr/share/rpcd/ucode/airoha-gpon.uc"
transaction="$openwrt/package/network/services/luci-app-airoha-gpon/root/usr/share/rpcd/ucode/airoha-gpon-transaction.uc"
acl="$openwrt/package/network/services/luci-app-airoha-gpon/root/usr/share/rpcd/acl.d/luci-app-airoha-gpon.json"
temporary="$(mktemp -d)"
trap 'status=$?; rm -rf "$temporary"; exit "$status"' EXIT INT TERM

mkdir -p "$temporary/bin"
state="$temporary/state"
ubus_log="$temporary/ubus.log"
printf '%s\n' xgpon > "$state"

cat > "$temporary/bin/ubus" <<'EOF'
#!/bin/sh
request="${4:-}"
[ "${3:-}" = set_mode ] || exit 1
mode=
for candidate in gpon xgpon xgspon epon-10g-1g epon-10g-10g; do
	case "$request" in *\"pon_mode\"*\"$candidate\"*) mode="$candidate" ;; esac
done
[ -n "$mode" ] || exit 1
printf '%s\n' "$mode" >> "$AIROHA_TEST_UBUS_LOG"
printf '%s\n' "$mode" > "$AIROHA_TEST_STATE"
if [ "${AIROHA_TEST_RPC_FAIL_AFTER_MODE:-}" = "$mode" ] &&
	[ ! -e "$AIROHA_TEST_RPC_FAIL_MARKER" ]; then
	: > "$AIROHA_TEST_RPC_FAIL_MARKER"
	exit 1
fi
printf '{"success":true}\n'
EOF

cat > "$temporary/bin/jsonfilter" <<'EOF'
#!/bin/sh
case "$*" in
	*'@.success'*) case "$2" in *'"success":true'*) printf '%s\n' true ;; *) printf '%s\n' false ;; esac ;;
	*) exit 1 ;;
esac
EOF

cat > "$temporary/bin/collector" <<'EOF'
#!/bin/sh
mode="$(cat "$AIROHA_TEST_STATE")"
expected="${1:-}"
[ -z "$expected" ] || [ "$expected" = "$mode" ] || exit 1
sequence=0
if [ -n "${AIROHA_TEST_COUNTER_FILE:-}" ]; then
	[ ! -r "$AIROHA_TEST_COUNTER_FILE" ] ||
		sequence="$(cat "$AIROHA_TEST_COUNTER_FILE")"
	sequence=$((sequence + 1))
	printf '%s\n' "$sequence" > "$AIROHA_TEST_COUNTER_FILE"
fi
net_step="${AIROHA_TEST_NET_STEP:-0}"
pon_step="${AIROHA_TEST_PON_STEP:-0}"
frame_step=0
[ "$pon_step" -eq 0 ] || frame_step=1
printf 'version=1\nruntime_mode=%s\nbosa_mode_match=1\n' "$mode"
printf 'core_switch_state=switching=0 stage=18 error=0 committed=1 rollback_failed=0 tx_disabled=0 fault_locked=0\n'
printf 'bosa_fault_locked=0\nbosa_los=0\nbosa_tx_disable=0\n'
case "$mode" in
	gpon)
		printf 'gpon_enabled=1\n'
		if [ "${AIROHA_TEST_UNREADY_MODE:-}" = "$mode" ]; then
			printf 'gpon_state=4 O4-ranging\n'
		else
			printf 'gpon_state=5 O5-operation\n'
		fi
		printf 'gpon_safety_status=ready=1 rogue_fault=0 olt_disabled=0\n'
		printf 'gpon_optical_link=los=0 lof=0 phy_ready=1\n'
		printf 'control_state=online\ncontrol_pon_mode=gpon\nplatform_state=applied\n'
		;;
	xgpon|xgspon)
		printf 'xgspon_enabled=1\n'
		if [ "${AIROHA_TEST_UNREADY_MODE:-}" = "$mode" ]; then
			printf 'xgspon_state=wait-registration\nxgspon_ready=0\n'
			printf 'xgspon_key_derivation_state=onu-id-assigned-registration-pending\n'
		else
			printf 'xgspon_state=registered\nxgspon_ready=1\n'
			printf 'xgspon_key_derivation_state=registered-secure-omcc-ready\n'
		fi
		printf 'control_state=online\ncontrol_pon_mode=%s\nplatform_state=applied\n' "$mode"
		;;
	epon-10g-1g|epon-10g-10g)
		printf 'epon_enabled=1\n'
		if [ "${AIROHA_TEST_UNREADY_MODE:-}" = "$mode" ]; then
			printf 'epon_mpcp_state=0:1:0000:0\n'
		else
			printf 'epon_mpcp_state=0:8:1234:0\n'
		fi
		printf 'control_state=online\ncontrol_pon_mode=%s\n' "$mode"
		printf 'control_control_plane=epon-oam\ncontrol_omci=false\n'
		;;
esac
case "$mode" in
	gpon)
		net_rx=$((800 + sequence * net_step))
		net_tx=$((1600 + sequence * net_step))
		pon_rx=$((800 + sequence * pon_step))
		pon_tx=$((1600 + sequence * pon_step))
		printf 'pon_counter_source=gpon-gem-data-payload\n'
		printf 'pon_rx_counter_unit=bytes\npon_tx_counter_unit=bytes\n'
		;;
	xgpon)
		net_rx=$((1000 + sequence * net_step))
		net_tx=$((2000 + sequence * net_step))
		pon_rx=$((1000 + sequence * pon_step))
		pon_tx=$((2000 + sequence * pon_step))
		printf 'pon_counter_source=xgs-mac-session-non-idle-bytes\n'
		printf 'pon_rx_counter_unit=bytes\npon_tx_counter_unit=bytes\n'
		;;
	xgspon)
		net_rx=$((1200 + sequence * net_step))
		net_tx=$((2400 + sequence * net_step))
		pon_rx=$((1200 + sequence * pon_step))
		pon_tx=$((2400 + sequence * pon_step))
		printf 'pon_counter_source=xgs-mac-session-non-idle-bytes\n'
		printf 'pon_rx_counter_unit=bytes\npon_tx_counter_unit=bytes\n'
		;;
	*)
		net_rx=$((1200 + sequence * net_step))
		net_tx=$((2400 + sequence * net_step))
		pon_rx=$((1200 + sequence * pon_step))
		pon_tx=$((40 + sequence * frame_step))
		printf 'pon_counter_source=epon-mac-session\n'
		printf 'pon_rx_counter_unit=bytes\npon_tx_counter_unit=frames\n'
		;;
esac
printf 'pon_counter_reset=%s\n' "${AIROHA_TEST_PON_COUNTER_RESET:-0}"
printf 'pon_rx_counter=%s\npon_tx_counter=%s\n' "$pon_rx" "$pon_tx"
printf 'net_rx_bytes=%s\nnet_tx_bytes=%s\n' "$net_rx" "$net_tx"
EOF
chmod +x "$temporary/bin/ubus" "$temporary/bin/jsonfilter" \
	"$temporary/bin/collector"

grep -F 'set_mode:' "$rpc" >/dev/null
grep -F 'prepare_mode_switch_config(read_config(),' "$rpc" >/dev/null
grep -F '"set_mode"' "$acl" >/dev/null
grep -F 'export function prepare_mode_switch_config' "$transaction" >/dev/null
grep -F 'call luci.airoha-gpon set_mode "$request"' "$program" >/dev/null
if grep -Eq 'serial_number|registration_id|epon_loid|uci_command|jshn' "$program"; then
	echo 'acceptance tool still handles registration identity directly' >&2
	exit 1
fi

run_program() {
	AIROHA_TEST_STATE="$state" \
	AIROHA_TEST_UBUS_LOG="$ubus_log" \
	AIROHA_TEST_COUNTER_FILE="$temporary/counter" \
	AIROHA_XPON_ACCEPT_COLLECTOR="$temporary/bin/collector" \
	AIROHA_XPON_ACCEPT_UBUS="$temporary/bin/ubus" \
	AIROHA_XPON_ACCEPT_JSONFILTER="$temporary/bin/jsonfilter" \
	AIROHA_XPON_ACCEPT_SLEEP=: \
	AIROHA_XPON_ACCEPT_LOCK_FILE="$temporary/accept.lock" \
	AIROHA_TEST_RPC_FAIL_MARKER="$temporary/rpc-failure-consumed" \
	PATH="$temporary/bin:$PATH" \
		sh "$program" "$@"
}

edge_count=0
for previous in gpon xgpon xgspon epon-10g-1g epon-10g-10g; do
	for target in gpon xgpon xgspon epon-10g-1g epon-10g-10g; do
		[ "$previous" != "$target" ] || continue
		edge_count=$((edge_count + 1))
		success="$temporary/edge-$edge_count"
		printf '%s\n' "$previous" > "$state"
		: > "$ubus_log"
		run_program "$target" "$success" >/dev/null
		[ "$(cat "$state")" = "$target" ]
		grep -Fx "from_mode=$previous" "$success/result.env" >/dev/null
		grep -Fx "target_mode=$target" "$success/result.env" >/dev/null
		grep -Fx 'result=passed' "$success/result.env" >/dev/null
		grep -Fx 'registration_ready_seen=1' "$success/result.env" >/dev/null
		if [ "$previous" = xgpon ] && [ "$target" = epon-10g-10g ]; then
			grep -Fx 'rx_delta=200' "$success/result.env" >/dev/null
			grep -Fx 'tx_delta=400' "$success/result.env" >/dev/null
		fi
		if grep -R -E 'HWTC12345678|subscriber' "$success" >/dev/null; then
			echo 'acceptance evidence exposed registration identity' >&2
			exit 1
		fi
	done
done
[ "$edge_count" -eq 20 ]

printf '%s\n' xgpon > "$state"
: > "$ubus_log"
timeout_output="$temporary/timeout"
if AIROHA_TEST_UNREADY_MODE=epon-10g-1g \
	AIROHA_XPON_ACCEPT_TIMEOUT=2 \
		run_program epon-10g-1g "$timeout_output" >/dev/null 2>&1; then
	echo 'unregistered EPON target passed edge acceptance' >&2
	exit 1
fi
[ "$(cat "$state")" = xgpon ]
[ "$(sed -n '1p' "$ubus_log")" = epon-10g-1g ]
[ "$(sed -n '2p' "$ubus_log")" = xgpon ]
grep -Fx 'result=target-not-ready-runtime-rolled-back' \
	"$timeout_output/result.env" >/dev/null
grep -Fx 'runtime_mode=xgpon' "$timeout_output/rollback.env" >/dev/null

printf '%s\n' xgspon > "$state"
: > "$ubus_log"
gpon_timeout="$temporary/gpon-timeout"
if AIROHA_TEST_UNREADY_MODE=gpon \
	AIROHA_XPON_ACCEPT_TIMEOUT=2 \
		run_program gpon "$gpon_timeout" >/dev/null 2>&1; then
	echo 'GPON target outside O5 passed edge acceptance' >&2
	exit 1
fi
[ "$(cat "$state")" = xgspon ]
grep -Fx 'result=target-not-ready-runtime-rolled-back' \
	"$gpon_timeout/result.env" >/dev/null
grep -Fx 'runtime_mode=xgspon' "$gpon_timeout/rollback.env" >/dev/null

printf '%s\n' xgpon > "$state"
: > "$ubus_log"
traffic_output="$temporary/traffic-timeout"
if AIROHA_XPON_ACCEPT_TIMEOUT=2 \
	AIROHA_XPON_ACCEPT_MIN_RX_BYTES=1 \
	AIROHA_XPON_ACCEPT_MIN_TX_BYTES=1 \
		run_program epon-10g-10g "$traffic_output" >/dev/null 2>&1; then
	echo 'static post-registration counters passed traffic acceptance' >&2
	exit 1
fi
[ "$(cat "$state")" = xgpon ]
grep -Fx 'registration_ready_seen=1' "$traffic_output/result.env" >/dev/null
grep -Fx 'traffic_baseline_after_registration=1' \
	"$traffic_output/result.env" >/dev/null
grep -Fx 'result=traffic-threshold-timeout-runtime-rolled-back' \
	"$traffic_output/result.env" >/dev/null

printf '%s\n' xgpon > "$state"
printf '%s\n' 0 > "$temporary/counter"
: > "$ubus_log"
traffic_pass="$temporary/traffic-pass"
AIROHA_TEST_NET_STEP=100 \
AIROHA_TEST_PON_STEP=100 \
AIROHA_XPON_ACCEPT_TIMEOUT=3 \
AIROHA_XPON_ACCEPT_MIN_RX_BYTES=50 \
AIROHA_XPON_ACCEPT_MIN_TX_BYTES=50 \
	run_program epon-10g-10g "$traffic_pass" >/dev/null
grep -Fx 'result=passed' "$traffic_pass/result.env" >/dev/null
grep -Fx 'rx_delta=100' "$traffic_pass/result.env" >/dev/null
grep -Fx 'tx_delta=100' "$traffic_pass/result.env" >/dev/null
grep -Fx 'pon_counter_source=epon-mac-session' \
	"$traffic_pass/result.env" >/dev/null
grep -Fx 'pon_rx_counter_unit=bytes' "$traffic_pass/result.env" >/dev/null
grep -Fx 'pon_tx_counter_unit=frames' "$traffic_pass/result.env" >/dev/null
grep -Fx 'pon_rx_delta=100' "$traffic_pass/result.env" >/dev/null
grep -Fx 'pon_tx_delta=1' "$traffic_pass/result.env" >/dev/null

printf '%s\n' xgspon > "$state"
printf '%s\n' 0 > "$temporary/counter"
: > "$ubus_log"
gpon_traffic="$temporary/gpon-traffic-pass"
AIROHA_TEST_NET_STEP=100 \
AIROHA_TEST_PON_STEP=100 \
AIROHA_XPON_ACCEPT_TIMEOUT=3 \
AIROHA_XPON_ACCEPT_MIN_RX_BYTES=50 \
AIROHA_XPON_ACCEPT_MIN_TX_BYTES=50 \
	run_program gpon "$gpon_traffic" >/dev/null
grep -Fx 'result=passed' "$gpon_traffic/result.env" >/dev/null
grep -Fx 'pon_counter_source=gpon-gem-data-payload' \
	"$gpon_traffic/result.env" >/dev/null
grep -Fx 'pon_rx_counter_unit=bytes' "$gpon_traffic/result.env" >/dev/null
grep -Fx 'pon_tx_counter_unit=bytes' "$gpon_traffic/result.env" >/dev/null
grep -Fx 'pon_rx_delta=100' "$gpon_traffic/result.env" >/dev/null
grep -Fx 'pon_tx_delta=100' "$gpon_traffic/result.env" >/dev/null

printf '%s\n' xgpon > "$state"
printf '%s\n' 0 > "$temporary/counter"
: > "$ubus_log"
net_only="$temporary/net-only-traffic"
if AIROHA_TEST_NET_STEP=100 \
	AIROHA_TEST_PON_STEP=0 \
	AIROHA_XPON_ACCEPT_TIMEOUT=3 \
	AIROHA_XPON_ACCEPT_MIN_RX_BYTES=50 \
	AIROHA_XPON_ACCEPT_MIN_TX_BYTES=50 \
		run_program epon-10g-10g "$net_only" >/dev/null 2>&1; then
	echo 'netdev-only traffic passed without EPON MAC evidence' >&2
	exit 1
fi
[ "$(cat "$state")" = xgpon ]
grep -Fx 'result=traffic-threshold-timeout-runtime-rolled-back' \
	"$net_only/result.env" >/dev/null
grep -Fx 'pon_rx_delta=0' "$net_only/result.env" >/dev/null
grep -Fx 'pon_tx_delta=0' "$net_only/result.env" >/dev/null

printf '%s\n' xgpon > "$state"
: > "$ubus_log"
rm -f "$temporary/rpc-failure-consumed"
rpc_failure="$temporary/rpc-failure"
if AIROHA_TEST_RPC_FAIL_AFTER_MODE=xgspon \
		run_program xgspon "$rpc_failure" >/dev/null 2>&1; then
	echo 'ambiguous RPC failure was accepted' >&2
	exit 1
fi
[ "$(cat "$state")" = xgpon ]
[ "$(sed -n '1p' "$ubus_log")" = xgspon ]
[ "$(sed -n '2p' "$ubus_log")" = xgpon ]
grep -Fx 'result=rpc-switch-failed-source-restored' \
	"$rpc_failure/result.env" >/dev/null
grep -Fx 'runtime_mode=xgpon' "$rpc_failure/rollback.env" >/dev/null

printf '%s\n' xgpon > "$state"
: > "$ubus_log"
lock_ready="$temporary/lock-ready"
(
	exec 8>"$temporary/accept.lock"
	flock -x 8
	: > "$lock_ready"
	sleep 1
) &
lock_pid=$!
while [ ! -e "$lock_ready" ]; do sleep 0.01; done
if run_program xgspon "$temporary/concurrent" >/dev/null 2>&1; then
	echo 'concurrent acceptance transaction was allowed' >&2
	kill "$lock_pid" 2>/dev/null || true
	wait "$lock_pid" 2>/dev/null || true
	exit 1
fi
wait "$lock_pid"
[ ! -s "$ubus_log" ]
[ ! -e "$temporary/concurrent" ]

printf '%s\n' xgpon > "$state"
: > "$ubus_log"
source_unready="$temporary/source-unready"
if AIROHA_TEST_UNREADY_MODE=xgpon \
		run_program xgspon "$source_unready" >/dev/null 2>&1; then
	echo 'unregistered source was allowed to begin an acceptance edge' >&2
	exit 1
fi
[ ! -s "$ubus_log" ]
grep -Fx 'result=source-not-ready' "$source_unready/result.env" >/dev/null

printf '%s\n' xgpon > "$state"
: > "$ubus_log"
if run_program xgpon "$temporary/same" >/dev/null 2>&1; then
	echo 'same-mode edge was accepted' >&2
	exit 1
fi
[ ! -s "$ubus_log" ]

printf '%s\n' 'Five-mode on-target directed-edge acceptance transaction: OK'
