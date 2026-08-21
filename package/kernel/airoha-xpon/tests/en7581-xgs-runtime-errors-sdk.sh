#!/bin/sh
set -eu

script_dir="$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)"
openwrt_dir="${1:-$(CDPATH= cd -- "$script_dir/../../../.." && pwd)}"
sdk_root="${AIROHA_SDK_ROOT:-$openwrt_dir/../tmp/airoha-sdk/airoha_sdk}"
sdk_header="$sdk_root/private/xpon_10g/inc/gpon/xgpon_mac_reg_c_header.h"
sdk_device="$sdk_root/private/xpon_10g/src/gpon/gpon_dev.c"
sdk_isr="$sdk_root/private/xpon_10g/src/gpon/gpon.c"
driver="$openwrt_dir/package/kernel/airoha-xpon/src/airoha-xgspon.c"

[ -f "$sdk_header" ] && [ -f "$sdk_device" ] && [ -f "$sdk_isr" ] || {
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

require_regex() {
	pattern="$1"
	file="$2"
	description="$3"

	grep -Eq -- "$pattern" "$file" || {
		echo "missing $description: $pattern" >&2
		exit 1
	}
}

require_fixed 'REG_RX_ERR_STS                  RX_ERR_STS;       // 5058' \
	"$sdk_header" 'SDK RX error status offset'
require_fixed 'REG_DBG_BWM_CKH_STS             DBG_BWM_CKH_STS;  // 5808' \
	"$sdk_header" 'SDK BWmap check status offset'
require_regex '^#define[[:space:]]+INT_STATUS_FLD_bwm_chk_err_int[[:space:]]+REG_FLD\(1, 13\)' \
	"$sdk_header" 'SDK BWmap summary interrupt bit'
require_regex '^#define[[:space:]]+INT_STATUS_FLD_rx_err_int[[:space:]]+REG_FLD\(1, 17\)' \
	"$sdk_header" 'SDK RX summary interrupt bit'
require_fixed 'gponIntEnable.Bits.rx_err_int_en                 = 1;' \
	"$sdk_device" 'SDK RX error interrupt enable'
require_fixed 'gponIntEnable.Bits.bwm_chk_err_int_en            = 1;' \
	"$sdk_device" 'SDK BWmap check interrupt enable'
require_fixed 'gponRxErrSts.Raw =	IO_GREG(RX_ERR_STS)' "$sdk_device" \
	'SDK RX detail read'
require_fixed 'gponbwpChkStatus.Raw = IO_GREG(DBG_BWM_CKH_STS);' \
	"$sdk_device" 'SDK BWmap detail read'
require_fixed 'IO_SREG(RX_ERR_STS, 0xffffffff);' "$sdk_device" \
	'SDK EN7581 RX detail W1C'
require_fixed 'IO_SREG(DBG_BWM_CKH_STS, 0xffffffff);' "$sdk_device" \
	'SDK BWmap detail W1C'
require_fixed 'if(intStatus.Bits.rx_err_int)' "$sdk_isr" \
	'SDK RX summary dispatch'
require_fixed 'if(intStatus.Bits.bwm_chk_err_int)' "$sdk_isr" \
	'SDK BWmap summary dispatch'

require_fixed '#define EN7581_XGSPON_RX_ERR_STS' "$driver" \
	'OpenWrt RX error status register'
require_fixed '0x058' "$driver" 'OpenWrt RX error status offset'
require_fixed '#define EN7581_XGSPON_BWMAP_CHECK_STS' "$driver" \
	'OpenWrt BWmap check status register'
require_fixed '0x808' "$driver" 'OpenWrt BWmap check status offset'
require_fixed '#define EN7581_XGSPON_INT_BWMAP_CHECK_ERROR' "$driver" \
	'OpenWrt BWmap summary interrupt'
require_fixed '#define EN7581_XGSPON_INT_RX_ERROR' "$driver" \
	'OpenWrt RX summary interrupt'
require_fixed 'EN7581_XGSPON_INT_DOWNSTREAM_ERRORS)' "$driver" \
	'downstream error interrupts enabled without TX authorization'
require_fixed 'en7581_xgspon_record_downstream_errors(priv, status);' "$driver" \
	'OpenWrt downstream error dispatch'
require_fixed 'writel(rx_errors, priv->mac + EN7581_XGSPON_RX_ERR_STS);' \
	"$driver" 'OpenWrt RX detail W1C'
require_fixed 'priv->mac + EN7581_XGSPON_BWMAP_CHECK_STS);' "$driver" \
	'OpenWrt BWmap detail W1C'
require_fixed 'loss_of_gem_delineation_events++;' "$driver" \
	'loss-of-GEM-delineation evidence'
require_fixed 'static DEVICE_ATTR_RO(mac_errors);' "$driver" \
	'read-only MAC error evidence'

handler="$(sed -n '/^static void en7581_xgspon_record_downstream_errors(/,/^}/p' "$driver")"
if printf '%s\n' "$handler" | grep -Eq \
	'activation_fault|emergency_disable|set_tx_enabled|tx_authorized'; then
	echo 'SDK observational RX/BWmap errors must not revoke optical TX' >&2
	exit 1
fi

echo "EN7581 XG/XGS runtime RX/BWmap error SDK checks passed"
