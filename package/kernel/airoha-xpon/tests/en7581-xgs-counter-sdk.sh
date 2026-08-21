#!/bin/sh
set -eu

script_dir="$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)"
openwrt_dir="${1:-$(CDPATH= cd -- "$script_dir/../../../.." && pwd)}"
sdk_archive="${AIROHA_SDK_ARCHIVE:-$openwrt_dir/../airoha_sdk.tar.gz}"
driver="$openwrt_dir/package/kernel/airoha-xpon/src/airoha-xgspon.c"
tmp_dir="$(mktemp -d "${TMPDIR:-/tmp}/en7581-xgs-counter.XXXXXX")"
trap 'rm -rf "$tmp_dir"' EXIT INT TERM

[ -f "$sdk_archive" ] || {
	echo "Airoha SDK archive not found at $sdk_archive" >&2
	exit 2
}
[ -f "$driver" ] || {
	echo "XGS-PON driver not found at $driver" >&2
	exit 2
}

tar -xOf "$sdk_archive" \
	airoha_sdk/private/xpon_10g/inc/gpon/xgpon_mac_reg_c_header.h \
	> "$tmp_dir/xgpon_mac_reg_c_header.h"
tar -xOf "$sdk_archive" airoha_sdk/private/xpon_10g/src/gpon/gpon_dev.c \
	> "$tmp_dir/gpon_dev.c"
tar -xOf "$sdk_archive" airoha_sdk/private/xpon_10g/src/gpon/gpon_act.c \
	> "$tmp_dir/gpon_act.c"
tar -xOf "$sdk_archive" airoha_sdk/private/xpon_10g/src/gpon/gpon.c \
	> "$tmp_dir/gpon.c"
tar -xOf "$sdk_archive" airoha_sdk/private/xpon_10g/src/gpon/gpon_ploam.c \
	> "$tmp_dir/gpon_ploam.c"
tar -xOf "$sdk_archive" airoha_sdk/private/xpon_10g/inc/gpon/gpon_const.h \
	> "$tmp_dir/gpon_const.h"
tar -xOf "$sdk_archive" airoha_sdk/private/xpon_10g/inc/gpon/gpon_ploam_raw.h \
	> "$tmp_dir/gpon_ploam_raw.h"

require_fixed() {
	pattern="$1"
	file="$2"
	description="$3"

	if ! grep -Fq -- "$pattern" "$file"; then
		echo "missing $description: $pattern" >&2
		exit 1
	fi
}

require_absent() {
	pattern="$1"
	file="$2"
	description="$3"

	if grep -Fq -- "$pattern" "$file"; then
		echo "unexpected $description: $pattern" >&2
		exit 1
	fi
}

require_regex() {
	pattern="$1"
	file="$2"
	description="$3"

	if ! grep -Eq -- "$pattern" "$file"; then
		echo "missing $description: $pattern" >&2
		exit 1
	fi
}

require_function_occurrences() {
	function_name="$1"
	pattern="$2"
	expected="$3"
	description="$4"
	count="$(awk -v function_name="$function_name" -v pattern="$pattern" '
		$0 ~ function_name { in_function = 1 }
		in_function && index($0, pattern) { count++ }
		in_function && /^}/ { print count + 0; exit }
	' "$driver")"

	if [ "$count" -ne "$expected" ]; then
		echo "$description: expected $expected occurrence(s), found $count" >&2
		exit 1
	fi
}

check_register() {
	sdk_name="$1"
	sdk_address="$2"
	driver_name="$3"
	driver_offset="$4"

	require_fixed "$sdk_name;" "$tmp_dir/xgpon_mac_reg_c_header.h" \
		"SDK $sdk_name register"
	require_fixed "// $sdk_address" "$tmp_dir/xgpon_mac_reg_c_header.h" \
		"SDK $sdk_name address"
	require_fixed "#define $driver_name" "$driver" "OpenWrt $driver_name register"
	require_fixed "$driver_offset" "$driver" "OpenWrt $driver_name offset"
}

# The SDK's generated register map uses logical base 0x5000; the OpenWrt
# resource starts at physical 0x1fb65000, so these offsets are address-0x5000.
check_register RX_HLEND_HEC_CNT 58FC EN7581_XGSPON_RX_HLEND_HEC_CNT 0x8fc
check_register RX_ALLOC_HEC_CNT 5900 EN7581_XGSPON_RX_ALLOC_HEC_CNT 0x900
check_register RX_HDR_HEC_CNT 5904 EN7581_XGSPON_RX_HDR_HEC_CNT 0x904
check_register RX_PHY_HEC_ERR_CNT 5908 EN7581_XGSPON_RX_PHY_HEC_ERR_CNT 0x908
check_register RX_MIC_ERR_CNT 590C EN7581_XGSPON_RX_MIC_ERR_CNT 0x90c
check_register RX_KEY_ERR_CNT 5918 EN7581_XGSPON_RX_KEY_ERR_CNT 0x918
check_register RX_LOST_WCNT 591C EN7581_XGSPON_RX_LOST_WCNT 0x91c
check_register INVLD_PROF_BST_GNT_CNT 5920 EN7581_XGSPON_INVLD_PROF_BST_GNT_CNT 0x920
check_register RX_PLOAMD_CNT 5950 EN7581_XGSPON_RX_PLOAMD_CNT 0x950
check_register TX_PLOAMU_CNT 5954 EN7581_XGSPON_TX_PLOAMU_CNT 0x954
check_register RX_OMCI_CNT 5960 EN7581_XGSPON_RX_OMCI_CNT 0x960
check_register TX_OMCI_CNT 5964 EN7581_XGSPON_TX_OMCI_CNT 0x964
check_register TX_XGEM_CNT 596C EN7581_XGSPON_TX_XGEM_CNT 0x96c
check_register RX_NON_IDLE_BCNT 5978 EN7581_XGSPON_RX_NON_IDLE_BCNT 0x978
check_register TX_NON_IDLE_BCNT 597C EN7581_XGSPON_TX_NON_IDLE_BCNT 0x97c
check_register TX_NLF_XGEM_CNT 5980 EN7581_XGSPON_TX_NLF_XGEM_CNT 0x980
check_register SW_RST 5000 EN7581_XGSPON_SW_RST 0x000
check_register MBI_MPI_STOP 5004 EN7581_XGSPON_MBI_MPI_STOP 0x004
check_register US_PROF_VLD 511C EN7581_XGSPON_US_PROF_VLD 0x11c

require_fixed 'CNT_CLR_FLD_nml_cnt_clr' "$tmp_dir/xgpon_mac_reg_c_header.h" \
	'SDK explicit normal-counter clear bit'
require_fixed 'CNT_CLR_FLD_err_cnt_clr' "$tmp_dir/xgpon_mac_reg_c_header.h" \
	'SDK explicit error-counter clear bit'
require_fixed 'pXgponTcCounter->PSBdHECErrCount +=' "$tmp_dir/gpon_dev.c" \
	'SDK PSBd HEC aggregation'
require_fixed 'pXgponTcCounter->XGTCHECErrCount +=' "$tmp_dir/gpon_dev.c" \
	'SDK XGTC HEC aggregation'
require_fixed 'pXgponTcCounter->XGEMHECErrCount +=' "$tmp_dir/gpon_dev.c" \
	'SDK XGEM HEC aggregation'
require_fixed 'gpGponPriv->dsPloamCounter[PLOAM_DOWN_MSG_PROFILE]' "$tmp_dir/gpon_dev.c" \
	'SDK software downstream PLOAM counters'
require_fixed 'gpGponPriv->usPloamCounter[PLOAM_UP_MSG_SERIAL_NUMBER]' "$tmp_dir/gpon_dev.c" \
	'SDK software upstream PLOAM counters'
require_fixed 'pXgponUsMgntCounter->SleepReqMsgCnt = gpGponPriv->usPloamCounter[PLOAM_UP_MSG_SLEEP_REQUEST];' \
	"$tmp_dir/gpon_dev.c" 'SDK software upstream Sleep Request counter'
require_fixed 'pXgponDsMgntCounter->BaseOmciMsgRx = 0; /*actural value in omci app*/' \
	"$tmp_dir/gpon_dev.c" 'SDK OMCI application counter ownership'
require_fixed 'gponTCLODSEvent++' "$tmp_dir/gpon_act.c" \
	'SDK O5-to-O6 LODS event accounting'
require_fixed 'gponTCRestoreLODSEvent++' "$tmp_dir/gpon_act.c" \
	'SDK O6-to-O5 LODS restore accounting'
require_fixed 'gponTCReactivLODSEvent++' "$tmp_dir/gpon_act.c" \
	'SDK O6-to-O1 LODS reactivation accounting'
require_regex '^[[:space:]]*#define[[:space:]]+GPON_ACT_TO2_TIMER[[:space:]]+\(100\)[[:space:]]*$' \
	"$tmp_dir/gpon_const.h" 'SDK 100 ms O6 TO2 deadline'
require_fixed 'profileRegReadData = IO_GREG(US_PROF_VLD);' "$tmp_dir/gpon.c" \
	'SDK O5 profile snapshot before LODS'
require_fixed 'gponDevMpiStop(XPON_RESET_HOLD_ON);' "$tmp_dir/gpon.c" \
	'SDK O5 MPI quiesce before O6'
require_fixed 'gpon_act_change_state(GPON_10G_STATE_O6)' "$tmp_dir/gpon.c" \
	'SDK O5-to-O6 transition'
require_fixed 'gponKeyIdx.Bits.sw_set_pik_idx = gpGponPriv->gponCurKeyIdx.Bits.cur_pik_idx;' \
	"$tmp_dir/gpon.c" 'SDK PIK restoration after LODS'
require_fixed 'gponKeyIdx.Bits.sw_set_oik_idx = gpGponPriv->gponCurKeyIdx.Bits.cur_oik_idx;' \
	"$tmp_dir/gpon.c" 'SDK OIK restoration after LODS'
require_fixed 'IO_SREG(US_PROF_VLD, profileRegReadData)' "$tmp_dir/gpon.c" \
	'SDK upstream-profile restoration after LODS'
require_fixed 'SW_RST_FLD_xgpon_mac_sw_rst_n' "$tmp_dir/xgpon_mac_reg_c_header.h" \
	'SDK MAC reset field'
require_fixed 'MBI_MPI_STOP_FLD_mpi_tx_stop_done' "$tmp_dir/xgpon_mac_reg_c_header.h" \
	'SDK MPI stop completion field'
require_fixed 'US_PROF_VLD_FLD_us_prof3_vld' "$tmp_dir/xgpon_mac_reg_c_header.h" \
	'SDK upstream-profile valid fields'
require_fixed '#define PLOAM_UP_MSG_SLEEP_REQUEST' "$tmp_dir/gpon_ploam_raw.h" \
	'SDK Sleep Request message identifier'
require_fixed 'gponDevSendPloamMsg((PLOAM_RAW_General_T *)&ackMsg)' \
	"$tmp_dir/gpon_ploam.c" 'SDK XGS PLOAM send boundary'
require_absent 'PLOAM_UP_MSG_SLEEP_REQUEST' "$tmp_dir/gpon_ploam.c" \
	'SDK XGS Sleep Request send branch'
require_absent 'PLOAM_RAW_Sleep_Request_T' "$tmp_dir/gpon_ploam.c" \
	'SDK XGS Sleep Request sender'

# The diagnostic ABI must stay visibly incomplete until the cross-layer source
# and hardware counter semantics are board-verified.
require_fixed '#define EN7581_XGSPON_TC_HW_VALID' "$driver" \
	'OpenWrt TC field-valid mask'
require_fixed '#define EN7581_XGSPON_DS_HW_VALID' "$driver" \
	'OpenWrt downstream field-valid mask'
require_fixed '#define EN7581_XGSPON_US_HW_VALID' "$driver" \
	'OpenWrt upstream field-valid mask'
require_fixed '"version=1 complete=0 semantics=raw-hardware-modulo "' "$driver" \
	'fail-closed diagnostic snapshot marker'

# The versioned PM foundation must extend each finite-width hardware field,
# rebase on the kernel session and remain explicitly incomplete. Daemon-owned
# OMCI fields are merged outside the kernel. The SDK exposes its Sleep Request
# counter unconditionally but has no sender, so the driver-owned value is a
# valid constant zero; hardware wrap/reset semantics remain board-unverified.
require_fixed 'struct en7581_xgspon_pm_extender' "$driver" \
	'per-field monotonic counter extender'
require_fixed 'counter->total += (value - counter->previous) & counter->mask;' \
	"$driver" 'modulo hardware counter extension'
require_fixed 'priv->pm_hw[i].session_base = priv->pm_hw[i].total;' "$driver" \
	'kernel-session hardware baseline'
require_fixed '#define EN7581_XGSPON_PM_SAMPLE_MS' "$driver" \
	'bounded PM sampling interval'
require_fixed 'EN7581_XGSPON_PM_HLEND_HEC_1;' "$driver" \
	'independent 8-bit HEC field extension'
require_fixed 'priv->pm_profile_messages++' "$driver" \
	'accepted downstream Profile accounting'
require_function_occurrences \
	'en7581_xgspon_pm_record_accepted_locked' \
	'priv->pm_ranging_time_messages++;' 1 \
	'accepted Ranging Time message must increment PM exactly once'
require_fixed 'priv->pm_acknowledge_messages++' "$driver" \
	'successful upstream Acknowledge accounting'
require_fixed '"version=2 complete=0 semantics=kernel-instance-session-monotonic-partial "' \
	"$driver" 'fail-closed versioned PM snapshot marker'
require_fixed 'instance_generation=%llu' "$driver" \
	'kernel-instance PM snapshot boundary'
require_fixed 'sampling_assumption=single-wrap-no-reset-per-interval-unverified' \
	"$driver" 'truthful sampling limitation'
require_fixed '#define EN7581_XGSPON_TC_PM_REQUIRED' "$driver" \
	'complete TC field mask'
require_fixed '#define EN7581_XGSPON_DS_PM_REQUIRED' "$driver" \
	'complete downstream management field mask'
require_fixed '#define EN7581_XGSPON_US_PM_REQUIRED' "$driver" \
	'complete upstream management field mask'
require_regex '^[[:space:]]*#define[[:space:]]+EN7581_XGSPON_US_PM_VALID[[:space:]]+EN7581_XGSPON_US_PM_REQUIRED[[:space:]]*$' \
	"$driver" 'valid unsupported Sleep Request zero counter'
require_fixed '#define EN7581_XGSPON_ACTIVATION_O6' "$driver" \
	'XGS O6 activation state'
require_fixed 'struct en7581_xgspon_pm_lods' "$driver" \
	'LODS PM accounting state'
require_fixed 'state == EN7581_XGSPON_ACTIVATION_O6' "$driver" \
	'O5-to-O6 LODS transition accounting'
require_fixed 'static int en7581_xgspon_lods_begin_locked' "$driver" \
	'SDK-derived session-preserving LODS entry transaction'
require_fixed 'static int en7581_xgspon_lods_restore_locked' "$driver" \
	'SDK-derived session-preserving LODS restore transaction'
require_fixed '#define EN7581_XGSPON_LODS_TIMEOUT_MS' "$driver" \
	'bounded O6 recovery window'
require_fixed 'priv->lods_key_indices = readl(priv->mac + EN7581_XGSPON_CUR_KIDX)' \
	"$driver" 'LODS key-index snapshot'
require_fixed 'priv->service_tcont_count' "$driver" \
	'LODS service T-CONT readback coverage'
require_fixed 'priv->data_key_confirmed' "$driver" \
	'LODS confirmed data-key readback coverage'
require_fixed 'EN7581_XGSPON_SW_RESYNC_ENABLE | EN7581_XGSPON_SW_RESYNC_START' \
	"$driver" 'LODS TX resynchronization'
require_fixed '#define EN7581_XGSPON_TC_PM_VALID' "$driver" \
	'complete kernel TC mask after LODS restoration implementation'
require_fixed 'priv->pm_lods.reactivations++' "$driver" \
	'O6-to-O1 LODS reactivation accounting'
require_fixed 'lods_events=%llu lods_restored=%llu onu_reactivations_by_lods=%llu' \
	"$driver" 'LODS PM snapshot fields'
require_fixed 'baseline_omci_messages=0 extended_omci_messages=0' "$driver" \
	'explicit daemon-owned OMCI placeholders'
require_fixed 'sleep_request_messages=0' "$driver" \
	'authoritative unsupported Sleep Request zero field'

echo 'EN7581 XGS-PON counter evidence matches the recovered SDK register map'
