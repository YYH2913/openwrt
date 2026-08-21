#!/bin/sh
set -eu

script_dir="$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)"
openwrt_dir="${1:-$(CDPATH= cd -- "$script_dir/../../../.." && pwd)}"
sdk_archive="${AIROHA_SDK_ARCHIVE:-$openwrt_dir/../airoha_sdk.tar.gz}"
driver="$openwrt_dir/package/kernel/airoha-xpon/src/airoha-xgspon.c"
transaction="$openwrt_dir/package/kernel/airoha-xpon/src/airoha-xgs-mac-init.h"
test_source="$script_dir/xgs-mac-init-test.c"
tmp_dir="$(mktemp -d "${TMPDIR:-/tmp}/en7581-xgs-mac-init.XXXXXX")"
trap 'rm -rf "$tmp_dir"' EXIT INT TERM

[ -f "$sdk_archive" ] || {
	echo "Airoha SDK archive not found at $sdk_archive" >&2
	exit 2
}

tar -xOf "$sdk_archive" \
	airoha_sdk/private/xpon_10g/inc/gpon/xgpon_mac_reg_c_header.h \
	> "$tmp_dir/xgpon_mac_reg_c_header.h"
tar -xOf "$sdk_archive" airoha_sdk/private/xpon_10g/src/gpon/gpon_init.c \
	> "$tmp_dir/gpon_init.c"
tar -xOf "$sdk_archive" airoha_sdk/private/xpon_10g/src/gpon/gpon_dev.c \
	> "$tmp_dir/gpon_dev.c"
tar -xOf "$sdk_archive" airoha_sdk/private/xpon_10g/src/gpon/gpon_dvt.c \
	> "$tmp_dir/gpon_dvt.c"
tar -xOf "$sdk_archive" airoha_sdk/private/xpon_10g/inc/gpon/gpon_dev.h \
	> "$tmp_dir/gpon_dev.h"

require_fixed() {
	pattern="$1"
	file="$2"
	description="$3"

	if ! grep -Fq -- "$pattern" "$file"; then
		echo "missing $description: $pattern" >&2
		exit 1
	fi
}

# Prove the offsets and defaults against the recovered SDK before exercising
# the same settings through the production transaction core.
require_fixed 'REG_O23_O4_PLOAMU_CTRL          O23_O4_PLOAMU_CTRL; // 5100' \
	"$tmp_dir/xgpon_mac_reg_c_header.h" 'SDK O2/O3/O4 reply-control register'
require_fixed 'REG_ACTIVATION_ST               ACTIVATION_ST;    // 5104' \
	"$tmp_dir/xgpon_mac_reg_c_header.h" 'SDK activation-state register'
require_fixed 'REG_RSP_TIME                    RSP_TIME;         // 5108' \
	"$tmp_dir/xgpon_mac_reg_c_header.h" 'SDK response-time register'
require_fixed 'REG_IDLE_GEM_CTRL               IDLE_GEM_CTRL;    // 5280' \
	"$tmp_dir/xgpon_mac_reg_c_header.h" 'SDK idle-XGEM register'
require_fixed 'REG_MIB_CTRL                    MIB_CTRL;         // 5500' \
	"$tmp_dir/xgpon_mac_reg_c_header.h" 'SDK MIB register'
require_fixed 'REG_DBG_CAP_SETTING             DBG_CAP_SETTING;  // 5800' \
	"$tmp_dir/xgpon_mac_reg_c_header.h" 'SDK capability register'
require_fixed 'REG_DBG_RESYNC                  DBG_RESYNC;       // 582C' \
	"$tmp_dir/xgpon_mac_reg_c_header.h" 'SDK late-resync register'
require_fixed 'REG_DBG_CAP_SETTING1            DBG_CAP_SETTING1; // 5868' \
	"$tmp_dir/xgpon_mac_reg_c_header.h" 'SDK downstream-FEC register'
require_fixed '#define XGSPON_RSP_TIME' "$tmp_dir/gpon_dev.h" \
	'SDK XGS-PON response-time default'
require_fixed 'xgpon_priv_p->gponCfg.idleGemThreshold = 0x120;' \
	"$tmp_dir/gpon_init.c" 'SDK non-FPGA idle-XGEM threshold'
require_fixed 'dbg_cap_setting.Bits.o52_idle_only_en = 1;' \
	"$tmp_dir/gpon_init.c" 'SDK O5.2 idle-only behavior'
require_fixed 'gponDevSetDsFecMode(gpGponPriv->gponCfg.dsFecMode);' \
	"$tmp_dir/gpon_dev.c" 'SDK downstream-FEC initialization'
require_fixed 'gponDevSetIdleGemThreshold(gpGponPriv->gponCfg.idleGemThreshold);' \
	"$tmp_dir/gpon_dev.c" 'SDK idle-XGEM initialization'
require_fixed 'gponDevSetMibCounterType(GPON_10G_COUNTER_TYPE_ETHERNET);' \
	"$tmp_dir/gpon_dev.c" 'SDK Ethernet MIB counter mode'
require_fixed '{(regAddr_t)O23_O4_PLOAMU_CTRL,            0x0,            0x1},' \
	"$tmp_dir/gpon_dvt.c" 'SDK hardware PLOAM reply reset default'
require_fixed '{(regAddr_t)ACTIVATION_ST,                 0x1,            0xf},' \
	"$tmp_dir/gpon_dvt.c" 'SDK O1 activation reset default'
require_fixed '{(regAddr_t)MIB_CTRL,                      0x1,            0x101},' \
	"$tmp_dir/gpon_dvt.c" 'SDK enabled MIB reset default and writable fields'
require_fixed 'gponDevMbiStop(XPON_RESET_HOLD_ON, XPON_WITH_GDM2CDM2_STOP) ;' \
	"$tmp_dir/gpon_dev.c" 'SDK MBI stop before MAC reset hold'
require_fixed 'gponDevMpiStop(XPON_RESET_HOLD_ON) ;' \
	"$tmp_dir/gpon_dev.c" 'SDK MPI stop before MAC reset hold'
require_fixed 'gponDevMacReset(XPON_RESET_RELEASE);' \
	"$tmp_dir/gpon_dev.c" 'SDK MAC reset release before interface release'
require_fixed 'gponDevMbiStop(XPON_RESET_RELEASE, XPON_WITH_GDM2CDM2_STOP) ;' \
	"$tmp_dir/gpon_dev.c" 'SDK MBI release after MAC reset'
require_fixed 'gponDevMpiStop(XPON_RESET_RELEASE) ;' \
	"$tmp_dir/gpon_dev.c" 'SDK MPI release after MAC reset'

require_fixed '#include "airoha-xgs-mac-init.h"' "$driver" \
	'production transaction include'
require_fixed 'return airoha_xgs_mac_init_transaction_mode(' "$driver" \
	'mode-aware production transaction call'
require_fixed 'airoha_xgpon_mac_init_settings' "$transaction" \
	'XG-PON MAC initialization profile'
require_fixed '0x00000551U' "$transaction" \
	'SDK XG-PON response-time default'
require_fixed '0x0000001aU' "$transaction" \
	'SDK XG-PON idle-XGEM threshold'
require_fixed 'static DEVICE_ATTR_RO(mac_initialization);' "$driver" \
	'live MAC initialization readback evidence'
require_fixed 'setting->reg == AIROHA_XGS_MAC_INIT_PLOAM_CONTROL ||' "$driver" \
	'dynamic activation fields excluded from persistent verification'
require_fixed 'airoha_en7572_emergency_disable(priv->bosa);' "$driver" \
	'fail-closed optical TX disable'
require_fixed 'writel(0, priv->mac + EN7581_XGSPON_INT_ENABLE);' "$driver" \
	'fail-closed MAC interrupt mask'
require_fixed 'ret = en7581_xgspon_set_ploam_reply_mode(priv, true);' "$driver" \
	'activation-gated software PLOAM reply selection'
require_fixed '#define EN7581_XGSPON_PATH_STOP_MASK' "$driver" \
	'complete XG/XGS MBI and MPI stop mask'
require_fixed 'ret = en7581_xgspon_set_path_stopped(priv, true);' "$driver" \
	'polling XG/XGS datapath stop boundary'
require_fixed 'reset_ret = en7581_xgspon_set_mac_reset(priv, true);' "$driver" \
	'fail-closed XG/XGS MAC reset hold'
require_fixed 'ret = en7581_xgspon_prepare_mac(priv);' "$driver" \
	'MAC preparation with data paths held'
require_fixed 'ret = en7581_xgspon_set_path_stopped(priv, false);' "$driver" \
	'XG/XGS datapath release after QDMA clear'

sed -n '/static int en7581_xgspon_xpon_stop_datapath(/,/^}/p' "$driver" \
	> "$tmp_dir/stop_datapath.c"
sed -n '/static int en7581_xgspon_xpon_start_datapath(/,/^}/p' "$driver" \
	> "$tmp_dir/start_datapath.c"
awk '
	/en7581_xgspon_clear_service_locked/ { clear = NR }
	/en7581_xgspon_set_path_stopped\(priv, true\)/ { stop = NR }
	END { exit !(clear && stop && clear < stop) }
' "$tmp_dir/stop_datapath.c" || {
	echo 'XG/XGS stop sequence does not drain services before path stop' >&2
	exit 1
}
awk '
	/airoha_xgs_qdma_service_clear/ { clear = NR }
	/en7581_xgspon_set_path_stopped\(priv, false\)/ { release = NR }
	END { exit !(clear && release && clear < release) }
' "$tmp_dir/start_datapath.c" || {
	echo 'XG/XGS start sequence does not clear QDMA before path release' >&2
	exit 1
}

cc -std=c11 -Wall -Wextra -Werror -pedantic \
	-I"$(dirname "$transaction")" "$test_source" -o "$tmp_dir/test"
"$tmp_dir/test"

echo 'EN7581 XG-PON/XGS-PON MAC initialization matches SDK defaults and fails closed'
