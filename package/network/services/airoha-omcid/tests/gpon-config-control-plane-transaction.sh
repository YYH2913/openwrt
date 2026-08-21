#!/bin/sh

set -eu

openwrt_root="${1:?OpenWrt tree path is required}"
package="$openwrt_root/package/network/services/airoha-omcid"
program="$package/files/airoha-gpon-config"
functions="$package/tests/gpon-config-functions"
rpc="$openwrt_root/package/network/services/luci-app-airoha-gpon/root/usr/share/rpcd/ucode/airoha-gpon.uc"
temporary="$(mktemp -d)"
gpon="$temporary/sys/bus/platform/drivers/airoha-gpon/gpon0"
xgspon="$temporary/sys/bus/platform/drivers/airoha-xgspon/xgspon0"
epon="$temporary/sys/bus/platform/drivers/airoha-en7581-epon/epon0"
xpon="$temporary/sys/bus/platform/drivers/airoha-xpon-core/xpon0"
service_state="$temporary/service-state"
epon_source="$openwrt_root/package/kernel/airoha-xpon/src/airoha-epon.c"

cleanup() {
	rm -rf "$temporary"
}
trap cleanup EXIT INT TERM

mkdir -p "$gpon" "$xgspon" "$epon" "$xpon" \
	"$temporary/services" "$service_state"
printf '0\n' > "$gpon/enabled"
printf '5 active\n' > "$gpon/state"
printf '2:100\n' > "$gpon/tconts"
printf 'off\n' > "$gpon/data_gem"
: > "$gpon/serial_number"
: > "$gpon/password"
printf '0\n' > "$xgspon/enabled"
printf '1\n' > "$xgspon/activation_ready"
printf 'ready\n' > "$xgspon/activation_blockers"
printf '0\n' > "$xgspon/registration_id_configured"
: > "$xgspon/serial_number"
: > "$xgspon/registration_id"
printf '0\n' > "$epon/enabled"
printf '02:00:00:00:00:01\n' > "$epon/onu_mac"
printf '0x00000001\n' > "$epon/llid_mask"
printf '60\n' > "$epon/silent_time"
printf 'switching=0 stage=18 error=0 committed=1 rollback_failed=0 tx_disabled=1 fault_locked=0\n' \
	> "$xpon/switch_state"

printf '%s\n' '#!/bin/sh' \
	'service="${0##*/}"' \
	'state="$AIROHA_TEST_SERVICE_STATE/$service.running"' \
	'case "$1" in' \
	'  running) [ -f "$state" ] ;;' \
	'  stop)' \
	'    printf "%s:stop\n" "$service" >> "$AIROHA_TEST_SERVICE_LOG"' \
	'    printf "mode-at-%s-stop=%s\n" "$service" "$(cat "$AIROHA_TEST_XPON_MODE_FILE")" >> "$AIROHA_TEST_SERVICE_LOG"' \
	'    if [ "${AIROHA_TEST_FAIL_ACTION:-}" = "$service:stop" ] && [ ! -e "$AIROHA_TEST_SERVICE_STATE/failure-consumed" ]; then' \
	'      : > "$AIROHA_TEST_SERVICE_STATE/failure-consumed"' \
	'      exit 1' \
	'    fi' \
	'    rm -f "$state"' \
	'    ;;' \
	'  restart)' \
	'    mode="${AIROHA_OMCI_PON_MODE_OVERRIDE:-${AIROHA_EPON_PON_MODE_OVERRIDE:-$AIROHA_TEST_PON_MODE}}"' \
	'    printf "%s:restart:%s\n" "$service" "$mode" >> "$AIROHA_TEST_SERVICE_LOG"' \
	'    rm -f "$state"' \
	'    if [ "${AIROHA_TEST_FAIL_ACTION:-}" = "$service:restart" ] && [ ! -e "$AIROHA_TEST_SERVICE_STATE/failure-consumed" ]; then' \
	'      : > "$AIROHA_TEST_SERVICE_STATE/failure-consumed"' \
	'      exit 1' \
	'    fi' \
	'    : > "$state"' \
	'    if [ "${AIROHA_TEST_NO_READY:-}" = "$service:$mode" ] && [ ! -e "$AIROHA_TEST_SERVICE_STATE/failure-consumed" ]; then' \
	'      : > "$AIROHA_TEST_SERVICE_STATE/failure-consumed"' \
	'      exit 0' \
	'    fi' \
	'    if [ "$service" = airoha-omcid ]; then' \
	'      case "$mode" in xgpon|xgspon)' \
	'        printf "xgs-enabled-before-omci=%s\n" "$(cat "$AIROHA_TEST_XGSPON_ENABLED")" >> "$AIROHA_TEST_SERVICE_LOG"' \
	'        printf "xgs-identity-before-omci=%s/%s\n" "$(cat "$AIROHA_TEST_XGSPON_SERIAL")" "$(cat "$AIROHA_TEST_XGSPON_REGISTRATION_ID")" >> "$AIROHA_TEST_SERVICE_LOG"' \
	'        ;; esac' \
	'      printf "{\"state\":\"online\",\"pon_mode\":\"%s\"}\n" "$mode" > "$AIROHA_TEST_OMCI_STATUS"' \
	'    else' \
	'      printf "epon-enabled-before-oam=%s\n" "$(cat "$AIROHA_TEST_EPON_ENABLED")" >> "$AIROHA_TEST_SERVICE_LOG"' \
	'      ready_mode="${AIROHA_TEST_EPON_READY_MODE_OVERRIDE:-$mode}"' \
	'      printf "pid=1 control_plane=epon-oam pon_mode=%s omci=0\n" "$ready_mode" > "$AIROHA_TEST_EPON_READY"' \
	'    fi' \
	'    ;;' \
	'  *) exit 2 ;;' \
	'esac' > "$temporary/services/control-plane"
chmod +x "$temporary/services/control-plane"
ln -s control-plane "$temporary/services/airoha-omcid"
ln -s control-plane "$temporary/services/airoha-epon-oamd"

grep -q '!priv->onu_mac_set' "$epon_source" || {
	echo 'EPON start_mac no longer exposes its cached ONU-MAC prerequisite' >&2
	exit 1
}
sed -n '/if epon_mode "$pon_mode" && ! epon_mode "$previous_pon_mode";/,/epon_prepared=1/p' \
	"$program" | grep -q 'prepare_epon' || {
	echo 'first ITU-to-EPON switch does not stage the ONU MAC before mode commit' >&2
	exit 1
}
grep -q "airoha-gpon-config', 'sync-deferred'" "$rpc" || {
	echo 'LuCI does not defer runtime rollback until old UCI is restored' >&2
	exit 1
}

set_previous_mode() {
	local mode="$1"

	rm -f "$service_state"/*.running "$service_state/failure-consumed" \
		"$temporary/service.log" \
		"$temporary/omci-status.json" "$temporary/epon-ready"
	printf '0\n' > "$gpon/enabled"
	printf '0\n' > "$xgspon/enabled"
	printf '0\n' > "$epon/enabled"
	printf '%s\n' "$mode" > "$xpon/mode"
	case "$mode" in
		gpon)
			printf '1\n' > "$gpon/enabled"
			: > "$service_state/airoha-omcid.running"
			;;
		xgpon|xgspon)
			printf '1\n' > "$xgspon/enabled"
			: > "$service_state/airoha-omcid.running"
			;;
		epon-10g-1g|epon-10g-10g)
			printf '1\n' > "$epon/enabled"
			: > "$service_state/airoha-epon-oamd.running"
			;;
	esac
}

run_sync() {
	AIROHA_GPON_FUNCTIONS="$functions" \
	AIROHA_GPON_SYSFS_ROOT="$temporary/sys" \
	AIROHA_GPON_STATE_DIR="$temporary/run" \
	AIROHA_GPON_SERVICE_ROOT="$temporary/services" \
	AIROHA_GPON_EPON_OAM_READY="$temporary/epon-ready" \
	AIROHA_GPON_OMCI_STATUS="$temporary/omci-status.json" \
	AIROHA_GPON_READY_ATTEMPTS=2 \
	AIROHA_TEST_SERVICE_STATE="$service_state" \
	AIROHA_TEST_SERVICE_LOG="$temporary/service.log" \
	AIROHA_TEST_OMCI_STATUS="$temporary/omci-status.json" \
	AIROHA_TEST_EPON_READY="$temporary/epon-ready" \
	AIROHA_TEST_EPON_READY_MODE_OVERRIDE="${AIROHA_TEST_EPON_READY_MODE_OVERRIDE:-}" \
	AIROHA_TEST_XGSPON_ENABLED="$xgspon/enabled" \
	AIROHA_TEST_XGSPON_SERIAL="$xgspon/serial_number" \
	AIROHA_TEST_XGSPON_REGISTRATION_ID="$xgspon/registration_id" \
	AIROHA_TEST_EPON_ENABLED="$epon/enabled" \
	AIROHA_TEST_XPON_MODE_FILE="$xpon/mode" \
	AIROHA_TEST_FAIL_ACTION="${AIROHA_TEST_FAIL_ACTION:-}" \
	AIROHA_TEST_NO_READY="${AIROHA_TEST_NO_READY:-}" \
	AIROHA_TEST_ENABLED=1 \
	AIROHA_TEST_OMCI_MODE=auto \
	AIROHA_TEST_MANUAL_GEM=0 \
	AIROHA_TEST_GEM_ID=0 \
	AIROHA_TEST_TCONT=0 \
	AIROHA_TEST_ENCRYPTED=0 \
	AIROHA_TEST_PASSWORD= \
	AIROHA_TEST_PON_MODE="$1" \
	AIROHA_TEST_REGISTRATION_ID="${2:-registration-123}" \
	AIROHA_TEST_EPON_ONU_MAC="${3:-02:aa:bb:cc:dd:ee}" \
		sh "$program" "${AIROHA_TEST_SYNC_COMMAND:-sync}"
}

expect_mode_and_owner() {
	local mode="$1" owner="$2"

	[ "$(cat "$xpon/mode")" = "$mode" ] || {
		echo "mode $(cat "$xpon/mode"), want $mode" >&2
		exit 1
	}
	[ -f "$service_state/$owner.running" ] || {
		echo "$owner was not restored for $mode" >&2
		exit 1
	}
}

expect_order() {
	local first="$1" second="$2" first_line second_line

	first_line="$(grep -n -m1 -F "$first" "$temporary/service.log" | cut -d: -f1)"
	second_line="$(grep -n -m1 -F "$second" "$temporary/service.log" | cut -d: -f1)"
	[ "$first_line" -lt "$second_line" ] || {
		echo "$first did not precede $second" >&2
		exit 1
	}
}

# A successful cross-family switch quiesces OMCI before EPON OAM owns the
# endpoint, so both control planes can never consume it concurrently.
set_previous_mode gpon
run_sync epon-10g-1g
expect_mode_and_owner epon-10g-1g airoha-epon-oamd
expect_order 'airoha-omcid:stop' 'airoha-epon-oamd:restart:epon-10g-1g'
grep -qx 'mode-at-airoha-omcid-stop=gpon' "$temporary/service.log"
grep -qx 'epon-enabled-before-oam=0' "$temporary/service.log"
[ ! -f "$service_state/airoha-omcid.running" ]

# Exercise every directed edge among the four requested 10G PON modes at the
# userspace ownership boundary. The kernel transaction has its own 12-edge
# fault matrix; this loop proves that OMCI and EPON OAM are also replaced in
# the right order and that the exact target rate is enabled only after its
# control plane publishes a fresh ready marker.
for previous in xgpon xgspon epon-10g-1g epon-10g-10g; do
	for target in xgpon xgspon epon-10g-1g epon-10g-10g; do
		[ "$previous" != "$target" ] || continue
		set_previous_mode "$previous"
		run_sync "$target"
		case "$previous" in
			xgpon|xgspon) previous_service=airoha-omcid ;;
			*) previous_service=airoha-epon-oamd ;;
		esac
		case "$target" in
			xgpon|xgspon)
			target_service=airoha-omcid
			target_enabled="$xgspon/enabled"
			inactive_service=airoha-epon-oamd
			;;
			*)
			target_service=airoha-epon-oamd
			target_enabled="$epon/enabled"
			inactive_service=airoha-omcid
			;;
		esac
		expect_mode_and_owner "$target" "$target_service"
		expect_order "$previous_service:stop" \
			"$target_service:restart:$target"
		[ "$(cat "$target_enabled")" = 1 ] || {
			echo "$previous -> $target left target protocol disabled" >&2
			exit 1
		}
		[ ! -f "$service_state/$inactive_service.running" ] || {
			echo "$previous -> $target left $inactive_service running" >&2
			exit 1
		}
	done
done

# ITU -> EPON: a target OAM start failure restores the old mode and OMCI.
set_previous_mode gpon
if AIROHA_TEST_FAIL_ACTION=airoha-epon-oamd:restart run_sync epon-10g-10g; then
	echo 'failed EPON OAM restart committed an ITU-to-EPON switch' >&2
	exit 1
fi
expect_mode_and_owner gpon airoha-omcid
grep -q 'airoha-omcid:restart:gpon' "$temporary/service.log"

# EPON -> ITU: a target OMCI readiness failure restores the old kernel mode,
# but cannot safely restart EPON OAM while UCI contains the target credentials.
# The old EPON path must remain fail-closed until a caller restores old UCI.
set_previous_mode epon-10g-1g
if AIROHA_TEST_NO_READY=airoha-omcid:xgspon run_sync xgspon; then
	echo 'unready OMCI committed an EPON-to-ITU switch' >&2
	exit 1
fi
[ "$(cat "$xpon/mode")" = epon-10g-1g ]
[ "$(cat "$epon/enabled")" = 0 ]
[ ! -f "$service_state/airoha-epon-oamd.running" ]
if grep -q 'airoha-epon-oamd:restart:epon-10g-1g' "$temporary/service.log"; then
	echo 'old EPON OAM restarted against unverified target credentials' >&2
	exit 1
fi

# LuCI uses a two-phase failure path: while its target UCI is still committed,
# the coordinator leaves every owner stopped and every backend disabled. After
# LuCI restores old UCI, an ordinary sync rebuilds the old runtime without ever
# starting the old owner against target credentials.
set_previous_mode gpon
if AIROHA_TEST_NO_READY=airoha-omcid:xgspon \
	AIROHA_TEST_SYNC_COMMAND=sync-deferred run_sync xgspon; then
	echo 'deferred rollback committed an unready XGS-PON switch' >&2
	exit 1
fi
[ "$(cat "$xpon/mode")" = xgspon ]
[ "$(cat "$gpon/enabled")" = 0 ]
[ "$(cat "$xgspon/enabled")" = 0 ]
[ "$(cat "$epon/enabled")" = 0 ]
[ ! -f "$service_state/airoha-omcid.running" ]
[ ! -f "$service_state/airoha-epon-oamd.running" ]
if grep -q 'airoha-omcid:restart:gpon' "$temporary/service.log"; then
	echo 'old OMCI restarted before LuCI restored old UCI' >&2
	exit 1
fi
rm -f "$service_state/failure-consumed"
run_sync gpon
expect_mode_and_owner gpon airoha-omcid
[ "$(cat "$gpon/enabled")" = 1 ]

# The same deferred boundary protects EPON LOID/password because the OAM daemon
# reads them directly from UCI. A failed rate-family restart leaves no OAM
# process alive; after old UCI is restored, ordinary sync also restores the old
# ONU MAC before starting OAM for the previous rate.
set_previous_mode epon-10g-1g
printf '02:00:00:00:00:99\n' > "$epon/onu_mac"
if AIROHA_TEST_FAIL_ACTION=airoha-epon-oamd:restart \
	AIROHA_TEST_SYNC_COMMAND=sync-deferred run_sync epon-10g-10g; then
	echo 'deferred rollback committed a failed EPON rate switch' >&2
	exit 1
fi
[ "$(cat "$xpon/mode")" = epon-10g-10g ]
[ "$(cat "$epon/enabled")" = 0 ]
[ ! -f "$service_state/airoha-epon-oamd.running" ]
if grep -q 'airoha-epon-oamd:restart:epon-10g-1g' "$temporary/service.log"; then
	echo 'old EPON OAM restarted before LuCI restored old UCI' >&2
	exit 1
fi
rm -f "$service_state/failure-consumed"
run_sync epon-10g-1g '' 02:00:00:00:00:99
expect_mode_and_owner epon-10g-1g airoha-epon-oamd
[ "$(cat "$epon/onu_mac")" = 02:00:00:00:00:99 ]
[ "$(cat "$epon/enabled")" = 1 ]

# A daemon ready marker for the wrong EPON rate cannot commit a same-family
# hot switch. Without an old credential snapshot, direct rollback keeps the
# restored kernel rate disabled instead of starting OAM with candidate UCI.
set_previous_mode epon-10g-1g
if AIROHA_TEST_EPON_READY_MODE_OVERRIDE=epon-10g-1g \
	run_sync epon-10g-10g; then
	echo 'wrong-rate EPON OAM readiness committed a rate switch' >&2
	exit 1
fi
[ "$(cat "$xpon/mode")" = epon-10g-1g ]
[ "$(cat "$epon/enabled")" = 0 ]
[ ! -f "$service_state/airoha-epon-oamd.running" ]

# Both EPON rates share OAM. A failed direct restart rolls optical/MAC mode and
# registration parameters back, then leaves the backend disabled because the
# old LOID/password cannot be recovered from candidate UCI.
set_previous_mode epon-10g-1g
printf '02:00:00:00:00:99\n' > "$epon/onu_mac"
if AIROHA_TEST_FAIL_ACTION=airoha-epon-oamd:restart run_sync epon-10g-10g; then
	echo 'failed OAM restart committed an EPON rate switch' >&2
	exit 1
fi
[ "$(cat "$xpon/mode")" = epon-10g-1g ]
[ "$(cat "$epon/onu_mac")" = 02:00:00:00:00:99 ]
[ "$(cat "$epon/enabled")" = 0 ]
[ ! -f "$service_state/airoha-epon-oamd.running" ]

# Candidate UCI remains authoritative for a plain sync. Once the transient OAM
# failure is gone, retrying reaches the requested rate from the disabled state.
run_sync epon-10g-10g
expect_mode_and_owner epon-10g-10g airoha-epon-oamd
[ "$(cat "$epon/enabled")" = 1 ]
[ "$(cat "$epon/onu_mac")" = 02:aa:bb:cc:dd:ee ]

# XG-PON -> XGS-PON uses the shared OMCI service with a new protocol mode. A
# failed new instance must be replaced by an instance for the previous mode.
set_previous_mode xgpon
if AIROHA_TEST_NO_READY=airoha-omcid:xgspon run_sync xgspon; then
	echo 'unready XGS OMCI committed an XG-to-XGS switch' >&2
	exit 1
fi
expect_mode_and_owner xgpon airoha-omcid
grep -q 'airoha-omcid:restart:xgpon' "$temporary/service.log"
[ "$(cat "$xgspon/enabled")" = 1 ]

# Rewriting the XG/XGS identity invalidates session keys in the kernel. The
# coordinator must disable the shared backend before it stages the identity or
# starts OMCI for the target protocol.
set_previous_mode xgpon
: > "$xgspon/serial_number"
: > "$xgspon/registration_id"
run_sync xgspon
expect_mode_and_owner xgspon airoha-omcid
grep -qx 'xgs-enabled-before-omci=0' "$temporary/service.log"
grep -qx 'xgs-identity-before-omci=HWTC12345678/registration-123' \
	"$temporary/service.log"
[ "$(cat "$xgspon/enabled")" = 1 ]

# Failure while quiescing the old owner aborts before target startup and also
# restores the old owner because a failed stop may have partially acted.
set_previous_mode gpon
if AIROHA_TEST_FAIL_ACTION=airoha-omcid:stop run_sync epon-10g-1g; then
	echo 'failed OMCI stop committed a cross-family switch' >&2
	exit 1
fi
expect_mode_and_owner gpon airoha-omcid
if grep -q 'airoha-epon-oamd:restart:epon-10g-1g' "$temporary/service.log"; then
	echo 'target OAM started after the old owner failed to stop' >&2
	exit 1
fi

echo 'PON control-plane switch and rollback matrix passed'
