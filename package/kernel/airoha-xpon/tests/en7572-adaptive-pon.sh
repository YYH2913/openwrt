#!/bin/sh
set -eu

script_dir="$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)"
source_dir="$(dirname -- "$script_dir")"
driver="$source_dir/src/airoha-en7572.c"
dts="$source_dir/../../../target/linux/airoha/dts/an7581-axon-xg2010g-ubi.dts"

require_fixed() {
	needle="$1"
	file="$2"
	message="$3"

	grep -Fq "$needle" "$file" || {
		echo "$message" >&2
		exit 1
	}
}

xg_mapping="$(sed -n '/case AIROHA_XPON_MODE_XGPON:/,/return 0;/p' "$driver")"
epon_mapping="$(sed -n '/case AIROHA_XPON_MODE_EPON_10G_1G:/,/return 0;/p' "$driver")"
gpon_mapping="$(sed -n '/case AIROHA_XPON_MODE_GPON:/,/return 0;/p' "$driver")"

printf '%s\n' "$gpon_mapping" | grep -Fq '*bank = EN7572_BOB_BANK_A0;'
printf '%s\n' "$xg_mapping" | grep -Fq 'AIROHA_XPON_MODE_XGSPON'
printf '%s\n' "$xg_mapping" | grep -Fq '*bank = EN7572_BOB_BANK_A2;'
printf '%s\n' "$epon_mapping" | grep -Fq 'AIROHA_XPON_MODE_EPON_10G_10G'
printf '%s\n' "$epon_mapping" | grep -Fq '*bank = EN7572_BOB_BANK_A0;'

require_fixed 'name = EN7572_DEFAULT_10G_BOB_FW;' "$driver" \
	'four 10G modes must share one device BOB record'
require_fixed 'property = "airoha,calibration-10g-firmware";' "$driver" \
	'missing shared 10G calibration firmware override'
require_fixed 'cell_name = "calibration-10g";' "$driver" \
	'missing shared 10G calibration nvmem cell'
require_fixed 'en7572_mode_uses_shared_10g_bob(priv->pon_mode) && require_bank' "$driver" \
	'A2-to-A0 lab override must not bypass the GPON calibration cell'
require_fixed 'ret = require_bank ? -ENODATA : -EINVAL;' "$driver" \
	'missing target eye must fail distinctly before optical TX can be enabled'
require_fixed 'EN7572_BEN_OFF' "$driver" \
	'AdaptivePon port must force BEN off while changing eyes'
require_fixed 'EN7572_DCL_CTRL2' "$driver" \
	'AdaptivePon port must restore IAV/IMOD calibration'
require_fixed 'en7572_update_bits(priv, EN7572_TIA_CTRL, EN7572_TIA_CURRENT,' "$driver" \
	'AdaptivePon port must program TIA current before ERC'
require_fixed 'en7572_update_bits(priv, EN7572_TIA_CTRL, EN7572_TIA_GAIN_BW,' "$driver" \
	'AdaptivePon port must program TIA gain/bandwidth after ERC'
require_fixed 'EN7572_BOB_TSSI_OFFSET' "$driver" \
	'AdaptivePon port must restore per-eye TX DDMI calibration'
require_fixed 'EN7572_LOOP_ENABLE, 0' "$driver" \
	'AdaptivePon port must reset the dual-closed loop'
require_fixed 'EN7572_LOOP_ENABLE, EN7572_LOOP_ENABLE' "$driver" \
	'AdaptivePon port must restart the dual-closed loop'
require_fixed 'ret = en7572_verify_tx_eye(priv, bank, false);' "$driver" \
	'AdaptivePon write must verify the complete active TX eye by readback'
require_fixed 'ret = en7572_verify_tx_eye(priv, bank, true);' "$driver" \
	'runtime owner validation must retain the stable TX-eye readback gate'
require_fixed 'active TX-eye readback mismatch' "$driver" \
	'active TX-eye mismatch must remain observable'
require_fixed 'en7572_mode_uses_shared_10g_bob(previous_mode) &&' "$driver" \
	'four 10G modes must use the cached shared-BOB fast path'
require_fixed 'ret = en7572_switch_10g_mode(priv, previous_mode, mode);' "$driver" \
	'10G runtime switching must use the SDK AdaptivePon-style path'
require_fixed 'if (target_bank != previous_bank)' "$driver" \
	'same-eye XG/XGS and XE/XES switches must suppress redundant writes'
require_fixed 'rollback_ret = en7572_select_tx_eye(priv, priv->calibration,' "$driver" \
	'failed eye selection must restore the previous calibrated eye'
require_fixed 'priv->calibration_valid = false;' "$driver" \
	'uncertain optical calibration rollback must invalidate the cache'
require_fixed 'nvmem-cell-names = "calibration-gpon", "calibration-10g";' "$dts" \
	'DTS must expose the shared 10G calibration record'

if grep -Eq 'en7572-bob-(xgpon|epon-10g)' "$driver"; then
	echo 'per-protocol 10G BOB filenames must not replace SDK dual-eye selection' >&2
	exit 1
fi
