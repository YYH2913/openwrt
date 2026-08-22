#!/bin/sh
set -eu

script_dir="$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)"
openwrt_dir="${1:-$(CDPATH= cd -- "$script_dir/../../../.." && pwd)}"
omci_dir="${AIROHA_OMCI_DIR:-$openwrt_dir/../airoha-omci}"
sdk_archive="${AIROHA_SDK_ARCHIVE:-$openwrt_dir/../airoha_sdk.tar.gz}"
security="$openwrt_dir/package/kernel/airoha-xpon/src/airoha-xgs-security.c"
driver="$openwrt_dir/package/kernel/airoha-xpon/src/airoha-xgspon.c"
eqd_transaction="$openwrt_dir/package/kernel/airoha-xpon/src/airoha-xgs-eqd.h"
to1_transaction="$openwrt_dir/package/kernel/airoha-xpon/src/airoha-xgs-to1.h"
to1_test="$openwrt_dir/package/kernel/airoha-xpon/tests/xgs-to1-test.c"
gpon_driver="$openwrt_dir/package/kernel/airoha-xpon/src/airoha-gpon.c"
en7572_driver="$openwrt_dir/package/kernel/airoha-xpon/src/airoha-en7572.c"
package_makefile="$openwrt_dir/package/kernel/airoha-xpon/Makefile"
board_dts="$openwrt_dir/target/linux/airoha/dts/an7581-axon-xg2010g-ubi.dts"
luci_rpc="$openwrt_dir/package/network/services/luci-app-airoha-gpon/root/usr/share/rpcd/ucode/airoha-gpon.uc"
luci_view="$openwrt_dir/package/network/services/luci-app-airoha-gpon/htdocs/luci-static/resources/view/airoha-gpon.js"
ethernet_patch="$openwrt_dir/target/linux/airoha/patches-6.18/930-net-airoha-add-xgs-omcc-consumer.patch"
qos_patch="$openwrt_dir/target/linux/airoha/patches-6.18/931-net-airoha-expand-qdma-service-channel-map.patch"
tmp_dir="$(mktemp -d "${TMPDIR:-/tmp}/en7581-xgs-security.XXXXXX")"
trap 'rm -rf "$tmp_dir"' EXIT INT TERM

[ -f "$sdk_archive" ] || {
	echo "Airoha SDK archive not found at $sdk_archive" >&2
	exit 2
}
[ -f "$omci_dir/go.mod" ] || {
	echo "airoha-omci checkout not found at $omci_dir" >&2
	exit 2
}
[ -f "$ethernet_patch" ] && [ -f "$qos_patch" ] || {
	echo 'XGS Ethernet patches are missing' >&2
	exit 2
}
[ -f "$to1_transaction" ] && [ -f "$to1_test" ] || {
	echo 'XGS TO1 transaction sources are missing' >&2
	exit 2
}

"${HOSTCC:-cc}" -std=c11 -Wall -Wextra -Werror \
	"$to1_test" -o "$tmp_dir/xgs-to1-test"
"$tmp_dir/xgs-to1-test"

tar -xOf "$sdk_archive" airoha_sdk/private/xpon_10g/src/gpon/gpon_proc.c \
	> "$tmp_dir/gpon_proc.c"
tar -xOf "$sdk_archive" airoha_sdk/private/xpon_10g/src/gpon/gpon.c \
	> "$tmp_dir/gpon.c"
tar -xOf "$sdk_archive" airoha_sdk/private/xpon_10g/src/gpon/gpon_act.c \
	> "$tmp_dir/gpon_act.c"
tar -xOf "$sdk_archive" airoha_sdk/private/xpon_10g/src/gpon/gpon_dev.c \
	> "$tmp_dir/gpon_dev.c"
tar -xOf "$sdk_archive" airoha_sdk/private/xpon_10g/src/gpon/gpon_security.c \
	> "$tmp_dir/gpon_security.c"
tar -xOf "$sdk_archive" airoha_sdk/private/xpon_10g/src/gpon/gpon_ploam.c \
	> "$tmp_dir/gpon_ploam.c"
tar -xOf "$sdk_archive" airoha_sdk/private/xpon_10g/src/pwan/gpon_wan.c \
	> "$tmp_dir/gpon_wan.c"
tar -xOf "$sdk_archive" airoha_sdk/private/xpon_10g/inc/gpon/gpon_ploam_raw.h \
	> "$tmp_dir/gpon_ploam_raw.h"
tar -xOf "$sdk_archive" airoha_sdk/private/xpon_10g/inc/gpon/gpon_ploam.h \
	> "$tmp_dir/gpon_ploam.h"
tar -xOf "$sdk_archive" airoha_sdk/private/xpon_10g/inc/gpon/gpon_const.h |
	tr -d '\r' > "$tmp_dir/gpon_const.h"
tar -xOf "$sdk_archive" airoha_sdk/private/xpon_10g/inc/gpon/xgpon_mac_reg_c_header.h \
	> "$tmp_dir/xgpon_mac_reg_c_header.h"
tar -xOf "$sdk_archive" airoha_sdk/private/xpon_phy_10g/inc/en7581_reg.h \
	> "$tmp_dir/en7581_phy_reg.h"
tar -xOf "$sdk_archive" airoha_sdk/private/xpon_phy_10g/src/en7583.c \
	> "$tmp_dir/en7583_phy.c"
tar -xOf "$sdk_archive" airoha_sdk/private/xpon_phy_10g/src/phy_init.c \
	> "$tmp_dir/phy_init.c"

require_fixed() {
	pattern="$1"
	file="$2"
	description="$3"

	if ! grep -Fq -- "$pattern" "$file"; then
		echo "missing $description: $pattern" >&2
		exit 1
	fi
}

reject_fixed() {
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

# Preserve the SDK/G.9807.1 byte strings exactly. The PLOAM spelling is
# normative, including its missing second "i" in "Integrty".
require_fixed 'sk_string[] ={"SessionK"}' "$tmp_dir/gpon_proc.c" \
	'SDK session-key label'
require_fixed 'ploam_string[]= {"PLOAMIntegrtyKey"}' "$tmp_dir/gpon_proc.c" \
	'SDK PLOAM integrity label'
require_fixed 'omci_string[]= {"OMCIIntegrityKey"}' "$tmp_dir/gpon_proc.c" \
	'SDK OMCI integrity label'
require_fixed 'kek_string[]= {"KeyEncryptionKey"}' "$tmp_dir/gpon_proc.c" \
	'SDK key-encryption label'
require_fixed 'unchar defaultKey[16] ={0x55,0x55,0x55,0x55' \
	"$tmp_dir/gpon_proc.c" 'SDK default PLOAM key'
require_fixed 'gpGponPriv->gponCfg.reg_id ,36, msk0' "$tmp_dir/gpon_proc.c" \
	'SDK Registration-ID to MSK input'
require_fixed 'void gponDevSetPloamIk0(unchar *key)' "$tmp_dir/gpon_dev.c" \
	'SDK PLOAM IK programming boundary'
require_fixed 'void gponDevSetOmciIk0(unchar *key)' "$tmp_dir/gpon_dev.c" \
	'SDK OMCI IK programming boundary'
require_fixed 'void gponDevSetPloamIkIdx(unchar index)' "$tmp_dir/gpon_dev.c" \
	'SDK PLOAM IK selector boundary'
require_fixed 'void gponDevSetOmciIkIdx(unchar index)' "$tmp_dir/gpon_dev.c" \
	'SDK OMCI IK selector boundary'
require_fixed 'gponKeyIdx.Raw = IO_GREG(SW_SET_KIDX)' "$tmp_dir/gpon_dev.c" \
	'SDK key-index selector read-modify-write'
require_fixed 'gponKeyIdx.Bits.sw_set_pik_en = 1' "$tmp_dir/gpon_dev.c" \
	'SDK PLOAM key-index selector enable'
require_fixed 'gponKeyIdx.Bits.sw_set_oik_en = 1' "$tmp_dir/gpon_dev.c" \
	'SDK OMCI key-index selector enable'
require_fixed 'int gponDevSetSerialNumber(unchar *sn)' "$tmp_dir/gpon_dev.c" \
	'SDK XGS-PON serial register programming'
require_fixed 'gponVendor.Bits.vendor_id = (sn[0]<<24)' "$tmp_dir/gpon_dev.c" \
	'SDK serial vendor word layout'
require_fixed 'void gponDevSetRegId(unchar *regId)' "$tmp_dir/gpon_dev.c" \
	'SDK Registration-ID register programming'
require_fixed 'gponRegId3_0.Bits.rgs_id3_0 = (regId[32]<<24)' \
	"$tmp_dir/gpon_dev.c" 'SDK reverse Registration-ID word layout'
require_fixed 'gponTag0.Bits.pon_tag_0 =  (ponTag[4]<<24)' \
	"$tmp_dir/gpon_dev.c" 'SDK low-address PON-tag word layout'
require_fixed 'gponTag1.Bits.pon_tag_1 = (ponTag[0]<<24)' \
	"$tmp_dir/gpon_dev.c" 'SDK high-address PON-tag word layout'
require_fixed 'ploamIk0.Bits.pik0_0 = (key[12]<<24)' \
	"$tmp_dir/gpon_dev.c" 'SDK low-address PLOAM-key word layout'
require_fixed 'omciIk0.Bits.oik0_0 = (key[12]<<24)' \
	"$tmp_dir/gpon_dev.c" 'SDK low-address OMCI-key word layout'
require_fixed 'kek0.Bits.kek0_0 = (key[12]<<24)' \
	"$tmp_dir/gpon_dev.c" 'SDK low-address KEK word layout'
require_fixed 'gponDevSetPloamIkIdx(GPON_PLOAM_IK_IDX1);' \
	"$tmp_dir/gpon_security.c" 'SDK initial PLOAM key index'
require_fixed 'gponDevSetOmciIkIdx(GPON_OMCI_IK_IDX1);' \
	"$tmp_dir/gpon_security.c" 'SDK initial OMCI key index'
require_fixed 'mac will detect the registration ploam, and then change the ploamIk to index 0' \
	"$tmp_dir/gpon_ploam.c" 'SDK Registration key-switch trigger'
require_fixed 'regMsg.raw.key_index = GPON_PLOAM_IK_IDX0;' \
	"$tmp_dir/gpon_ploam.c" 'SDK Registration key index'
require_fixed '#define PLOAM_UP_MSG_LENGTH' "$tmp_dir/gpon_ploam.h" \
	'SDK 11-word upstream PLOAM record'
require_fixed 'for(i=0 ; i< PLOAM_UP_MSG_LENGTH ; i++)' \
	"$tmp_dir/gpon_dev.c" 'SDK upstream PLOAM FIFO write length'
require_fixed 'IO_SREG(PLOAMu_WDATA, htonl(pPloamMsg->value[i]))' \
	"$tmp_dir/gpon_dev.c" 'SDK upstream PLOAM FIFO byte order'
require_fixed 'regMsg.raw.dest_id[PLOAM_DEST_ID_HIGHER] = (unchar)(PLOAM_UNASSIGNED_ADDR >> 8)' \
	"$tmp_dir/gpon_ploam.c" 'SDK Serial Number unassigned destination'
require_fixed 'regMsg.raw.us_line_rate_cap = 0x03;' \
	"$tmp_dir/gpon_ploam.c" 'SDK XGS-PON upstream line-rate capability'
require_fixed 'regMsg.raw.us_line_rate_cap = 0x00;' \
	"$tmp_dir/gpon_ploam.c" 'SDK XG-PON upstream line-rate capability'
require_fixed '}else if(XMCS_IF_WAN_DETECT_MODE_XGSPON == gpPonSysData->sysPonMode' \
	"$tmp_dir/gpon_ploam.c" 'SDK fixed-mode line-rate capability branch'
require_fixed 'memcpy(regMsg.raw.registration_id, regId, GPON_REG_ID_LENS)' \
	"$tmp_dir/gpon_ploam.c" 'SDK Registration-ID message content'
require_fixed '#define PLOAM_DOWN_MSG_PROFILE' "$tmp_dir/gpon_ploam_raw.h" \
	'SDK Profile message identifier'
require_fixed 'GPON_CURR_STATE == GPON_10G_STATE_O2_3) || (GPON_CURR_STATE == GPON_10G_STATE_O4) || (GPON_CURR_STATE == GPON_10G_STATE_O5' \
	"$tmp_dir/gpon_ploam.c" 'SDK O2/O3/O4/O5 Profile acceptance states'
require_fixed 'if(GPON_CURR_STATE == GPON_10G_STATE_O2_3)' \
	"$tmp_dir/gpon_ploam.c" 'SDK initial-only Profile key derivation gate'
require_fixed 'ploam_send_acknowledge_msg(pRecvProfMsg->raw.seq_no,XGPON_PLOAM_ACK_OK)' \
	"$tmp_dir/gpon_ploam.c" 'SDK directed Profile acknowledgement'
require_fixed '#define PLOAM_DOWN_MSG_ASSIGN_ONUID' "$tmp_dir/gpon_ploam_raw.h" \
	'SDK Assign ONU-ID message identifier'
require_fixed '#define PLOAM_DOWN_MSG_RANGING_TIME' "$tmp_dir/gpon_ploam_raw.h" \
	'SDK Ranging Time message identifier'
require_fixed '#define PLOAM_DOWN_MSG_DEACTIVATE_ONUID' "$tmp_dir/gpon_ploam_raw.h" \
	'SDK Deactivate ONU-ID message identifier'
require_fixed '#define PLOAM_DOWN_MSG_DISABLE_SERIAL_NUM' "$tmp_dir/gpon_ploam_raw.h" \
	'SDK Disable Serial Number message identifier'
require_fixed '#define PLOAM_DOWN_MSG_ASSIGN_ALLOCID' "$tmp_dir/gpon_ploam_raw.h" \
	'SDK Assign Alloc-ID message identifier'
require_fixed '#define PLOAM_UP_MSG_ACKNOWLEDGE' "$tmp_dir/gpon_ploam_raw.h" \
	'SDK Acknowledge message identifier'
require_fixed '#define PLOAM_DOWN_MSG_KEY_CONTROL' "$tmp_dir/gpon_ploam_raw.h" \
	'SDK Key Control message identifier'
require_fixed '#define PLOAM_DOWN_MSG_SLEEP_ALLOW' "$tmp_dir/gpon_ploam_raw.h" \
	'SDK Sleep Allow message identifier'
require_fixed '#define PLOAM_DOWN_MSG_TUNING_CONTROL' "$tmp_dir/gpon_ploam_raw.h" \
	'SDK NG-PON2 Tuning Control message identifier'
require_fixed '#define PLOAM_DOWN_MSG_PROTECTION_CONTROL' "$tmp_dir/gpon_ploam_raw.h" \
	'SDK NG-PON2 Protection Control message identifier'
require_fixed '#define PLOAM_DOWN_MSG_CHANGE_POWER_LEVEL' "$tmp_dir/gpon_ploam_raw.h" \
	'SDK Change Power Level message identifier'
require_fixed 'static int ploam_recv_calibration_request' "$tmp_dir/gpon_ploam.c" \
	'SDK recognized Calibration Request no-op handler'
require_fixed 'static int ploam_recv_adjust_tx_wavelength' "$tmp_dir/gpon_ploam.c" \
	'SDK recognized Adjust TX Wavelength no-op handler'
require_fixed 'static int ploam_recv_change_power_level' "$tmp_dir/gpon_ploam.c" \
	'SDK recognized Change Power Level no-op handler'
require_fixed 'static int ploam_recv_power_consum_inquire' "$tmp_dir/gpon_ploam.c" \
	'SDK recognized Power Consumption Inquire no-op handler'
require_fixed 'static int ploam_recv_rate_control' "$tmp_dir/gpon_ploam.c" \
	'SDK recognized Rate Control no-op handler'
require_fixed 'ploam_recv_tuning_control do nothing in non-NGPON2 mode' \
	"$tmp_dir/gpon_ploam.c" 'SDK fixed-wavelength Tuning Control no-op'
require_fixed 'ploam_recv_system_profile do nothing in non-NGPON2 mode' \
	"$tmp_dir/gpon_ploam.c" 'SDK fixed-wavelength System Profile no-op'
require_fixed 'ploam_recv_channel_profile do nothing in non-NGPON2 mode' \
	"$tmp_dir/gpon_ploam.c" 'SDK fixed-wavelength Channel Profile no-op'
require_fixed 'ploam_recv_protect_control do nothing in non-NGPON2 mode' \
	"$tmp_dir/gpon_ploam.c" 'SDK fixed-wavelength Protection Control no-op'
require_fixed '#define PLOAM_UP_MSG_KEY_REPORT' "$tmp_dir/gpon_ploam_raw.h" \
	'SDK Key Report message identifier'
require_fixed 'unchar key_lens' "$tmp_dir/gpon_ploam_raw.h" \
	'SDK Key Control length field'
require_fixed 'unchar key_fragment[32]' "$tmp_dir/gpon_ploam_raw.h" \
	'SDK Key Report fragment boundary'
require_fixed 'get_random_bytes(dataKey, GPON_DATA_ENCRYPT_KEY_LENS)' \
	"$tmp_dir/gpon_dev.c" 'SDK random data-key generation'
require_fixed 'gpon_aes_ecb_encrypt' "$tmp_dir/gpon_dev.c" \
	'SDK KEK AES-ECB wrapping'
require_fixed 'gponDevSetAesTxKeyValid(dataKeyIndex)' "$tmp_dir/gpon_dev.c" \
	'SDK upstream data-key switch'
require_fixed 'XGPON_US_KEY_SWITCH_DONE_INT' "$tmp_dir/gpon_dev.c" \
	'SDK upstream key-switch completion'
require_fixed 'memcpy(hashData+16,keyName, GPON_DATA_ENCRYPT_KEY_LENS)' \
	"$tmp_dir/gpon_dev.c" 'SDK existing-key proof label'
require_fixed 'GPON_START_TIMER(gpGponPriv->gponSecurity.TK4_timer' \
	"$tmp_dir/gpon_dev.c" 'SDK TK4 data-key transaction deadline'
require_fixed 'GPON_START_TIMER(gpGponPriv->gponSecurity.TK5_timer' \
	"$tmp_dir/gpon_dev.c" 'SDK TK5 Key Report retry timer'
require_fixed 'unchar completion_code' "$tmp_dir/gpon_ploam_raw.h" \
	'SDK Acknowledge completion-code layout'
require_fixed 'unchar padding[35]' "$tmp_dir/gpon_ploam_raw.h" \
	'SDK Acknowledge padding boundary'
require_fixed 'XGPON_PLOAM_ACK_OK = 0' "$tmp_dir/gpon_ploam.h" \
	'SDK Acknowledge success completion code'
require_fixed 'XGPON_PLOAM_ACK_PROCES_ERR' "$tmp_dir/gpon_ploam.h" \
	'SDK Acknowledge processing-error completion code'
require_fixed 'ackMsg.raw.seq_no = seqNo' "$tmp_dir/gpon_ploam.c" \
	'SDK Acknowledge sequence echo'
require_fixed 'ackMsg.raw.completion_code = completionCode' \
	"$tmp_dir/gpon_ploam.c" 'SDK Acknowledge completion-code encoding'
require_fixed 'unchar eqd_value[4]' "$tmp_dir/gpon_ploam_raw.h" \
	'SDK Ranging Time EqD layout'
require_fixed 'unchar padding[31]' "$tmp_dir/gpon_ploam_raw.h" \
	'SDK Ranging Time padding boundary'
require_fixed 'unchar alloc_id_type' "$tmp_dir/gpon_ploam_raw.h" \
	'SDK Alloc-ID operation layout'
require_fixed 'unchar padding[33]' "$tmp_dir/gpon_ploam_raw.h" \
	'SDK Assign Alloc-ID padding boundary'
require_fixed '#define PLOAM_EQD_ABSOLUTE 1' "$tmp_dir/gpon_ploam.h" \
	'SDK absolute EqD operation'
require_fixed '#define PLOAM_EQD_NEGATIVE 1' "$tmp_dir/gpon_ploam.h" \
	'SDK negative relative EqD operation'
require_fixed '#define PLOAM_ALLOC_ID_ASSIGN' "$tmp_dir/gpon_ploam.h" \
	'SDK Alloc-ID assignment operation'
require_fixed '#define PLOAM_ALLOC_ID_DEALLOCATE' "$tmp_dir/gpon_ploam.h" \
	'SDK Alloc-ID deallocation operation'
require_fixed '/* Request_Registration message */' "$tmp_dir/gpon_ploam_raw.h" \
	'SDK Request Registration layout'
require_fixed 'ploam_send_registration_msg(pReqRegistMsg->raw.seq_no' \
	"$tmp_dir/gpon_ploam.c" 'SDK Request Registration sequence echo'
require_fixed 'if(GPON_CURR_STATE == GPON_10G_STATE_O5 || (ng2_o4_to_09 == 1 && GPON_CURR_STATE == GPON_10G_STATE_O9))' \
	"$tmp_dir/gpon_ploam.c" 'SDK Request Registration O5 state gate'
require_fixed 'gpon_act_change_state(GPON_10G_STATE_O4)' \
	"$tmp_dir/gpon_ploam.c" 'SDK Assign ONU-ID O2/O3-to-O4 transition'
require_fixed 'gpon_act_change_state(GPON_10G_STATE_O5)' \
	"$tmp_dir/gpon_ploam.c" 'SDK Ranging Time O4-to-O5 transition'
require_regex '^#define[[:space:]]+GPON_ACT_TO1_TIMER[[:space:]]+\(10000\)$' \
	"$tmp_dir/gpon_const.h" 'SDK 10-second TO1 interval'
require_fixed 'void gpon_act_to1_timer_expires' "$tmp_dir/gpon_act.c" \
	'SDK TO1 callback'
require_fixed 'if(GPON_CURR_STATE == GPON_10G_STATE_O4 )' \
	"$tmp_dir/gpon_act.c" 'SDK TO1 O4 state gate'
require_fixed 'gpon_act_change_state(GPON_10G_STATE_O2_3);' \
	"$tmp_dir/gpon_act.c" 'SDK TO1 O2/O3 fallback'
require_fixed 'gpon_set_alarmBit(SUF_INDEX);' "$tmp_dir/gpon_act.c" \
	'SDK TO1 startup-failure alarm'
require_fixed 'GPON_START_TIMER(gpGponPriv->to1_timer,gpGponPriv->gponCfg.to1Timer)' \
	"$tmp_dir/gpon_act.c" 'SDK O4 TO1 arming'
require_fixed 'tasklet_hi_schedule(&gpGponPriv->rangingAck_task)' \
	"$tmp_dir/gpon_ploam.c" 'SDK pre-Registration Ranging Time ACK scheduling'
require_fixed 'ackMsg.raw.key_index = gpGponPriv->gponSecurity.ploamIkIdx' \
	"$tmp_dir/gpon_ploam.c" 'SDK Ranging Time ACK active PLOAM key selection'
require_fixed 'tasklet_hi_schedule(&gpGponPriv->swreplyploam_task)' \
	"$tmp_dir/gpon_ploam.c" 'SDK initial Registration scheduling'
require_fixed 'ploam_send_registration_msg(0,gpGponPriv->gponCfg.reg_id);' \
	"$tmp_dir/gpon_dev.c" 'SDK initial Registration sequence zero'
require_fixed 'memcmp(gpGponPriv->gponCfg.sn, pAssignOnuIdMsg->raw.sn' \
	"$tmp_dir/gpon_ploam.c" 'SDK Assign ONU-ID serial ownership check'
require_fixed 'gponDevSetOnuId(msgAssignOnuId, GPON_ONU_ID_VALID)' \
	"$tmp_dir/gpon_ploam.c" 'SDK Assign ONU-ID hardware transition'
require_fixed 'gpGponPriv->gponCfg.omcc = gpGponPriv->gponCfg.onu_id' \
	"$tmp_dir/gpon_ploam.c" 'SDK default OMCC XGEM identity'
require_fixed 'gpWanPriv->gpon.allocId[0] = gpGponPriv->gponCfg.onu_id' \
	"$tmp_dir/gpon_ploam.c" 'SDK default OMCC Alloc-ID identity'
require_fixed 'gwan_create_new_gemport(msgAssignOnuId,0,GPON_UNICAST_GEM,msgAssignOnuId)' \
	"$tmp_dir/gpon_ploam.c" 'SDK default OMCC XGEM transaction'
require_regex '^#define[[:space:]]+CONFIG_GPON_10G_MAX_TCONT[[:space:]]+\(32\)$' \
	"$tmp_dir/gpon_const.h" 'SDK 32 T-CONT service limit'
require_regex '^\+#define[[:space:]]+AIROHA_QDMA_SERVICE_CHANNELS[[:space:]]+32$' \
	"$qos_patch" \
	'32-channel QDMA ownership map'
require_fixed 'DECLARE_BITMAP(qos_channel_map, AIROHA_QDMA_SERVICE_CHANNELS);' \
	"$qos_patch" 'QDMA ownership bitmap capacity'
require_fixed 'DECLARE_BITMAP(acquired, AIROHA_XGS_SERVICE_MAX_TCONTS);' \
	"$ethernet_patch" 'XGS provisional channel ownership'
require_fixed 'for_each_set_bit(channel, wanted,' "$ethernet_patch" \
	'XGS full-range channel iteration'
require_fixed 'AIROHA_XGS_SERVICE_MAX_TCONTS) {' "$ethernet_patch" \
	'XGS full-range channel iteration bound'
require_fixed 'test_and_set_bit(channel, qdma->qos_channel_map)' \
	"$ethernet_patch" 'XGS and ordinary QoS channel exclusion'
require_fixed 'mutex_lock(&flow_offload_mutex);' "$ethernet_patch" \
	'XGS QDMA mode-switch exclusion'
require_fixed 'qdma = airoha_qdma_deref(dev);' "$ethernet_patch" \
	'XGS protected QDMA dereference'
require_fixed 'mutex_unlock(&flow_offload_mutex);' "$ethernet_patch" \
	'XGS QDMA mode-switch exclusion release'
require_fixed 'for_each_set_bit(channel, oldmap, AIROHA_XGS_SERVICE_MAX_TCONTS)' \
	"$ethernet_patch" 'XGS removed-channel ownership release'
require_fixed 'REG_GEM_PORT_CFG                GEM_PORT_CFG;     // 5274' \
	"$tmp_dir/xgpon_mac_reg_c_header.h" 'SDK XGEM table command offset'
require_fixed 'REG_GEM_PORT_STS                GEM_PORT_STS;     // 5278' \
	"$tmp_dir/xgpon_mac_reg_c_header.h" 'SDK XGEM table status offset'
require_fixed 'GEM_PORT_CFG_FLD_gpid_cmd                              REG_FLD(1, 31)' \
	"$tmp_dir/xgpon_mac_reg_c_header.h" 'SDK XGEM table command bit'
require_fixed 'GEM_PORT_CFG_FLD_gpid_vld                              REG_FLD(1, 18)' \
	"$tmp_dir/xgpon_mac_reg_c_header.h" 'SDK XGEM valid bit'
require_fixed 'GEM_PORT_CFG_FLD_gpid_type                             REG_FLD(1, 17)' \
	"$tmp_dir/xgpon_mac_reg_c_header.h" 'SDK XGEM type bit'
require_fixed 'GEM_PORT_CFG_FLD_gpid_us_encrypt                       REG_FLD(1, 16)' \
	"$tmp_dir/xgpon_mac_reg_c_header.h" 'SDK XGEM upstream-encryption bit'
require_fixed 'GEM_PORT_CFG_FLD_gem_port_id                           REG_FLD(16, 0)' \
	"$tmp_dir/xgpon_mac_reg_c_header.h" 'SDK XGEM ID field'
require_fixed 'GEM_PORT_STS_FLD_gpid_cmd_done                         REG_FLD(1, 31)' \
	"$tmp_dir/xgpon_mac_reg_c_header.h" 'SDK XGEM command completion bit'
require_fixed 'GEM_PORT_STS_FLD_gpid_rd_sts                           REG_FLD(3, 0)' \
	"$tmp_dir/xgpon_mac_reg_c_header.h" 'SDK XGEM readback field'
require_fixed 'REG_EQD                         EQD;              // 5114' \
	"$tmp_dir/xgpon_mac_reg_c_header.h" 'SDK EqD register offset'
require_fixed 'gponEqd.Bits.eqd = (newEqd << 2);' "$tmp_dir/gpon_dev.c" \
	'SDK XGS-PON absolute EqD scaling'
require_fixed 'REG_TCONT_ID_CFG                TCONT_ID_CFG;     // 5250' \
	"$tmp_dir/xgpon_mac_reg_c_header.h" 'SDK T-CONT command offset'
require_fixed 'REG_TCONT_ID_STS                TCONT_ID_STS;     // 5254' \
	"$tmp_dir/xgpon_mac_reg_c_header.h" 'SDK T-CONT status offset'
require_fixed 'TCONT_ID_CFG_FLD_tcont_cmd                             REG_FLD(1, 31)' \
	"$tmp_dir/xgpon_mac_reg_c_header.h" 'SDK T-CONT command bit'
require_fixed 'TCONT_ID_CFG_FLD_tcont_id_index                        REG_FLD(5, 20)' \
	"$tmp_dir/xgpon_mac_reg_c_header.h" 'SDK T-CONT index field'
require_fixed 'TCONT_ID_CFG_FLD_wr_tcont_id_vld                       REG_FLD(1, 16)' \
	"$tmp_dir/xgpon_mac_reg_c_header.h" 'SDK T-CONT valid bit'
require_fixed 'TCONT_ID_CFG_FLD_wr_tcont_id                           REG_FLD(14, 0)' \
	"$tmp_dir/xgpon_mac_reg_c_header.h" 'SDK T-CONT Alloc-ID field'
require_fixed 'TCONT_ID_STS_FLD_tcont_cmd_done                        REG_FLD(1, 31)' \
	"$tmp_dir/xgpon_mac_reg_c_header.h" 'SDK T-CONT completion bit'
require_fixed 'TCONT_ID_STS_FLD_rd_tcont_id_vld                       REG_FLD(1, 16)' \
	"$tmp_dir/xgpon_mac_reg_c_header.h" 'SDK T-CONT readback valid bit'
require_fixed 'TCONT_ID_STS_FLD_rd_tcont_id                           REG_FLD(14, 0)' \
	"$tmp_dir/xgpon_mac_reg_c_header.h" 'SDK T-CONT readback Alloc-ID field'
require_fixed 'GPON_TCONT_CMD_RESULT_t gponDevSetTCont(' "$tmp_dir/gpon_dev.c" \
	'SDK T-CONT write transaction'
require_fixed 'static GPON_TCONT_CMD_RESULT_t gponDevGetTCont(' \
	"$tmp_dir/gpon_dev.c" 'SDK T-CONT read transaction'
require_fixed 'if(tcont_index < 0 || tcont_index > 31)' "$tmp_dir/gpon_dev.c" \
	'SDK 32-entry T-CONT table'
require_fixed 'Because the TCONT 0 is an shadow of ONU ID' "$tmp_dir/gpon_dev.c" \
	'SDK T-CONT 0 ONU-ID shadow rule'
require_fixed 'for(i = 1 ; i < CONFIG_GPON_10G_MAX_TCONT ; i++)' \
	"$tmp_dir/gpon_dev.c" 'SDK business T-CONT allocation starts at index 1'
require_fixed 'gponDevSetTCont(GPON_TCONT_INVALID, i, 0x3FF)' \
	"$tmp_dir/gpon_dev.c" 'SDK unassigned T-CONT value'
require_fixed 'REG_DBG_CAP_SETTING             DBG_CAP_SETTING;  // 5800' \
	"$tmp_dir/xgpon_mac_reg_c_header.h" 'SDK OMCI MIC capability offset'
require_fixed 'DBG_CAP_SETTING_FLD_hw_cal_ds_omci_mic                 REG_FLD(1, 4)' \
	"$tmp_dir/xgpon_mac_reg_c_header.h" 'SDK downstream hardware OMCI MIC bit'
require_fixed 'DBG_CAP_SETTING_FLD_hw_cal_us_omci_mic                 REG_FLD(1, 3)' \
	"$tmp_dir/xgpon_mac_reg_c_header.h" 'SDK upstream hardware OMCI MIC bit'
require_fixed 'if(pRxMsg->raw.no_mic)' "$tmp_dir/gpon_wan.c" \
	'SDK downstream software-MIC selector'
require_fixed 'gwan_check_ds_omci_mic(skb)' "$tmp_dir/gpon_wan.c" \
	'SDK downstream software OMCI MIC verification'
require_fixed 'pTxMsg->raw.mic_idx = gpGponPriv->gponSecurity.omciIkIdx' \
	"$tmp_dir/gpon_wan.c" 'SDK upstream OMCI key-index metadata'
require_fixed 'int gponDevSetGemInfoNoCheck(ushort gemPortId' \
	"$tmp_dir/gpon_dev.c" 'SDK XGEM table write transaction'
require_fixed 'int gponDevGetGemInfo(ushort gemPortId' \
	"$tmp_dir/gpon_dev.c" 'SDK XGEM table read transaction'
require_fixed 'int gwan_create_new_gemport(ushort gemPortId, unchar channel' \
	"$tmp_dir/gpon_wan.c" 'SDK service XGEM software binding boundary'
require_fixed 'gpWanPriv->gpon.gemPort[i].info.channel = channel' \
	"$tmp_dir/gpon_wan.c" 'SDK service XGEM to T-CONT channel binding'
require_fixed 'FE_API_SET_CHANNEL_ENABLE(FE_GDM_SEL_GDMA2, FE_GDM_SEL_TX, channel, FE_ENABLE)' \
	"$tmp_dir/gpon_wan.c" 'SDK GDM2 T-CONT channel enable'
require_fixed 'QDMA_API_SET_CHANNEL_CLOSE_STATUS(ECNT_QDMA_WAN,&chnlCloseStatusSet)' \
	"$tmp_dir/gpon_wan.c" 'SDK WAN QDMA T-CONT channel state'
require_fixed 'pTxMsg->raw.gem = gpWanPriv->gpon.gemPort[gemIdx].info.portId' \
	"$tmp_dir/gpon_wan.c" 'SDK service XGEM transmit descriptor identity'
require_fixed 'program_service restores T-CONTs before it re-enables the XGEMs' \
	"$driver" 'ordered XGS service rollback'
require_fixed 'old_tcont_count * sizeof(*old_tconts)' "$driver" \
	'complete XGS T-CONT rollback snapshot'
require_fixed 'old_xgem_count * sizeof(*old_xgems)' "$driver" \
	'complete XGS XGEM rollback snapshot'
require_fixed 'if (!ret)' "$driver" 'rollback snapshot retained on failure'
require_fixed 'rollback_both:' "$driver" \
	'XGS service clear rollback path'
require_fixed 'airoha_xgs_qdma_service_clear(priv->ethernet_np)' "$driver" \
	'XGS service clear QDMA transaction'
if ! sed -n '/static int en7581_xgspon_clear_service_locked/,/^}/p' "$driver" |
	grep -Fq 'airoha_xgs_qdma_service_apply(priv->ethernet_np,'; then
	echo 'XGS service clear does not restore QDMA state after failure' >&2
	exit 1
fi
if sed -n '/static int en7581_xgspon_restore_service_locked/,/^}/p' "$driver" |
	grep -A2 -F 'en7581_xgspon_program_xgem(priv, xgems[i].xgem_id,' |
	grep -Fq 'true, xgems[i].unicast'; then
	echo 'unexpected XGEM enable before T-CONT rollback restore' >&2
	exit 1
fi
require_fixed 'pTxMsg->raw.channel = gpWanPriv->gpon.gemPort[gemIdx].info.channel' \
	"$tmp_dir/gpon_wan.c" 'SDK service XGEM transmit descriptor channel'
require_fixed 'pTxMsg->raw.nboq = gpWanPriv->gpon.gemPort[gemIdx].info.channel' \
	"$tmp_dir/gpon_wan.c" 'SDK service XGEM transmit descriptor queue'
require_fixed 'unchar pon_tag[GPON_TAG_LENS]' "$tmp_dir/gpon_ploam_raw.h" \
	'SDK Profile PON-tag field'
require_fixed 'unchar msg_mic[PLOAM_MSG_MIC_LEN]' "$tmp_dir/gpon_ploam_raw.h" \
	'SDK downstream PLOAM MIC field'
require_fixed '#define PLOAM_DOWN_MSG_LENGTH' "$tmp_dir/gpon_ploam.h" \
	'SDK 13-word downstream PLOAM record'
require_fixed 'REG_PLOAMd_FIFO_STS             PLOAMd_FIFO_STS;  // 5308' \
	"$tmp_dir/xgpon_mac_reg_c_header.h" 'SDK downstream PLOAM FIFO status offset'
require_fixed 'REG_PLOAMd_RDATA                PLOAMd_RDATA;     // 530C' \
	"$tmp_dir/xgpon_mac_reg_c_header.h" 'SDK downstream PLOAM FIFO data offset'
require_fixed 'REG_PLOAMu_FIFO_STS             PLOAMu_FIFO_STS;  // 5300' \
	"$tmp_dir/xgpon_mac_reg_c_header.h" 'SDK upstream PLOAM FIFO status offset'
require_fixed 'REG_PLOAMu_WDATA                PLOAMu_WDATA;     // 5304' \
	"$tmp_dir/xgpon_mac_reg_c_header.h" 'SDK upstream PLOAM FIFO data offset'
require_fixed 'PLOAMu_FIFO_STS_FLD_ploamu_fifo_avail' \
	"$tmp_dir/xgpon_mac_reg_c_header.h" 'SDK upstream FIFO availability field'
require_fixed 'PLOAMu_FIFO_STS_FLD_ploamu_fifo_ovrn' \
	"$tmp_dir/xgpon_mac_reg_c_header.h" 'SDK upstream FIFO overrun field'
require_fixed 'O23_O4_PLOAMU_CTRL_FLD_o23_o4_ploamu_ctrl' \
	"$tmp_dir/xgpon_mac_reg_c_header.h" 'SDK software/hardware PLOAM control field'
require_fixed 'INT_STATUS_FLD_o23_sn_onu_send_int' \
	"$tmp_dir/xgpon_mac_reg_c_header.h" 'SDK Serial Number completion interrupt'
require_fixed 'INT_STATUS_FLD_o4_registration_send_int' \
	"$tmp_dir/xgpon_mac_reg_c_header.h" 'SDK Registration completion interrupt'
require_fixed 'INT_STATUS_FLD_us_no_msg_send_int' \
	"$tmp_dir/xgpon_mac_reg_c_header.h" 'SDK upstream no-message interrupt'
require_fixed 'INT_STATUS_FLD_ploamu_send_int                         REG_FLD(1, 1)' \
	"$tmp_dir/xgpon_mac_reg_c_header.h" 'SDK upstream PLOAM completion bit'
require_fixed 'INT_STATUS_FLD_o23_sn_onu_send_int                     REG_FLD(1, 3)' \
	"$tmp_dir/xgpon_mac_reg_c_header.h" 'SDK Serial Number completion bit'
require_fixed 'INT_STATUS_FLD_o4_registration_send_int                REG_FLD(1, 5)' \
	"$tmp_dir/xgpon_mac_reg_c_header.h" 'SDK Registration completion bit'
require_fixed 'INT_STATUS_FLD_us_no_msg_send_int                      REG_FLD(1, 6)' \
	"$tmp_dir/xgpon_mac_reg_c_header.h" 'SDK upstream no-message bit'
require_fixed 'INT_STATUS_FLD_fifo_err_int                            REG_FLD(1, 15)' \
	"$tmp_dir/xgpon_mac_reg_c_header.h" 'SDK FIFO error interrupt bit'
require_fixed 'INT_STATUS_FLD_tx_err_int                              REG_FLD(1, 16)' \
	"$tmp_dir/xgpon_mac_reg_c_header.h" 'SDK TX error interrupt bit'
require_fixed 'FIFO_ERR_STS_FLD_tx_ploamu_fifo_ovrn                   REG_FLD(1, 2)' \
	"$tmp_dir/xgpon_mac_reg_c_header.h" 'SDK upstream PLOAM FIFO error bit'
require_fixed 'TX_ERR_STS_FLD_tx_bst_sgl_diff_err                     REG_FLD(1, 0)' \
	"$tmp_dir/xgpon_mac_reg_c_header.h" 'SDK burst/signal mismatch bit'
require_fixed 'TX_ERR_STS_FLD_tx_late_start_err                       REG_FLD(1, 1)' \
	"$tmp_dir/xgpon_mac_reg_c_header.h" 'SDK late-start error bit'
require_fixed 'TX_ERR_STS_FLD_tx_prof_invld_err                       REG_FLD(1, 2)' \
	"$tmp_dir/xgpon_mac_reg_c_header.h" 'SDK invalid-profile error bit'
require_fixed 'INT_ENABLE_FLD_ploamd_recv_int_en                      REG_FLD(1, 0)' \
	"$tmp_dir/xgpon_mac_reg_c_header.h" 'SDK downstream PLOAM interrupt bit'
require_fixed 'REG_FIFO_ERR_STS                FIFO_ERR_STS;     // 5050' \
	"$tmp_dir/xgpon_mac_reg_c_header.h" 'SDK FIFO error status offset'
require_fixed 'REG_O23_O4_PLOAMU_CTRL          O23_O4_PLOAMU_CTRL; // 5100' \
	"$tmp_dir/xgpon_mac_reg_c_header.h" 'SDK software-reply control offset'
require_fixed 'REG_ACTIVATION_ST               ACTIVATION_ST;    // 5104' \
	"$tmp_dir/xgpon_mac_reg_c_header.h" 'SDK activation-state offset'
require_fixed 'REG_DBG_RESYNC                  DBG_RESYNC;       // 582C' \
	"$tmp_dir/xgpon_mac_reg_c_header.h" 'SDK TX-sync status offset'
require_fixed 'DBG_RESYNC_FLD_tx_sync_rdy                             REG_FLD(1, 31)' \
	"$tmp_dir/xgpon_mac_reg_c_header.h" 'SDK TX-sync ready bit'
require_fixed 'RDM_DLY_FLD_max_rdm_dly                                REG_FLD(12, 16)' \
	"$tmp_dir/xgpon_mac_reg_c_header.h" 'SDK random-delay field'
require_fixed 'randomDelay.Bits.max_rdm_dly =(randomDelay.Bits.max_rdm_dly|0x800)' \
	"$tmp_dir/gpon_ploam.c" 'SDK random-delay enable bit'
require_fixed 'delay = ((randomDelay.Bits.max_rdm_dly&0x7ff)*32*8)' \
	"$tmp_dir/gpon_ploam.c" 'SDK Serial Number random-delay conversion'
require_fixed 'if(intStatus.Bits.o23_sn_onu_send_int)' "$tmp_dir/gpon.c" \
	'SDK Serial Number completion handling'
require_fixed 'if(intStatus.Bits.o4_registration_send_int)' "$tmp_dir/gpon.c" \
	'SDK Registration completion handling'
require_fixed 'if(intStatus.Bits.o4_ranging_req_recv_int)' "$tmp_dir/gpon.c" \
	'SDK O4 Ranging Request hardware event'
require_fixed 'gpon_key_index_change_by_hw(&gpGponPriv->gponSecurity)' \
	"$tmp_dir/gpon.c" 'SDK O4 hardware key-index transition'
require_fixed 'gponDevGetPloamIkIdx(&ploamIkIdx)' "$tmp_dir/gpon_security.c" \
	'SDK PLOAM current-index readback'
require_fixed 'gponDevGetOmciIkIdx(&omciIkIdx)' "$tmp_dir/gpon_security.c" \
	'SDK OMCI current-index readback'
require_fixed 'if(intStatus.Bits.us_no_msg_send_int)' "$tmp_dir/gpon.c" \
	'SDK no-message handling'
require_fixed 'if(intStatus.Bits.fifo_err_int)' "$tmp_dir/gpon.c" \
	'SDK FIFO error handling'
require_fixed 'pPloamMsg->value[i] = ntohl(IO_GREG(PLOAMd_RDATA))' \
	"$tmp_dir/gpon_dev.c" 'SDK FIFO network-byte-order conversion'
require_fixed 'memcpy(gpGponPriv->gponCfg.ponTag, pRecvProfMsg->raw.pon_tag' \
	"$tmp_dir/gpon_ploam.c" 'SDK PON-tag ownership transition'
require_fixed 'tasklet_hi_schedule(&gpGponPriv->securityKey_task)' \
	"$tmp_dir/gpon_ploam.c" 'SDK post-Profile key derivation transition'

require_fixed '"SessionK"' "$security" 'OpenWrt session-key label'
require_fixed '"PLOAMIntegrtyKey"' "$security" 'OpenWrt PLOAM integrity label'
require_fixed '"OMCIIntegrityKey"' "$security" 'OpenWrt OMCI integrity label'
require_fixed '"KeyEncryptionKey"' "$security" 'OpenWrt key-encryption label'
require_fixed '[0 ... AIROHA_XGS_KEY_SIZE - 1] = 0x55' "$security" \
	'OpenWrt default PLOAM key'
require_fixed '0x24, 0x37, 0xbe, 0x54, 0xe9, 0x5e, 0x6e, 0xe3' "$security" \
	'zero Registration-ID MSK vector'
require_fixed '0x79, 0x5f, 0xcf, 0x6c, 0xb2, 0x15, 0x22, 0x40' "$security" \
	'session-key vector'
require_fixed '0x18, 0x4b, 0x8a, 0xd4, 0xd1, 0xac, 0x4a, 0xf4' "$security" \
	'OMCI integrity-key vector'
require_fixed '0xe2, 0x56, 0xce, 0x76, 0x78, 0x5c, 0x78, 0x71' "$security" \
	'PLOAM integrity-key vector'
require_fixed '0x6f, 0x9c, 0x99, 0xb8, 0x36, 0x17, 0x68, 0x93' "$security" \
	'key-encryption-key vector'
require_fixed 'SHASH_DESC_ON_STACK(desc, tfm);' "$security" \
	'incremental kernel CMAC descriptor'
require_fixed 'crypto_shash_update(desc, &direction_code' "$security" \
	'G.9807.1 direction prefix'
require_fixed 'crypto_shash_update(desc, content, content_len)' "$security" \
	'incremental PLOAM/OMCI content hashing'
require_fixed 'int airoha_xgs_ploam_mic(' "$security" \
	'trusted PLOAM MIC API'
require_fixed 'int airoha_xgs_verify_ploam_mic(' "$security" \
	'trusted PLOAM MIC verification API'
require_fixed 'int airoha_xgs_authenticate_initial_profile(' "$security" \
	'trusted initial Profile authentication API'
require_fixed 'int airoha_xgs_authenticate_initial_profile_words(' "$security" \
	'trusted 13-word FIFO Profile authentication API'
require_fixed 'int airoha_xgs_authenticate_initial_profile_words_mode(' "$security" \
	'mode-aware XG-PON/XGS-PON FIFO Profile authentication API'
require_fixed 'int airoha_xgs_authenticate_profile_words_mode_key(' "$security" \
	'current-key operational Profile authentication API'
require_fixed 'profile->destination = destination;' "$security" \
	'authenticated Profile destination ownership'
require_fixed 'profile->sequence = frame[3];' "$security" \
	'authenticated Profile ACK sequence ownership'
require_fixed 'int airoha_xgs_authenticate_assign_onu_id_words(' "$security" \
	'trusted Assign ONU-ID authentication API'
require_fixed 'AIROHA_XGS_ASSIGN_ONU_ID_PADDING_OFFSET' "$security" \
	'authenticated Assign ONU-ID padding tolerance vector'
require_fixed 'AIROHA_XGS_ASSIGN_ONU_ID_RATE_RESERVED' "$security" \
	'Assign ONU-ID nominal-rate reserved-bit validation'
require_fixed 'int airoha_xgs_authenticate_request_registration_words(' \
	"$security" 'trusted Request Registration authentication API'
require_fixed '0x0123095a, 0x00000000' "$security" \
	'Request Registration fixed content vector'
require_fixed '0x4b405a9f, 0xce554c85' "$security" \
	'Request Registration fixed MIC vector'
require_fixed 'altered_request_registration_frame' "$security" \
	'authenticated Request Registration padding tolerance vector'
reject_fixed 'memchr_inv(frame + 4, 0, AIROHA_XGS_PLOAM_CONTENT_SIZE - 4)' \
	"$security" 'receiver-side zero-padding rejection'
require_fixed 'int airoha_xgs_authenticate_ranging_time_words(' "$security" \
	'trusted Ranging Time authentication API'
require_fixed '0x01230433, 0x01001234, 0x56000000' "$security" \
	'Ranging Time fixed content vector'
require_fixed '0x74958638, 0x1f964887' "$security" \
	'Ranging Time fixed MIC vector'
require_fixed 'AIROHA_XGS_RANGING_RESERVED' "$security" \
	'Ranging Time reserved-bit validation'
require_fixed 'AIROHA_XGS_RANGING_PADDING_OFFSET' "$security" \
	'Ranging Time padding tolerance vector'
require_fixed 'AIROHA_XGS_RANGING_PON_ID_OFFSET' "$security" \
	'Ranging Time unused PON-ID validation'
require_fixed 'int airoha_xgs_authenticate_deactivate_onu_id_words(' \
	"$security" 'trusted Deactivate ONU-ID authentication API'
require_fixed '0x01230566, 0x00000000' "$security" \
	'Deactivate ONU-ID fixed content vector'
require_fixed '0xbe0f58a3, 0x44cb3b15' "$security" \
	'Deactivate ONU-ID fixed MIC vector'
require_fixed 'altered_control_frame[4] = 1' "$security" \
	'authenticated Deactivate ONU-ID padding tolerance vector'
require_fixed 'en7581_xgspon_accept_deactivate_onu_id(priv,' "$driver" \
	'authenticated Deactivate ONU-ID FIFO dispatch'
require_fixed 'en7581_xgspon_accept_disable_serial_number(priv,' "$driver" \
	'authenticated Disable Serial Number FIFO dispatch'
require_fixed 'EN7581_XGSPON_ACTIVATION_O7' "$driver" \
	'Disable Serial Number O7 transition'
require_fixed 'priv->serial_disabled = true;' "$driver" \
	'Disable Serial Number activation lockout'
require_fixed 'en7581_xgspon_accept_sleep_allow(priv, words)' "$driver" \
	'authenticated Sleep Allow FIFO dispatch'
require_fixed 'recognized no-op downstream control message' "$driver" \
	'SDK-recognized fixed-wavelength no-op controls'
require_fixed 'ignored NG-PON2-only downstream control message' "$driver" \
	'fixed-wavelength NG-PON2 control no-op'
require_fixed 'recognized_noop_ploam_events=' "$driver" \
	'recognized no-op PLOAM evidence'
reject_fixed 'NGPON2_CONTROL_FIRST_MESSAGE_ID' "$driver" \
	'undefined 0x16 folded into an NG-PON2 message range'
require_fixed 'unsupported_ploam_events=' "$driver" \
	'explicit unsupported PLOAM evidence'
require_fixed 'priv->enabled = false;' "$driver" \
	'Deactivate ONU-ID prevents automatic reactivation'
require_fixed 'service_ret = en7581_xgspon_clear_service_locked(priv);' \
	"$driver" 'session cleanup revokes XGS service state'
require_fixed 'ret = airoha_xgs_qdma_service_clear(priv->ethernet_np);' \
	"$driver" 'session cleanup revokes QDMA/GDM2/PSE state'
require_fixed 'deactivate_events=' "$driver" \
	'Deactivate ONU-ID non-secret evidence'
require_fixed 'int airoha_xgs_authenticate_assign_alloc_id_words(' "$security" \
	'trusted Assign Alloc-ID authentication API'
require_fixed '0x01230a44, 0x04560100' "$security" \
	'Assign Alloc-ID fixed content vector'
require_fixed '0x79dd442d, 0x40ea1ae5' "$security" \
	'Assign Alloc-ID fixed MIC vector'
require_fixed 'AIROHA_XGS_ALLOC_ID_RESERVED' "$security" \
	'Assign Alloc-ID receiver-side high-bit masking'
require_fixed 'AIROHA_XGS_ALLOC_ID_PADDING_OFFSET' "$security" \
	'Assign Alloc-ID padding tolerance vector'
require_fixed 'AIROHA_XGS_ALLOC_ID_SCOPE_OFFSET' "$security" \
	'Assign Alloc-ID unused scope validation'
require_fixed 'operation != AIROHA_XGS_ALLOC_ID_ASSIGN' "$security" \
	'Assign Alloc-ID operation validation'
require_fixed 'en7581_xgspon_accept_ranging_time(priv, words)' "$driver" \
	'authenticated Ranging Time FIFO dispatch'
require_fixed 'airoha_xgs_authenticate_ranging_time_words(' "$driver" \
	'trusted Ranging Time receive boundary'
require_fixed 'en7581_xgspon_program_equalization_delay(' "$driver" \
	'authenticated EqD hardware transaction'
require_fixed 'if (!ranging.absolute || !ranging.directed)' "$driver" \
	'initial EqD absolute-mode requirement'
require_fixed 'activation_state == EN7581_XGSPON_ACTIVATION_O5' "$driver" \
	'operational relative EqD state gate'
require_fixed 'priv->equalization_delay = resolved;' "$driver" \
	'converted absolute EqD commit after hardware success'
require_fixed 'en7581_xgspon_accept_assign_alloc_id(priv,' "$driver" \
	'authenticated Assign Alloc-ID FIFO dispatch'
require_fixed 'airoha_xgs_authenticate_assign_alloc_id_words(' "$driver" \
	'trusted Assign Alloc-ID receive boundary'
require_fixed 'en7581_xgspon_commit_alloc_id(priv, &assignment, &tcont_index)' \
	"$driver" 'authenticated T-CONT hardware transaction'
require_fixed '!priv->ranging_time_authenticated' "$driver" \
	'Alloc-ID requires authenticated ranging evidence'
require_fixed '!priv->equalization_delay_programmed' "$driver" \
	'Alloc-ID requires committed EqD'
require_fixed 'airoha_xgs_build_acknowledge_ploam(' "$security" \
	'authenticated Acknowledge builder'
require_fixed 'expected_acknowledge_ploam' "$security" \
	'Acknowledge fixed MIC vector'
require_fixed '#define EN7581_XGSPON_ACK_QUEUE_DEPTH' "$driver" \
	'bounded Acknowledge queue'
require_fixed 'en7581_xgspon_ack_tx_ready_locked(priv)' "$driver" \
	'post-Registration O5 Acknowledge gate'
require_fixed 'AIROHA_XGS_ACK_COMPLETION_PROCESS_ERROR' "$driver" \
	'Assign Alloc-ID processing-error Acknowledge'
require_fixed 'if (!en7581_xgspon_ack_tx_ready_locked(priv))' "$driver" \
	'Assign Alloc-ID registered-O5 transaction gate'
require_fixed 'ack->generation != priv->session_generation' "$driver" \
	'session-scoped Acknowledge rejection'
require_fixed 'priv->ack_in_flight = false;' "$driver" \
	'Acknowledge completion tracking'
require_fixed 'acknowledge_send_events=' "$driver" \
	'Acknowledge completion evidence'
reject_fixed 'ACK remains disabled' "$driver" \
	'obsolete disabled-Acknowledge claim'
require_fixed '#define EN7581_XGSPON_EQD' "$driver" \
	'OpenWrt EqD register definition'
require_fixed '#define EN7581_XGSPON_TCONT_ID_CFG' "$driver" \
	'OpenWrt T-CONT command register definition'
require_fixed '#define EN7581_XGSPON_TCONT_ID_STS' "$driver" \
	'OpenWrt T-CONT status register definition'
require_fixed '#define EN7581_XGSPON_MAX_TCONTS' "$driver" \
	'OpenWrt 32-entry T-CONT table definition'
require_fixed 'if (resolved > (~0U >> 2))' "$eqd_transaction" \
	'EqD scaling overflow rejection'
require_fixed '*encoded = resolved << 2;' "$eqd_transaction" \
	'XGS-PON EqD scaling implementation'
require_fixed '*encoded = resolved;' "$eqd_transaction" \
	'XG-PON unscaled EqD implementation'
require_fixed 'rollback_ret = ops->write(context, old_value);' "$eqd_transaction" \
	'EqD failure rollback'
require_fixed 'en7581_xgspon_read_tcont(priv, index, &read_valid,' "$driver" \
	'T-CONT immediate readback verification'
require_fixed 'for (i = 1; i < EN7581_XGSPON_MAX_TCONTS; i++)' "$driver" \
	'OpenWrt business T-CONT index 1 boundary'
require_fixed 'T-CONT 0 is the ONU-ID shadow and must never be written directly.' \
	"$driver" 'OpenWrt T-CONT 0 shadow rule'
require_fixed 'EN7581_XGSPON_UNASSIGNED_ALLOC_ID, false' "$driver" \
	'T-CONT invalidation value'
require_fixed 'if (rollback_ret)' "$driver" \
	'T-CONT failure rollback verification'
require_fixed 'priv->alloc_ids[0] = onu_id;' "$driver" \
	'ONU-ID software shadow state'
require_fixed 'int airoha_xgs_build_serial_number_ploam(' "$security" \
	'trusted Serial Number PLOAM builder'
require_fixed 'int airoha_xgs_build_registration_ploam(' "$security" \
	'trusted Registration PLOAM builder'
require_fixed 'int airoha_xgs_encode_upstream_ploam_fifo(' "$security" \
	'EN7581 upstream PLOAM FIFO encoder'
require_fixed 'AIROHA_XGS_PLOAM_UPSTREAM_FIFO_WORDS' "$security" \
	'11-word upstream PLOAM FIFO boundary'
require_fixed 'words[0] = key_index;' "$security" \
	'upstream PLOAM hardware key-index metadata word'
require_fixed 'words[i + 1] = get_unaligned_be32(content' "$security" \
	'upstream PLOAM FIFO content byte order'
require_fixed 'AIROHA_XGS_PLOAM_KEY_INDEX_1, upstream_fifo' "$security" \
	'Serial Number default-key FIFO encoding'
require_fixed 'AIROHA_XGS_PLOAM_KEY_INDEX_0, upstream_fifo' "$security" \
	'Registration real-key FIFO encoding'
require_fixed 'airoha_xgs_parse_key_control_words' "$security" \
	'trusted Key Control parser'
require_fixed 'altered_control_frame[AIROHA_XGS_KEY_CONTROL_RESERVED_OFFSET] = 1' \
	"$security" 'authenticated Key Control reserved-byte tolerance vector'
require_fixed 'altered_control_frame[AIROHA_XGS_KEY_CONTROL_PADDING_OFFSET] = 1' \
	"$security" 'authenticated Key Control padding tolerance vector'
require_fixed 'AIROHA_XGS_KEY_CONTROL_FIELD_RESERVED' "$security" \
	'Key Control semantic reserved-bit rejection'
require_fixed 'airoha_xgs_authenticate_disable_serial_number_words' "$security" \
	'trusted Disable Serial Number parser'
require_fixed 'altered_control_frame[AIROHA_XGS_DISABLE_SERIAL_PADDING_OFFSET] = 1' \
	"$security" 'authenticated Disable Serial Number padding tolerance vector'
require_fixed 'AIROHA_XGS_DISABLE_SERIAL_DENIED_SPECIFIC' "$security" \
	'Disable Serial Number specific-deny mode'
require_fixed 'AIROHA_XGS_DISABLE_SERIAL_ALLOWED_ALL' "$security" \
	'Disable Serial Number global-allow mode'
require_fixed 'airoha_xgs_authenticate_sleep_allow_words' "$security" \
	'trusted Sleep Allow parser'
require_fixed 'altered_control_frame[AIROHA_XGS_SLEEP_ALLOW_PADDING_OFFSET] = 1' \
	"$security" 'authenticated Sleep Allow padding tolerance vector'
require_fixed 'AIROHA_XGS_SLEEP_ALLOW_RESERVED' "$security" \
	'Sleep Allow reserved-bit validation'
package_release="$(sed -n 's/^PKG_RELEASE:=//p' "$package_makefile")"
case "$package_release" in
	''|*[!0-9]*)
		echo "invalid XPON package release: $package_release" >&2
		exit 1
		;;
esac
[ "$package_release" -ge 15 ] || {
	echo "PLOAM receiver interoperability requires package release >= 15" >&2
	exit 1
}
require_fixed 'airoha_xgs_build_key_report' "$security" \
	'trusted Key Report builder'
require_fixed 'crypto_alloc_sync_skcipher("ecb(aes)", 0, 0)' "$security" \
	'synchronous KEK AES-ECB implementation'
require_fixed 'source_buffer = kmemdup' "$security" \
	'AES-ECB source must use page-backed storage for scatterwalk'
require_fixed 'destination_buffer = kzalloc' "$security" \
	'AES-ECB destination must use page-backed storage for scatterwalk'
require_fixed 'kfree_sensitive(destination_buffer)' "$security" \
	'AES-ECB temporary destination must be scrubbed'
reject_fixed 'sg_init_one(&source, input' "$security" \
	'AES-ECB scatterlist must not reference a caller stack buffer'
reject_fixed 'sg_init_one(&destination, output' "$security" \
	'AES-ECB scatterlist must not reference a caller output buffer'
require_fixed '"3141592653589793"' "$security" \
	'existing-key proof label'
require_fixed '0x00000001, 0x03ff0100, 0x564e4452, 0x00112233' \
	"$security" 'Serial Number 11-word FIFO vector'
require_fixed '0x00000000, 0x01230222, 0x00010203, 0x04050607' \
	"$security" 'Registration 11-word FIFO vector'
require_fixed 'put_unaligned_be16(AIROHA_XGS_BROADCAST_ONU_ID, frame)' \
	"$security" 'Serial Number unassigned destination encoding'
require_fixed 'AIROHA_XGPON_UPSTREAM_LINE_RATE_CAP' "$security" \
	'XG-PON upstream line-rate capability encoding'
require_fixed 'AIROHA_XGSPON_UPSTREAM_LINE_RATE_CAP' "$security" \
	'XGS-PON upstream line-rate capability encoding'
require_fixed 'priv->serial, random_delay, priv->active_mode, frame' \
	"$driver" 'active-mode Serial Number PLOAM selection'
require_fixed 'expected_xgpon_serial_number_fifo' "$security" \
	'fixed XG-PON Serial Number FIFO vector'
require_fixed 'expected_xgspon_serial_number_fifo' "$security" \
	'fixed XGS-PON Serial Number FIFO vector'
require_fixed 'invalid_serial_number_modes' "$security" \
	'non-XG Serial Number mode rejection vectors'
require_fixed '0xdb, 0x79, 0xf1, 0xb4' "$security" \
	'fixed XG-PON Serial Number upstream MIC vector'
require_fixed 'memcpy(frame + 4, registration_id' "$security" \
	'Registration-ID content encoding'
require_fixed '0x90, 0x58, 0x6c, 0xd0' "$security" \
	'fixed Serial Number upstream MIC vector'
require_fixed '0xea, 0x91, 0x1f, 0xf7' "$security" \
	'fixed Registration upstream MIC vector'
require_fixed '0x2dedc831, 0x8aff6976' "$security" \
	'authenticated Assign ONU-ID fixed CMAC vector'
require_fixed 'memcmp(frame + AIROHA_XGS_ASSIGN_ONU_ID_SERIAL_OFFSET, serial' \
	"$security" 'trusted Assign ONU-ID serial check'
require_fixed 'AIROHA_XGS_PLOAM_FRAME_WORDS' "$security" \
	'48-byte standard-frame boundary'
require_fixed 'put_unaligned_be32(words[i]' "$security" \
	'FIFO network-byte-order conversion'
require_fixed '0xa5a5a5a5' "$security" \
	'ignored hardware-status word self-test'
require_fixed 'AIROHA_XGS_PROFILE_PON_TAG_OFFSET' "$security" \
	'Profile PON-tag offset'
require_fixed 'airoha_xgs_default_ploam_key' "$security" \
	'default-key Profile verification'
require_fixed '0xc9,' "$security" 'initial Profile MIC vector'
require_fixed 'ret != -EADDRNOTAVAIL' "$security" \
	'non-broadcast initial Profile rejection'
require_fixed 'ret != -EPROTO' "$security" \
	'non-XGS initial Profile rejection'
require_fixed 'int airoha_xgs_omci_mic(' "$security" \
	'trusted OMCI MIC API'
require_fixed 'int airoha_xgs_verify_omci_mic(' "$security" \
	'trusted OMCI MIC verification API'
require_fixed 'int airoha_xgs_authenticate_downstream_omci(' "$security" \
	'trusted downstream OMCI authentication adapter'
require_fixed 'int airoha_xgs_sign_upstream_omci(' "$security" \
	'trusted upstream OMCI signing adapter'
require_fixed 'AIROHA_XGS_OMCI_BASELINE_CONTENT_SIZE' "$security" \
	'baseline OMCI authenticated-content boundary'
require_fixed 'AIROHA_XGS_OMCI_EXTENDED_LENGTH_OFFSET' "$security" \
	'extended OMCI content-length boundary'
require_fixed 'get_unaligned_be32(content +' "$security" \
	'baseline OMCI end-of-message length validation'
require_fixed 'payload_len = get_unaligned_be16(' "$security" \
	'extended OMCI payload length validation'
require_fixed 'key, AIROHA_XGS_DOWNSTREAM, wire, authenticated_len' "$security" \
	'downstream OMCI direction and complete-content verification'
require_fixed 'key, AIROHA_XGS_UPSTREAM,' "$security" \
	'upstream OMCI direction selection'
require_fixed 'memzero_explicit(content, content_capacity)' "$security" \
	'downstream OMCI rejected-output wipe'
require_fixed 'memzero_explicit(wire, wire_capacity)' "$security" \
	'upstream OMCI rejected-output wipe'
require_fixed '0x46, 0x39, 0x87, 0x56, 0x28, 0x08, 0x14, 0xe6' "$security" \
	'downstream PLOAM MIC vector'
require_fixed '0xfe, 0xaf, 0x8d, 0x09, 0x20, 0x8f, 0x0d, 0x9b' "$security" \
	'upstream PLOAM MIC vector'
require_fixed '0x78, 0xdc, 0xa5, 0x3d' "$security" \
	'downstream OMCI MIC vector'
require_fixed '0x68, 0x2f, 0x5c, 0x73' "$security" \
	'upstream baseline OMCI MIC vector'
require_fixed '0x6e, 0xa5, 0x9c, 0xee' "$security" \
	'downstream extended OMCI MIC vector'
require_fixed '0x7b, 0x7b, 0x7c, 0x8f' "$security" \
	'upstream extended OMCI MIC vector'
require_fixed 'crypto_memneq' "$security" 'constant-time self-test comparison'
require_fixed 'ret = -EBADMSG;' "$security" 'tampered MIC rejection'
require_fixed 'shash_desc_zero(desc);' "$security" \
	'incremental CMAC descriptor wipe'
require_fixed 'memzero_explicit(full_tag' "$security" \
	'full CMAC tag wipe'
require_fixed 'memzero_explicit(expected' "$security" \
	'verification tag wipe'
require_fixed 'memzero_explicit(registration_msk' "$security" \
	'self-test Registration MSK wipe'
require_fixed 'memzero_explicit(&keys' "$security" 'self-test shared-key wipe'

# The SDK signs or verifies exactly the G.988 baseline content (44 bytes) or
# extended header plus declared payload, and passes the selected direction to
# CMAC0. Keep the clean-room adapter consistent without importing SDK code.
require_fixed 'omci_msg_len = (OMCI_BASIC_MSG_FIX_LEN - OMCI_CRC_LEN);' \
	"$tmp_dir/gpon_wan.c" 'SDK baseline OMCI authenticated length'
require_fixed 'omci_msg_len = ntohs(pOmciHeader->msgContLen) + sizeof(Omci_Header_T);' \
	"$tmp_dir/gpon_wan.c" 'SDK extended OMCI authenticated length'
require_fixed 'GPON_CMAC_UPSTREAM' "$tmp_dir/gpon_wan.c" \
	'SDK upstream OMCI CMAC direction'
require_fixed 'GPON_CMAC_DOWNSTREAM' "$tmp_dir/gpon_wan.c" \
	'SDK downstream OMCI CMAC direction'

# Registration-ID and its derived MSK commit together. No session key may be
# derived until authenticated XGS PLOAM supplies the PON tag.
require_fixed '#include "airoha-xgs-security.h"' "$driver" \
	'XGS security interface'
require_fixed 'bool registration_msk_set;' "$driver" \
	'Registration MSK ownership state'
require_fixed 'airoha_xgs_derive_registration_msk(registration_id' "$driver" \
	'Registration-ID MSK derivation'
require_fixed 'memcpy(priv->registration_msk, registration_msk' "$driver" \
	'atomic Registration MSK commit'
require_fixed 'priv->registration_msk_set = true;' "$driver" \
	'Registration MSK ready transition'
require_fixed '"registration-msk-unset"' "$driver" \
	'unset derivation state'
require_fixed '"registration-msk-ready-pon-tag-pending"' "$driver" \
	'PON-tag pending state'
require_fixed 'memzero_explicit(priv->registration_msk' "$driver" \
	'persistent Registration MSK wipe'
require_fixed 'memzero_explicit(registration_msk' "$driver" \
	'temporary Registration MSK wipe'
require_fixed 'airoha_xgs_authenticate_profile_words_mode_key(' "$driver" \
	'mode-aware current-key FIFO Profile authentication boundary'
require_fixed 'words, authentication_key, xgspon, &profile' "$driver" \
	'active-mode Profile line-rate selection'
sed -n '/^static int en7581_xgspon_accept_profile(/,/^}/p' "$driver" \
	> "$tmp_dir/openwrt-profile.c"
require_fixed 'EN7581_XGSPON_ACTIVATION_O2_3' \
	"$tmp_dir/openwrt-profile.c" 'OpenWrt Profile O2/O3 state gate'
require_fixed 'EN7581_XGSPON_ACTIVATION_O4' \
	"$tmp_dir/openwrt-profile.c" 'OpenWrt Profile O4 update state gate'
require_fixed 'EN7581_XGSPON_ACTIVATION_O5' \
	"$tmp_dir/openwrt-profile.c" 'OpenWrt Profile O5 update state gate'
require_fixed 'crypto_memneq(profile.pon_tag, priv->pon_tag' \
	"$tmp_dir/openwrt-profile.c" 'runtime Profile PON-tag continuity gate'
require_fixed 'en7581_xgspon_program_profile_locked(priv, &profile)' \
	"$tmp_dir/openwrt-profile.c" 'transactional runtime Profile programming'
require_fixed 'en7581_xgspon_ack_schedule_status_locked(' \
	"$tmp_dir/openwrt-profile.c" 'directed Profile ACK reservation preflight'
require_fixed 'en7581_xgspon_commit_ack_locked(' \
	"$tmp_dir/openwrt-profile.c" 'directed Profile ACK queue commit'
reject_fixed 'ret = -EALREADY;' "$tmp_dir/openwrt-profile.c" \
	'obsolete initial-only Profile rejection'
require_fixed 'EN7581_XGSPON_PLOAMD_FIFO_STS' "$driver" \
	'SDK downstream PLOAM FIFO status register'
require_fixed 'EN7581_XGSPON_PLOAMD_RDATA' "$driver" \
	'SDK downstream PLOAM FIFO data register'
require_fixed '#define EN7581_XGSPON_PLOAMU_FIFO_STS' "$driver" \
	'SDK upstream PLOAM FIFO status register'
require_fixed '#define EN7581_XGSPON_PLOAMU_WDATA' "$driver" \
	'SDK upstream PLOAM FIFO data register'
require_fixed 'FIELD_GET(EN7581_XGSPON_PLOAMU_AVAILABLE, fifo_status)' \
	"$driver" 'complete-record upstream FIFO capacity check'
require_fixed 'fifo_status & EN7581_XGSPON_PLOAMU_OVERRUN' "$driver" \
	'upstream FIFO overrun rejection'
require_fixed 'writel(words[i], priv->mac + EN7581_XGSPON_PLOAMU_WDATA)' \
	"$driver" 'SDK-order upstream FIFO word transaction'
require_fixed 'if (!priv->tx_armed)' "$driver" \
	'explicit temporary upstream TX authorization gate'
require_fixed 'ret = -EACCES;' "$driver" \
	'fail-closed unauthorized FIFO result'
require_fixed 'en7581_xgspon_enqueue_upstream_ploam_locked' "$driver" \
	'locked upstream PLOAM FIFO transaction'
require_fixed 'en7581_xgspon_enable_upstream_irqs_locked' "$driver" \
	'gated upstream completion IRQ enable'
require_fixed 'writel(status, priv->mac + EN7581_XGSPON_INT_STATUS)' "$driver" \
	'stale upstream interrupt clear'
require_fixed 'writel(status, priv->mac + EN7581_XGSPON_FIFO_ERR_STS)' "$driver" \
	'stale FIFO error clear'
require_fixed 'writel(status, priv->mac + EN7581_XGSPON_TX_ERR_STS)' "$driver" \
	'stale TX error clear'
require_fixed 'EN7581_XGSPON_INT_ACTIVE);' "$driver" \
	'verified active upstream interrupt mask'
require_fixed 'AIROHA_XGS_PLOAM_FIFO_WORDS' "$driver" \
	'13-word FIFO drain boundary'
require_fixed 'en7581_xgspon_accept_profile(priv, words)' "$driver" \
	'authenticated Profile FIFO dispatch'
require_fixed 'en7581_xgspon_accept_assign_onu_id(priv, words)' "$driver" \
	'authenticated Assign ONU-ID FIFO dispatch'
require_fixed 'en7581_xgspon_accept_request_registration(priv,' "$driver" \
	'authenticated Request Registration FIFO dispatch'
require_fixed 'authenticated O5 Request Registration; upstream response scheduled' \
	"$driver" 'authenticated Request Registration state'
require_fixed 'priv->request_registration_sequence = sequence;' "$driver" \
	'authenticated Request Registration sequence ownership'
sed -n '/^static int en7581_xgspon_accept_assign_onu_id(/,/^}/p' "$driver" \
	> "$tmp_dir/openwrt-assign-onu-id.c"
sed -n '/^static int en7581_xgspon_accept_request_registration(/,/^}/p' "$driver" \
	> "$tmp_dir/openwrt-request-registration.c"
sed -n '/^static int en7581_xgspon_accept_ranging_time(/,/^}/p' "$driver" \
	> "$tmp_dir/openwrt-ranging-time.c"
sed -n '/^static int en7581_xgspon_queue_registration_locked(/,/^}/p' \
	"$driver" > "$tmp_dir/openwrt-queue-registration.c"
sed -n '/^static void en7581_xgspon_to1_work(/,/^}/p' "$driver" \
	> "$tmp_dir/openwrt-to1-work.c"
sed -n '/^static int en7581_xgspon_clear_session_keys(/,/^}/p' "$driver" \
	> "$tmp_dir/openwrt-clear-session.c"
require_fixed 'activation_state != EN7581_XGSPON_ACTIVATION_O2_3' \
	"$tmp_dir/openwrt-assign-onu-id.c" 'OpenWrt Assign ONU-ID O2/O3 state gate'
require_fixed 'priv, EN7581_XGSPON_ACTIVATION_O4' \
	"$tmp_dir/openwrt-assign-onu-id.c" 'OpenWrt Assign ONU-ID O4 transition'
require_fixed 'priv->registration_response_pending = true;' \
	"$tmp_dir/openwrt-assign-onu-id.c" \
	'OpenWrt Assign ONU-ID initial Registration trigger'
require_fixed 'priv->request_registration_sequence = 0;' \
	"$tmp_dir/openwrt-assign-onu-id.c" \
	'OpenWrt initial Registration sequence zero'
require_fixed 'en7581_xgspon_start_to1_locked(priv);' \
	"$tmp_dir/openwrt-assign-onu-id.c" 'OpenWrt O4 TO1 arming point'
require_fixed 'activation_state != EN7581_XGSPON_ACTIVATION_O5' \
	"$tmp_dir/openwrt-request-registration.c" \
	'OpenWrt Request Registration O5-only state gate'
require_fixed 'priv->tx_state != EN7581_XGSPON_TX_REGISTERED' \
	"$tmp_dir/openwrt-request-registration.c" \
	'OpenWrt repeated O5 Request Registration acceptance'
reject_fixed 'priv, EN7581_XGSPON_ACTIVATION_O4' \
	"$tmp_dir/openwrt-request-registration.c" \
	'obsolete Request Registration O4 transition'
require_fixed 'priv, EN7581_XGSPON_ACTIVATION_O5' \
	"$tmp_dir/openwrt-ranging-time.c" 'OpenWrt Ranging Time O4-to-O5 transition'
require_fixed 'goto rollback_ranging;' "$tmp_dir/openwrt-ranging-time.c" \
	'OpenWrt atomic EqD/state/ACK rollback'
require_fixed 'en7581_xgspon_cancel_to1_locked(priv);' \
	"$tmp_dir/openwrt-ranging-time.c" 'OpenWrt committed O5 TO1 cancellation'
require_fixed 'en7581_xgspon_ranging_ack_tx_ready_locked' "$driver" \
	'pre-Registration Ranging Time ACK authorization gate'
require_fixed 'ack->key_index == AIROHA_XGS_PLOAM_KEY_INDEX_1' "$driver" \
	'pre-Registration Ranging Time ACK default key slot'
require_fixed 'bool registration_response_pending;' "$driver" \
	'bounded Request Registration response ownership'
require_fixed 'priv->registration_response_pending = false;' "$driver" \
	'Registration response dequeue commit'
require_fixed 'initial = priv->tx_state == EN7581_XGSPON_TX_WAIT_REGISTRATION' \
	"$tmp_dir/openwrt-queue-registration.c" \
	'OpenWrt distinct initial Registration path'
require_fixed 'activation_state != EN7581_XGSPON_ACTIVATION_O4' \
	"$tmp_dir/openwrt-queue-registration.c" \
	'OpenWrt initial Registration O4 gate'
require_fixed 'sequence = 0;' "$tmp_dir/openwrt-queue-registration.c" \
	'OpenWrt initial Registration sequence'
require_fixed 'priv, frame, AIROHA_XGS_PLOAM_KEY_INDEX_0, 2' \
	"$tmp_dir/openwrt-queue-registration.c" \
	'OpenWrt duplicate initial Registration records'
require_fixed 'activation_state != EN7581_XGSPON_ACTIVATION_O5' \
	"$tmp_dir/openwrt-queue-registration.c" \
	'OpenWrt requested Registration O5 gate'
require_fixed '#define EN7581_XGSPON_TO1_MS' "$driver" \
	'OpenWrt XG/XGS TO1 interval'
require_fixed 'priv->to1_generation = priv->session_generation;' "$driver" \
	'OpenWrt TO1 session-generation snapshot'
require_fixed 'priv->to1_expected_state = EN7581_XGSPON_ACTIVATION_O4;' \
	"$driver" 'OpenWrt TO1 expected-state snapshot'
require_fixed '#include "airoha-xgs-to1.h"' "$driver" \
	'OpenWrt TO1 transaction integration'
require_fixed 'decision = airoha_xgs_to1_decide(' \
	"$tmp_dir/openwrt-to1-work.c" 'OpenWrt TO1 state decision call'
require_fixed 'state->armed_generation != state->current_generation' \
	"$to1_transaction" 'OpenWrt stale-session TO1 rejection'
require_fixed 'state->current_state != state->expected_state' \
	"$to1_transaction" 'OpenWrt stale-state TO1 rejection'
require_fixed 'airoha_xgs_to1_time_before(now, state->deadline)' \
	"$to1_transaction" 'OpenWrt wrap-safe absolute TO1 deadline'
require_fixed 'priv->to1_timeout_events++;' \
	"$tmp_dir/openwrt-to1-work.c" 'OpenWrt TO1 timeout accounting'
require_fixed 'cleanup_ret = airoha_xgs_to1_cleanup_transaction(' \
	"$tmp_dir/openwrt-to1-work.c" 'OpenWrt TO1 cleanup transaction call'
require_fixed '.disable_tx = en7581_xgspon_to1_disable_tx,' "$driver" \
	'OpenWrt TO1 optical-disable transaction stage'
require_fixed '.clear_session = en7581_xgspon_to1_clear_session,' "$driver" \
	'OpenWrt TO1 complete-session cleanup stage'
require_fixed 'result->session_error = ops->clear_session(context);' \
	"$to1_transaction" 'OpenWrt cleanup-after-disable-failure invariant'
require_fixed 'restart_discovery && !READ_ONCE(priv->removing)' \
	"$tmp_dir/openwrt-to1-work.c" 'OpenWrt verified TO1 rediscovery'
require_fixed 'en7581_xgspon_cancel_to1_locked(priv);' \
	"$tmp_dir/openwrt-clear-session.c" 'OpenWrt session-boundary TO1 cancellation'
require_fixed 'static DEVICE_ATTR_RO(to1_evidence);' "$driver" \
	'OpenWrt read-only TO1 evidence ABI'
[ "$(grep -Fc 'cancel_delayed_work_sync(&priv->to1_work);' "$driver")" -ge 3 ] || {
	echo 'missing synchronous TO1 lifecycle cancellation' >&2
	exit 1
}
require_fixed '.shutdown = en7581_xgspon_shutdown,' "$driver" \
	'OpenWrt XG/XGS shutdown callback'
sed -n '/^static void en7581_xgspon_shutdown(/,/^}/p' "$driver" \
	> "$tmp_dir/openwrt-shutdown.c"
require_fixed 'cancel_delayed_work_sync(&priv->to1_work);' \
	"$tmp_dir/openwrt-shutdown.c" 'OpenWrt shutdown TO1 cancellation'
require_fixed 'airoha_en7572_emergency_disable(priv->bosa);' \
	"$tmp_dir/openwrt-shutdown.c" 'OpenWrt shutdown optical lockout'
require_fixed '#define EN7581_XGSPON_FIFO_ERR_STS' "$driver" \
	'SDK FIFO error status register offset'
require_fixed '#define EN7581_XGSPON_O23_O4_PLOAMU_CTRL' "$driver" \
	'SDK software-reply control register offset'
require_fixed '#define EN7581_XGSPON_ACTIVATION_ST' "$driver" \
	'SDK activation-state register offset'
require_fixed '#define EN7581_XGSPON_DBG_RESYNC' "$driver" \
	'SDK TX-sync status register offset'
require_fixed '#define EN7581_XGSPON_INT_PLOAMU' "$driver" \
	'upstream PLOAM completion event bit'
require_fixed '#define EN7581_XGSPON_INT_O23_SN_SENT' "$driver" \
	'Serial Number completion event bit'
require_fixed '#define EN7581_XGSPON_INT_O23_SN_REQUEST' "$driver" \
	'Serial Number request event bit'
require_fixed '#define EN7581_XGSPON_INT_O4_RANGING_REQUEST' "$driver" \
	'Ranging Request event bit'
require_fixed '#define EN7581_XGSPON_INT_O4_REGISTRATION_SENT' "$driver" \
	'Registration completion event bit'
require_fixed '#define EN7581_XGSPON_INT_US_NO_MESSAGE' "$driver" \
	'upstream no-message event bit'
require_fixed '#define EN7581_XGSPON_INT_FIFO_ERROR' "$driver" \
	'FIFO error event bit'
require_fixed '#define EN7581_XGSPON_INT_TX_ERROR' "$driver" \
	'TX error event bit'
require_fixed '#define EN7581_XGSPON_TX_ERR_STS' "$driver" \
	'SDK TX error status register offset'
require_fixed 'EN7581_XGSPON_TX_ERR_ALL' "$driver" \
	'complete SDK TX error detail mask'
require_fixed 'en7581_xgspon_tx_transition(' "$driver" \
	'fail-closed upstream state transition'
require_fixed 'EN7581_XGSPON_TX_REGISTRATION_QUEUED' "$driver" \
	'explicit Registration completion state'
require_fixed 'registration_key_switch_verified = true;' "$driver" \
	'post-Registration IK0 proof ownership'
require_fixed 'en7581_xgspon_authorize_o5_locked(priv)' "$driver" \
	'separate O5 authorization gate'
require_fixed 'EN7581_XGSPON_CUR_PIK_IDX |' "$driver" \
	'post-Registration PLOAM key-index readback'
require_fixed 'EN7581_XGSPON_CUR_OIK_IDX' "$driver" \
	'post-Registration OMCI key-index readback'
require_fixed 'airoha_en7572_emergency_disable(priv->bosa)' "$driver" \
	'upstream fault optical lockout'
require_fixed 'en7581_xgspon_tx_state_selftest()' "$driver" \
	'upstream state transition self-test gate'
require_fixed '#define EN7581_XGSPON_RDM_DLY' "$driver" \
	'SDK random-delay register offset'
require_fixed 'en7581_xgspon_serial_random_delay(random_delay_register)' \
	"$driver" 'Serial Number random-delay evidence'
require_fixed 'en7581_xgspon_record_upstream_events(priv, status)' "$driver" \
	'upstream completion and error state recorder'
require_fixed 'static DEVICE_ATTR_RO(activation_evidence);' "$driver" \
	'non-secret activation evidence ABI'
require_fixed '#define EN7581_XGSPON_INT_ENABLED	(EN7581_XGSPON_INT_PLOAMD |' \
	"$driver" 'fault-path downstream interrupt mask'
require_fixed 'EN7581_XGSPON_INT_DOWNSTREAM_ERRORS)' "$driver" \
	'SDK receive-error summaries in downstream interrupt mask'
require_fixed 'EN7581_XGSPON_ACTIVATION_O5' "$driver" \
	'post-Registration O5 activation transition'
require_fixed 'priv->tx_authorized = true;' "$driver" \
	'post-O5 upstream TX authorization'
require_fixed 'airoha_en7572_set_tx_enabled(priv->bosa, true)' "$driver" \
	'gated optical TX enable'
require_fixed 'priv->optical_tx_enabled ? "enabled" : "disabled"' "$driver" \
	'truthful optical TX activation evidence'
require_fixed 'en7581_xgspon_set_mac_irq_enable(' "$driver" \
	'verified fault-path downstream-only interrupt restore'
require_fixed 'en7581_xgspon_program_onu_id(priv, onu_id, true)' "$driver" \
	'authenticated ONU-ID hardware programming'
require_fixed '#define EN7581_XGSPON_GEM_PORT_CFG' "$driver" \
	'SDK-derived XGEM table command offset'
require_fixed '#define EN7581_XGSPON_GEM_PORT_STS' "$driver" \
	'SDK-derived XGEM table status offset'
require_fixed 'en7581_xgspon_program_omcc_xgem(priv, onu_id, true)' "$driver" \
	'authenticated default OMCC XGEM programming'
require_fixed 'return en7581_xgspon_program_xgem(priv, onu_id, valid, true, false);' \
	"$driver" 'unicast unencrypted default OMCC XGEM policy'
require_fixed 'ret = en7581_xgspon_read_xgem(priv, xgem_id, &status);' "$driver" \
	'XGEM table readback verification'
require_fixed 'en7581_xgspon_rollback_onu_assignment(priv, onu_id)' "$driver" \
	'atomic ONU-ID and OMCC XGEM rollback'
require_fixed 'omcc_xgem_programmed=%u omcc_xgem_id=%u' "$driver" \
	'non-secret default OMCC XGEM evidence'
require_fixed 'upstream response scheduled' "$driver" \
	'truthful authenticated Registration state'
require_fixed 'devm_request_threaded_irq(dev, priv->mac_irq' "$driver" \
	'XGS MAC threaded interrupt ownership'
require_fixed '#define REG_BASE_XGPON_PHY' "$tmp_dir/en7581_phy_reg.h" \
	'SDK XGS-PON PHY CSR base'
require_fixed '#define EN7581_XGPON_PHY_XG_PON_INT_STA' \
	"$tmp_dir/en7581_phy_reg.h" 'SDK XGS-PON PHY interrupt status register'
require_fixed '#define EN7581_XGPON_PHY_DBG_RX_SYNC_ST' \
	"$tmp_dir/en7581_phy_reg.h" 'SDK XGS-PON RX sync status register'
require_fixed '#define EN7581_XGPON_PHY_SFP_STA' "$tmp_dir/en7581_phy_reg.h" \
	'SDK XGS-PON LOS status register'
require_fixed '#define EN7581_XGPON_PHY_XG_PHY_STA' "$tmp_dir/en7581_phy_reg.h" \
	'SDK XGS-PON PHYA-ready status register'
require_fixed 'ret = request_irq(get_pon_phy_irq(),phy_isr_request' \
	"$tmp_dir/phy_init.c" 'SDK independent PHY IRQ ownership'
require_fixed 'phyIntStatus=IO_GPHYREG(EN7581_XGPON_PHY_XG_PON_INT_STA);' \
	"$tmp_dir/en7583_phy.c" 'SDK XGS-PON PHY status interrupt handling'
require_fixed '#define EN7581_XGSPON_PHY_INT_STATUS' "$driver" \
	'XGS PHY CSR interrupt status offset'
require_fixed 'priv->phy_csr = devm_ioremap(dev, resource->start,' "$driver" \
	'shared XGS PHY CSR mapping'
require_fixed 'platform_get_irq_byname(pdev, "phy")' "$driver" \
	'XGS PHY IRQ resource validation'
require_fixed 'devm_request_threaded_irq(dev, priv->phy_irq' "$driver" \
	'XGS PHY threaded interrupt ownership'
require_fixed 'writel(status, priv->phy_csr + EN7581_XGSPON_PHY_INT_STATUS);' \
	"$driver" 'SDK-order same-value W1C interrupt clear'
require_fixed 'static DEVICE_ATTR_RO(phy_evidence);' "$driver" \
	'read-only XGS PHY evidence ABI'
require_fixed 'irq_owned=%u' "$driver" \
	'truthful PHY IRQ ownership evidence'
require_fixed 'Observation only: do not clear status or alter the SDK interrupt mask.' \
	"$driver" 'non-mutating sysfs observation contract'
	require_fixed 'static bool en7581_xgspon_phy_trusted_locked' "$driver" \
	'locked trusted-PHY gate'
	require_fixed 'trusted_phy = en7581_xgspon_phy_trusted_locked(priv);' \
	"$driver" 'per-message PLOAM PHY gate'
	require_fixed 'IRQF_ONESHOT | IRQF_SHARED' "$driver" \
	'shared XGS-PON IRQ ownership'
require_fixed 'XG-PON/XGS-PON backends ready for runtime activation' "$driver" \
	'passive dual-mode runtime-switch backend'
require_fixed 'en7581_gpon_board_mode(dev, &hardware_selected)' "$gpon_driver" \
	'GPON initial PCS mode discovery'
require_fixed 'hardware_selected &&' "$gpon_driver" \
	'initial GPON BOSA mode ownership gate'
require_fixed 'airoha_en7572_get_mode(priv->bosa) != initial_mode' \
	"$driver" 'initial XG-PON/XGS-PON BOSA mode ownership gate'
require_fixed 'bool airoha_en7572_is_xgspon(' "$en7572_driver" \
	'EN7572 mode ownership query'
require_fixed 'airoha_xgs_derive_shared_keys(priv->registration_msk' "$driver" \
	'post-authentication session-key derivation'
require_fixed 'memcpy(&priv->session_keys, &keys' "$driver" \
	'atomic session-key commit'
require_fixed 'en7581_xgspon_clear_session_keys(priv);' "$driver" \
	'session-key invalidation'
require_fixed 'memzero_explicit(&priv->session_keys' "$driver" \
	'persistent session-key wipe'
require_fixed 'memzero_explicit(&keys' "$driver" \
	'temporary session-key wipe'
require_fixed '#define EN7581_XGSPON_PIK0' "$driver" \
	'SDK PLOAM IK0 register offset'
require_fixed '#define EN7581_XGSPON_OIK0' "$driver" \
	'SDK OMCI IK0 register offset'
require_fixed '#define EN7581_XGSPON_KEK0' "$driver" \
	'SDK KEK0 register offset'
require_fixed '#define EN7581_XGSPON_PON_TAG0' "$driver" \
	'SDK PON-tag register offset'
require_fixed '#define EN7581_XGSPON_SW_SET_KIDX' "$driver" \
	'SDK software key-index register offset'
require_fixed 'value + size - (word + 1) * sizeof(u32)' "$driver" \
	'SDK reverse-word identity/key programming order'
require_fixed 'en7581_xgspon_key_word(key, 0) == 0x0c0d0e0f' "$driver" \
	'key-register word-order self-test'
require_fixed 'en7581_xgspon_pon_tag_word(pon_tag, 0) == 0x14151617' \
	"$driver" 'PON-tag word-order self-test'
require_fixed '0x20212223' "$driver" \
	'Registration-ID register word-order self-test'
require_fixed 'XGS-PON key register layout self-test failed' "$driver" \
	'fail-closed register-layout self-test'
require_fixed 'en7581_xgspon_write_key(priv, EN7581_XGSPON_PIK0, keys->ploam)' \
	"$driver" 'authenticated PLOAM IK0 programming'
require_fixed 'en7581_xgspon_write_key(priv, EN7581_XGSPON_PIK1, default_ploam)' \
	"$driver" 'default PLOAM IK1 programming'
require_fixed 'en7581_xgspon_write_key(priv, EN7581_XGSPON_OIK0, keys->omci)' \
	"$driver" 'authenticated OMCI IK0 programming'
require_fixed 'en7581_xgspon_write_key(priv, EN7581_XGSPON_KEK0, keys->kek)' \
	"$driver" 'authenticated KEK0 programming'
require_fixed 'key_index = readl(priv->mac + EN7581_XGSPON_SW_SET_KIDX)' \
	"$driver" 'SDK-style key-index selector read-modify-write'
require_fixed 'key_index |= EN7581_XGSPON_SET_PIK_IDX | EN7581_XGSPON_SET_PIK_EN' \
	"$driver" 'initial hardware PLOAM key-index selection'
require_fixed 'key_index |= EN7581_XGSPON_SET_OIK_IDX | EN7581_XGSPON_SET_OIK_EN' \
	"$driver" 'initial hardware OMCI key-index selection'
require_fixed 'en7581_xgspon_program_serial(priv, serial)' "$driver" \
	'XGS-PON serial hardware programming'
require_fixed 'en7581_xgspon_program_registration_id(priv, registration_id)' \
	"$driver" 'XGS-PON Registration-ID hardware programming'
require_fixed 'ret = en7581_xgspon_clear_hardware_identity(priv);' "$driver" \
	'fail-closed stale identity handling'
require_fixed 'en7581_xgspon_wipe_hardware_keys(priv)' "$driver" \
	'hardware key wipe on failure or invalidation'
require_fixed 'static int en7581_xgspon_scrub_hardware_locked' "$driver" \
	'complete hardware-session scrub helper'
require_fixed 'error = en7581_xgspon_clear_session_keys(priv);' "$driver" \
	'startup scrub of ONU assignment, service tables and all key classes'
require_fixed 'cleanup_ret = en7581_xgspon_scrub_hardware_locked(priv);' \
	"$driver" 'verified cleanup after partial XG/XGS-PON start failure'
require_fixed '#define EN7581_XGSPON_US_AES_KEY_CTRL' "$driver" \
	'SDK upstream data-key control register'
require_fixed '#define EN7581_XGSPON_DS_AES_KEY_VALID' "$driver" \
	'SDK downstream data-key valid register'
require_fixed '#define EN7581_XGSPON_AES_UC_KEY0' "$driver" \
	'SDK unicast AES slot zero'
require_fixed '#define EN7581_XGSPON_AES_UC_KEY1' "$driver" \
	'SDK unicast AES slot one'
require_fixed 'EN7581_XGSPON_INT_US_KEY_SWITCH_DONE' "$driver" \
	'upstream data-key switch completion bit'
require_fixed 'en7581_xgspon_rollback_data_key_exchange_locked' "$driver" \
	'data-key transaction rollback'
require_fixed 'en7581_xgspon_drop_queued_key_reports_locked' "$driver" \
	'stale Key Report rollback cleanup'
require_fixed 'priv->data_key_index == key_index' "$driver" \
	'active data-key slot overwrite rejection'
require_fixed 'priv->data_key_confirm_pending = true;' "$driver" \
	'confirmation report completion gate'
require_fixed 'priv->active_key_report.report_type ==' "$driver" \
	'confirmation report IRQ matching'
require_fixed 'if (reschedule && !READ_ONCE(priv->removing))' "$driver" \
	'TK5 reschedule outside the driver mutex'
require_fixed '!priv->data_key_confirmed' "$driver" \
	'encrypted XGEM confirmation gate'
require_fixed '"session-keys-loaded-awaiting-onu-id-tx-disabled"' "$driver" \
	'truthful hardware-loaded pre-ONU-ID state'
require_fixed '"onu-id-assigned-registration-pending"' "$driver" \
	'truthful post-ONU-ID registration state'
require_fixed '"registered-secure-omcc-ready"' "$driver" \
	'truthful registered secure-OMCC state'
require_fixed '"hardware-state-uncertain-tx-disabled"' "$driver" \
	'truthful hardware cleanup failure state'
require_fixed 'failed to verify XGS-PON hardware key wipe' "$driver" \
	'hardware key wipe readback verification'
reject_fixed 'DEVICE_ATTR_WO(pon_tag)' "$driver" \
	'userspace PON-tag trust bypass'
reject_fixed 'gponDevSetPloamIk' "$driver" 'vendor key-register programming'
reject_fixed 'gponDevSetOmciIk' "$driver" 'vendor OMCI key-register programming'

# Keep every XGS activation and upstream OMCC trust gate explicit and fail-closed.
require_fixed '"phy calibration tc-ploam secure-omcc xgem"' "$driver" \
	'XGS activation blockers'
require_fixed '__u32 info = AIROHA_XGS_OMCC_ABI_VERSION;' "$driver" \
	'secure-OMCC capability query'
require_fixed '#define AIROHA_XGS_OMCC_ABI_VERSION	3' \
	"$openwrt_dir/package/kernel/airoha-xpon/src/airoha-xgs-omcc.h" \
	'v3 secure-OMCC ABI version'
require_fixed '__be64 instance_generation;' \
	"$openwrt_dir/package/kernel/airoha-xpon/src/airoha-xgs-omcc.h" \
	'trusted kernel-instance field in secure-OMCC ABI'
require_fixed '__be64 session_generation;' \
	"$openwrt_dir/package/kernel/airoha-xpon/src/airoha-xgs-omcc.h" \
	'trusted kernel-session field in secure-OMCC ABI'
require_fixed 'record->session_generation = cpu_to_be64(generation);' "$driver" \
	'kernel-session population in secure-OMCC RX record'
require_fixed 'header.instance_generation ||' "$driver" \
	'zero upstream instance field validation'
require_fixed 'header.session_generation ||' "$driver" \
	'zero upstream session field validation'
require_fixed 'en7581_xgspon_omcc_tx_ready_locked(priv)' "$driver" \
	'OMCC TX runtime readiness gate'
require_fixed 'static DEVICE_ATTR_RO(activation_ready);' "$driver" \
	'separate activation prerequisite state'
require_fixed 'priv->omcc_consumer_registered &&' "$driver" \
	'strict operational readiness consumer gate'
require_fixed 'airoha_xgs_omcc_tx_available(priv->ethernet_np)' "$driver" \
	'strict operational readiness QDMA gate'
require_fixed 'FIELD_GET(EN7581_XGSPON_ACTIVATION_STATE,' "$driver" \
	'OMCC O5 activation gate'
require_fixed 'AIROHA_XGS_OMCC_CAP_DS_MIC_VERIFIED' "$driver" \
	'downstream-MIC capability publication gate'
require_fixed 'AIROHA_XGS_OMCC_CAP_US_MIC_SIGNED' "$driver" \
	'upstream-MIC capability publication gate'
require_fixed 'airoha_xgs_sign_upstream_omci(' "$driver" \
	'in-kernel upstream OMCI signing'
require_fixed 'metadata.xgem_id = priv->onu_id;' "$driver" \
	'upstream OMCC XGEM metadata'
require_fixed 'metadata.channel = 0;' "$driver" \
	'upstream OMCC channel metadata'
require_fixed 'metadata.nboq = 0;' "$driver" \
	'upstream OMCC NBOQ metadata'
require_fixed 'metadata.mic_idx =' "$driver" \
	'upstream OMCC MIC-index metadata'
require_fixed 'airoha_xgs_omcc_transmit(priv->ethernet_np' "$driver" \
	'upstream OMCC QDMA transmit boundary'
require_fixed '+kmod-crypto-cmac' "$package_makefile" 'kernel CMAC dependency'
require_fixed '+kmod-crypto-ecb' "$package_makefile" 'kernel AES-ECB dependency'
require_fixed 'airoha,pon-mode = "gpon";' "$board_dts" \
	'fail-closed board default'
require_fixed 'bosa-controller = <&en7572>;' "$board_dts" \
	'XGS BOSA mode gate'
require_fixed 'airoha,pcs = <&pon_pcs>;' "$board_dts" \
	'XGS PCS mode gate'
require_fixed '<0x0 0x1faf0000 0x0 0x1000>' "$board_dts" \
	'XGS PHY CSR resource'
require_fixed '<GIC_SPI 43 IRQ_TYPE_LEVEL_HIGH>' "$board_dts" \
	'XGS optical PHY IRQ resource'
require_fixed 'interrupt-names = "mac", "phy";' "$board_dts" \
	'XGS named MAC and PHY IRQ ownership'
require_fixed "read_attribute(xgspon, 'phy_evidence')" "$luci_rpc" \
	'XGS PHY evidence RPC boundary'
require_fixed 'xgspon_phy_evidence: xgspon_phy_evidence' "$luci_rpc" \
	'XGS PHY evidence RPC result'
require_fixed "statusRow(_('XGS PHY evidence'), 'gpon-xgs-phy-evidence')" \
	"$luci_view" 'independent PON LuCI XGS PHY status row'
require_fixed "evidence.irq_owned === '1'" "$luci_view" \
	'truthful LuCI PHY IRQ ownership display'

# Do not promote incomplete SDK PM inputs into a G.988 counter backend. The
# vendor source itself assigns several fields to TC/PLOAM/OMCI software.
require_fixed 'int gponDevGetTCCounter(GPON_10G_TC_COUNTER_T *pXgponTcCounter)' \
	"$tmp_dir/gpon_dev.c" 'SDK XGS TC counter boundary'
require_fixed 'pXgponTcCounter->LODSEventCount = gponTCLODSEvent;' \
	"$tmp_dir/gpon_dev.c" 'SDK TC-owned LODS event counter'
require_fixed 'pXgponTcCounter->LODSEventRestoreCount = gponTCRestoreLODSEvent;' \
	"$tmp_dir/gpon_dev.c" 'SDK TC-owned LODS restore counter'
require_fixed 'pXgponTcCounter->ONUReactivLODSEvents = gponTCReactivLODSEvent;' \
	"$tmp_dir/gpon_dev.c" 'SDK TC-owned LODS reactivation counter'
require_fixed 'pXgponDsMgntCounter->BaseOmciMsgRx = 0; /*actural value in omci app*/' \
	"$tmp_dir/gpon_dev.c" 'SDK OMCI application-owned baseline counter'
require_fixed 'pXgponDsMgntCounter->ExtOmciMsgRx = 0;  /*actural value in omci app*/' \
	"$tmp_dir/gpon_dev.c" 'SDK OMCI application-owned extended counter'
require_fixed 'cnt_clear.Bits.nml_cnt_clr = XPON_ENABLE;' "$tmp_dir/gpon_dev.c" \
	'SDK explicit normal-counter clear'
require_fixed 'cnt_clear.Bits.err_cnt_clr = XPON_ENABLE;' "$tmp_dir/gpon_dev.c" \
	'SDK explicit error-counter clear'
require_fixed 'version=1 complete=0 semantics=raw-hardware-modulo' "$driver" \
	'incomplete XGS PM diagnostic contract'
require_fixed 'This is diagnostic evidence, not a complete G.988 PM backend.' \
	"$driver" 'fail-closed XGS PM backend boundary'
reject_fixed 'DEVICE_ATTR_RO(xgspon_counters)' "$driver" \
	'unverified complete XGS PM sysfs endpoint'
require_fixed 'gpGponPriv->dsPloamCounter[msgId] ++;' "$tmp_dir/gpon_ploam.c" \
	'SDK downstream PLOAM parser-dispatch counter boundary'
require_fixed 'gpGponPriv->usPloamCounter[PLOAM_UP_MSG_ACKNOWLEDGE] ++;' \
	"$tmp_dir/gpon_ploam.c" 'SDK upstream PLOAM send counter boundary'
require_fixed 'static DEVICE_ATTR_RO(xgs_ploam_evidence);' "$driver" \
	'read-only XGS PLOAM lifecycle evidence ABI'
require_fixed 'version=3 complete=0 semantics=driver-lifecycle' "$driver" \
	'versioned XGS PLOAM diagnostic contract'
require_fixed 'semantics=driver-lifecycle' "$driver" \
	'explicit XGS PLOAM diagnostic semantics'
require_fixed 'instance_generation=%llu session_generation=%llu' "$driver" \
	'XGS PLOAM diagnostic instance/session boundary'
require_fixed 'session_generation=%llu' "$driver" \
	'XGS PLOAM diagnostic session-generation field'
require_fixed 'session_generation = priv->session_generation;' "$driver" \
	'XGS PLOAM diagnostic session-generation snapshot'
require_fixed 'priv->session_generation++;' "$driver" \
	'XGS OMCC session-generation transition'
require_fixed 'priv->profile_messages_received++;' "$driver" \
	'SDK-style Profile parser-dispatch count'
require_fixed 'priv->key_control_messages_received++;' "$driver" \
	'SDK-style Key Control parser-dispatch count'
require_fixed 'serial_number_messages_completed=%llu' "$driver" \
	'completed upstream Serial Number diagnostic count'
require_fixed 'sleep_request_messages_completed=0' "$driver" \
	'explicitly unsupported Sleep Request count'

(
	cd "$omci_dir"
	go test ./internal/xgssecurity -run \
		'^(TestG98071KeyDerivationVector|TestRegistrationDerivationAndInputLengths)$' \
		-count=1
)

echo 'EN7581 trusted Registration-ID key derivation matches SDK evidence and remains fail-closed'
