#!/bin/sh

set -eu

openwrt="${1:?OpenWrt tree path is required}"
helper="$openwrt/package/network/services/airoha-omcid/files/airoha-xgspon-evidence"
temporary="$(mktemp -d)"
trap 'status=$?; rm -rf "$temporary"; exit "$status"' EXIT
trap 'exit 1' INT TERM

if grep -Eq '(^|[[:space:];])[^#]*(>|>>)[[:space:]]*\$' "$helper"; then
	echo 'evidence helper writes to a sysfs path' >&2
	exit 1
fi
if grep -Eq 'registration_id|key_derivation_state|serial_number' "$helper"; then
	echo 'evidence helper reads a sensitive XGS-PON attribute' >&2
	exit 1
fi

mkdir -p "$temporary/xgspon" "$temporary/bosa" \
	"$temporary/proc/sys/kernel/random"
printf '%s\n' 11111111-2222-3333-4444-555555555555 > \
	"$temporary/proc/sys/kernel/random/boot_id"
printf '%s\n' '123.45 67.89' > "$temporary/proc/uptime"
printf '%s\n' xgspon > "$temporary/bosa/pon_mode"
printf '%s\n' 1 > "$temporary/bosa/initialized"
printf '%s\n' nvmem:xgspon > "$temporary/bosa/calibration_source"
printf '%s\n' ACME > "$temporary/bosa/vendor"
printf '%s\n' EN7572 > "$temporary/bosa/part_number"
printf '%s\n' 0x42 > "$temporary/bosa/firmware_version"
printf '%s\n' 0x00 > "$temporary/bosa/mcu_idle"
printf '%s\n' 0 > "$temporary/bosa/los"
printf '%s\n' 1 > "$temporary/bosa/tx_disable"
printf '%s\n' '100 200 300 400 500' > "$temporary/bosa/optical_diagnostics"

printf '%s\n' xgspon > "$temporary/xgspon/pon_mode"
printf '%s\n' 3 > "$temporary/xgspon/control_abi_version"
printf '%s\n' 1 > "$temporary/xgspon/activation_ready"
printf '%s\n' 1 > "$temporary/xgspon/ready"
printf '%s\n' registered > "$temporary/xgspon/state"
printf '%s\n' none > "$temporary/xgspon/activation_blockers"
printf '%s\n' 'version=1 complete=1 initialized=1 verified=1' > \
	"$temporary/xgspon/mac_initialization"
printf '%s\n' 'version=3 complete=0 hardware_selected=1 irq_owned=1 lods_session_preserved=0 lods_timeout_ms=100' > "$temporary/xgspon/phy_evidence"
printf '%s\n' 'version=1 complete=0 activation_state=5' > "$temporary/xgspon/activation_evidence"
printf '%s\n' 'version=1 generation=7 tconts=1 xgems=2' > "$temporary/xgspon/service_state"
printf '%s\n' 'version=1 records=0' > "$temporary/xgspon/omcc_rx_evidence"
printf '%s\n' 'version=1 complete=0 semantics=raw-hardware-modulo' > \
	"$temporary/xgspon/xgs_counter_evidence"
printf '%s\n' 'version=3 complete=0' > "$temporary/xgspon/xgs_ploam_evidence"
printf '%s\n' 'version=2 complete=0' > "$temporary/xgspon/xgs_pm_snapshot"

control="$temporary/control"
cat > "$control" <<'EOF'
#!/bin/sh
read -r request
case "$request" in
	'{"action":"xgs-omci-evidence","ani_entity_id":32769}')
		printf '%s\n' '{"version":4,"complete":false,"kernel_instance_generation":100,"kernel_session_generation":2,"baseline_messages":34,"extended_messages":35}'
		;;
	'{"action":"xgs-pm-evidence","ani_entity_id":32769}')
		printf '%s\n' '{"version":2,"complete":false,"semantics":"cross-layer-instance-session-consistent-partial","kernel_instance_generation":100,"kernel_session_generation":2,"dispatcher_generation":4}'
		;;
	*) exit 1 ;;
esac
EOF
chmod +x "$control"

output="$(env AIROHA_XGSPON_EVIDENCE_XGSPON_PATH="$temporary/xgspon" \
	AIROHA_XGSPON_EVIDENCE_BOSA_PATH="$temporary/bosa" \
	AIROHA_XGSPON_EVIDENCE_PROCFS_ROOT="$temporary/proc" \
	AIROHA_XGSPON_EVIDENCE_CONTROL="$control" sh "$helper")"
printf '%s\n' "$output" | grep -Fx 'version=1' >/dev/null
printf '%s\n' "$output" | grep -Fx 'semantics=read-only-target-evidence-not-acceptance' >/dev/null
printf '%s\n' "$output" | grep -Eq '^collected_at_utc=[0-9]{4}-[0-9]{2}-[0-9]{2}T[0-9]{2}:[0-9]{2}:[0-9]{2}Z$'
printf '%s\n' "$output" | grep -Eq '^kernel_release=.+$'
printf '%s\n' "$output" | grep -Fx 'boot_id=11111111-2222-3333-4444-555555555555' >/dev/null
printf '%s\n' "$output" | grep -Fx 'uptime_seconds=123.45' >/dev/null
printf '%s\n' "$output" | grep -Fx 'bosa_calibration_source=nvmem:xgspon' >/dev/null
printf '%s\n' "$output" | grep -Fx 'xgspon_mac_initialization=version=1 complete=1 initialized=1 verified=1' >/dev/null
printf '%s\n' "$output" | grep -Fx 'xgspon_activation_evidence=version=1 complete=0 activation_state=5' >/dev/null
printf '%s\n' "$output" | grep -Fx 'xgspon_xgs_counter_evidence=version=1 complete=0 semantics=raw-hardware-modulo' >/dev/null
printf '%s\n' "$output" | grep -Fx 'xgspon_xgs_pm_snapshot=version=2 complete=0' >/dev/null
printf '%s\n' "$output" | grep -Fx 'xgspon_xgs_omci_evidence={"version":4,"complete":false,"kernel_instance_generation":100,"kernel_session_generation":2,"baseline_messages":34,"extended_messages":35}' >/dev/null
printf '%s\n' "$output" | grep -Fx 'xgspon_xgs_pm_evidence={"version":2,"complete":false,"semantics":"cross-layer-instance-session-consistent-partial","kernel_instance_generation":100,"kernel_session_generation":2,"dispatcher_generation":4}' >/dev/null

rm -f "$temporary/xgspon/phy_evidence"
output="$(env AIROHA_XGSPON_EVIDENCE_XGSPON_PATH="$temporary/xgspon" \
	AIROHA_XGSPON_EVIDENCE_BOSA_PATH="$temporary/bosa" \
	AIROHA_XGSPON_EVIDENCE_PROCFS_ROOT="$temporary/proc" \
	AIROHA_XGSPON_EVIDENCE_CONTROL="$control" sh "$helper")"
printf '%s\n' "$output" | grep -Fx 'xgspon_phy_evidence=unavailable' >/dev/null

rm -f "$temporary/proc/uptime" "$temporary/xgspon/xgs_counter_evidence"
output="$(env AIROHA_XGSPON_EVIDENCE_XGSPON_PATH="$temporary/xgspon" \
	AIROHA_XGSPON_EVIDENCE_BOSA_PATH="$temporary/bosa" \
	AIROHA_XGSPON_EVIDENCE_PROCFS_ROOT="$temporary/proc" \
	AIROHA_XGSPON_EVIDENCE_CONTROL="$control" sh "$helper")"
printf '%s\n' "$output" | grep -Fx 'uptime_seconds=unavailable' >/dev/null
printf '%s\n' "$output" | grep -Fx 'xgspon_xgs_counter_evidence=unavailable' >/dev/null

output="$(env AIROHA_XGSPON_EVIDENCE_XGSPON_PATH="$temporary/xgspon" \
	AIROHA_XGSPON_EVIDENCE_BOSA_PATH="$temporary/bosa" \
	AIROHA_XGSPON_EVIDENCE_PROCFS_ROOT="$temporary/proc" \
	AIROHA_XGSPON_EVIDENCE_CONTROL="$temporary/missing-control" sh "$helper")"
printf '%s\n' "$output" | grep -Fx 'xgspon_xgs_omci_evidence=unavailable' >/dev/null
printf '%s\n' "$output" | grep -Fx 'xgspon_xgs_pm_evidence=unavailable' >/dev/null

if env AIROHA_XGSPON_EVIDENCE_XGSPON_PATH="$temporary/missing" \
	AIROHA_XGSPON_EVIDENCE_BOSA_PATH="$temporary/bosa" sh "$helper" >/dev/null 2>&1; then
	echo 'evidence helper accepted a missing XGS-PON driver' >&2
	exit 1
fi
