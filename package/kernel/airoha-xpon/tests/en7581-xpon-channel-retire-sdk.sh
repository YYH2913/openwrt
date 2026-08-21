#!/bin/sh
set -eu

script_dir="$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)"
openwrt_dir="${1:-$(CDPATH= cd -- "$script_dir/../../../.." && pwd)}"
sdk_root="${AIROHA_SDK_ROOT:-$openwrt_dir/../tmp/airoha-sdk/airoha_sdk}"
sdk_fe="$sdk_root/private/fe/fe_api.c"
sdk_epon="$sdk_root/private/xpon_10g/src/epon/epon_compile_option_wrapper.c"
retire_patch="$openwrt_dir/target/linux/airoha/patches-6.18/935-net-airoha-add-xpon-channel-retire.patch"
eth_header="$openwrt_dir/package/kernel/airoha-xpon/src/airoha-eth.h"
epon_driver="$openwrt_dir/package/kernel/airoha-xpon/src/airoha-epon.c"
xgs_driver="$openwrt_dir/package/kernel/airoha-xpon/src/airoha-xgspon.c"
package_makefile="$openwrt_dir/package/kernel/airoha-xpon/Makefile"

require_fixed() {
	pattern="$1"
	file="$2"
	description="$3"

	if ! grep -Fq -- "$pattern" "$file"; then
		echo "missing $description: $pattern" >&2
		exit 1
	fi
}

[ -f "$sdk_fe" ] && [ -f "$sdk_epon" ] && [ -f "$retire_patch" ] || {
	echo 'SDK FE sources or OpenWrt channel-retire patch are missing' >&2
	exit 2
}

require_fixed 'static int fe_channel_retire_one' "$sdk_fe" \
	'SDK single-channel retirement implementation'
require_fixed 'write_reg_word(hwreg+(i<<2), 0);' "$sdk_fe" \
	'SDK QDMA queue quiesce before all-channel retirement'
require_fixed 'GDMA_CHN_RLS_STAT_OFFSET' "$sdk_fe" \
	'SDK GDM release completion check'
require_fixed 'QDMA2_CHN_VLD_BASE' "$sdk_fe" \
	'SDK QDMA channel-valid check'
require_fixed 'disable_gdma2_and_channel_retire' "$sdk_epon" \
	'SDK EPON reset retirement boundary'
require_fixed 'FE_API_SET_HWFWD_CHANNEL(FE_CDM_SEL_CDMA2, i, FE_DISABLE);' \
	"$sdk_epon" 'SDK CDM2 hardware-forwarding shutdown'
require_fixed 'FE_API_SET_CHANNEL_RETIRE_ALL' "$sdk_epon" \
	'SDK EPON GDM2 retire operation'
require_fixed 'mbi_hang_unlock_by_terminate(gdm_sel);' "$sdk_fe" \
	'SDK MBI TX/RX hang termination'

require_fixed 'REG_TXQ_CHAN_VALID' "$retire_patch" \
	'OpenWrt QDMA channel-valid register'
require_fixed 'TXQ_DISABLE_CHAN_QUEUE_MASK' "$retire_patch" \
	'OpenWrt per-channel queue close'
require_fixed 'REG_GDM_TX_CHN_VLD' "$retire_patch" \
	'OpenWrt GDM2 valid register'
require_fixed 'REG_GDM_RX_CHN_VLD' "$retire_patch" \
	'OpenWrt GDM2 RX-valid register'
require_fixed 'read_poll_timeout(airoha_xpon_channel_retired' "$retire_patch" \
	'bounded GDM/QDMA retirement completion wait'
require_fixed 'airoha_xpon_mbi_terminate_direction' "$retire_patch" \
	'OpenWrt MBI TX/RX hang termination'
require_fixed 'mutex_lock(&flow_offload_mutex);' "$retire_patch" \
	'shared QDMA ownership lock'
require_fixed 'mutex_lock(&dev->xgs_service_lock);' "$retire_patch" \
	'XGS service exclusion'
require_fixed 'mutex_lock(&dev->epon_service_lock);' "$retire_patch" \
	'EPON service exclusion'
require_fixed 'old_queue_close[AIROHA_XPON_QDMA_CLOSE_REGS]' "$retire_patch" \
	'QDMA queue-close snapshot'
require_fixed 'old_queue_close[i]);' "$retire_patch" \
	'QDMA queue-close restoration'
require_fixed 'old_txchn' "$retire_patch" 'GDM2 TX snapshot and restore'
require_fixed 'old_rxchn' "$retire_patch" 'GDM2 RX snapshot and restore'
require_fixed 'old_cdm_hwf' "$retire_patch" \
	'CDM2 hardware-forwarding snapshot and restore'
require_fixed 'REG_CDM_HWF_CHN_EN(2)' "$retire_patch" \
	'CDM2 hardware-forwarding disable and readback'
require_fixed 'AIROHA_XPON_GDM_RLS_WRITABLE_MASK' "$retire_patch" \
	'GDM2 release-command restoration mask'
require_fixed 'old_release & AIROHA_XPON_GDM_RLS_WRITABLE_MASK' \
	"$retire_patch" 'GDM2 release-command restoration and readback'
require_fixed 'usleep_range(1000, 2000);' "$retire_patch" \
	'SDK-compatible pre-retire drain delay'
require_fixed 'EXPORT_SYMBOL_GPL(airoha_xpon_qdma_channels_retire);' \
	"$retire_patch" 'XPON retirement API export'

require_fixed 'int airoha_xpon_qdma_channels_retire(' "$eth_header" \
	'XPON module retirement declaration'
require_fixed 'ret = airoha_xpon_qdma_channels_retire(priv->ethernet_np,' \
	"$epon_driver" 'EPON stop retirement call'
require_fixed 'u32 retire_mask = priv->datapath_llid_mask;' "$epon_driver" \
	'EPON active-channel retirement mask'
require_fixed 'u32 retire_mask = BIT(0);' "$xgs_driver" \
	'XG/XGS default T-CONT retirement mask'
require_fixed 'retire_mask |= BIT(priv->service_tcont_hw_index[i]);' \
	"$xgs_driver" 'XG/XGS service-channel retirement mask'
require_fixed 'ret = airoha_xpon_qdma_channels_retire(priv->ethernet_np, retire_mask);' \
	"$xgs_driver" 'XG/XGS stop retirement call'

release="$(sed -n 's/^PKG_RELEASE:=//p' "$package_makefile")"
case "$release" in
	''|*[!0-9]*)
		echo "invalid XPON package release: $release" >&2
		exit 1
		;;
esac
[ "$release" -ge 52 ] || {
	echo 'complete XPON channel retirement requires package release >= 52' >&2
	exit 1
}

echo 'AN758x XPON stop closes, retires and verifies active GDM2/QDMA channels'
