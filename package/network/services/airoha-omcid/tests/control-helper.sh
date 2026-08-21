#!/bin/sh

set -eu

openwrt="${1:?OpenWrt tree path is required}"
package="$openwrt/package/network/services/airoha-omcid"
temporary="$(mktemp -d)"
trap 'status=$?; rm -rf "$temporary"; exit "$status"' EXIT
trap 'exit 1' INT TERM

mkdir -p "$temporary/gpon"
: > "$temporary/gpon/gem_counters"
printf '%s\n' \
	'corrected_bytes=11 corrected_codewords=22 uncorrectable_codewords=33 total_codewords=44 fec_seconds=55' \
	> "$temporary/gpon/fec_counters"

mkdir -p "$temporary/xgspon"
printf '%s\n' \
	'version=1 complete=0 semantics=raw-hardware-modulo tc_valid=1023 downstream_valid=8195 upstream_valid=1 psbd_hec_errors=1 xgtc_hec_errors=2 unknown_profiles=3 transmitted_xgem_frames=4 fragment_xgem_frames=5 xgem_hec_lost_words=6 xgem_key_errors=7 xgem_hec_errors=8 transmitted_non_idle_bytes=9 received_non_idle_bytes=10 ploam_mic_errors=11 downstream_ploam_messages=12 omci_mic_errors=13 upstream_ploam_messages=14 rx_omci_mac_messages=15 rx_omci_fe_messages=16 tx_omci_mac_messages=17 tx_omci_fe_messages=18' \
	> "$temporary/xgspon/xgs_counter_evidence"
printf '%s\n' \
	'version=3 complete=0 semantics=driver-lifecycle instance_generation=100 session_generation=2 profile_messages_received=21 assign_onu_id_messages_received=22 ranging_time_messages_received=23 deactivate_onu_id_messages_received=24 disable_serial_number_messages_received=25 request_registration_messages_received=26 assign_alloc_id_messages_received=27 key_control_messages_received=28 sleep_allow_messages_received=29 serial_number_messages_completed=30 registration_messages_completed=31 key_report_messages_completed=32 acknowledge_messages_completed=33 sleep_request_messages_completed=0' \
	> "$temporary/xgspon/xgs_ploam_evidence"
printf '%s\n' \
	'version=2 complete=0 semantics=kernel-instance-session-monotonic-partial instance_generation=100 generation=2 sequence_begin=4 sequence_end=4 sampling_interval_ms=10 sampling_assumption=single-wrap-no-reset-per-interval-unverified tc_valid=8191 tc_required=8191 downstream_valid=13311 downstream_required=16383 upstream_valid=63 upstream_required=63 psbd_hec_errors=101 xgtc_hec_errors=102 unknown_profiles=103 transmitted_xgem_frames=104 fragment_xgem_frames=105 xgem_hec_lost_words=106 xgem_key_errors=107 xgem_hec_errors=108 transmitted_non_idle_bytes=109 received_non_idle_bytes=110 lods_events=128 lods_restored=130 onu_reactivations_by_lods=129 ploam_mic_errors=111 downstream_ploam_messages=112 profile_messages=113 ranging_time_messages=114 deactivate_onu_id_messages=115 disable_serial_number_messages=116 request_registration_messages=117 assign_alloc_id_messages=118 key_control_messages=119 sleep_allow_messages=120 baseline_omci_messages=0 extended_omci_messages=0 assign_onu_id_messages=121 omci_mic_errors=122 upstream_ploam_messages=123 serial_number_messages=124 registration_messages=125 key_report_messages=126 acknowledge_messages=127 sleep_request_messages=0' \
	> "$temporary/xgspon/xgs_pm_snapshot"
printf '%s\n' \
	'{"version":4,"complete":false,"semantics":"trusted-transport-kernel-instance-session","pon_mode":"xgspon","started_at":"2026-08-14T00:00:00Z","updated_at":"2026-08-14T00:00:01Z","kernel_instance_generation":100,"kernel_session_generation":2,"dispatcher_generation":3,"baseline_messages":34,"extended_messages":35}' \
	> "$temporary/xgs-omci-evidence.json"

mkdir -p "$temporary/bin"
cat > "$temporary/bin/jsonfilter" <<'EOF'
#!/bin/sh
set -eu

input=
expression=
while [ "$#" -gt 0 ]; do
	case "$1" in
		-i) input="$2"; shift 2 ;;
		-e) expression="$2"; shift 2 ;;
		*) exit 1 ;;
	esac
done
[ -n "$input" ] && [ -n "$expression" ]
jq -r "${expression#@}" "$input"
EOF
chmod +x "$temporary/bin/jsonfilter"
cat > "$temporary/bin/cat" <<'EOF'
#!/bin/sh
set -eu

if [ "${AIROHA_TEST_PM_CAT_ALTERNATE:-0}" = 1 ] &&
	[ "$#" -eq 1 ] && [ "$1" = "${AIROHA_TEST_PM_SNAPSHOT:?}" ]; then
	state="${AIROHA_TEST_PM_CAT_STATE:?}"
	count=0
	[ ! -r "$state" ] || count="$(/bin/cat "$state")"
	count=$((count + 1))
	printf '%u\n' "$count" > "$state"
	if [ $((count % 2)) -eq 0 ]; then
		sed 's/instance_generation=100 /instance_generation=101 /' "$1"
	else
		/bin/cat "$1"
	fi
	exit 0
fi
exec /bin/cat "$@"
EOF
chmod +x "$temporary/bin/cat"

printf '%s\n' '{"action":"reboot","reboot_condition":0}' | \
	env AIROHA_OMCI_RESTART_REASON="$temporary/persistent/restart-reason" \
	AIROHA_OMCI_REBOOT=/bin/true AIROHA_OMCI_REBOOT_DELAY=0 \
	PATH="$temporary/bin:$PATH" \
	sh "$package/files/airoha-omci-control"
[ "$(cat "$temporary/persistent/restart-reason")" = 1 ]

mkdir "$temporary/invalid-restart-reason"
if printf '%s\n' '{"action":"reboot","reboot_condition":0}' | \
	env AIROHA_OMCI_RESTART_REASON="$temporary/invalid-restart-reason" \
	AIROHA_OMCI_REBOOT=/bin/true AIROHA_OMCI_REBOOT_DELAY=0 \
	PATH="$temporary/bin:$PATH" \
	sh "$package/files/airoha-omci-control" >/dev/null 2>&1; then
	echo "reboot accepted an unwritable restart-reason target" >&2
	exit 1
fi

output="$(printf '%s\n' '{"action":"fec-counters","ani_entity_id":32769}' | \
	env AIROHA_OMCI_GPON_PATH="$temporary/gpon" \
	PATH="$temporary/bin:$PATH" \
	sh "$package/files/airoha-omci-control")"
printf '%s\n' "$output" | jq -e '
	.ani_entity_id == 32769 and
	.corrected_bytes == 11 and .corrected_codewords == 22 and
	.uncorrectable_codewords == 33 and .total_codewords == 44 and
	.fec_seconds == 55
' >/dev/null

if printf '%s\n' '{"action":"fec-counters","ani_entity_id":32770}' | \
	env AIROHA_OMCI_GPON_PATH="$temporary/gpon" \
	PATH="$temporary/bin:$PATH" \
	sh "$package/files/airoha-omci-control" >/dev/null 2>&1; then
	echo "fec-counters accepted an unknown ANI-G" >&2
	exit 1
fi

printf '%s\n' \
	'corrected_bytes=11 uncorrectable_codewords=33 corrected_codewords=22 total_codewords=44 fec_seconds=55' \
	> "$temporary/gpon/fec_counters"
if printf '%s\n' '{"action":"fec-counters","ani_entity_id":32769}' | \
	env AIROHA_OMCI_GPON_PATH="$temporary/gpon" \
	PATH="$temporary/bin:$PATH" \
	sh "$package/files/airoha-omci-control" >/dev/null 2>&1; then
	echo "fec-counters accepted a malformed kernel snapshot" >&2
	exit 1
fi

output="$(printf '%s\n' '{"action":"xgs-counter-evidence","ani_entity_id":32769}' | \
	env AIROHA_OMCI_XGSPON_PATH="$temporary/xgspon" \
	PATH="$temporary/bin:$PATH" \
	sh "$package/files/airoha-omci-control")"
printf '%s\n' "$output" | jq -e '
	.ani_entity_id == 32769 and .version == 1 and .complete == false and
	.semantics == "raw-hardware-modulo" and
	.valid == {"tc":1023,"downstream_management":8195,"upstream_management":1} and
	.tc.psbd_hec_errors == 1 and .tc.received_non_idle_bytes == 10 and
	.downstream_management.ploam_mic_errors == 11 and
	.downstream_management.ploam_messages == 12 and
	.downstream_management.omci_mic_errors == 13 and
	.upstream_management.ploam_messages == 14 and
	.diagnostic.rx_omci_mac_messages == 15 and
	.diagnostic.tx_omci_fe_messages == 18
' >/dev/null

if printf '%s\n' '{"action":"xgs-counter-evidence","ani_entity_id":32770}' | \
	env AIROHA_OMCI_XGSPON_PATH="$temporary/xgspon" \
	PATH="$temporary/bin:$PATH" \
	sh "$package/files/airoha-omci-control" >/dev/null 2>&1; then
	echo "xgs-counter-evidence accepted an unknown ANI-G" >&2
	exit 1
fi

sed 's/version=1 /version=2 /' "$temporary/xgspon/xgs_counter_evidence" \
	> "$temporary/xgspon/xgs_counter_evidence.invalid"
mv "$temporary/xgspon/xgs_counter_evidence.invalid" \
	"$temporary/xgspon/xgs_counter_evidence"
if printf '%s\n' '{"action":"xgs-counter-evidence","ani_entity_id":32769}' | \
	env AIROHA_OMCI_XGSPON_PATH="$temporary/xgspon" \
	PATH="$temporary/bin:$PATH" \
	sh "$package/files/airoha-omci-control" >/dev/null 2>&1; then
	echo "xgs-counter-evidence accepted an unknown ABI version" >&2
	exit 1
fi

sed 's/version=2 /version=1 /' "$temporary/xgspon/xgs_counter_evidence" \
	> "$temporary/xgspon/xgs_counter_evidence.valid"
mv "$temporary/xgspon/xgs_counter_evidence.valid" \
	"$temporary/xgspon/xgs_counter_evidence"

output="$(printf '%s\n' '{"action":"xgs-ploam-evidence","ani_entity_id":32769}' | \
	env AIROHA_OMCI_XGSPON_PATH="$temporary/xgspon" \
	PATH="$temporary/bin:$PATH" \
	sh "$package/files/airoha-omci-control")"
printf '%s\n' "$output" | jq -e '
	.ani_entity_id == 32769 and .version == 3 and .complete == false and
	.semantics == "driver-lifecycle" and
	.instance_generation == 100 and .session_generation == 2 and
	.downstream.profile_messages == 21 and
	.downstream.key_control_messages == 28 and
	.upstream_completed.serial_number_messages == 30 and
	.upstream_completed.acknowledge_messages == 33 and
	.upstream_completed.sleep_request_messages == 0
' >/dev/null

if printf '%s\n' '{"action":"xgs-ploam-evidence","ani_entity_id":32770}' | \
	env AIROHA_OMCI_XGSPON_PATH="$temporary/xgspon" \
	PATH="$temporary/bin:$PATH" \
	sh "$package/files/airoha-omci-control" >/dev/null 2>&1; then
	echo "xgs-ploam-evidence accepted an unknown ANI-G" >&2
	exit 1
fi

sed 's/version=3 /version=2 /' "$temporary/xgspon/xgs_ploam_evidence" \
	> "$temporary/xgspon/xgs_ploam_evidence.invalid"
mv "$temporary/xgspon/xgs_ploam_evidence.invalid" \
	"$temporary/xgspon/xgs_ploam_evidence"
if printf '%s\n' '{"action":"xgs-ploam-evidence","ani_entity_id":32769}' | \
	env AIROHA_OMCI_XGSPON_PATH="$temporary/xgspon" \
	PATH="$temporary/bin:$PATH" \
	sh "$package/files/airoha-omci-control" >/dev/null 2>&1; then
	echo "xgs-ploam-evidence accepted an obsolete ABI version" >&2
	exit 1
fi

sed 's/version=2 /version=3 /' "$temporary/xgspon/xgs_ploam_evidence" \
	> "$temporary/xgspon/xgs_ploam_evidence.valid"
mv "$temporary/xgspon/xgs_ploam_evidence.valid" \
	"$temporary/xgspon/xgs_ploam_evidence"

output="$(printf '%s\n' '{"action":"xgs-omci-evidence","ani_entity_id":32769}' | \
	env AIROHA_OMCI_XGS_OMCI_EVIDENCE_PATH="$temporary/xgs-omci-evidence.json" \
	PATH="$temporary/bin:$PATH" \
	sh "$package/files/airoha-omci-control")"
printf '%s\n' "$output" | jq -e '
	.ani_entity_id == 32769 and .version == 4 and .complete == false and
	.semantics == "trusted-transport-kernel-instance-session" and
	.kernel_instance_generation == 100 and .kernel_session_generation == 2 and
	.dispatcher_generation == 3 and
	.baseline_messages == 34 and .extended_messages == 35
' >/dev/null

sed 's/"version":4/"version":3/' "$temporary/xgs-omci-evidence.json" \
	> "$temporary/xgs-omci-evidence.invalid"
if printf '%s\n' '{"action":"xgs-omci-evidence","ani_entity_id":32769}' | \
	env AIROHA_OMCI_XGS_OMCI_EVIDENCE_PATH="$temporary/xgs-omci-evidence.invalid" \
	PATH="$temporary/bin:$PATH" \
	sh "$package/files/airoha-omci-control" >/dev/null 2>&1; then
	echo "xgs-omci-evidence accepted the pre-transport-boundary ABI" >&2
	exit 1
fi

sed 's/trusted-transport-kernel-instance-session/application-accepted-kernel-instance-session/' \
	"$temporary/xgs-omci-evidence.json" > "$temporary/xgs-omci-evidence.invalid"
if printf '%s\n' '{"action":"xgs-pm-evidence","ani_entity_id":32769}' | \
	env AIROHA_OMCI_XGSPON_PATH="$temporary/xgspon" \
	AIROHA_OMCI_XGS_OMCI_EVIDENCE_PATH="$temporary/xgs-omci-evidence.invalid" \
	PATH="$temporary/bin:$PATH" \
	sh "$package/files/airoha-omci-control" >/dev/null 2>&1; then
	echo "xgs-pm-evidence accepted application-result OMCI counters" >&2
	exit 1
fi

output="$(printf '%s\n' '{"action":"xgs-pm-evidence","ani_entity_id":32769}' | \
	env AIROHA_OMCI_XGSPON_PATH="$temporary/xgspon" \
	AIROHA_OMCI_XGS_OMCI_EVIDENCE_PATH="$temporary/xgs-omci-evidence.json" \
	PATH="$temporary/bin:$PATH" \
	sh "$package/files/airoha-omci-control")"
printf '%s\n' "$output" | jq -e '
	.ani_entity_id == 32769 and .version == 2 and .complete == false and
	.semantics == "cross-layer-instance-session-consistent-partial" and
	.kernel_instance_generation == 100 and .kernel_session_generation == 2 and
	.dispatcher_generation == 3 and .sequence == 4 and
	.sampling == {"interval_ms":10,"assumption":"single-wrap-no-reset-per-interval-unverified"} and
		.valid == {"tc":8191,"tc_required":8191,"downstream_management":16383,"downstream_required":16383,"upstream_management":63,"upstream_required":63} and
	.tc.psbd_hec_errors == 101 and .tc.received_non_idle_bytes == 110 and
	.tc.lods_events == 128 and .tc.lods_restored == 130 and
	.tc.onu_reactivations_by_lods == 129 and
	.downstream_management.ploam_mic_errors == 111 and
	.downstream_management.profile_messages == 113 and
	.downstream_management.baseline_omci_messages == 34 and
	.downstream_management.extended_omci_messages == 35 and
	.downstream_management.assign_onu_id_messages == 121 and
	.upstream_management.ploam_messages == 123 and
	.upstream_management.acknowledge_messages == 127 and
	.upstream_management.sleep_request_messages == 0
' >/dev/null

# XG-PON uses the same counter ABI, but its daemon evidence remains bound to
# the distinct xgpon state domain. Cross-mode evidence must not be accepted.
sed 's/"pon_mode":"xgspon"/"pon_mode":"xgpon"/' \
	"$temporary/xgs-omci-evidence.json" > "$temporary/xgs-omci-evidence.xgpon"
output="$(printf '%s\n' '{"action":"xgs-omci-evidence","ani_entity_id":32769}' | \
	env AIROHA_OMCI_PON_MODE=xgpon \
	AIROHA_OMCI_XGS_OMCI_EVIDENCE_PATH="$temporary/xgs-omci-evidence.xgpon" \
	PATH="$temporary/bin:$PATH" \
	sh "$package/files/airoha-omci-control")"
printf '%s\n' "$output" | jq -e '.kernel_instance_generation == 100' >/dev/null
output="$(printf '%s\n' '{"action":"xgs-pm-evidence","ani_entity_id":32769}' | \
	env AIROHA_OMCI_PON_MODE=xgpon \
	AIROHA_OMCI_XGSPON_PATH="$temporary/xgspon" \
	AIROHA_OMCI_XGS_OMCI_EVIDENCE_PATH="$temporary/xgs-omci-evidence.xgpon" \
	PATH="$temporary/bin:$PATH" \
	sh "$package/files/airoha-omci-control")"
printf '%s\n' "$output" | jq -e '.kernel_session_generation == 2' >/dev/null
if printf '%s\n' '{"action":"xgs-omci-evidence","ani_entity_id":32769}' | \
	env AIROHA_OMCI_PON_MODE=xgspon \
	AIROHA_OMCI_XGS_OMCI_EVIDENCE_PATH="$temporary/xgs-omci-evidence.xgpon" \
	PATH="$temporary/bin:$PATH" \
	sh "$package/files/airoha-omci-control" >/dev/null 2>&1; then
	echo "XGS-PON accepted XG-PON daemon evidence" >&2
	exit 1
fi

if printf '%s\n' '{"action":"xgs-pm-evidence","ani_entity_id":32770}' | \
	env AIROHA_OMCI_XGSPON_PATH="$temporary/xgspon" \
	AIROHA_OMCI_XGS_OMCI_EVIDENCE_PATH="$temporary/xgs-omci-evidence.json" \
	PATH="$temporary/bin:$PATH" \
	sh "$package/files/airoha-omci-control" >/dev/null 2>&1; then
	echo "xgs-pm-evidence accepted an unknown ANI-G" >&2
	exit 1
fi

if printf '%s\n' '{"action":"xgs-pm-evidence","ani_entity_id":32769}' | \
	env AIROHA_OMCI_XGSPON_PATH="$temporary/xgspon" \
	AIROHA_OMCI_XGS_OMCI_EVIDENCE_PATH="$temporary/xgs-omci-evidence.json" \
	AIROHA_TEST_PM_CAT_ALTERNATE=1 \
	AIROHA_TEST_PM_SNAPSHOT="$temporary/xgspon/xgs_pm_snapshot" \
	AIROHA_TEST_PM_CAT_STATE="$temporary/pm-cat-state" \
	PATH="$temporary/bin:$PATH" \
	sh "$package/files/airoha-omci-control" >/dev/null 2>&1; then
	echo "xgs-pm-evidence mixed two kernel instances" >&2
	exit 1
fi

sed 's/"kernel_session_generation":2/"kernel_session_generation":3/' \
	"$temporary/xgs-omci-evidence.json" > "$temporary/xgs-omci-evidence.invalid"
mv "$temporary/xgs-omci-evidence.invalid" "$temporary/xgs-omci-evidence.json"
if printf '%s\n' '{"action":"xgs-pm-evidence","ani_entity_id":32769}' | \
	env AIROHA_OMCI_XGSPON_PATH="$temporary/xgspon" \
	AIROHA_OMCI_XGS_OMCI_EVIDENCE_PATH="$temporary/xgs-omci-evidence.json" \
	PATH="$temporary/bin:$PATH" \
	sh "$package/files/airoha-omci-control" >/dev/null 2>&1; then
	echo "xgs-pm-evidence mixed kernel and daemon generations" >&2
	exit 1
fi
sed 's/"kernel_session_generation":3/"kernel_session_generation":2/' \
	"$temporary/xgs-omci-evidence.json" > "$temporary/xgs-omci-evidence.valid"
mv "$temporary/xgs-omci-evidence.valid" "$temporary/xgs-omci-evidence.json"

sed 's/"kernel_instance_generation":100/"kernel_instance_generation":101/' \
	"$temporary/xgs-omci-evidence.json" > "$temporary/xgs-omci-evidence.invalid"
mv "$temporary/xgs-omci-evidence.invalid" "$temporary/xgs-omci-evidence.json"
if printf '%s\n' '{"action":"xgs-pm-evidence","ani_entity_id":32769}' | \
	env AIROHA_OMCI_XGSPON_PATH="$temporary/xgspon" \
	AIROHA_OMCI_XGS_OMCI_EVIDENCE_PATH="$temporary/xgs-omci-evidence.json" \
	PATH="$temporary/bin:$PATH" \
	sh "$package/files/airoha-omci-control" >/dev/null 2>&1; then
	echo "xgs-pm-evidence mixed kernel and daemon instances" >&2
	exit 1
fi
sed 's/"kernel_instance_generation":101/"kernel_instance_generation":100/' \
	"$temporary/xgs-omci-evidence.json" > "$temporary/xgs-omci-evidence.valid"
mv "$temporary/xgs-omci-evidence.valid" "$temporary/xgs-omci-evidence.json"

sed 's/complete=0/complete=1/' "$temporary/xgspon/xgs_pm_snapshot" \
	> "$temporary/xgspon/xgs_pm_snapshot.invalid"
mv "$temporary/xgspon/xgs_pm_snapshot.invalid" \
	"$temporary/xgspon/xgs_pm_snapshot"
if printf '%s\n' '{"action":"xgs-pm-evidence","ani_entity_id":32769}' | \
	env AIROHA_OMCI_XGSPON_PATH="$temporary/xgspon" \
	AIROHA_OMCI_XGS_OMCI_EVIDENCE_PATH="$temporary/xgs-omci-evidence.json" \
	PATH="$temporary/bin:$PATH" \
	sh "$package/files/airoha-omci-control" >/dev/null 2>&1; then
	echo "xgs-pm-evidence accepted a falsely complete kernel snapshot" >&2
	exit 1
fi
sed 's/complete=1/complete=0/' "$temporary/xgspon/xgs_pm_snapshot" \
	> "$temporary/xgspon/xgs_pm_snapshot.valid"
mv "$temporary/xgspon/xgs_pm_snapshot.valid" \
	"$temporary/xgspon/xgs_pm_snapshot"

sed 's/sleep_request_messages=0/sleep_request_messages=1/' \
	"$temporary/xgspon/xgs_pm_snapshot" > "$temporary/xgspon/xgs_pm_snapshot.invalid"
mv "$temporary/xgspon/xgs_pm_snapshot.invalid" \
	"$temporary/xgspon/xgs_pm_snapshot"
if printf '%s\n' '{"action":"xgs-pm-evidence","ani_entity_id":32769}' | \
	env AIROHA_OMCI_XGSPON_PATH="$temporary/xgspon" \
	AIROHA_OMCI_XGS_OMCI_EVIDENCE_PATH="$temporary/xgs-omci-evidence.json" \
	PATH="$temporary/bin:$PATH" \
	sh "$package/files/airoha-omci-control" >/dev/null 2>&1; then
	echo "xgs-pm-evidence accepted a Sleep Request count without a sender" >&2
	exit 1
fi
sed 's/sleep_request_messages=1/sleep_request_messages=0/' \
	"$temporary/xgspon/xgs_pm_snapshot" > "$temporary/xgspon/xgs_pm_snapshot.valid"
mv "$temporary/xgspon/xgs_pm_snapshot.valid" \
	"$temporary/xgspon/xgs_pm_snapshot"

if printf '%s\n' '{"action":"xgs-omci-evidence","ani_entity_id":32770}' | \
	env AIROHA_OMCI_XGS_OMCI_EVIDENCE_PATH="$temporary/xgs-omci-evidence.json" \
	PATH="$temporary/bin:$PATH" \
	sh "$package/files/airoha-omci-control" >/dev/null 2>&1; then
	echo "xgs-omci-evidence accepted an unknown ANI-G" >&2
	exit 1
fi

sed 's/"complete":false/"complete":true/' "$temporary/xgs-omci-evidence.json" \
	> "$temporary/xgs-omci-evidence.invalid"
mv "$temporary/xgs-omci-evidence.invalid" "$temporary/xgs-omci-evidence.json"
if printf '%s\n' '{"action":"xgs-omci-evidence","ani_entity_id":32769}' | \
	env AIROHA_OMCI_XGS_OMCI_EVIDENCE_PATH="$temporary/xgs-omci-evidence.json" \
	PATH="$temporary/bin:$PATH" \
	sh "$package/files/airoha-omci-control" >/dev/null 2>&1; then
	echo "xgs-omci-evidence accepted a falsely complete daemon snapshot" >&2
	exit 1
fi

sed 's/complete=0/complete=1/' "$temporary/xgspon/xgs_ploam_evidence" \
	> "$temporary/xgspon/xgs_ploam_evidence.invalid"
mv "$temporary/xgspon/xgs_ploam_evidence.invalid" \
	"$temporary/xgspon/xgs_ploam_evidence"
if printf '%s\n' '{"action":"xgs-ploam-evidence","ani_entity_id":32769}' | \
	env AIROHA_OMCI_XGSPON_PATH="$temporary/xgspon" \
	PATH="$temporary/bin:$PATH" \
	sh "$package/files/airoha-omci-control" >/dev/null 2>&1; then
	echo "xgs-ploam-evidence accepted a falsely complete kernel snapshot" >&2
	exit 1
fi

sed 's/complete=0/complete=1/' "$temporary/xgspon/xgs_counter_evidence" \
	> "$temporary/xgspon/xgs_counter_evidence.invalid"
mv "$temporary/xgspon/xgs_counter_evidence.invalid" \
	"$temporary/xgspon/xgs_counter_evidence"
if printf '%s\n' '{"action":"xgs-counter-evidence","ani_entity_id":32769}' | \
	env AIROHA_OMCI_XGSPON_PATH="$temporary/xgspon" \
	PATH="$temporary/bin:$PATH" \
	sh "$package/files/airoha-omci-control" >/dev/null 2>&1; then
	echo "xgs-counter-evidence accepted a falsely complete kernel snapshot" >&2
	exit 1
fi
