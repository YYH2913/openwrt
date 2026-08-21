#!/bin/sh
set -eu

script_dir="$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)"
program="$script_dir/../files/airoha-xpon-evidence"
temporary="$(mktemp -d "${TMPDIR:-/tmp}/airoha-xpon-evidence.XXXXXX")"
trap 'status=$?; rm -rf "$temporary"; exit "$status"' EXIT INT TERM

sysfs="$temporary/sys"
procfs="$temporary/proc"
run_root="$temporary/run"
core="$sysfs/bus/platform/drivers/airoha-xpon-core/core"
bosa="$sysfs/bus/i2c/drivers/airoha-en7572/bosa"
gpon="$sysfs/bus/platform/drivers/airoha-gpon/gpon"
xgspon="$sysfs/bus/platform/drivers/airoha-xgspon/xgspon"
epon="$sysfs/bus/platform/drivers/airoha-en7581-epon/epon"
net="$sysfs/class/net/pon"

write_value() {
	mkdir -p "$(dirname -- "$1")"
	printf '%s\n' "$2" > "$1"
}

expect() {
	printf '%s\n' "$output" | grep -Fqx "$1" || {
		echo "missing evidence: $1" >&2
		exit 1
	}
}

write_value "$core/available_modes" \
	'gpon xgpon xgspon epon-10g-1g epon-10g-10g'
write_value "$core/switch_state" \
	'switching=0 error=0 committed=1 rollback_failed=0'
write_value "$core/dying_gasp" 'events=0 clear_errors=0'
write_value "$bosa/initialized" 1
write_value "$bosa/calibration_source" test-calibration
write_value "$bosa/vendor" TEST
write_value "$bosa/part_number" XG2010G
write_value "$bosa/firmware_version" 1
write_value "$bosa/mcu_idle" 1
write_value "$bosa/los" 0
write_value "$bosa/tx_disable" 0
write_value "$bosa/fault_locked" 0
write_value "$bosa/optical_diagnostics" '1 2 3 4 5'
write_value "$procfs/sys/kernel/random/boot_id" test-boot-id
write_value "$procfs/uptime" '123.00 100.00'
write_value "$net/operstate" up
for counter in rx_bytes tx_bytes rx_packets tx_packets rx_errors tx_errors; do
	write_value "$net/statistics/$counter" 10
done

write_value "$gpon/enabled" 1
for attribute in state onu_id omcc_id equalization_delay tconts data_gems \
	safety_status optical_link ber_sample stats; do
	write_value "$gpon/$attribute" "gpon-$attribute"
done

write_value "$xgspon/enabled" 1
write_value "$xgspon/control_abi_version" 3
write_value "$xgspon/activation_ready" 1
write_value "$xgspon/ready" 1
write_value "$xgspon/state" '5 registered'
write_value "$xgspon/activation_blockers" none
write_value "$xgspon/key_derivation_state" ready
write_value "$xgspon/registration_id_configured" 1
for attribute in mac_initialization phy_evidence activation_evidence \
	to1_evidence mac_errors service_state omcc_rx_evidence \
	xgs_counter_evidence xgs_ploam_evidence; do
	write_value "$xgspon/$attribute" "xgs-$attribute"
done
write_value "$xgspon/xgs_pm_snapshot" \
	'version=2 transmitted_non_idle_bytes=2400 received_non_idle_bytes=1200'

write_value "$epon/enabled" 1
write_value "$epon/llid_mask" 0x1
write_value "$epon/silent_time" 60
write_value "$epon/holdover" inactive
write_value "$epon/mpcp_state" registered
write_value "$epon/counter_evidence" \
	'version=1 counter_reset=0 eth_byte_count_enabled=1 rx_ethernet_bytes=1200 rx_mbi_ethernet_frames=20 rx_mpi_ethernet_frames=21 tx_mbi_ethernet_frames=39 tx_mpi_ethernet_frames=40'
write_value "$epon/statistics" 'registration_events=1'

for mode in gpon xgpon xgspon epon-10g-1g epon-10g-10g; do
	write_value "$core/mode" "$mode"
	write_value "$bosa/pon_mode" "$mode"
	case "$mode" in
		gpon) driver=gpon ;;
		xgpon|xgspon)
			driver=xgspon
			write_value "$xgspon/pon_mode" "$mode"
			;;
		*) driver=epon ;;
	esac
	output="$(AIROHA_XPON_EVIDENCE_SYSFS_ROOT="$sysfs" \
		AIROHA_XPON_EVIDENCE_PROCFS_ROOT="$procfs" \
		AIROHA_XPON_EVIDENCE_RUN_ROOT="$run_root" \
		AIROHA_XPON_EVIDENCE_JSONFILTER=/bin/false \
		sh "$program" "$mode")"
	expect 'version=1'
	expect 'semantics=read-only-target-evidence-not-acceptance'
	expect "runtime_mode=$mode"
	expect "selected_driver=$driver"
	expect 'bosa_mode_match=1'
	expect "expected_mode=$mode"
	expect 'expected_mode_match=1'
	expect "core_mode=$mode"
	expect "bosa_pon_mode=$mode"
	expect 'core_switch_state=switching=0 error=0 committed=1 rollback_failed=0'
	expect 'bosa_fault_locked=0'
	expect 'net_rx_bytes=10'
	case "$driver" in
		gpon) expect 'gpon_state=gpon-state' ;;
		xgspon)
			expect "xgspon_pon_mode=$mode"
			expect 'xgspon_to1_evidence=xgs-to1_evidence'
			expect 'pon_counter_source=xgs-mac-session-non-idle-bytes'
			expect 'pon_counter_reset=0'
			expect 'pon_rx_counter=1200'
			expect 'pon_rx_counter_unit=bytes'
			expect 'pon_tx_counter=2400'
			expect 'pon_tx_counter_unit=bytes'
			;;
		epon)
			expect 'epon_mpcp_state=registered'
			expect 'epon_counter_evidence=version=1 counter_reset=0 eth_byte_count_enabled=1 rx_ethernet_bytes=1200 rx_mbi_ethernet_frames=20 rx_mpi_ethernet_frames=21 tx_mbi_ethernet_frames=39 tx_mpi_ethernet_frames=40'
			expect 'epon_statistics=registration_events=1'
			expect 'pon_counter_source=epon-mac-session'
			expect 'pon_counter_reset=0'
			expect 'pon_rx_counter=1200'
			expect 'pon_rx_counter_unit=bytes'
			expect 'pon_tx_counter=40'
			expect 'pon_tx_counter_unit=frames'
			;;
	esac
done

write_value "$core/mode" xgpon
write_value "$bosa/pon_mode" xgpon
write_value "$xgspon/pon_mode" xgpon
if AIROHA_XPON_EVIDENCE_SYSFS_ROOT="$sysfs" \
	AIROHA_XPON_EVIDENCE_PROCFS_ROOT="$procfs" \
	sh "$program" xgspon >/dev/null 2>&1; then
	echo 'collector accepted an expected/runtime mode mismatch' >&2
	exit 1
fi

if grep -Eq 'emit_file .* (serial_number|password|registration_id)( |$)' \
	"$program"; then
	echo 'collector exposes a registration secret' >&2
	exit 1
fi

echo 'Four-mode read-only XPON target evidence collector: OK'
