#!/bin/sh
set -eu

script_dir="$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)"
openwrt_dir="${1:-$(CDPATH= cd -- "$script_dir/../../../.." && pwd)}"
sdk_root="${AIROHA_SDK_ROOT:-$openwrt_dir/../tmp/airoha-sdk/airoha_sdk}"
sdk_xpon="$sdk_root/private/xpon_10g/src/xpondrv.c"
sdk_global="$sdk_root/private/xpon_10g/inc/common/xpon_global.h"
sdk_scu="$sdk_root/linux/arch/arm64/mach-econet/ecnt_scu.c"
sdk_epon_act="$sdk_root/private/xpon_10g/src/epon/epon_act.c"
sdk_ic="$sdk_root/private/xpon_10g/src/ic/AN7581.c"
core="$openwrt_dir/package/kernel/airoha-xpon/src/airoha-xpon-core.c"
xgs="$openwrt_dir/package/kernel/airoha-xpon/src/airoha-xgspon.c"
epon="$openwrt_dir/package/kernel/airoha-xpon/src/airoha-epon.c"
xgs_init="$openwrt_dir/package/kernel/airoha-xpon/src/airoha-xgs-mac-init.h"
epon_init="$openwrt_dir/package/kernel/airoha-xpon/src/airoha-epon-mac-init.h"
board_dts="$openwrt_dir/target/linux/airoha/dts/an7581-axon-xg2010g-ubi.dts"

[ -f "$sdk_xpon" ] && [ -f "$sdk_global" ] && [ -f "$sdk_scu" ] &&
	[ -f "$sdk_epon_act" ] && [ -f "$sdk_ic" ] || {
	echo "Airoha SDK checkout not found at $sdk_root" >&2
	exit 2
}

require_fixed() {
	pattern="$1"
	file="$2"
	description="$3"

	grep -Fq -- "$pattern" "$file" || {
		echo "missing $description: $pattern" >&2
		exit 1
	}
}

require_fixed '#define CR_NP_SCU_DMTC' "$sdk_scu" 'SDK SCU DMTC register'
require_fixed '(CR_NP_SCU_BASE + 0x84)' "$sdk_scu" 'SDK DMTC offset'
require_fixed '#define SCU_DYING_GASP_STATUS' "$sdk_global" \
	'SDK Dying Gasp status definition'
require_fixed '(1<<16)' "$sdk_global" 'SDK Dying Gasp status bit'
require_fixed 'irq = get_xpon_irq(1);' "$sdk_xpon" 'SDK dedicated IRQ index'
require_fixed 'raw = raw | SCU_DYING_GASP_STATUS;' "$sdk_xpon" \
	'SDK W1C status acknowledgement'
require_fixed 'SET_DMTC(raw);' "$sdk_xpon" 'SDK SCU status writeback'
require_fixed 'isEponHwFlag = 1;' "$sdk_xpon" \
	'SDK default hardware EPON Dying Gasp'

require_fixed '#define EN7581_SCU_DMTC' "$core" 'OpenWrt DMTC register'
require_fixed '0x84' "$core" 'OpenWrt DMTC offset'
require_fixed '#define EN7581_SCU_DYING_GASP_STATUS' "$core" \
	'OpenWrt Dying Gasp status definition'
require_fixed 'BIT(16)' "$core" 'OpenWrt Dying Gasp status bit'
require_fixed 'regmap_write_bits(core->scu, EN7581_SCU_DMTC,' "$core" \
	'forced W1C status acknowledgement'
require_fixed 'devm_request_irq(dev, core->dying_gasp_irq,' "$core" \
	'common-owner IRQ request'
require_fixed 'atomic64_inc(&core->dying_gasp_events);' "$core" \
	'observable Dying Gasp event count'
require_fixed 'static DEVICE_ATTR_RO(dying_gasp);' "$core" \
	'read-only Dying Gasp evidence'

handler="$(sed -n '/^static irqreturn_t airoha_xpon_dying_gasp_irq(/,/^}/p' "$core")"
printf '%s\n' "$handler" | grep -Fq 'airoha_xpon_clear_dying_gasp(core)'
if printf '%s\n' "$handler" | grep -Eq \
	'airoha_en7572|set_tx_enabled|emergency_disable|xmit_|send_'; then
	echo 'Dying Gasp IRQ must leave optical TX and hardware MAC transmission intact' >&2
	exit 1
fi

xpon_node="$(sed -n '/xpon_controller: xpon-controller {/,/^[[:space:]]*};/p' "$board_dts")"
printf '%s\n' "$xpon_node" | grep -Fq '<GIC_SPI 34 IRQ_TYPE_LEVEL_HIGH>'
printf '%s\n' "$xpon_node" | grep -Fq 'interrupt-names = "dying-gasp";'
printf '%s\n' "$xpon_node" | grep -Fq 'airoha,scu = <&scuclk>;'
[ "$(grep -Fc '<GIC_SPI 34 IRQ_TYPE_LEVEL_HIGH>' "$board_dts")" -eq 1 ]
[ "$(grep -Fc 'interrupt-names = "dying-gasp";' "$board_dts")" -eq 1 ]
if grep -Fq 'dying_gasp_irq' "$xgs"; then
	echo 'XG/XGS backend must not retain the common Dying Gasp IRQ' >&2
	exit 1
fi

# All four requested modes use MAC hardware transmission after the common SCU
# IRQ acknowledges the power-loss status. EPON starts with the hardware bit
# clear and follows the SDK's reset-release, burst-mode, enable order.
[ "$(grep -Fc \
	'{ AIROHA_XGS_MAC_INIT_DYING_GASP, 0xfffff001U, 0x000ff001U }' \
	"$xgs_init")" -eq 2 ]
[ "$(grep -Fc \
	'{ AIROHA_EPON_MAC_INIT_DYING_GASP, 0x8000ff00U, 0x00000100U }' \
	"$epon_init")" -eq 2 ]
require_fixed '0x62ac, 0x00000100, 0x8000ff00' "$sdk_ic" \
	'SDK reset-held EPON Dying Gasp default'
ready="$(sed -n '/int epon_phy_ready_hw_init(/,/^}/p' "$sdk_epon_act")"
printf '%s\n' "$ready" | grep -Fq 'EPON_LOGIC_RESET_HOLD_OFF'
printf '%s\n' "$ready" | grep -Fq 'epon_dev_set_tx_burst_mode(TRUE)'
printf '%s\n' "$ready" |
	grep -Fq 'REGISTER_ACTION_EPON_SET_DYGASP_HW_EN'
require_fixed 'airoha_epon_dying_gasp_set(' "$epon" \
	'OpenWrt post-reset EPON Dying Gasp enable transaction'

echo "EN7581 common Dying Gasp SDK correspondence checks passed"
