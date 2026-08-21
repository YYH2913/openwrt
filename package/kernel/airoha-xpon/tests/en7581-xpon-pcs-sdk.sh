#!/bin/sh
set -eu

script_dir="$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)"
openwrt_dir="${1:-$(CDPATH= cd -- "$script_dir/../../../.." && pwd)}"
sdk_archive="${AIROHA_SDK_ARCHIVE:-$openwrt_dir/../airoha_sdk.tar.gz}"
pcs_patch="$openwrt_dir/target/linux/airoha/patches-6.18/609-net-pcs-airoha-add-runtime-xpon-modes.patch"
pcs_stage_patch="$openwrt_dir/target/linux/airoha/patches-6.18/610-net-pcs-airoha-split-xpon-wan-selection.patch"
pcs_verify_patch="$openwrt_dir/target/linux/airoha/patches-6.18/611-net-pcs-airoha-verify-xpon-quiesce-recover.patch"
pcs_profile_patch="$openwrt_dir/target/linux/airoha/patches-6.18/612-net-pcs-airoha-verify-xpon-mode-profile.patch"
pcs_feos_patch="$openwrt_dir/target/linux/airoha/patches-6.18/613-net-pcs-airoha-apply-xpon-feos-workarounds.patch"
tmp_dir="$(mktemp -d "${TMPDIR:-/tmp}/en7581-xpon-pcs.XXXXXX")"
trap 'rm -rf "$tmp_dir"' EXIT INT TERM

[ -f "$sdk_archive" ] || {
	echo "Airoha SDK archive not found at $sdk_archive" >&2
	exit 2
}
[ -f "$pcs_patch" ] || {
	echo "PCS runtime-mode patch not found at $pcs_patch" >&2
	exit 2
}
[ -f "$pcs_stage_patch" ] || {
	echo "PCS WAN-selection patch not found at $pcs_stage_patch" >&2
	exit 2
}
[ -f "$pcs_verify_patch" ] || {
	echo "PCS quiesce verification patch not found at $pcs_verify_patch" >&2
	exit 2
}
[ -f "$pcs_profile_patch" ] || {
	echo "PCS profile verification patch not found at $pcs_profile_patch" >&2
	exit 2
}
[ -f "$pcs_feos_patch" ] || {
	echo "PCS FEOS workaround patch not found at $pcs_feos_patch" >&2
	exit 2
}

tar -xOf "$sdk_archive" \
	airoha_sdk/private/xpon_phy_10g/inc/en7581_pma.h > "$tmp_dir/en7581_pma.h"
tar -xOf "$sdk_archive" \
	airoha_sdk/private/xpon_phy_10g/inc/phy_reg.h > "$tmp_dir/phy_reg.h"
tar -xOf "$sdk_archive" \
	airoha_sdk/private/xpon_phy_10g/src/en7581_pma.c > "$tmp_dir/en7581_pma.c"

require_fixed() {
	pattern="$1"
	file="$2"
	description="$3"

	if ! grep -Fq -- "$pattern" "$file"; then
		echo "missing $description: $pattern" >&2
		exit 1
	fi
}

require_define() {
	symbol="$1"
	value="$2"
	file="$3"
	description="$4"

	if ! awk -v symbol="$symbol" -v value="$value" '
		$1 == "#define" && $2 == symbol && $3 == value { found = 1 }
		END { exit !found }
	' "$file"; then
		echo "missing $description: #define $symbol $value" >&2
		exit 1
	fi
}

sdk_profile() {
	mode="$1"
	output="$2"

	awk -v mode="$mode" '
		$0 ~ "case " mode ":" { capture = 1 }
		capture { print }
		capture && /break;/ { exit }
	' "$tmp_dir/en7581_pma.c" > "$output"

	[ -s "$output" ] || {
		echo "SDK profile not found for $mode" >&2
		exit 1
	}
}

check_profile() {
	mode="$1"
	txpll="$2"
	rx="$3"
	ana="$4"
	profile="$tmp_dir/profile-$mode"

	sdk_profile "$mode" "$profile"
	require_fixed "InitSpd_TXPLL = $txpll;" "$profile" "$mode TXPLL rate"
	require_fixed "InitSpd_RX = $rx;" "$profile" "$mode RX rate"
	require_fixed "InitSpd_ANA = $ana;" "$profile" "$mode analog rate"
}

# SDK mode identity and SCU selector values.
require_fixed '#define Async_XEPON       6' "$tmp_dir/en7581_pma.h" \
	'SDK 10G/1G EPON mode ID'
require_fixed '#define Sync_XEPON        7' "$tmp_dir/en7581_pma.h" \
	'SDK 10G/10G EPON mode ID'
require_fixed '#define Async_XGPON_1     9' "$tmp_dir/en7581_pma.h" \
	'SDK XG-PON mode ID'
require_fixed '#define Sync_XGSPON_1     10' "$tmp_dir/en7581_pma.h" \
	'SDK XGS-PON mode ID'
require_define SCU_WAN_CONF_REG_WAN_SEL_XEPON_10G_1G 6 \
	"$tmp_dir/phy_reg.h" 'SDK 10G/1G EPON selector'
require_define SCU_WAN_CONF_REG_WAN_SEL_XEPON_10G_10G 7 \
	"$tmp_dir/phy_reg.h" 'SDK 10G/10G EPON selector'
require_define SCU_WAN_CONF_REG_WAN_SEL_XGPON 9 \
	"$tmp_dir/phy_reg.h" 'SDK XG-PON selector'
require_define SCU_WAN_CONF_REG_WAN_SEL_XGSPON 10 \
	"$tmp_dir/phy_reg.h" 'SDK XGS-PON selector'

check_profile Async_XGPON_1 9 9 1
check_profile Sync_XGSPON_1 9 9 9
check_profile Async_XEPON 10 10 1
check_profile Sync_XEPON 10 10 10

# SDK clock families and receive frequency windows.
require_fixed '0x07F66E86' "$tmp_dir/en7581_pma.c" 'SDK 9.95328G TXPLL PCW'
require_fixed '0x0FECDD0C' "$tmp_dir/en7581_pma.c" 'SDK 9.95328G HRDDS PCW'
require_fixed '0x08400000' "$tmp_dir/en7581_pma.c" 'SDK 10.3125G TXPLL PCW'
require_fixed '0x10800000' "$tmp_dir/en7581_pma.c" 'SDK 10.3125G HRDDS PCW'
require_fixed '0xA436' "$tmp_dir/en7581_pma.c" 'SDK XG(S)-PON window start'
require_fixed '0xA4FF' "$tmp_dir/en7581_pma.c" 'SDK XG(S)-PON window end'
require_fixed '0x9E7A' "$tmp_dir/en7581_pma.c" 'SDK 10G-EPON window start'
require_fixed '0x9F43' "$tmp_dir/en7581_pma.c" 'SDK 10G-EPON window end'
require_fixed '0x1030' "$tmp_dir/en7581_pma.c" \
	'SDK symmetric/XG receive-front-end profile'
require_fixed '0x18B0' "$tmp_dir/en7581_pma.c" \
	'SDK asymmetric EPON receive-front-end profile'
require_fixed 'EN7581_XPON_PMA_ADD_DIG_RESERVE_12, 0, 0, 0x01' \
	"$tmp_dir/en7581_pma.c" 'SDK U22 FEOS workaround'
require_fixed 'EN7581_XPON_PMA_ADD_DIG_RESERVE_47, 28, 28, 0x01' \
	"$tmp_dir/en7581_pma.c" 'SDK T22 FEOS bit 28 workaround'
require_fixed 'EN7581_XPON_PMA_ADD_DIG_RESERVE_47, 31, 31, 0x01' \
	"$tmp_dir/en7581_pma.c" 'SDK T22 FEOS bit 31 workaround'

# OpenWrt mode selectors and per-rate PMA profiles recovered from the SDK.
require_fixed 'AIROHA_SCU_WAN_SEL_XGPON' "$pcs_patch" 'OpenWrt XG-PON selector'
require_fixed 'FIELD_PREP_CONST(AIROHA_SCU_WAN_SEL, 0x9)' "$pcs_patch" \
	'OpenWrt XG-PON selector value'
require_fixed 'FIELD_PREP_CONST(AIROHA_SCU_WAN_SEL, 0xa)' "$pcs_patch" \
	'OpenWrt XGS-PON selector value'
require_fixed 'FIELD_PREP_CONST(AIROHA_SCU_WAN_SEL, 0x6)' "$pcs_patch" \
	'OpenWrt 10G/1G EPON selector value'
require_fixed 'FIELD_PREP_CONST(AIROHA_SCU_WAN_SEL, 0x7)' "$pcs_patch" \
	'OpenWrt 10G/10G EPON selector value'
require_fixed 'pcw = epon ? 0x08400000 : 0x07f66e86;' "$pcs_patch" \
	'OpenWrt protocol-specific TXPLL PCW'
require_fixed 'hrdds_pcw = epon ? 0x10800000 : 0x0fecdd0c;' "$pcs_patch" \
	'OpenWrt protocol-specific HRDDS PCW'
require_fixed 'ckin_divisor = 0x3;' "$pcs_patch" 'OpenWrt XG-PON TX divisor'
require_fixed 'tx_delay_data = 0x8;' "$pcs_patch" 'OpenWrt XG-PON TX delay'
require_fixed 'ckin_divisor = 0x1;' "$pcs_patch" 'OpenWrt 10G/1G EPON TX divisor'
require_fixed 'tx_rate_ctrl = 0x1;' "$pcs_patch" 'OpenWrt 10G/1G EPON TX rate'
require_fixed 'tx_delay_ben = 0x2;' "$pcs_patch" 'OpenWrt 10G/1G EPON BEN delay'
require_fixed 'tx_delay_data = 0x6;' "$pcs_patch" 'OpenWrt 10G/1G EPON data delay'
require_fixed 'ckin_divisor = 0x5;' "$pcs_patch" 'OpenWrt symmetric 10G TX divisor'
require_fixed 'tx_rate_ctrl = 0x2;' "$pcs_patch" 'OpenWrt symmetric 10G TX rate'
require_fixed 'tx_delay_ben = 0xd;' "$pcs_patch" 'OpenWrt symmetric 10G BEN delay'
require_fixed 'tx_delay_data = 0x30;' "$pcs_patch" 'OpenWrt symmetric 10G data delay'
require_fixed 'target_begin = 0x9e7a;' "$pcs_patch" 'OpenWrt 10G-EPON window start'
require_fixed 'target_end = 0x9f43;' "$pcs_patch" 'OpenWrt 10G-EPON window end'
require_fixed 'target_begin = 0xa436;' "$pcs_patch" 'OpenWrt XG(S)-PON window start'
require_fixed 'target_end = 0xa4ff;' "$pcs_patch" 'OpenWrt XG(S)-PON window end'

# Runtime switching must expose real, serialized WAN-select and PMA stages.
require_fixed 'rtnl_lock();' "$pcs_stage_patch" 'RTNL mode-switch serialization'
require_fixed 'mutex_lock(&priv->recovery_lock);' "$pcs_stage_patch" \
	'PCS recovery-lock serialization'
require_fixed 'int airoha_pcs_xpon_select_wan(struct phylink_pcs *pcs,' \
	"$pcs_stage_patch" 'explicit WAN-selection API'
require_fixed 'regmap_read(priv->scu, AIROHA_SCU_WAN_CONF, &value);' \
	"$pcs_stage_patch" 'WAN-selector hardware readback'
require_fixed 'if ((value & AIROHA_SCU_WAN_SEL) != expected)' \
	"$pcs_stage_patch" 'WAN-selector readback gate'
require_fixed 'if (!priv->pon_quiesced || !priv->pon_wan_selected)' \
	"$pcs_stage_patch" 'WAN-selection gate before PMA setup'
require_fixed 'if (!priv->pon_quiesced || !priv->pon_pma_ready)' "$pcs_patch" \
	'PMA-ready gate before recovery'
require_fixed 'priv->pon_pma_ready = true;' "$pcs_stage_patch" \
	'PMA success publication'
require_fixed 'SDK XPON_DIG_reset_hold()' "$pcs_patch" \
	'SDK reset-hold sequence mapping'
require_fixed 'if ((value & stop_mask) != stop_mask)' "$pcs_verify_patch" \
	'quiesce MAC-stop readback gate'
require_fixed 'if (value & stop_mask)' "$pcs_verify_patch" \
	'recovery MAC-stop readback gate'
require_fixed 'regmap_read(pcs_pma, AIROHA_PCS_PMA_SW_RST_SET, &value);' \
	"$pcs_verify_patch" 'PMA reset-hold readback gate'
require_fixed 'return value & reset_mask ? -EIO : 0;' "$pcs_verify_patch" \
	'PMA reset-hold verification'
require_fixed 'priv->pon_mode == AIROHA_PCS_PON_MODE_EPON_10G_1G' \
	"$pcs_profile_patch" '10G/1G EPON receive-front-end selection'
require_fixed 'rx_fe_gain_ctrl = 0x3;' "$pcs_profile_patch" \
	'10G/1G EPON SDK receive gain'
require_fixed 'rx_rev_0 = 0x18b0;' "$pcs_profile_patch" \
	'10G/1G EPON SDK receive revision profile'
require_fixed 'rx_fe_gain_ctrl = 0x1;' "$pcs_profile_patch" \
	'XG/XGS and symmetric EPON SDK receive gain'
require_fixed 'rx_rev_0 = 0x1030;' "$pcs_profile_patch" \
	'XG/XGS and symmetric EPON SDK receive revision profile'
require_fixed 'airoha_pcs_xpon_verify_mode(priv, port->index, internal);' \
	"$pcs_profile_patch" 'PMA mode-profile readback gate'
require_fixed 'AIROHA_PCS_PMA_PXP_TXPLL_SDM_PCW' "$pcs_profile_patch" \
	'TXPLL clock-family readback'
require_fixed 'AIROHA_PCS_PMA_XPON_TX_RATE_CTRL' "$pcs_profile_patch" \
	'upstream-rate readback'
require_fixed 'AIROHA_PCS_PMA_TX_DLY_CTRL' "$pcs_profile_patch" \
	'BEN/data-delay readback'
require_fixed 'AIROHA_PCS_PMA_SS_RX_FREQ_DET_2' "$pcs_profile_patch" \
	'receive-frequency-window readback'
require_fixed 'AIROHA_PCS_ANA_PXP_RX_REV_0' "$pcs_profile_patch" \
	'receive analog-profile readback'
require_fixed 'AIROHA_PCS_PMA_DIG_RESERVE_12' "$pcs_feos_patch" \
	'OpenWrt U22 FEOS programming and readback'
require_fixed 'AIROHA_PCS_PMA_RESERVE_12_FEOS_0' "$pcs_feos_patch" \
	'OpenWrt U22 FEOS enable bit'
require_fixed 'AIROHA_PCS_PMA_DIG_RESERVE_47' "$pcs_feos_patch" \
	'OpenWrt T22 FEOS programming and readback'
require_fixed 'AIROHA_PCS_PMA_RESERVE_47_FEOS_T22' "$pcs_feos_patch" \
	'OpenWrt T22 FEOS enable mask'
require_fixed 'GENMASK(31, 28)' "$pcs_feos_patch" \
	'OpenWrt complete T22 FEOS bit range'

quiesce_line="$(awk '/^+int airoha_pcs_xpon_quiesce\(/ { print NR; exit }' "$pcs_patch")"
select_line="$(awk '/^+int airoha_pcs_xpon_select_wan\(/ { print NR; exit }' "$pcs_stage_patch")"
set_mode_line="$(awk '/^+int airoha_pcs_xpon_set_mode\(/ { print NR; exit }' "$pcs_stage_patch")"
recover_line="$(awk '/^+int airoha_pcs_xpon_recover\(/ { print NR; exit }' "$pcs_patch")"
[ -n "$quiesce_line" ] && [ -n "$select_line" ] &&
	[ -n "$set_mode_line" ] && [ -n "$recover_line" ] &&
	[ "$select_line" -lt "$set_mode_line" ] || {
	echo 'runtime PCS API must split WAN selection before PMA setup' >&2
	exit 1
}

echo 'EN7581 XG-PON, XGS-PON and 10G-EPON PCS profiles match SDK evidence'
