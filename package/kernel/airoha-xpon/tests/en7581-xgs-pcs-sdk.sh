#!/bin/sh
set -eu

script_dir="$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)"
openwrt_dir="${1:-$(CDPATH= cd -- "$script_dir/../../../.." && pwd)}"
sdk_archive="${AIROHA_SDK_ARCHIVE:-$openwrt_dir/../airoha_sdk.tar.gz}"
pcs_patch="$openwrt_dir/target/linux/airoha/patches-6.18/608-net-pcs-airoha-add-en7581-gpon-mode.patch"
pcs_runtime_patch="$openwrt_dir/target/linux/airoha/patches-6.18/609-net-pcs-airoha-add-runtime-xpon-modes.patch"
pcs_stage_patch="$openwrt_dir/target/linux/airoha/patches-6.18/610-net-pcs-airoha-split-xpon-wan-selection.patch"
pcs_validation_patch="$openwrt_dir/target/linux/airoha/patches-6.18/614-net-pcs-airoha-validate-runtime-xpon-mode.patch"
driver="$openwrt_dir/package/kernel/airoha-xpon/src/airoha-xgspon.c"
board_dts="$openwrt_dir/target/linux/airoha/dts/an7581-axon-xg2010g-ubi.dts"
tmp_dir="$(mktemp -d "${TMPDIR:-/tmp}/en7581-xgs-pcs.XXXXXX")"
trap 'rm -rf "$tmp_dir"' EXIT INT TERM

[ -f "$sdk_archive" ] || {
	echo "Airoha SDK archive not found at $sdk_archive" >&2
	exit 2
}
[ -f "$pcs_patch" ] || {
	echo "PCS patch not found at $pcs_patch" >&2
	exit 2
}
[ -f "$pcs_runtime_patch" ] || {
	echo "runtime PCS patch not found at $pcs_runtime_patch" >&2
	exit 2
}
[ -f "$pcs_validation_patch" ] || {
	echo "runtime PCS validation patch not found at $pcs_validation_patch" >&2
	exit 2
}

tar -xOf "$sdk_archive" airoha_sdk/private/xpon_phy_10g/inc/en7581_pma.h > "$tmp_dir/en7581_pma.h"
tar -xOf "$sdk_archive" airoha_sdk/private/xpon_phy_10g/inc/phy_reg.h > "$tmp_dir/phy_reg.h"
tar -xOf "$sdk_archive" airoha_sdk/private/xpon_phy_10g/src/en7581_pma.c > "$tmp_dir/en7581_pma.c"
tar -xOf "$sdk_archive" airoha_sdk/private/xpon_phy_10g/inc/en7581_reg.h > "$tmp_dir/en7581_reg.h"
tar -xOf "$sdk_archive" airoha_sdk/private/xpon_phy_10g/src/en7581.c > "$tmp_dir/en7581.c"

require_fixed() {
	pattern="$1"
	file="$2"
	description="$3"

	if ! grep -Fq -- "$pattern" "$file"; then
		echo "missing $description: $pattern" >&2
		exit 1
	fi
}

# SDK mode identity and the Sync_XGSPON_1 rate tuple.
require_fixed '#define Sync_XGSPON_1     10' "$tmp_dir/en7581_pma.h" 'SDK XGS mode ID'
require_fixed '#define SCU_WAN_CONF_REG_WAN_SEL_XGSPON' "$tmp_dir/phy_reg.h" 'SDK XGS WAN selector'
require_fixed 'InitSpd_TXPLL = 9;' "$tmp_dir/en7581_pma.c" 'SDK XGS TXPLL rate'
require_fixed 'InitSpd_RX = 9;' "$tmp_dir/en7581_pma.c" 'SDK XGS RX rate'
require_fixed 'InitSpd_ANA = 9;' "$tmp_dir/en7581_pma.c" 'SDK XGS analog rate'
require_fixed '0x07F66E86' "$tmp_dir/en7581_pma.c" 'SDK 9.95328G TXPLL PCW'
require_fixed '0x0FECDD0C' "$tmp_dir/en7581_pma.c" 'SDK 9.95328G TDC PCW'
require_fixed '0xA49A' "$tmp_dir/en7581_pma.c" 'SDK CDR calibration target'
require_fixed '0xA436' "$tmp_dir/en7581_pma.c" 'SDK XGS lock window start'
require_fixed '0xA4FF' "$tmp_dir/en7581_pma.c" 'SDK XGS lock window end'
require_fixed 'void XPON_DIG_reset_release(void)' "$tmp_dir/en7581_pma.c" \
	'SDK XPON digital reset release sequence'
require_fixed 'EN7581_XPON_PMA_SW_RST_SET, 6, 6, 0x01' \
	"$tmp_dir/en7581_pma.c" 'SDK TX FIFO reset release'
require_fixed 'EN7581_XPON_PMA_SW_RST_SET, 5, 5, 0x01' \
	"$tmp_dir/en7581_pma.c" 'SDK reference reset release'
require_fixed 'EN7581_XPON_PMA_SW_RST_SET, 4, 4, 0x01' \
	"$tmp_dir/en7581_pma.c" 'SDK all-PCS reset release'
require_fixed 'EN7581_XPON_PMA_SW_RST_SET, 3, 3, 0x01' \
	"$tmp_dir/en7581_pma.c" 'SDK PMA reset release'
require_fixed 'EN7581_XPON_PMA_SW_RST_SET, 2, 2, 0x01' \
	"$tmp_dir/en7581_pma.c" 'SDK TX reset release'
require_fixed 'EN7581_XPON_PMA_SW_RST_SET, 1, 1, 0x01' \
	"$tmp_dir/en7581_pma.c" 'SDK RX reset release'
require_fixed 'EN7581_XPON_PMA_SW_RST_SET, 0, 0, 0x01' \
	"$tmp_dir/en7581_pma.c" 'SDK RX FIFO reset release'

# OpenWrt's clean-room mapping of the same branch. Keep GPON selected in DTS
# until a valid XGS EN7572 table and the TC/PLOAM/OMCC/XGEM paths exist.
require_fixed 'AIROHA_SCU_WAN_SEL_XGSPON' "$pcs_patch" 'OpenWrt XGS WAN selector'
require_fixed 'FIELD_PREP_CONST(AIROHA_SCU_WAN_SEL, 0xa)' "$pcs_patch" 'OpenWrt selector value'
require_fixed 'ckin_divisor = 0x5;' "$pcs_patch" 'OpenWrt XGS TX divisor'
require_fixed 'tx_rate_ctrl = 0x2;' "$pcs_patch" 'OpenWrt XGS TX bus rate'
require_fixed 'fir_c0b = 0xe;' "$pcs_patch" 'OpenWrt XGS FIR c0b'
require_fixed 'fir_c1 = 0x4;' "$pcs_patch" 'OpenWrt XGS FIR c1'
require_fixed '? 0xd : 0)' "$pcs_patch" 'OpenWrt XGS TX delay high field'
require_fixed '? 0x30 : 0x4)' "$pcs_patch" 'OpenWrt XGS TX delay low field'
require_fixed '? 0x0 : 0x2;' "$pcs_patch" 'OpenWrt XGS RX OSR'
require_fixed '? 0x2 : 0x0;' "$pcs_patch" 'OpenWrt XGS RX rate control'
require_fixed '? 0x100 : 0x300)' "$pcs_patch" 'OpenWrt XGS RX digital mode'
require_fixed 'AIROHA_PCS_ANA_RX_BUSBIT_SEL_16BIT' "$pcs_patch" 'OpenWrt XGS RX bus width'
require_fixed '0x07f66e86' "$pcs_patch" 'OpenWrt 9.95328G TXPLL PCW'
require_fixed '0x0fecdd0c' "$pcs_patch" 'OpenWrt 9.95328G TDC PCW'
require_fixed 'target_fl_out = 0xa49a;' "$pcs_patch" 'OpenWrt calibration target'
require_fixed 'target_begin = 0xa436;' "$pcs_patch" 'OpenWrt lock window start'
require_fixed 'target_end = 0xa4ff;' "$pcs_patch" 'OpenWrt lock window end'
require_fixed 'an7581_pcs_pon_tdc_off' "$pcs_patch" 'shared PON TDC off sequence'
require_fixed 'an7581_pcs_pon_tdc_on' "$pcs_patch" 'shared PON TDC on sequence'

# The public runtime proof must re-read every owner-defining hardware layer.
require_fixed 'int airoha_pcs_xpon_validate_mode(struct phylink_pcs *pcs,' \
	"$pcs_validation_patch" 'public runtime mode validation API'
require_fixed 'mutex_lock(&priv->recovery_lock);' "$pcs_validation_patch" \
	'serialized runtime mode validation'
require_fixed 'regmap_read(priv->scu, AIROHA_SCU_WAN_CONF, &value);' \
	"$pcs_validation_patch" 'live WAN selector readback'
require_fixed 'airoha_pcs_xpon_verify_mode(priv, port->index, internal);' \
	"$pcs_validation_patch" 'complete SDK PMA fingerprint revalidation'
require_fixed 'regmap_read(maps->pcs_mac, AIROHA_PCS_XFI_MAC_XFI_GIB_CFG,' \
	"$pcs_validation_patch" 'live PCS MAC stop-bit readback'
require_fixed 'AIROHA_PCS_PMA_SW_RST_SET, &value);' "$pcs_validation_patch" \
	'live XPON reset-release readback'
require_fixed 'if ((value & reset_mask) != reset_mask)' \
	"$pcs_validation_patch" 'exact SDK reset-release verification'
require_fixed 'EXPORT_SYMBOL_GPL(airoha_pcs_xpon_validate_mode);' \
	"$pcs_validation_patch" 'runtime mode validation export'

# The digital PHY owner follows the SDK interrupt and recovery contract.
require_fixed '#define EN7581_XGPON_PHY_XG_PON_RX_SYNC_CTRL' \
	"$tmp_dir/en7581_reg.h" 'SDK XGS RX control register'
require_fixed '#define EN7581_XGPON_PHY_XG_PHY_RST_N' \
	"$tmp_dir/en7581_reg.h" 'SDK XGS digital reset register'
require_fixed '#define EN7581_XGPON_PHY_XG_PON_INT_STA' \
	"$tmp_dir/en7581_reg.h" 'SDK XGS interrupt status register'
require_fixed '#define EN7581_XGPON_PHY_XG_PON_RX_SYNC_CTRL_RX_ENABLE' \
	"$tmp_dir/en7581_reg.h" 'SDK XGS RX-enable bit'
require_fixed '#define EN7581_XGPON_PHY_XG_PHY_RST_N_ON' \
	"$tmp_dir/en7581_reg.h" 'SDK XGS reset hold value'
require_fixed '#define EN7581_XGPON_PHY_XG_PHY_RST_N_OFF' \
	"$tmp_dir/en7581_reg.h" 'SDK XGS reset release value'
require_fixed 'phyIntStatus=IO_GPHYREG(EN7581_XGPON_PHY_XG_PON_INT_STA);' \
	"$tmp_dir/en7581.c" 'SDK interrupt-status read'
require_fixed 'IO_SPHYREG(EN7581_XGPON_PHY_XG_PON_INT_STA, phyIntStatus);' \
	"$tmp_dir/en7581.c" 'SDK same-value W1C interrupt clear'
require_fixed 'phy_delay1ms(2);' "$tmp_dir/en7581.c" \
	'SDK RX-ready recovery delay'
require_fixed 'phy_delay1ms(8); //after pma reset, have to wait the whole system stable.' \
	"$tmp_dir/en7581.c" 'SDK post-reset settle delay'

require_fixed '#define EN7581_XGSPON_PHY_RX_SYNC_CTRL' "$driver" \
	'OpenWrt XGS RX control register'
require_fixed '0xa04' "$driver" 'OpenWrt XGS RX control offset'
require_fixed '#define EN7581_XGSPON_PHY_RESET' "$driver" \
	'OpenWrt XGS reset register'
require_fixed '0xa0c' "$driver" 'OpenWrt XGS reset offset'
require_fixed '#define EN7581_XGSPON_PHY_INT_STATUS' "$driver" \
	'OpenWrt XGS interrupt-status register'
require_fixed '0xa10' "$driver" 'OpenWrt XGS interrupt-status offset'
require_fixed '#define EN7581_XGSPON_PHY_RX_ENABLE' "$driver" \
	'OpenWrt XGS RX-enable definition'
require_fixed 'BIT(16)' "$driver" 'OpenWrt XGS RX-enable bit'
require_fixed '#define EN7581_XGSPON_PHY_RESET_HOLD' "$driver" \
	'OpenWrt reset hold definition'
require_fixed '#define EN7581_XGSPON_PHY_RESET_RELEASE' "$driver" \
	'OpenWrt reset release definition'
require_fixed 'GENMASK(1, 0)' "$driver" 'OpenWrt reset release value'
require_fixed '#define EN7581_XGSPON_PHY_RECOVERY_DELAY_MS' "$driver" \
	'OpenWrt RX-ready recovery delay definition'
require_fixed '#define EN7581_XGSPON_PHY_SETTLE_MS' "$driver" \
	'OpenWrt post-reset settle delay definition'
require_fixed 'writel(status, priv->phy_csr + EN7581_XGSPON_PHY_INT_STATUS);' \
	"$driver" 'OpenWrt same-value W1C interrupt clear'
require_fixed 'devm_request_threaded_irq(dev, priv->phy_irq' "$driver" \
	'OpenWrt threaded PHY interrupt ownership'
require_fixed 'airoha_pcs_xpon_quiesce(priv->pcs)' "$driver" \
	'OpenWrt mode-aware PCS LOS quiesce boundary'
require_fixed 'airoha_pcs_xpon_select_wan(priv->pcs, pcs_mode)' "$driver" \
	'OpenWrt same-mode WAN selection boundary'
require_fixed 'airoha_pcs_xpon_set_mode(priv->pcs, pcs_mode)' "$driver" \
	'OpenWrt same-mode PCS PMA reinitialization boundary'
require_fixed 'airoha_pcs_xpon_recover(priv->pcs)' "$driver" \
	'OpenWrt mode-aware PCS PMA recovery boundary'
require_fixed 'static bool en7581_xgspon_phy_trusted_locked' "$driver" \
	'OpenWrt locked trusted-PHY gate'
require_fixed 'trusted_phy = en7581_xgspon_phy_trusted_locked(priv);' "$driver" \
	'per-record PLOAM PHY revalidation'
require_fixed 'int airoha_pcs_xgspon_rx_quiesce(struct phylink_pcs *pcs);' \
	"$pcs_patch" 'public PCS quiesce API'
require_fixed 'int airoha_pcs_xgspon_rx_recover(struct phylink_pcs *pcs);' \
	"$pcs_patch" 'public PCS recovery API'
require_fixed 'int airoha_pcs_xpon_quiesce(struct phylink_pcs *pcs);' \
	"$pcs_runtime_patch" 'public mode-aware PCS quiesce API'
require_fixed 'int airoha_pcs_xpon_select_wan(struct phylink_pcs *pcs,' \
	"$pcs_stage_patch" 'public mode-aware WAN-selection API'
require_fixed 'int airoha_pcs_xpon_recover(struct phylink_pcs *pcs);' \
	"$pcs_runtime_patch" 'public mode-aware PCS recovery API'
require_fixed 'struct mutex recovery_lock;' "$pcs_patch" \
	'serialized PCS recovery state'
require_fixed '<GIC_SPI 43 IRQ_TYPE_LEVEL_HIGH>' "$board_dts" \
	'XG2010G digital PHY SPI 43'
require_fixed 'interrupt-names = "mac", "phy";' "$board_dts" \
	'XGS MAC and PHY interrupt names'
require_fixed 'pcs-handle = <&pon_pcs>;' "$board_dts" \
	'XGS PCS provider reference'

recovery_block="$tmp_dir/recovery-worker.c"
sed -n '/^static void en7581_xgspon_phy_recovery_work/,/^}/p' \
	"$driver" > "$recovery_block"
recovery_quiesce_line="$(awk '/airoha_pcs_xpon_quiesce/ { print NR; exit }' \
	"$recovery_block")"
recovery_select_line="$(awk '/airoha_pcs_xpon_select_wan/ { print NR; exit }' \
	"$recovery_block")"
recovery_set_mode_line="$(awk '/airoha_pcs_xpon_set_mode/ { print NR; exit }' \
	"$recovery_block")"
recovery_release_line="$(awk '/airoha_pcs_xpon_recover/ { print NR; exit }' \
	"$recovery_block")"
[ -n "$recovery_quiesce_line" ] && [ -n "$recovery_select_line" ] && \
	[ -n "$recovery_set_mode_line" ] && \
	[ -n "$recovery_release_line" ] && \
	[ "$recovery_quiesce_line" -lt "$recovery_select_line" ] && \
	[ "$recovery_select_line" -lt "$recovery_set_mode_line" ] && \
	[ "$recovery_set_mode_line" -lt "$recovery_release_line" ] || {
	echo 'PHY recovery order must be quiesce -> select -> setup -> recover' >&2
	exit 1
}

require_fixed 'airoha,pon-mode = "xgspon";' "$openwrt_dir/target/linux/airoha/dts/an7581-axon-xg2010g-ubi.dts" 'fail-closed XGS-PON board default'

echo 'EN7581 XGS-PON PCS parameters match the recovered SDK evidence'
